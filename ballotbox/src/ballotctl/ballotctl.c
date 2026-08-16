/*
 * ballotctl.c - the real admin client entry point.
 *
 * Replaces the demo mock (main.c/mock.c/mock.h/screens.c, left on disk
 * untouched) with a client that actually talks to ballotd's local admin
 * channel: a one-shot AF_UNIX connection per request (bcl_set_ctl_path +
 * bcl_send), never the voter TCP+tetrissh session - ballotctl has no
 * voter-channel traffic at all. See src/ballotu/ballotu.c for the voter
 * side of the same integration; this file mirrors its shape.
 *
 * IMPORTANT, read before running this against a real ballotd: the database
 * seam (db_exec) is still a stub. CREATE "succeeds" (writes return BB_OK
 * and log) but nothing is retrievable afterward - OPEN/CLOSE/PUBLISH and
 * View Results all need a read first and come back BB_ERR_NOT_IMPLEMENTED,
 * for any election id, every time. This is the frozen DB boundary working
 * as intended, not a bug here. Once that lands, nothing in this file needs
 * to change.
 *
 * Also: the admin channel has no cert-identity check at all today (see
 * dispatch.c - none of CREATE/OPEN/CLOSE/PUBLISH call bb_verify_cert), and
 * is local-only by design (same-host access to the socket is the trust
 * boundary, not the cert name typed at login). So login here cannot reject
 * a bad admin cert the way the old mock did; the name is remembered purely
 * to send as Cert-Name.
 *
 * Not carried over: "Election status" (list every election). The real
 * protocol has no list-all operation - every op that names an election
 * takes an explicit id - so there is nothing honest to back that screen
 * with. Look an election up by id (Open/Close/Publish/View Results already
 * ask for one) instead.
 */

#include "libballotclient/admin.h"
#include "libtetrisui/tetrisui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define DEFAULT_CTL_PATH "var/run/ballotd.ctl"

static bcl_ctx *g_ctx;
static char g_cert_name[BB_CERT_LEN];
static char g_ctl_path[256];

/* ---- bb_result_t -> admin-facing text ---------------------------------- */

static const char *result_text(bb_result_t rc) {
  switch (rc) {
    case BB_OK:
      return "OK";
    case BB_ERR_CONFIG_TITLE:
      return "Config invalid: title is required.";
    case BB_ERR_CONFIG_OPTIONS:
      return "Config invalid: at least two options are required.";
    case BB_ERR_CONFIG_TIME:
      return "Config invalid: close time must be after open time.";
    case BB_ERR_CONFIG_ID_TAKEN:
      return "That election ID is already in use - pick another.";
    case BB_ERR_ILLEGAL_TRANSITION:
      return "Not a legal transition from the election's current state.";
    case BB_ERR_NOT_FOUND:
      return "Election not found.";
    case BB_ERR_NOT_PUBLISHED:
      return "Results not available.";
    case BB_ERR_NOT_ELIGIBLE:
      return "Not eligible to view this election's results.";
    case BB_ERR_NOT_IMPLEMENTED:
      return "The backend storage is not wired up yet - try again once it is.";
    case BB_ERR_DB:
      return "Could not reach ballotd.";
    default:
      return "Rejected by System.";
  }
}

/* ---- login / connectivity check ------------------------------------------ */

static int ctl_reachable(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return 0;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

  int ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
  close(fd);
  return ok;
}

/* Same reasoning as split_csv above: identity comparisons are byte-exact,
 * so a stray space typed here must not be the reason a later comparison
 * (e.g. eligibility, if this cert is ever also a voter) fails. */
static void trim_inplace(char *s) {
  size_t start = 0;
  while (s[start] == ' ') start++;
  size_t len = strlen(s + start);
  memmove(s, s + start, len + 1);
  while (len > 0 && s[len - 1] == ' ') s[--len] = '\0';
}

