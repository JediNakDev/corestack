#include "ballotu/screens.h"
#include "ballotu/mock.h"
#include "libtetrisui/tetrisui.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>

static int hash_counter = 1;

int screen_login(void) {
  for (;;) {
    char name[32] = "";
    if (tetrisui_input("ballotu - login", "Enter your cert name (Esc to quit):", name, sizeof(name)) != 0) {
      return 0;
    }
    if (strlen(name) == 0) continue;

    const char *steps[] = {"Opening secure session", "Presenting cert (tetrissh handshake)"};
    tetrisui_progress("Authenticating", steps, 2);

    cert_status_t status = mock_check_voter_cert(name);
    if (status == CERT_EXPIRED) {
      const char *lines[] = {"Cert expired or forged.", "Rejected by System."};
      tetrisui_message("Login failed", lines, 2);
      continue;
    }
    if (status == CERT_INVALID) {
      const char *lines[] = {"Unrecognized cert.", "Rejected by System."};
      tetrisui_message("Login failed", lines, 2);
      continue;
    }
    strncpy(g_session.cert_name, name, sizeof(g_session.cert_name) - 1);
    tetrisui_set_status("ballotu", name, "");
    return 1;
  }
}

static int pick_election(void) {
  const char *items[MOCK_MAX_ELECTIONS];
  char labels[MOCK_MAX_ELECTIONS][64];
  for (int i = 0; i < g_election_count; i++) {
    snprintf(labels[i], sizeof(labels[i]), "%s  %-24s [%s]", g_elections[i].id,
             g_elections[i].title, mock_state_str(g_elections[i].state));
    items[i] = labels[i];
  }
  return tetrisui_menu("Select election", items, g_election_count, NULL);
}

void screen_join_election(void) {
  char id[16] = "";
  if (tetrisui_input("Join election (UC-2)", "Enter election ID (e.g. E-100):", id, sizeof(id)) != 0) return;

  int idx = -1;
  for (int i = 0; i < g_election_count; i++) {
    if (strcasecmp(g_elections[i].id, id) == 0) { idx = i; break; }
  }
  if (idx < 0) {
    char line[64];
    snprintf(line, sizeof(line), "Election '%s' not found.", id);
    const char *lines[] = {line};
    tetrisui_message("Join failed", lines, 1);
    return;
  }
  election_t *e = &g_elections[idx];
  if (e->state != ELECTION_OPEN) {
    char line[96];
    snprintf(line, sizeof(line), "Cannot join %s: election is %s, not Open.", e->id, mock_state_str(e->state));
    const char *lines[] = {line, "Refused."};
    tetrisui_message("Join refused", lines, 2);
    return;
  }

  if (!mock_is_eligible(e, g_session.cert_name)) {
    const char *lines[] = {"Your cert is not on the eligible-voter list", "for this election. Refused."};
    tetrisui_message("Join refused", lines, 2);
    return;
  }

  const char *steps[] = {"Presenting voter cert", "Verifying eligibility", "Confirming election is Open"};
  tetrisui_progress("Joining election", steps, 3);

  g_session.joined_election = idx;

  if (g_session.has_ballot) {
    const char *lines[] = {"You already have a ballot for this election.", "Routing you to Update Vote (UC-4)."};
    tetrisui_message("Already voted", lines, 2);
    screen_update_vote();
    return;
  }

  char lines_buf[MOCK_MAX_OPTIONS + 2][80];
  const char *lines[MOCK_MAX_OPTIONS + 2];
  snprintf(lines_buf[0], sizeof(lines_buf[0]), "Joined %s: %s", e->id, e->title);
  lines[0] = lines_buf[0];
  strcpy(lines_buf[1], "Ballot options:");
  lines[1] = lines_buf[1];
  for (int i = 0; i < e->option_count; i++) {
    snprintf(lines_buf[i + 2], sizeof(lines_buf[i + 2]), "  %d) %s", i + 1, e->options[i]);
    lines[i + 2] = lines_buf[i + 2];
  }
  tetrisui_message("Join successful", lines, e->option_count + 2);
}

