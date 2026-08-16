#ifndef BALLOTU_MOCK_H
#define BALLOTU_MOCK_H

#define MOCK_MAX_OPTIONS 8
#define MOCK_MAX_HASHES 40
#define MOCK_MAX_ELECTIONS 4
#define MOCK_HASH_LEN 65

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
  int tally[MOCK_MAX_OPTIONS];
  ballot_hash_t hashes[MOCK_MAX_HASHES];
  int hash_count;
} election_t;

typedef enum { CERT_INVALID, CERT_EXPIRED, CERT_NOT_ELIGIBLE, CERT_VALID } cert_status_t;

/* voter's session state */
typedef struct {
  char cert_name[32];
  int joined_election; /* index into g_elections, -1 if none */
  int has_ballot;
  int ballot_version;
  char my_hash[MOCK_HASH_LEN];
} voter_session_t;

extern election_t g_elections[MOCK_MAX_ELECTIONS];
extern int g_election_count;
extern voter_session_t g_session;

void mock_init(void);
cert_status_t mock_check_voter_cert(const char *name);
int mock_is_eligible(const election_t *e, const char *cert_name);
void mock_generate_hash(char *out, int seed);
/* derive a receipt hash from the voter's secret ballot key (mock KDF) */
void mock_hash_key(const char *key, char *out);
const char *mock_state_str(election_state_t s);

#endif
