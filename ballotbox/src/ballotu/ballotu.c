/*
 * ballotu.c - the real voter client entry point.
 *
 * Replaces the demo mock (main.c/mock.c/mock.h/screens.c, left on disk
 * untouched) with a client that actually talks to ballotd: TCP + tetrissh
 * to the voter channel, the shared HTTTP codec, and libballotclient's
 * bu_join/bu_submit_vote session flows. One self-contained file rather than
 * a main.c + screens.c split, so the old files and this one can never be
 * wildcarded together into the same binary by accident.
 *
 * Flow: connect screen (host/port, asked - not assumed from a flag), then the
 * log-in-or-register gate, then the voter menu. Each screen hands back a
 * bu_screen_t so the menu loop learns when the session has died and can offer
 * a reconnect instead of failing every later choice in turn; Esc walks back one
 * step at each level rather than exiting the program. Shape and reasoning
 * follow tetriSH's tetrisu (src/tetrisu/screens.c, screen_connect +
 * ScreenResult), the client this one was originally modelled on.
 *
 * Identity is real, not typed-and-trusted: after connecting, this file runs
 * a genuine log-in-or-register exchange (bcl_auth, over the same session,
 * answered daemon-side by libtetrisauth's auth_login() - unmodified, the
 * same library and the same PBKDF2/JWT machinery tetriSH's own tetrisu
 * uses) before anything voter-shaped is possible. The certificate name used
 * for eligibility from here on is the SERVER's confirmed username, not
 * anything this file could claim on its own - ballotd overwrites whatever a
 * request's Cert-Name header says with the auth_login()-verified identity
 * (see ballotd/session.c), so a forged header buys nothing.
 */

#include "libballotclient/voter.h"
#include "libtetrisui/tetrisui.h"
#include "libtetrisutil/name.h"

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 7676
#define DEFAULT_CA_PATH "auth/cacsertificate.crt"

static bcl_ctx *g_ctx;
static bu_session_t g_session;

static char g_host[64];
static int g_port;
static char g_ca_path[512];

/* "host:port", as the status bar shows it. Written once per successful connect
 * so every screen can report where this client is talking to without each of
 * them re-formatting it. */
static char g_where[40];

/* ---- bb_result_t -> voter-facing text -------------------------------- */

static const char *result_text(bb_result_t rc) {
  switch (rc) {
  case BB_OK:
    return "OK";
  case BB_ERR_NOT_OPEN:
    return "The election is not open.";
  case BB_ERR_CLOSED:
    return "Election closed mid-submit. Rejected by System.";
  case BB_ERR_NOT_ELIGIBLE:
    return "Your cert is not on the eligible-voter list.";
  case BB_ERR_CERT_INVALID:
    return "Cert rejected.";
  case BB_ERR_CERT_EXPIRED:
    return "Cert expired or forged.";
  case BB_ERR_REPLAY:
    return "That ballot was already submitted (replay).";
  case BB_ERR_BAD_OPTION:
    return "Selected option was out of range.";
  case BB_ERR_DECRYPT:
    return "Ballot could not be decrypted.";
  case BB_ERR_NOT_PUBLISHED:
    return "Results not available.";
  case BB_ERR_NOT_FOUND:
    return "Not found.";
  case BB_ERR_NOT_IMPLEMENTED:
    return "The backend storage is not wired up yet - try again once it is.";
  case BB_ERR_DB:
    return "Could not reach ballotd.";
  default:
    return "Rejected by System.";
  }
}

/* ---- what a screen wants the caller to do next --------------------------- */

/*
 * Screens used to be void, which left them unable to say the one thing that
 * matters most: that the transport died under them. The menu loop then kept
 * offering voter actions against a dead session, each failing with its own
 * box, forever - no reconnect, no exit. Same fix tetrisu made with its
 * ScreenResult, minus the states BallotBox has no equivalent of (there is no
 * pushed game to start, and no room to leave).
 */
typedef enum {
  BU_SCREEN_OK,          /* finished; back to the menu that called it */
  BU_SCREEN_QUIT,        /* the voter asked to leave */
  BU_SCREEN_DISCONNECTED /* the session is gone; reconnect or quit */
} bu_screen_t;

/*
 * Classify what just happened to a call that failed.
 *
 * Asks the transport rather than reading the bb_result_t, because BB_ERR_DB is
 * two different things - "the wire broke" and "the daemon answered, and its
 * answer was a database failure" - and only the first is worth offering a
 * reconnect for. See client.h's bcl_connected.
 */