static int screen_login(void) {
  for (;;) {
    char name[BB_CERT_LEN] = "";
    if (tetrisui_input("ballotctl - admin login", "Enter admin cert name (Esc to quit):", name,
                       sizeof(name)) != 0) {
      return 0;
    }
    trim_inplace(name);
    if (strlen(name) == 0) continue;

    const char *steps[] = {"Contacting ballotd admin socket"};
    tetrisui_progress_begin("Authenticating", steps, 1);
    int reachable = ctl_reachable(g_ctl_path);
    tetrisui_progress_step(0, reachable);
    tetrisui_progress_end();

    if (!reachable) {
      char line[sizeof(g_ctl_path) + 64];
      snprintf(line, sizeof(line), "Could not reach ballotd's admin socket at %s.", g_ctl_path);
      const char *lines[] = {line, "Check the server is running and try again."};
      tetrisui_message("Connection failed", lines, 2);
      continue;
    }

    bcl_set_ctl_path(g_ctx, g_ctl_path);
    snprintf(g_cert_name, sizeof(g_cert_name), "%s", name);
    tetrisui_set_status("ballotctl", name, "");
    return 1;
  }
}

/* ---- helpers -------------------------------------------------------------- */

/* Eligibility and every other identity comparison in this system (bb_join,
 * bb_record_ballot, ...) is a byte-exact strcmp with no normalisation - a
 * stray space typed on either side of a comma here silently produces a cert
 * name that can never match what a voter types at login (#seen in testing:
 * "Kenji , Pop, Jedi" stored "Kenji " as the first eligible entry). Trimmed
 * on both ends so the field's own formatting habits cannot cause that.
 *
 * Also used for the options field, which is free text and must NOT be
 * case-folded (unlike eligible voter certs - see bc_fold_eligible,
 * libballotclient/admin.c, applied by the one caller that needs it). */
static void split_csv(const char *src, char out[][BB_OPTION_LEN], int max, int *count) {
  *count = 0;
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", src);
  char *tok = strtok(buf, ",");
  while (tok != NULL && *count < max) {
    while (*tok == ' ') tok++;
    size_t len = strlen(tok);
    while (len > 0 && tok[len - 1] == ' ') tok[--len] = '\0';
    snprintf(out[*count], BB_OPTION_LEN, "%s", tok);
    (*count)++;
    tok = strtok(NULL, ",");
  }
}

static int prompt_election_id(const char *title, char out[BB_ID_LEN]) {
  out[0] = '\0';
  if (tetrisui_input(title, "Enter election ID:", out, BB_ID_LEN) != 0) return -1;
  return strlen(out) == 0 ? -1 : 0;
}

/* ---- UC-1: create ----------------------------------------------------------- */

void screen_create_election(void) {
  char values[6][TETRISUI_FIELD_LEN] = {"", "", "", "", "2026-01-01T09:00:00Z",
                                        "2026-01-08T09:00:00Z"};
  const char *labels[6] = {"Title",
                           "Election ID",
                           "Options (comma-separated)",
                           "Eligible voter usernames (comma-separated, not case-sensitive)",
                           "Open time",
                           "Close time"};

  /* Pre-fill with what CREATE would auto-allocate if left as-is (a plain
   * peek, BCL_ADMIN_NEXT_ID - reserves nothing), so the operator sees a
   * usable default and can edit it instead of typing one from scratch. A
   * failure here just leaves the field blank - CREATE still auto-allocates
   * server-side in that case, same as before this field existed. */
  bcl_request_t peek;
  memset(&peek, 0, sizeof(peek));
  peek.op = BCL_ADMIN_NEXT_ID;
  bcl_response_t peek_resp;
  memset(&peek_resp, 0, sizeof(peek_resp));
  if (bcl_send(g_ctx, &peek, &peek_resp) == BB_OK) {
    snprintf(values[1], sizeof(values[1]), "%s", peek_resp.election.id);
  }

  for (;;) {
    if (tetrisui_form("Create election (UC-1)", labels, values, 6) != 0) return; /* cancelled */

    bb_config_t config;
    memset(&config, 0, sizeof(config));
    snprintf(config.title, BB_TITLE_LEN, "%s", values[0]);
    split_csv(values[2], config.options, BB_MAX_OPTIONS, &config.option_count);
    split_csv(values[3], config.eligible, BB_MAX_VOTERS, &config.eligible_count);
    snprintf(config.open_time, BB_TIME_LEN, "%s", values[4]);
    snprintf(config.close_time, BB_TIME_LEN, "%s", values[5]);

    char bad_entry[BB_CERT_LEN];
    if (bc_fold_eligible(config.eligible, &config.eligible_count, bad_entry, sizeof bad_entry) != 0) {
      char line1[104];
      snprintf(line1, sizeof line1, "'%s' is not a valid username: 1-15 chars, letters/digits/_/-.",
              bad_entry);
      const char *lines[] = {line1, "Staying in Draft - fix and retry."};
      tetrisui_message("Rejected (alt flow 4a)", lines, 2);
      continue;
    }

    bcl_request_t req;
    bb_result_t vr = bc_build_create(&config, &req);
    if (vr != BB_OK) {
      const char *lines[] = {result_text(vr), "Staying in Draft - fix and retry."};
      tetrisui_message("Rejected (alt flow 4a)", lines, 2);
      continue;
    }
    snprintf(req.election_id, BB_ID_LEN, "%s", values[1]);
    snprintf(req.cert_name, BB_CERT_LEN, "%s", g_cert_name);

    const char *steps[] = {"Sending to ballotd"};
    tetrisui_progress_begin("Creating election", steps, 1);
    bcl_response_t resp;
    memset(&resp, 0, sizeof(resp));
    bb_result_t rc = bcl_send(g_ctx, &req, &resp);
    tetrisui_progress_step(0, rc == BB_OK);
    tetrisui_progress_end();

    if (rc != BB_OK) {
      const char *lines[] = {result_text(rc)};
      tetrisui_message("Create failed", lines, 1);
      return;
    }

    char line1[64];
    snprintf(line1, sizeof(line1), "Election %s created in Draft.", resp.election.id);
    const char *lines[] = {line1, "Use Open Election to make it live."};
    tetrisui_message("Created", lines, 2);
    return;
  }
}

