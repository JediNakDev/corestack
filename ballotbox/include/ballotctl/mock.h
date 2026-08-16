#ifndef BALLOTCTL_MOCK_H
#define BALLOTCTL_MOCK_H

#define MOCK_MAX_OPTIONS 8
#define MOCK_MAX_HASHES 40
#define MOCK_MAX_ELECTIONS 8
#define MOCK_HASH_LEN 65
#define MOCK_MAX_VOTERS 8

typedef enum {
  ELECTION_DRAFT,
  ELECTION_OPEN,
  ELECTION_CLOSED,
  ELECTION_PUBLISHED
} election_state_t;

typedef struct {
  char hash[MOCK_HASH_LEN];
  int option_index;
  int version;
  int superseded;
} ballot_hash_t;

typedef struct {
  char id[16];
  char title[64];
  election_state_t state;
  char options[MOCK_MAX_OPTIONS][32];
  int option_count;
  char eligible[MOCK_MAX_VOTERS][32];
  int eligible_count;
  char open_time[32];
  char close_time[32];
  int tally[MOCK_MAX_OPTIONS];
  ballot_hash_t hashes[MOCK_MAX_HASHES];
  int hash_count;
} election_t;

extern election_t g_elections[MOCK_MAX_ELECTIONS];
extern int g_election_count;

void mock_init(void);
int mock_check_admin_cert(const char *name);
void mock_generate_hash(char *out, int seed);
const char *mock_state_str(election_state_t s);

#endif
