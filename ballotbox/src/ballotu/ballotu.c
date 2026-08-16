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
      tetrisui_set_status("ballotu", folded, "");
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

static int screen_login(void) {
  const char *steps[] = {"Opening secure session",
                         "Presenting cert (tetrissh handshake)"};
  tetrisui_progress_begin("Connecting", steps, 2);
  bb_result_t rc = bcl_connect(g_ctx, g_host, g_port, g_ca_path);
  /* One real round trip covers both steps - see client.h's bcl_connect
   * doc comment for why the two are not reported separately. */
  tetrisui_progress_step(0, rc == BB_OK);
  tetrisui_progress_step(1, rc == BB_OK);
  tetrisui_progress_end();

  if (rc != BB_OK) {
    char line[96];
    snprintf(line, sizeof(line), "Could not reach ballotd at %s:%d.", g_host,
             g_port);
    const char *lines[] = {line, "Check the server is running and try again."};
    tetrisui_message("Connection failed", lines, 2);
    return 0;
  }

  for (;;) {
    const char *items[] = {"Log in", "Register"};
    int sel = tetrisui_menu("ballotu - login", items, 2, "Esc to quit");
    if (sel < 0) {
      bcl_disconnect(g_ctx);
      return 0;
    }
    if (screen_credential_flow(sel == 1))
      return 1;
    /* form was cancelled: stay connected, back to this menu */
  }
}

/* ---- UC-2: join -----------------------------------------------------------
 */

/* Defined below (UC-3/UC-4) - forward declared so screen_join_election can
 * actually route into it rather than just announcing that it would. */
static void cast_common(int is_update);

static void screen_join_election(void) {
  char id[BB_ID_LEN] = "";
  if (tetrisui_input("Join election (UC-2)",
                     "Enter election ID (e.g. E-100):", id, sizeof(id)) != 0)
    return;
  if (strlen(id) == 0)
    return;

  const char *steps[] = {"Contacting ballotd"};
  tetrisui_progress_begin("Joining election", steps, 1);
  bu_join_outcome_t outcome =
      bu_join(g_ctx, &g_session, id, g_session.cert_name);
  tetrisui_progress_step(0, outcome != BU_JOIN_TIMEOUT);
  tetrisui_progress_end();

  switch (outcome) {
  case BU_JOIN_TIMEOUT: {
    const char *lines[] = {"Could not reach the election.",
                           result_text(BB_ERR_NOT_IMPLEMENTED)};
    tetrisui_message("Join failed", lines, 2);
    return;
  }
  case BU_JOIN_NOT_FOUND: {
    char line[64];
    snprintf(line, sizeof(line), "Election '%s' not found.", id);
    const char *lines[] = {line};
    tetrisui_message("Join failed", lines, 1);
    return;
  }
  case BU_JOIN_NOT_ELIGIBLE: {
    const char *lines[] = {"Your cert is not on the eligible-voter list",
                           "for this election. Refused."};
    tetrisui_message("Join refused", lines, 2);
    return;
  }
  case BU_JOIN_NOT_OPEN: {
    char line[96];
    snprintf(line, sizeof(line), "Cannot join %s: election is not Open.", id);
    const char *lines[] = {line, "Refused."};
    tetrisui_message("Join refused", lines, 2);
    return;
  }
  case BU_JOIN_ADMITTED:
    break;
  }

  /* The status bar's third field is otherwise blank for ballotu (set to ""
   * at login) - once a JOIN is admitted there is a current election to show,
   * so it reads there for every screen from here on, same as the app/actor
   * fields already do. g_state is 32 bytes; snprintf here guarantees NUL
   * termination within that even if id+title together would not fit,
   * unlike tetrisui_set_status's own strncpy on a too-long source. */
  char state[32];
  snprintf(state, sizeof(state), "%s: %s", g_session.election_id,
           g_session.title);
  tetrisui_set_status("ballotu", g_session.cert_name, state);

  if (g_session.has_ballot) {
    const char *lines[] = {"You already have a ballot for this election.",
                           "Routing you to Update Vote (UC-4)."};
    tetrisui_message("Already voted", lines, 2);
    cast_common(1); /* the message above is the routing, not just an
                       announcement of it */
    return;
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
}

/* ---- UC-3 / UC-4: cast / update --------------------------------------------
 */

static void cast_common(int is_update) {
  if (!g_session.joined) {
    const char *lines[] = {"You must join an election first (UC-2)."};
    tetrisui_message("Not joined", lines, 1);
    return;
  }

  bu_vote_action_t action = bu_route_vote(&g_session);
  if (is_update && action == BU_CAST) {
    const char *lines[] = {"You have no prior ballot yet.",
                           "Routing you to Cast Vote (UC-3)."};
    tetrisui_message("Nothing to update", lines, 2);
    is_update = 0;
  } else if (!is_update && action == BU_UPDATE) {
    const char *lines[] = {"You already have a final ballot.",
                           "Routing you to Update Vote (UC-4)."};
    tetrisui_message("Already voted", lines, 2);
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
                          "Enter to select your option");
  if (sel < 0)
    return;

  char q[96];
  snprintf(q, sizeof(q), "Confirm vote for '%s'?", g_session.options[sel]);
  if (!tetrisui_confirm("Confirm ballot", q))
    return;

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
    const char *lines[] = {result_text(rc)};
    tetrisui_message("Vote rejected", lines, 1);
    return;
  }

  const char *lines[] = {is_update ? "Vote updated. New receipt issued:"
                                   : "Vote recorded. Receipt issued:",
                         receipt.hash,
                         "Keep this hash to check your vote later (UC-6)."};
  tetrisui_message("Success", lines, 3);
}