/* ---- lifecycle: open / close / publish -------------------------------------- */

static void transition(bcl_op_t op, const char *verb, const char *screen_title) {
  char id[BB_ID_LEN];
  if (prompt_election_id(screen_title, id) != 0) return;

  char q[96];
  snprintf(q, sizeof(q), "%s election %s?", verb, id);
  if (!tetrisui_confirm("Confirm", q)) return;

  bcl_request_t req;
  bc_build_transition(op, id, &req);
  snprintf(req.cert_name, BB_CERT_LEN, "%s", g_cert_name);

  const char *steps[] = {"Sending to ballotd"};
  tetrisui_progress_begin(verb, steps, 1);
  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  bb_result_t rc = bcl_send(g_ctx, &req, &resp);
  tetrisui_progress_step(0, rc == BB_OK);
  tetrisui_progress_end();

  if (rc != BB_OK) {
    const char *lines[] = {result_text(rc)};
    tetrisui_message("Invalid transition", lines, 1);
    return;
  }

  char line1[64];
  snprintf(line1, sizeof(line1), "Election %s: %s done.", id, verb);
  const char *lines[] = {line1};
  tetrisui_message("Done", lines, 1);
}

void screen_open_election(void) { transition(BCL_OPEN, "Open", "Open election"); }
void screen_close_election(void) { transition(BCL_CLOSE, "Close", "Close election"); }
void screen_publish_results(void) { transition(BCL_PUBLISH, "Publish", "Publish results"); }

/* ---- UC-5: results ---------------------------------------------------------- */

void screen_view_results(void) {
  char id[BB_ID_LEN];
  if (prompt_election_id("View results (UC-5)", id) != 0) return;

  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  req.op = BCL_ADMIN_RESULTS;
  snprintf(req.election_id, BB_ID_LEN, "%s", id);

  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  bb_result_t rc = bcl_send(g_ctx, &req, &resp);
  bb_result_t status = (rc != BB_OK) ? rc : resp.status;

  if (status != BB_OK) {
    const char *lines[] = {result_text(status)};
    tetrisui_message("Not published", lines, 1);
    return;
  }

  /* Counted tally only - no per-ballot hash listing. A voter who wants to
   * verify their own receipt hash still can, via Check a ballot (UC-6);
   * listing every hash here bought nothing an admin needed and only added
   * noise to the result. Same trim as ballotu's screen_view_results. */
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
    if (fill < 0) fill = 0;
    memset(bar, '#', (size_t)fill);
    bar[fill] = '\0';
    snprintf(buf[n], sizeof(buf[n]), "  %-10s %3d %s", resp.options[i], resp.tally[i], bar);
    lines[n] = buf[n];
    n++;
  }
  tetrisui_list_view("Election results", lines, n);
}