static bu_screen_t still_live(void) {
  return bcl_connected(g_ctx) ? BU_SCREEN_OK : BU_SCREEN_DISCONNECTED;
}

/* ---- small helpers ------------------------------------------------------- */

/* One- and two-line message boxes.
 *
 * Every report in this file was three lines of ceremony - an array of
 * const char *, then the call - repeated some twenty times, which buried the
 * sentence being said inside the machinery of saying it. Same helper, same
 * reason, as tetrisu's screens.c say(). */
static void say(const char *title, const char *line) {
  const char *lines[] = {line};
  tetrisui_message(title, lines, 1);
}

static void say2(const char *title, const char *first, const char *second) {
  const char *lines[] = {first, second};
  tetrisui_message(title, lines, 2);
}

/*
 * Election ids, checked to the rule the wire actually imposes.
 *
 * The id travels in the HTTTP request line (codec.c's path_election_id), so a
 * space or a control byte in it is not a rejected election - it is a malformed
 * frame, answered 400, with nothing in it for the voter to read. Checking here
 * means the refusal can name the real rule.
 *
 * Deliberately no shape rule beyond the character set: ballotctl auto-allocates
 * "E-<n>", but nothing in libballotbrain requires that form, so a client-side
 * pattern match would refuse ids the daemon is perfectly willing to hold.
 */
static bool election_id_ok(const char *s) {
  size_t n = strlen(s);
  if (n == 0 || n >= BB_ID_LEN) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) {
      return false;
    }
  }
  return true;
}

/*
 * A receipt hash is bb_issue_receipt's output: exactly 64 hex characters
 * (BB_HASH_LEN counts the NUL too).
 *
 * Folded to lowercase in place on the way past, because the stored value is
 * lowercase and the daemon's comparison is literal - so a voter who typed or
 * pasted their receipt in capitals would otherwise be told their ballot was
 * DROPPED, the most alarming sentence this client can produce, over a
 * transcription habit.
 */
static bool receipt_hash_ok(char *s) {
  size_t n = strlen(s);
  if (n != BB_HASH_LEN - 1) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'F') {
      c = (char)(c - 'A' + 'a');
      s[i] = c;
    }
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

/* Port as a TCP port, not as whatever atoi() makes of it: "abc" is 0 and
 * "99999" is a number no socket will ever carry, and both used to be accepted
 * silently by the -p flag. */
static bool port_ok(const char *s, int *out) {
  if (s == NULL || *s == '\0') {
    return false;
  }
  char *end = NULL;
  long v = strtol(s, &end, 10);
  if (end == NULL || *end != '\0' || v < 1 || v > 65535) {
    return false;
  }
  *out = (int)v;
  return true;
}

/*
 * Ask for an election id until the answer is usable, or the voter backs out.
 *
 * Returns 1 with `out` filled, 0 on Esc. Empty input re-prompts instead of
 * closing the screen: every input screen here used to `return` on an empty
 * string, so a stray Enter threw the voter all the way back to the main menu
 * with no explanation. tetrisu's screen_join_room continues in the same spot,
 * for the same reason.
 */
static int ask_election_id(const char *title, const char *prompt,
                           char out[BB_ID_LEN]) {
  for (;;) {
    out[0] = '\0';
    if (tetrisui_input(title, prompt, out, BB_ID_LEN) != 0) {
      return 0;
    }
    if (out[0] == '\0') {
      continue;
    }
    if (!election_id_ok(out)) {
      say2("Invalid election ID",
           "Use letters, digits, '-' or '_' only (e.g. E-100),",
           "up to 15 characters.");
      continue;
    }
    return 1;
  }
}

/* Same contract as ask_election_id, for the UC-6 receipt hash. */
static int ask_receipt_hash(const char *title, char out[BB_HASH_LEN]) {
  for (;;) {
    out[0] = '\0';
    if (tetrisui_input(title, "Enter your receipt hash:", out, BB_HASH_LEN) !=
        0) {
      return 0;
    }
    if (out[0] == '\0') {
      continue;
    }
    if (!receipt_hash_ok(out)) {
      say2("Invalid receipt hash",
           "A receipt is exactly 64 hexadecimal characters -",
           "the hash shown when the ballot was cast.");
      continue;
    }
    return 1;
  }
}

/* ---- login / connect ---------------------------------------------------- */

