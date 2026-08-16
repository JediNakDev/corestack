#include "ballotctl/screens.h"
#include "ballotctl/mock.h"
#include "libtetrisui/tetrisui.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int screen_login(void) {
  for (;;) {
    char name[32] = "";
    if (tetrisui_input("ballotctl - admin login", "Enter admin cert name (Esc to quit):", name, sizeof(name)) != 0) {
      return 0;
    }
    if (strlen(name) == 0) continue;

    const char *steps[] = {"Opening secure session", "Presenting admin cert"};
    tetrisui_progress("Authenticating", steps, 2);

    if (!mock_check_admin_cert(name)) {
      const char *lines[] = {"Invalid admin cert.", "Rejected and logged."};
      tetrisui_message("Login failed", lines, 2);
      continue;
    }
    tetrisui_set_status("ballotctl", name, "");
    return 1;
  }
}

/* returns index of election chosen by admin, or -1 if cancelled */
static int pick_election(const char *title) {
  const char *items[MOCK_MAX_ELECTIONS];
  char labels[MOCK_MAX_ELECTIONS][64];
  for (int i = 0; i < g_election_count; i++) {
    snprintf(labels[i], sizeof(labels[i]), "%s  %-24s [%s]", g_elections[i].id,
              g_elections[i].title, mock_state_str(g_elections[i].state));
    items[i] = labels[i];
  }
  return tetrisui_menu(title, items, g_election_count, NULL);
}

