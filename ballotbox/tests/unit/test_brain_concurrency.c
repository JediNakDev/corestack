/*
 * U-22: concurrency-safe recording.
 *
 * N threads submit ballots for N distinct voters at the same time. All must be
 * accepted, and the store must see exactly N appended rows with N distinct
 * hashes - no lost write, no torn read-check-write. The unit under test is
 * bb_record_ballot's use of the instance write lock (R1); the store itself is
 * the substitute seam, which counts what it was asked to do.
 */

#include "fake_brain_seams.h"
#include "unity.h"

#include <pthread.h>
#include <string.h>

#define VOTERS 16

static bb_ctx *ctx;
static const char *eligible_storage[VOTERS];
static char eligible_names[VOTERS][BB_CERT_LEN];

typedef struct {
  int index;
  bb_result_t result;
  bb_receipt_t receipt;
} voter_task_t;

void setUp(void) {
  fake_reset();
  for (int i = 0; i < VOTERS; i++) {
    snprintf(eligible_names[i], BB_CERT_LEN, "voter-%02d", i);
    eligible_storage[i] = eligible_names[i];
  }
  fake_seed_election("E-100", BB_STATE_OPEN, 3, eligible_storage, VOTERS);
  ctx = bb_create();
  bb_set_log(ctx, NULL);
}

void tearDown(void) {
  bb_destroy(ctx);
}

static void *submit(void *arg) {
  voter_task_t *task = arg;
  bb_ballot_t b;
  memset(&b, 0, sizeof(b));
  snprintf(b.cert_name, BB_CERT_LEN, "voter-%02d", task->index);
  snprintf(b.nonce, BB_NONCE_LEN, "nonce-%02d", task->index);
  b.payload[0] = (uint8_t)(task->index % 3);
  b.payload_len = 1;
  task->result = bb_record_ballot(ctx, "E-100", &b, &task->receipt);
  return NULL;
}

void test_U22_concurrent_recording(void) {
  pthread_t threads[VOTERS];
  voter_task_t tasks[VOTERS];

  for (int i = 0; i < VOTERS; i++) {
    memset(&tasks[i], 0, sizeof(tasks[i]));
    tasks[i].index = i;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&threads[i], NULL, submit, &tasks[i]));
  }
  for (int i = 0; i < VOTERS; i++) {
    pthread_join(threads[i], NULL);
  }

  /* Every submission was accepted. */
  for (int i = 0; i < VOTERS; i++) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(BB_OK, tasks[i].result, eligible_names[i]);
  }

  /* Exactly one row and one consumed nonce per voter. */
  TEST_ASSERT_EQUAL_INT(VOTERS, fake_count(BB_DB_APPEND_BALLOT));
  TEST_ASSERT_EQUAL_INT(VOTERS, fake_count(BB_DB_NONCE_MARK));

  /* All receipts are distinct, so no two voters share a ballot row. */
  for (int i = 0; i < VOTERS; i++) {
    for (int j = i + 1; j < VOTERS; j++) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(tasks[i].receipt.hash, tasks[j].receipt.hash) != 0,
                               "two concurrent voters got the same receipt hash");
    }
  }

  /* Each appended row is one of the issued receipts, version 1, in range. */
  int matched = 0;
  for (int c = 0; c < fake.call_count; c++) {
    if (fake.calls[c].op != BB_DB_APPEND_BALLOT) {
      continue;
    }
    TEST_ASSERT_EQUAL_INT(1, fake.calls[c].row.version);
    TEST_ASSERT_TRUE(fake.calls[c].row.option_index >= 0 && fake.calls[c].row.option_index < 3);
    for (int i = 0; i < VOTERS; i++) {
      if (strcmp(fake.calls[c].row.hash, tasks[i].receipt.hash) == 0) {
        matched++;
        break;
      }
    }
  }
  TEST_ASSERT_EQUAL_INT(VOTERS, matched);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_U22_concurrent_recording);
  return UNITY_END();
}