/* REGISTER's password bounds, mirroring libtetrisauth/credential.c's private
 * CRED_PASS_MIN/CRED_PASS_MAX (not exported - auth_login() enforces the
 * real rule server-side regardless; this only saves a wire round trip on
 * input that would just come back 400). Never enforced at LOGIN: raising
 * this later must not make an existing shorter password unloggable, same
 * reasoning tetriSH's own client follows. */
#define AUTH_PASS_MIN 8
#define AUTH_PASS_MAX 128

/* One log-in-or-register attempt: a two-field form (password masked, bit 1),
 * sent as a real LOGIN/REGISTER over the session already open, answered by
 * ballotd's auth_login(). Returns 1 with g_session.cert_name set to the
 * server-confirmed username on success, 0 if the form was cancelled back to
 * the caller's menu. Loops on a rejection (wrong password, taken username,
 * ...) by reopening the same form with an inline error, same shape as
 * tetrisu's screen_auth.c. */
static int screen_credential_flow(bool registering) {
  char values[2][TETRISUI_FIELD_LEN] = {"", ""};
  const char *labels[2] = {"Username", "Password"};
  const char *error = NULL;
  int start_field = 0;

  for (;;) {
    if (tetrisui_form_ex(registering ? "Register" : "Log in", labels, values, 2,
                         1u << 1, error, start_field) != 0) {
      return 0; /* cancelled: back to the Log in / Register menu */
    }

    char folded[MAX_USER_NAME];
    size_t ulen = strlen(values[0]);
    if (!user_name_ok(values[0], ulen) ||
        user_name_fold(folded, sizeof folded, values[0], ulen) != 0) {
      error = "Username must be 1-15 characters: letters, digits, _ or -.";
      start_field = 0;
      continue;
    }

    size_t plen = strlen(values[1]);
    if (plen == 0) {
      error = "Password is required.";
      start_field = 1;
      continue;
    }
    if (registering && (plen < AUTH_PASS_MIN || plen > AUTH_PASS_MAX)) {
      error = "Password must be 8-128 characters.";
      start_field = 1;
      continue;
    }

    const char *steps[] = {registering ? "Registering" : "Logging in"};
    tetrisui_progress_begin("Contacting ballotd", steps, 1);
    int status = 0;
    int rc = bcl_auth(g_ctx, registering ? "REGISTER" : "LOGIN", folded,
                      values[1], &status);
    tetrisui_progress_step(0, rc == 0 && status == 200);
    tetrisui_progress_end();
    memset(values[1], 0,
           sizeof values[1]); /* scrub the typed password either way */

    if (rc != 0) {
      error = "Could not reach ballotd.";
      start_field = 0;
      continue;
    }

    switch (status) {
    case 200:
      memset(&g_session, 0, sizeof(g_session));
      snprintf(g_session.cert_name, BB_CERT_LEN, "%s", folded);
      /* The state field carries where we are connected until a JOIN gives it
       * something better to say (screen_join_election overwrites it with the
       * election). Blank was the old value, and it hid the one fact a voter on
       * a shared machine most needs to confirm before voting: which ballotd
       * this client is actually talking to. */
      tetrisui_set_status("ballotu", folded, g_where);
      return 1;
    case 400:
      error = "Rejected: malformed request.";
      start_field = 0;
      break;
    case 401:
      error = "Incorrect password.";
      start_field = 1;
      break;
    case 404:
      error = "No account with that username. Register instead?";
      start_field = 0;
      break;
    case 409:
      error = "That username is already taken - pick another.";
      start_field = 0;
      break;
    case 500:
    default:
      error = "Could not reach the account service - try again shortly.";
      start_field = 0;
      break;
    }
  }
}

/* What a failed connect attempt should say, per cause (client.h's bcl_conn_t).
 * One sentence each, naming what the voter can act on - the point of
 * distinguishing the causes at all. */
static const char *conn_text(bcl_conn_t why) {
  switch (why) {
  case BCL_CONN_ERR_ADDRESS:
    return "That is not a dotted-quad IPv4 address (e.g. 127.0.0.1).";
  case BCL_CONN_ERR_REFUSED:
    return "No route to that address, or nothing is listening on that port.";
  case BCL_CONN_ERR_CERT:
    return "Reached a server, but its certificate failed CA verification.";
  case BCL_CONN_ERR_IO:
    return "Reached a server, which then closed the connection mid-handshake.";
  case BCL_CONN_ERR_PROTO:
    return "Reached a server, but the secure handshake failed.";
  case BCL_CONN_ERR_SOCKET:
  case BCL_CONN_OK:
  default:
    return "Could not open a socket on this machine.";
  }
}

