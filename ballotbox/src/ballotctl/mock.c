#include "ballotctl/mock.h"

#include <string.h>

election_t g_elections[MOCK_MAX_ELECTIONS];
int g_election_count = 0;

const char *mock_state_str(election_state_t s) {
  switch (s) {
    case ELECTION_DRAFT: return "Draft";
    case ELECTION_OPEN: return "Open";
    case ELECTION_CLOSED: return "Closed";
    case ELECTION_PUBLISHED: return "Published";
  }
  return "?";
}

void mock_generate_hash(char *out, int seed) {
  static const char hex[] = "0123456789abcdef";
  unsigned int state = (unsigned int)seed * 2654435761u + 1;
  for (int i = 0; i < MOCK_HASH_LEN - 1; i++) {
    state = state * 1103515245u + 12345u;
    out[i] = hex[(state >> 16) & 0xF];
  }
  out[MOCK_HASH_LEN - 1] = '\0';
}

void mock_init(void) {
  g_election_count = 0;

  election_t *e = &g_elections[g_election_count++];
  strcpy(e->id, "E-042");
  strcpy(e->title, "Budget Ratification");
  e->state = ELECTION_PUBLISHED;
  e->option_count = 2;
  strcpy(e->options[0], "Yes");
  strcpy(e->options[1], "No");
  strcpy(e->open_time, "2026-06-01 09:00");
  strcpy(e->close_time, "2026-06-08 09:00");
  e->eligible_count = 2;
  strcpy(e->eligible[0], "alice");
  strcpy(e->eligible[1], "bob");
  e->tally[0] = 14;
  e->tally[1] = 6;
  e->hash_count = 0;
  /* v1 was superseded by the voter's v2 re-cast; only v2 counts */
  ballot_hash_t *old = &e->hashes[e->hash_count++];
  mock_generate_hash(old->hash, 1001);
  old->option_index = 0;
  old->version = 1;
  old->superseded = 1;
  ballot_hash_t *cur = &e->hashes[e->hash_count++];
  mock_generate_hash(cur->hash, 1002);
  cur->option_index = 1;
  cur->version = 2;
  cur->superseded = 0;
  /* fill the remaining counted ballots so the hash list matches the tally */
  for (int o = 0; o < e->option_count; o++) {
    int counted = o == 1 ? 1 : 0; /* the v2 ballot above already counts as 'No' */
    while (counted < e->tally[o] && e->hash_count < MOCK_MAX_HASHES) {
      ballot_hash_t *bh = &e->hashes[e->hash_count++];
      mock_generate_hash(bh->hash, 3000 + o * 100 + counted);
      bh->option_index = o;
      bh->version = 1;
      bh->superseded = 0;
      counted++;
    }
  }
}

int mock_check_admin_cert(const char *name) {
  return strcmp(name, "admin") == 0;
}