/* ---- UC-6: check a ballot (admin) -------------------------------------------- */

/* Admin-channel path to the same check ballotu.c's "Check your vote" does
 * (BCL_ADMIN_CHECK, same bb_lookup_hash underneath - see dispatch.c), for
 * an operator with no voter session to route plain CHECK through. Still
 * needs the exact receipt hash; this cannot enumerate or count anyone's
 * ballot without it. */
void screen_check_ballot(void) {
  char id[BB_ID_LEN];
  if (prompt_election_id("Check a ballot (UC-6)", id) != 0) return;

  char hash[BB_HASH_LEN] = "";
  if (tetrisui_input("Check a ballot (UC-6)", "Enter the receipt hash:", hash, sizeof(hash)) != 0)
    return;
  if (strlen(hash) == 0) return;

  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  req.op = BCL_ADMIN_CHECK;
  snprintf(req.election_id, BB_ID_LEN, "%s", id);
  snprintf(req.hash, BB_HASH_LEN, "%s", hash);

  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  bb_result_t rc = bcl_send(g_ctx, &req, &resp);
  bb_result_t status = (rc != BB_OK) ? rc : resp.status;

  char line1[96];
  snprintf(line1, sizeof(line1), "Receipt hash: %s", hash);

  if (status == BB_OK && resp.found) {
    char line2[96];
    snprintf(line2, sizeof(line2), "Found: counted for option index %d (%s).", resp.found_option,
            resp.found_option_name);
    const char *lines[] = {line1, line2};
    tetrisui_message("Verified", lines, 2);
    return;
  }
  if (status == BB_ERR_NOT_FOUND) {
    const char *lines[] = {line1, "Not found: no live (non-superseded) ballot with this hash."};
    tetrisui_message("Not found", lines, 2);
    return;
  }
  const char *lines[] = {line1, result_text(status)};
  tetrisui_message("Check unavailable", lines, 2);
}

/* ---- entry point ------------------------------------------------------------ */

static void usage(FILE *out, const char *argv0) {
  fprintf(out,
          "usage: %s [-C ctl_socket] [-h]\n"
          "  -C ctl_socket  ballotd's admin Unix socket path (default %s)\n"
          "  -h             show this help\n",
          argv0, DEFAULT_CTL_PATH);
}

int main(int argc, char **argv) {
  snprintf(g_ctl_path, sizeof(g_ctl_path), "%s", DEFAULT_CTL_PATH);

  int opt;
  while ((opt = getopt(argc, argv, "C:h")) != -1) {
    switch (opt) {
      case 'C':
        snprintf(g_ctl_path, sizeof(g_ctl_path), "%s", optarg);
        break;
      case 'h':
        usage(stdout, argv[0]);
        return 0;
      default:
        usage(stderr, argv[0]);
        return 2;
    }
  }

  g_ctx = bcl_create();
  if (g_ctx == NULL) {
    fprintf(stderr, "ballotctl: bcl_create failed\n");
    return 1;
  }

  tetrisui_init();
  tetrisui_set_status("ballotctl", "(not logged in)", "");

  if (screen_login()) {
    const char *items[] = {"Create election (UC-1)", "Open election", "Close election",
                           "Publish results", "View results (UC-5)", "Check a ballot (UC-6)",
                           "Quit"};
    for (;;) {
      int sel = tetrisui_menu("ballotctl - admin menu", items, 7, NULL);
      if (sel < 0 || sel == 6) break;
      switch (sel) {
        case 0:
          screen_create_election();
          break;
        case 1:
          screen_open_election();
          break;
        case 2:
          screen_close_election();
          break;
        case 3:
          screen_publish_results();
          break;
        case 4:
          screen_view_results();
          break;
        case 5:
          screen_check_ballot();
          break;
      }
    }
  }

  tetrisui_shutdown();
  bcl_destroy(g_ctx);
  return 0;
}