/*
 * Where to vote, asked rather than assumed.
 *
 * The address used to be compile-time (DEFAULT_HOST) or a flag, and the client
 * dialled it before drawing anything - so a voter on the wrong machine had no
 * way to reach the right ballotd, and no moment at which to notice that it was
 * connecting at all. Same screen, and same retry-in-place shape, as tetrisu's
 * screen_connect: -H/-p now prefill this form instead of being the only way in.
 *
 * Returns 1 connected (g_host/g_port/g_where updated), 0 if the voter quit.
 */
static int screen_connect(void) {
  char host_buf[sizeof g_host];
  char port_buf[16];
  snprintf(host_buf, sizeof host_buf, "%s", g_host);
  snprintf(port_buf, sizeof port_buf, "%d", g_port);

  const char *error = NULL;
  int start_field = 0;

  for (;;) {
    const char *labels[2] = {"Server IP", "Port"};
    char values[2][TETRISUI_FIELD_LEN];
    snprintf(values[0], TETRISUI_FIELD_LEN, "%s", host_buf);
    snprintf(values[1], TETRISUI_FIELD_LEN, "%s", port_buf);

    if (tetrisui_form_ex("ballotu - connect", labels, values, 2, 0u, error,
                         start_field) != 0) {
      return 0; /* Esc on the first screen means quit */
    }

    /* Keep what was typed even when it is refused below, so a correction is an
     * edit rather than a retype. */
    snprintf(host_buf, sizeof host_buf, "%s", values[0]);
    snprintf(port_buf, sizeof port_buf, "%s", values[1]);

    int port = 0;
    if (!port_ok(port_buf, &port)) {
      error = "Port must be a number between 1 and 65535.";
      start_field = 1;
      continue;
    }

    /* Two steps, reported from two different facts - see client.h. A rejected
     * certificate is NOT an unreachable server, and marking step 0 failed for
     * it would send the voter to check a machine that answered fine. */
    const char *steps[] = {"Opening TCP connection",
                           "tetrissh handshake (verifying server cert)"};
    tetrisui_progress_begin("Connecting", steps, 2);
    bcl_conn_t why = BCL_CONN_OK;
    bb_result_t rc = bcl_connect_why(g_ctx, host_buf, port, g_ca_path, &why);
    bool tcp_up = (rc == BB_OK) || why == BCL_CONN_ERR_CERT ||
                  why == BCL_CONN_ERR_IO || why == BCL_CONN_ERR_PROTO;
    tetrisui_progress_step(0, tcp_up);
    if (tcp_up) {
      /* Left as "..." when the TCP step failed: the handshake was never
       * attempted, and FAILED would claim otherwise. */
      tetrisui_progress_step(1, rc == BB_OK);
    }
    tetrisui_progress_end();

    if (rc != BB_OK) {
      /* Reported in the re-opened form, not in a message box first: the box
       * said the same sentence the form is about to show inline, so it cost a
       * keypress to be told twice. The cursor goes to the host either way -
       * for a refusal the port is as likely to be wrong as the address, but
       * guessing "port" and being wrong moves the voter's attention away from
       * the field they need. Only a port that failed to PARSE is certain
       * enough to jump to (above). */
      error = conn_text(why);
      start_field = 0;
      continue;
    }

    snprintf(g_host, sizeof g_host, "%s", host_buf);
    g_port = port;
    snprintf(g_where, sizeof g_where, "%s:%d", g_host, g_port);
    tetrisui_set_status("ballotu", "(not logged in)", g_where);
    return 1;
  }
}

/*
 * The log-in-or-register gate, over the session screen_connect just opened.
 *
 * Returns 1 authenticated, 0 if the voter backed out - which means "back to the
 * connect screen", not "quit": having chosen the wrong server is exactly the
 * mistake this screen is where you notice, and the old code's only answer to
 * Esc here was to exit the program.
 */
static int screen_auth(void) {
  for (;;) {
    const char *items[] = {"Log in", "Register"};
    int sel = tetrisui_menu("ballotu - login", items, 2,
                            "Up/Down move  Enter select  Esc back to connect");
    if (sel < 0) {
      return 0;
    }
    if (screen_credential_flow(sel == 1)) {
      return 1;
    }
    /* form was cancelled: stay connected, back to this menu */
  }
}