static void screen_cast_vote(void) { cast_common(0); }
static void screen_update_vote(void) { cast_common(1); }

/* ---- UC-5: results ----------------------------------------------------------
 */

static void screen_view_results(void) {
  char id[BB_ID_LEN] = "";
  if (tetrisui_input("View results (UC-5)", "Enter election ID:", id,
                     sizeof(id)) != 0)
    return;
  if (strlen(id) == 0)
    return;

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
    const char *lines[] = {result_text(status)};
    tetrisui_message("Not published", lines, 1);
    return;
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
}

/* ---- UC-6: check your vote --------------------------------------------------
 */

static void screen_check_vote(void) {
  /* Server-side FIND_HASH is a direct, literal lookup of the receipt hash
   * bu_submit_vote showed after casting (bb_issue_receipt derives it from
   * the ballot's nonce+version, not from any client-held secret) - so this
   * screen asks for and sends that same hash verbatim. bu_derive_receipt
   * (a from-a-secret-key KDF) is unrelated to that derivation today and
   * would never match a real stored hash; it stays as scaffolding for a
   * future real commitment scheme, just not wired in here until the server
   * side actually uses a matching KDF. */
  char hash[BB_HASH_LEN] = "";
  if (tetrisui_input("Check your vote (UC-6)", "Enter your receipt hash:", hash,
                     sizeof(hash)) != 0)
    return;
  if (strlen(hash) == 0)
    return;

  char id[BB_ID_LEN] = "";
  if (tetrisui_input("Check your vote (UC-6)",
                     "Enter the election ID to check against:", id,
                     sizeof(id)) != 0)
    return;
  if (strlen(id) == 0)
    return;

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
    const char *lines[] = {line1, line2};
    tetrisui_message("Verified", lines, 2);
    return;
  }
  case BU_CHECK_DROPPED: {
    const char *lines[] = {
        line1, "Hash not found among live (non-superseded) ballots.",
        "Verification FAILED - dropped ballot.",
        "Please raise this with the Admin."};
    tetrisui_message("Verification failed", lines, 4);
    return;
  }
  case BU_CHECK_UNAVAILABLE: {
    const char *lines[] = {line1, result_text(status)};
    tetrisui_message("Verification unavailable", lines, 2);
    return;
  }
  }
}

/* ---- entry point ------------------------------------------------------------
 */

static void usage(FILE *out, const char *argv0) {
  fprintf(
      out,
      "usage: %s [-H host] [-p port] [-C ca_cert] [-h]\n"
      "  -H host     ballotd's voter-channel host, dotted-quad (default %s)\n"
      "  -p port     ballotd's voter-channel TCP port (default %d)\n"
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
      g_port = atoi(optarg);
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

  g_ctx = bcl_create();
  if (g_ctx == NULL) {
    fprintf(stderr, "ballotu: bcl_create failed\n");
    return 1;
  }

  tetrisui_init();
  tetrisui_set_status("ballotu", "(not logged in)", "");

  if (screen_login()) {
    const char *items[] = {"Join election (UC-2)",   "Cast vote (UC-3)",
                           "Update vote (UC-4)",     "View results (UC-5)",
                           "Check your vote (UC-6)", "Quit"};
    for (;;) {
      int sel = tetrisui_menu("ballotu - voter menu", items, 6, NULL);
      if (sel < 0 || sel == 5)
        break;
      switch (sel) {
      case 0:
        screen_join_election();
        break;
      case 1:
        screen_cast_vote();
        break;
      case 2:
        screen_update_vote();
        break;
      case 3:
        screen_view_results();
        break;
      case 4:
        screen_check_vote();
        break;
      }
    }
  }

  tetrisui_shutdown();
  bcl_disconnect(g_ctx);
  bcl_destroy(g_ctx);
  return 0;
}
