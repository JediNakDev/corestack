#include "ballotu/mock.h"

#include <string.h>
#include <stdio.h>

election_t g_elections[MOCK_MAX_ELECTIONS];
int g_election_count = 0;
voter_session_t g_session;

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

  election_t *e1 = &g_elections[g_election_count++];
  strcpy(e1->id, "E-100");
  strcpy(e1->title, "Board Motion 2026");
  e1->state = ELECTION_OPEN;
  e1->option_count = 3;
  strcpy(e1->options[0], "Approve");
  strcpy(e1->options[1], "Reject");
  strcpy(e1->options[2], "Abstain");
  memset(e1->tally, 0, sizeof(e1->tally));
  e1->hash_count = 0;

  election_t *e2 = &g_elections[g_election_count++];
  strcpy(e2->id, "E-042");
  strcpy(e2->title, "Budget Ratification");
  e2->state = ELECTION_PUBLISHED;
  e2->option_count = 2;
  strcpy(e2->options[0], "Yes");
  strcpy(e2->options[1], "No");
  e2->tally[0] = 14;
  e2->tally[1] = 6;
  e2->hash_count = 0;
  /* v1 was superseded by the voter's v2 re-cast; only v2 counts */
  ballot_hash_t *old = &e2->hashes[e2->hash_count++];
  mock_generate_hash(old->hash, 1001);
  old->option_index = 0;
  old->version = 1;
  old->superseded = 1;
  ballot_hash_t *cur = &e2->hashes[e2->hash_count++];
  mock_generate_hash(cur->hash, 1002);
  cur->option_index = 1;
  cur->version = 2;
  cur->superseded = 0;
  /* fill the remaining counted ballots so the hash list matches the tally */
  for (int o = 0; o < e2->option_count; o++) {
    int counted = o == 1 ? 1 : 0; /* the v2 ballot above already counts as 'No' */
    while (counted < e2->tally[o] && e2->hash_count < MOCK_MAX_HASHES) {
      ballot_hash_t *bh = &e2->hashes[e2->hash_count++];
      mock_generate_hash(bh->hash, 3000 + o * 100 + counted);
      bh->option_index = o;
      bh->version = 1;
      bh->superseded = 0;
      counted++;
    }
  }

  memset(&g_session, 0, sizeof(g_session));
  g_session.joined_election = -1;
}

void mock_hash_key(const char *key, char *out) {
  /* demo key that maps to the counted 'No' ballot in E-042 */
  if (strcmp(key, "alice23489ut49499") == 0) {
    mock_generate_hash(out, 1002);
    return;
  }
  unsigned int h = 5381;
  for (const char *p = key; *p; p++) h = h * 33u + (unsigned char)*p;
  mock_generate_hash(out, (int)(h | 0x40000000u)); /* keep clear of seeded ranges */
}

cert_status_t mock_check_voter_cert(const char *name) {
  if (strcmp(name, "expired") == 0) return CERT_EXPIRED;
  if (strcmp(name, "mallory") == 0) return CERT_NOT_ELIGIBLE; /* valid cert, just not eligible */
  if (strcmp(name, "alice") == 0 || strcmp(name, "bob") == 0) return CERT_VALID;
  return CERT_INVALID;
}

int mock_is_eligible(const election_t *e, const char *cert_name) {
  (void)e;
  return strcmp(cert_name, "mallory") != 0;
}