/* ---- UC-2: join -----------------------------------------------------------
 */

/* Defined below (UC-3/UC-4) - forward declared so screen_join_election can
 * actually route into it rather than just announcing that it would. */
static bu_screen_t cast_common(int is_update);

static bu_screen_t screen_join_election(void) {
  char id[BB_ID_LEN] = "";
  if (!ask_election_id("Join election (UC-2)",
                       "Enter election ID (e.g. E-100):", id)) {
    return BU_SCREEN_OK;
  }

  const char *steps[] = {"Contacting ballotd"};
  tetrisui_progress_begin("Joining election", steps, 1);
  bu_join_outcome_t outcome =
      bu_join(g_ctx, &g_session, id, g_session.cert_name);
  tetrisui_progress_step(0, outcome != BU_JOIN_TIMEOUT);
  tetrisui_progress_end();

  switch (outcome) {
  case BU_JOIN_TIMEOUT:
    /* bu_classify_join folds every non-verdict into TIMEOUT, so this covers
     * both "the wire broke" and "the daemon's DB seam refused". still_live
     * separates them: only the first is worth a reconnect offer. */
    say2("Join failed", "Could not reach the election.",
         result_text(BB_ERR_NOT_IMPLEMENTED));
    return still_live();
  case BU_JOIN_NOT_FOUND: {
    char line[64];
    snprintf(line, sizeof(line), "Election '%s' not found.", id);
    say("Join failed", line);
    return BU_SCREEN_OK;
  }
  case BU_JOIN_NOT_ELIGIBLE:
    say2("Join refused", "Your cert is not on the eligible-voter list",
         "for this election. Refused.");
    return BU_SCREEN_OK;
  case BU_JOIN_NOT_OPEN: {
    char line[96];
    snprintf(line, sizeof(line), "Cannot join %s: election is not Open.", id);
    say2("Join refused", line, "Refused.");
    return BU_SCREEN_OK;
  }
  case BU_JOIN_ADMITTED:
    break;
  }

  /* The status bar's third field carried the server address until now (set by
   * screen_connect) - once a JOIN is admitted there is a current election to
   * show, which is the more useful of the two from here on, so it takes the
   * field. g_state is 32 bytes; snprintf here guarantees NUL termination
   * within that even if id+title together would not fit, unlike
   * tetrisui_set_status's own strncpy on a too-long source. */
  char state[32];
  snprintf(state, sizeof(state), "%s: %s", g_session.election_id,
           g_session.title);
  tetrisui_set_status("ballotu", g_session.cert_name, state);

  if (g_session.has_ballot) {
    say2("Already voted", "You already have a ballot for this election.",
         "Routing you to Update Vote (UC-4).");
    /* the message above is the routing, not just an announcement of it */
    return cast_common(1);
  }

  char lines_buf[BB_MAX_OPTIONS + 2][96];
  const char *lines[BB_MAX_OPTIONS + 2];
  snprintf(lines_buf[0], sizeof(lines_buf[0]), "Joined %s: %s",
           g_session.election_id, g_session.title);
  lines[0] = lines_buf[0];
  snprintf(lines_buf[1], sizeof(lines_buf[1]), "Ballot options:");
  lines[1] = lines_buf[1];
  for (int i = 0; i < g_session.option_count; i++) {
    snprintf(lines_buf[i + 2], sizeof(lines_buf[i + 2]), "  %d) %s", i + 1,
             g_session.options[i]);
    lines[i + 2] = lines_buf[i + 2];
  }
  tetrisui_message("Join successful", lines, g_session.option_count + 2);
  return BU_SCREEN_OK;
}

/* ---- UC-3 / UC-4: cast / update --------------------------------------------
 */