static void split_csv(const char *src, char out[][32], int max, int *count) {
  *count = 0;
  char buf[256];
  strncpy(buf, src, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  char *tok = strtok(buf, ",");
  while (tok && *count < max) {
    while (*tok == ' ') tok++;
    strncpy(out[*count], tok, 31);
    out[*count][31] = '\0';
    (*count)++;
    tok = strtok(NULL, ",");
  }
}

void screen_create_election(void) {
  char values[5][TETRISUI_FIELD_LEN] = {"", "", "", "2026-01-01 09:00", "2026-01-08 09:00"};
  const char *labels[5] = {"Title", "Options (comma-separated)", "Eligible voter certs (comma-separated)",
                            "Open time", "Close time"};

  for (;;) {
    if (tetrisui_form("Create election (UC-1)", labels, values, 5) != 0) return; /* cancelled */

    char opts[MOCK_MAX_OPTIONS][32];
    int opt_count;
    split_csv(values[1], opts, MOCK_MAX_OPTIONS, &opt_count);

    if (strlen(values[0]) == 0 || opt_count == 0) {
      const char *lines[] = {"Config invalid: title and options are required.",
                              "Staying in Draft - fix and retry."};
      tetrisui_message("Rejected (alt flow 4a)", lines, 2);
      continue;
    }
    if (strcmp(values[3], values[4]) >= 0) {
      const char *lines[] = {"Config invalid: close time must be after open time.",
                              "Staying in Draft - fix and retry."};
      tetrisui_message("Rejected (alt flow 4a)", lines, 2);
      continue;
    }
    if (g_election_count >= MOCK_MAX_ELECTIONS) {
      const char *lines[] = {"Demo limit reached: cannot create more elections."};
      tetrisui_message("Rejected", lines, 1);
      return;
    }

    char voters[MOCK_MAX_VOTERS][32];
    int voter_count;
    split_csv(values[2], voters, MOCK_MAX_VOTERS, &voter_count);

    election_t *e = &g_elections[g_election_count];
    snprintf(e->id, sizeof(e->id), "E-%03d", 100 + g_election_count);
    strncpy(e->title, values[0], sizeof(e->title) - 1);
    e->state = ELECTION_DRAFT;
    e->option_count = opt_count;
    for (int i = 0; i < opt_count; i++) strcpy(e->options[i], opts[i]);
    e->eligible_count = voter_count;
    for (int i = 0; i < voter_count; i++) strcpy(e->eligible[i], voters[i]);
    strncpy(e->open_time, values[3], sizeof(e->open_time) - 1);
    strncpy(e->close_time, values[4], sizeof(e->close_time) - 1);
    memset(e->tally, 0, sizeof(e->tally));
    e->hash_count = 0;
    g_election_count++;

    const char *steps[] = {"Verifying admin cert", "Validating configuration",
                            "Initialising authoritative store (Draft)"};
    tetrisui_progress("Creating election", steps, 3);

    char line1[64];
    snprintf(line1, sizeof(line1), "Election %s created in Draft.", e->id);
    const char *lines[] = {line1, "Use Open Election to make it live."};
    tetrisui_message("Created", lines, 2);
    return;
  }
}

static void transition(election_state_t from, election_state_t to, const char *verb, const char *steps[], int n) {
  int idx = pick_election(verb);
  if (idx < 0) return;
  election_t *e = &g_elections[idx];
  if (e->state != from) {
    char line[96];
    snprintf(line, sizeof(line), "Cannot %s: election is currently %s.", verb, mock_state_str(e->state));
    const char *lines[] = {line};
    tetrisui_message("Invalid transition", lines, 1);
    return;
  }
  char q[96];
  snprintf(q, sizeof(q), "%s election %s?", verb, e->id);
  if (!tetrisui_confirm("Confirm", q)) return;

  tetrisui_progress(verb, steps, n);
  e->state = to;

  char line1[64];
  snprintf(line1, sizeof(line1), "Election %s is now %s.", e->id, mock_state_str(to));
  const char *lines[] = {line1};
  tetrisui_message("Done", lines, 1);
}

void screen_open_election(void) {
  const char *steps[] = {"Transitioning Draft -> Open", "System begins accepting voters"};
  transition(ELECTION_DRAFT, ELECTION_OPEN, "Open", steps, 2);
}

void screen_close_election(void) {
  const char *steps[] = {"Transitioning Open -> Closed", "System stops accepting new ballots"};
  transition(ELECTION_OPEN, ELECTION_CLOSED, "Close", steps, 2);
}

void screen_publish_results(void) {
  int idx = pick_election("Publish");
  if (idx < 0) return;
  election_t *e = &g_elections[idx];
  if (e->state != ELECTION_CLOSED) {
    char line[96];
    snprintf(line, sizeof(line), "Cannot publish: election is currently %s.", mock_state_str(e->state));
    const char *lines[] = {line};
    tetrisui_message("Invalid transition", lines, 1);
    return;
  }
  char q[64];
  snprintf(q, sizeof(q), "Publish results for %s?", e->id);
  if (!tetrisui_confirm("Confirm", q)) return;

  const char *steps[] = {"Aggregating tally", "Compiling ballot hash list", "Publishing result view"};
  tetrisui_progress("Publish", steps, 3);

  /* mock tally with one hash per counted vote */
  if (e->hash_count == 0) {
    for (int o = 0; o < e->option_count; o++) {
      e->tally[o] = (o + 1) * 3;
      for (int i = 0; i < e->tally[o] && e->hash_count < MOCK_MAX_HASHES; i++) {
        ballot_hash_t *bh = &e->hashes[e->hash_count++];
        mock_generate_hash(bh->hash, 2000 + o * 100 + i);
        bh->option_index = o;
        bh->version = 1;
        bh->superseded = 0;
      }
    }
  }
  e->state = ELECTION_PUBLISHED;

  char line1[64];
  snprintf(line1, sizeof(line1), "Election %s published.", e->id);
  const char *lines[] = {line1};
  tetrisui_message("Done", lines, 1);
}

void screen_view_results(void) {
  int idx = pick_election("View results (UC-5)");
  if (idx < 0) return;
  election_t *e = &g_elections[idx];
  if (e->state != ELECTION_PUBLISHED) {
    const char *lines[] = {"Results not available."};
    tetrisui_message("Not published", lines, 1);
    return;
  }

  enum { COL_W = 66, MAX_LINES = MOCK_MAX_OPTIONS + MOCK_MAX_HASHES + 6 };
  static char buf[MAX_LINES][MOCK_MAX_OPTIONS * COL_W + 1];
  const char *lines[MAX_LINES];
  int n = 0;
  snprintf(buf[n], sizeof(buf[n]), "%s: %s", e->id, e->title);
  lines[n] = buf[n]; n++;
  strcpy(buf[n], "Tally:"); lines[n] = buf[n]; n++;
  for (int i = 0; i < e->option_count; i++) {
    char bar[32];
    int fill = e->tally[i] > 20 ? 20 : e->tally[i];
    memset(bar, '#', (size_t)fill);
    bar[fill] = '\0';
    snprintf(buf[n], sizeof(buf[n]), "  %-10s %3d %s", e->options[i], e->tally[i], bar);
    lines[n] = buf[n]; n++;
  }
  strcpy(buf[n], "Ballot hashes (counted votes, one column per option):");
  lines[n] = buf[n]; n++;

  /* group counted (non-superseded) hashes by option, one column each */
  int per[MOCK_MAX_OPTIONS][MOCK_MAX_HASHES];
  int cnt[MOCK_MAX_OPTIONS] = {0};
  int max_cnt = 0;
  for (int i = 0; i < e->hash_count; i++) {
    if (e->hashes[i].superseded) continue;
    int o = e->hashes[i].option_index;
    per[o][cnt[o]++] = i;
    if (cnt[o] > max_cnt) max_cnt = cnt[o];
  }
  int pos = 0;
  for (int o = 0; o < e->option_count; o++) {
    pos += snprintf(buf[n] + pos, sizeof(buf[n]) - (size_t)pos, "%-*s", COL_W, e->options[o]);
  }
  lines[n] = buf[n]; n++;
  for (int r = 0; r < max_cnt; r++) {
    pos = 0;
    for (int o = 0; o < e->option_count; o++) {
      pos += snprintf(buf[n] + pos, sizeof(buf[n]) - (size_t)pos, "%-*s", COL_W,
                      r < cnt[o] ? e->hashes[per[o][r]].hash : "");
    }
    lines[n] = buf[n]; n++;
  }
  tetrisui_list_view("Election results", lines, n);
}

void screen_election_status(void) {
  char buf[MOCK_MAX_ELECTIONS][100];
  const char *lines[MOCK_MAX_ELECTIONS];
  for (int i = 0; i < g_election_count; i++) {
    election_t *e = &g_elections[i];
    snprintf(buf[i], sizeof(buf[i]), "%s  %-24s [%s]  open:%s close:%s", e->id, e->title,
              mock_state_str(e->state), e->open_time, e->close_time);
    lines[i] = buf[i];
  }
  tetrisui_list_view("Election status", lines, g_election_count);
}