static void cast_common(int is_update) {
  if (g_session.joined_election < 0) {
    const char *lines[] = {"You must join an election first (UC-2)."};
    tetrisui_message("Not joined", lines, 1);
    return;
  }
  election_t *e = &g_elections[g_session.joined_election];
  if (e->state != ELECTION_OPEN) {
    const char *lines[] = {"Election closed mid-submit.", "Rejected by System."};
    tetrisui_message("Vote rejected", lines, 2);
    return;
  }

  if (!is_update && g_session.has_ballot) {
    const char *lines[] = {"You already have a final ballot.", "Routing you to Update Vote (UC-4)."};
    tetrisui_message("Already voted", lines, 2);
    screen_update_vote();
    return;
  }
  if (is_update && !g_session.has_ballot) {
    const char *lines[] = {"You have no prior ballot yet.", "Routing you to Cast Vote (UC-3)."};
    tetrisui_message("Nothing to update", lines, 2);
    screen_cast_vote();
    return;
  }

  const char *items[MOCK_MAX_OPTIONS];
  for (int i = 0; i < e->option_count; i++) items[i] = e->options[i];
  char title[64];
  if (is_update) {
    snprintf(title, sizeof(title), "Update vote (prior ballot v%d exists)", g_session.ballot_version);
  } else {
    snprintf(title, sizeof(title), "Cast vote: %s", e->title);
  }
  int sel = tetrisui_menu(title, items, e->option_count, "Enter to select your option");
  if (sel < 0) return;

  char q[96];
  snprintf(q, sizeof(q), "Confirm vote for '%s'?", e->options[sel]);
  if (!tetrisui_confirm("Confirm ballot", q)) return;

  const char *steps[] = {"Encrypting ballot (RSA-OAEP)", "Attaching anti-replay nonce",
                          "Submitting over session", "System verifying nonce/eligibility",
                          "Recording in authoritative store"};
  tetrisui_progress(is_update ? "Submitting updated ballot" : "Submitting ballot", steps, 5);

  if (is_update) {
    for (int i = 0; i < e->hash_count; i++) {
      if (!e->hashes[i].superseded && strcmp(e->hashes[i].hash, g_session.my_hash) == 0) {
        e->hashes[i].superseded = 1;
      }
    }
    g_session.ballot_version++;
  } else {
    g_session.ballot_version = 1;
    g_session.has_ballot = 1;
  }

  char new_hash[MOCK_HASH_LEN];
  mock_generate_hash(new_hash, hash_counter++);
  strncpy(g_session.my_hash, new_hash, sizeof(g_session.my_hash) - 1);

  if (e->hash_count < MOCK_MAX_HASHES) {
    ballot_hash_t *bh = &e->hashes[e->hash_count++];
    strcpy(bh->hash, new_hash);
    bh->option_index = sel;
    bh->version = g_session.ballot_version;
    bh->superseded = 0;
  }

  char line1[96];
  snprintf(line1, sizeof(line1), "%s", is_update ? "Vote updated. New receipt issued:" : "Vote recorded. Receipt issued:");
  const char *lines[] = {line1, new_hash, "Keep this hash to check your vote later (UC-6)."};
  tetrisui_message("Success", lines, 3);
}

void screen_cast_vote(void) { cast_common(0); }
void screen_update_vote(void) { cast_common(1); }

void screen_view_results(void) {
  int idx = pick_election();
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

void screen_check_vote(void) {
  char key[64] = "";
  if (tetrisui_input("Check your vote (UC-6)", "Enter your secret ballot key:", key, sizeof(key)) != 0) return;
  if (strlen(key) == 0) return;

  char hash[MOCK_HASH_LEN];
  mock_hash_key(key, hash);

  char line1[96];
  snprintf(line1, sizeof(line1), "Receipt hash: %s", hash);

  for (int e = 0; e < g_election_count; e++) {
    election_t *el = &g_elections[e];
    if (el->state != ELECTION_PUBLISHED) continue; /* UC-6 precondition */
    for (int i = 0; i < el->hash_count; i++) {
      if (el->hashes[i].superseded) continue;
      if (strcmp(el->hashes[i].hash, hash) == 0) {
        char line2[96], line3[96];
        snprintf(line2, sizeof(line2), "%s: %s", el->id, el->title);
        snprintf(line3, sizeof(line3), "Your vote is '%s' and is included in the tally.",
                 el->options[el->hashes[i].option_index]);
        const char *lines[] = {line1, line2, line3};
        tetrisui_message("Verified", lines, 3);
        return;
      }
    }
  }
  const char *lines[] = {line1, "Hash not found in the published tally.",
                          "Verification FAILED - dropped ballot.", "Please raise this with the Admin."};
  tetrisui_message("Verification failed", lines, 4);
}