static bu_screen_t cast_common(int is_update) {
  if (!g_session.joined) {
    say("Not joined", "You must join an election first (UC-2).");
    return BU_SCREEN_OK;
  }

  bu_vote_action_t action = bu_route_vote(&g_session);
  if (is_update && action == BU_CAST) {
    say2("Nothing to update", "You have no prior ballot yet.",
         "Routing you to Cast Vote (UC-3).");
    is_update = 0;
  } else if (!is_update && action == BU_UPDATE) {
    say2("Already voted", "You already have a final ballot.",
         "Routing you to Update Vote (UC-4).");
    is_update = 1;
  }

  const char *items[BB_MAX_OPTIONS];
  for (int i = 0; i < g_session.option_count; i++)
    items[i] = g_session.options[i];

  char title[96];
  if (is_update) {
    snprintf(title, sizeof(title), "Update vote (prior ballot v%d exists)",
             g_session.ballot_version);
  } else {
    snprintf(title, sizeof(title), "Cast vote: %s", g_session.title);
  }
  int sel = tetrisui_menu(title, items, g_session.option_count,
                          "Up/Down move  Enter select  Esc cancel");
  if (sel < 0) {
    return BU_SCREEN_OK;
  }

  char q[96];
  snprintf(q, sizeof(q), "Confirm vote for '%s'?", g_session.options[sel]);
  if (!tetrisui_confirm("Confirm ballot", q)) {
    return BU_SCREEN_OK;
  }

  char nonce[BB_NONCE_LEN];
  snprintf(nonce, sizeof(nonce), "%08lx%08x", (unsigned long)time(NULL),
           rand());

  const char *steps[] = {"Encrypting ballot and submitting"};
  tetrisui_progress_begin(
      is_update ? "Submitting updated ballot" : "Submitting ballot", steps, 1);
  bb_receipt_t receipt;
  memset(&receipt, 0, sizeof(receipt));
  bb_result_t rc = bu_submit_vote(g_ctx, &g_session, sel, nonce, &receipt);
  tetrisui_progress_step(0, rc == BB_OK);
  tetrisui_progress_end();

  if (rc != BB_OK) {
    say("Vote rejected", result_text(rc));
    return still_live();
  }

  const char *lines[] = {is_update ? "Vote updated. New receipt issued:"
                                   : "Vote recorded. Receipt issued:",
                         receipt.hash,
                         "Keep this hash to check your vote later (UC-6)."};
  tetrisui_message("Success", lines, 3);
  return BU_SCREEN_OK;
}

static bu_screen_t screen_cast_vote(void) { return cast_common(0); }
static bu_screen_t screen_update_vote(void) { return cast_common(1); }

/* ---- UC-5: results ----------------------------------------------------------
 */

static bu_screen_t screen_view_results(void) {
  char id[BB_ID_LEN] = "";
  if (!ask_election_id("View results (UC-5)", "Enter election ID:", id)) {
    return BU_SCREEN_OK;
  }

  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  req.op = BCL_RESULTS;
  snprintf(req.election_id, BB_ID_LEN, "%s", id);
  snprintf(req.cert_name, BB_CERT_LEN, "%s", g_session.cert_name);

  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  bb_result_t rc = bcl_send(g_ctx, &req, &resp);
  bb_result_t status = (rc != BB_OK) ? rc : resp.status;

  if (status != BB_OK) {
    say("Not published", result_text(status));
    return still_live();
  }

  /* Counted tally only - no per-ballot hash listing. Each voter already saw
   * their own receipt hash at cast/update time (UC-3/UC-4) and can verify it
   * individually via Check your vote (UC-6); listing every hash here bought
   * nothing a voter needed and only added noise to the result. */
  enum { MAX_LINES = BB_MAX_OPTIONS + 3 };
  static char buf[MAX_LINES][96];
  const char *lines[MAX_LINES];
  int n = 0;

  snprintf(buf[n], sizeof(buf[n]), "Title: %s", resp.election.title);
  lines[n] = buf[n];
  n++;
  snprintf(buf[n], sizeof(buf[n]), "ID: %s", id);
  lines[n] = buf[n];
  n++;
  snprintf(buf[n], sizeof(buf[n]), "Tally:");
  lines[n] = buf[n];
  n++;
  for (int i = 0; i < resp.option_count; i++) {
    char bar[32];
    int fill = resp.tally[i] > 20 ? 20 : resp.tally[i];
    if (fill < 0)
      fill = 0;
    memset(bar, '#', (size_t)fill);
    bar[fill] = '\0';
    snprintf(buf[n], sizeof(buf[n]), "  %-10s %3d %s", resp.options[i],
             resp.tally[i], bar);
    lines[n] = buf[n];
    n++;
  }
  tetrisui_list_view("Election results", lines, n);
  return BU_SCREEN_OK;
}

/* ---- UC-6: check your vote --------------------------------------------------
 */

static bu_screen_t screen_check_vote(void) {
  /* Server-side FIND_HASH is a direct, literal lookup of the receipt hash
   * bu_submit_vote showed after casting (bb_issue_receipt derives it from
   * the ballot's nonce+version, not from any client-held secret) - so this
   * screen asks for and sends that same hash verbatim. bu_derive_receipt
   * (a from-a-secret-key KDF) is unrelated to that derivation today and
   * would never match a real stored hash; it stays as scaffolding for a
   * future real commitment scheme, just not wired in here until the server
   * side actually uses a matching KDF. */
  char hash[BB_HASH_LEN] = "";
  if (!ask_receipt_hash("Check your vote (UC-6)", hash)) {
    return BU_SCREEN_OK;
  }

  char id[BB_ID_LEN] = "";
  if (!ask_election_id("Check your vote (UC-6)",
                       "Enter the election ID to check against:", id)) {
    return BU_SCREEN_OK;
  }

  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  req.op = BCL_CHECK;
  snprintf(req.election_id, BB_ID_LEN, "%s", id);
  snprintf(req.hash, BB_HASH_LEN, "%s", hash);

  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  bb_result_t rc = bcl_send(g_ctx, &req, &resp);
  bb_result_t status = (rc != BB_OK) ? rc : resp.status;

  char line1[96];
  snprintf(line1, sizeof(line1), "Receipt hash: %s", hash);

  bu_check_outcome_t outcome = bu_classify_check(status, resp.found);
  switch (outcome) {
  case BU_CHECK_COUNTED: {
    char line2[96];
    snprintf(line2, sizeof(line2),
             "Your vote is option index %d (%s) and is included in the tally.",
             resp.found_option, resp.found_option_name);
    say2("Verified", line1, line2);
    return BU_SCREEN_OK;
  }
  case BU_CHECK_DROPPED: {
    const char *lines[] = {
        line1, "Hash not found among live (non-superseded) ballots.",
        "Verification FAILED - dropped ballot.",
        "Please raise this with the Admin."};
    tetrisui_message("Verification failed", lines, 4);
    return BU_SCREEN_OK;
  }
  case BU_CHECK_UNAVAILABLE:
    say2("Verification unavailable", line1, result_text(status));
    /* Never reported as a dropped ballot (bu_classify_check is careful about
     * exactly this), so a dead session must not look like a lost vote - it
     * looks like a reconnect. */
    return still_live();
  }
  return BU_SCREEN_OK;
}

/* ---- the voter menu, and what to do when the session dies ---------------- */

/*
 * One pass through the voter menu. Returns BU_SCREEN_QUIT when the voter is
 * done, or BU_SCREEN_DISCONNECTED the moment any screen reports that the
 * session is gone - which is the point of threading the result out of the
 * screens at all. Before this, a dead session meant every menu choice from
 * then on drew its own failure box and the menu offered them again forever.
 */
static bu_screen_t voter_menu(void) {
  const char *items[] = {"Join election (UC-2)",   "Cast vote (UC-3)",
                         "Update vote (UC-4)",     "View results (UC-5)",
                         "Check your vote (UC-6)", "Quit"};
  for (;;) {
    int sel = tetrisui_menu("ballotu - voter menu", items, 6,
                            "Up/Down move  Enter select  q quit");
    if (sel < 0 || sel == 5) {
      return BU_SCREEN_QUIT;
    }

    bu_screen_t r = BU_SCREEN_OK;
    switch (sel) {
    case 0:
      r = screen_join_election();
      break;
    case 1:
      r = screen_cast_vote();
      break;
    case 2:
      r = screen_update_vote();
      break;
    case 3:
      r = screen_view_results();
      break;
    case 4:
      r = screen_check_vote();
      break;
    }
    if (r == BU_SCREEN_DISCONNECTED) {
      return r;
    }
  }
}

/* Offered only on a real disconnect, and only as a question: a client that
 * reconnected silently would hide from the voter that their session - and with
 * it the JOIN their next vote depends on - was rebuilt underneath them. */
static int offer_reconnect(void) {
  return tetrisui_confirm("Disconnected",
                          "The connection to ballotd is gone. Reconnect?");
}

/* ---- entry point ------------------------------------------------------------
 */

/*
 * Ctrl-C with a curses screen up: put the terminal back before dying.
 *
 * Without this the process is gone while the tty is still in cbreak/noecho
 * (tetrisui_init), so the shell the voter returns to has no echo and no line
 * editing - a broken terminal, from the outside indistinguishable from a
 * broken machine. ncurses does install a cleanup handler of its own, but only
 * while the disposition is still SIG_DFL, and it exits 1 - which reads as "the
 * client failed" rather than "the voter interrupted it". Own the handler so
 * both halves are ours to promise: terminal restored, and 128 + signo, the
 * status a shell decodes back into SIGINT.
 *
 * _Exit, not exit: atexit handlers and stdio flushing are not async-signal
 * safe. endwin() (inside tetrisui_shutdown) is not either, strictly speaking -
 * but a wedged terminal is the worse of the two failures, and this is the same
 * trade tetriSH's own tetrisu makes for the same reason.
 */
static void handle_sigint(int signo) {
  (void)signo;
  tetrisui_shutdown();
  _Exit(128 + SIGINT);
}

static void usage(FILE *out, const char *argv0) {
  fprintf(
      out,
      "usage: %s [-H host] [-p port] [-C ca_cert] [-h]\n"
      "  -H host     prefill the connect screen's host, dotted-quad (default "
      "%s)\n"
      "  -p port     prefill the connect screen's TCP port (default %d)\n"
      "  -C ca_cert  CA certificate PEM to verify ballotd against (default "
      "%s)\n"
      "  -h          show this help\n",
      argv0, DEFAULT_HOST, DEFAULT_PORT, DEFAULT_CA_PATH);
}

int main(int argc, char **argv) {
  snprintf(g_host, sizeof(g_host), "%s", DEFAULT_HOST);
  g_port = DEFAULT_PORT;
  snprintf(g_ca_path, sizeof(g_ca_path), "%s", DEFAULT_CA_PATH);

  int opt;
  while ((opt = getopt(argc, argv, "H:p:C:h")) != -1) {
    switch (opt) {
    case 'H':
      snprintf(g_host, sizeof(g_host), "%s", optarg);
      break;
    case 'p':
      /* Refused here rather than carried into the connect screen: atoi turned
       * "-p abc" into 0 and accepted "-p 99999", both of which then failed
       * inside bcl_connect as an unexplained "could not reach ballotd". */
      if (!port_ok(optarg, &g_port)) {
        fprintf(stderr, "ballotu: bad port: %s\n", optarg);
        return 2;
      }
      break;
    case 'C':
      snprintf(g_ca_path, sizeof(g_ca_path), "%s", optarg);
      break;
    case 'h':
      usage(stdout, argv[0]);
      return 0;
    default:
      usage(stderr, argv[0]);
      return 2;
    }
  }

  srand((unsigned)time(NULL));

  /* A ballotd that dies mid-request must not take this client with it: without
   * this, the write inside bcl_send raises SIGPIPE and the default action kills
   * us outright - with the terminal still in curses mode, and before any
   * screen can report what happened. ballotd already ignores it on both sides
   * of its own sockets (ballotd/main.c, ballotd/session.c); the client half
   * was missing. Errors from the write are handled where they are returned. */
  signal(SIGPIPE, SIG_IGN);

  g_ctx = bcl_create();
  if (g_ctx == NULL) {
    fprintf(stderr, "ballotu: bcl_create failed\n");
    return 1;
  }

  tetrisui_init();
  /* After tetrisui_init, never before: the handler tears down curses state, so
   * it must not be reachable until there is curses state to tear down. */
  signal(SIGINT, handle_sigint);
  tetrisui_set_status("ballotu", "(not logged in)", "");

  for (;;) {
    /* Esc on the connect form is the only way out of the program besides Quit:
     * it is the first screen, so there is nothing behind it to go back to. */
    if (!screen_connect()) {
      break;
    }

    /* Esc at the login menu means "wrong server", not "quit" - drop the
     * session and re-open the connect form with the address still in it. */
    if (!screen_auth()) {
      bcl_disconnect(g_ctx);
      tetrisui_set_status("ballotu", "(not logged in)", "");
      continue;
    }

    bu_screen_t r = voter_menu();
    if (r != BU_SCREEN_DISCONNECTED) {
      break; /* the voter chose Quit */
    }

    /* The session is gone. Close what is left of it before asking, so a
     * declined reconnect does not leave a dead transport attached to the ctx,
     * and so the retry starts from a clean one. */
    bcl_disconnect(g_ctx);
    memset(&g_session, 0, sizeof(g_session));
    tetrisui_set_status("ballotu", "(not logged in)", "");
    if (!offer_reconnect()) {
      break;
    }
  }

  tetrisui_shutdown();
  bcl_disconnect(g_ctx);
  bcl_destroy(g_ctx);
  return 0;
}
