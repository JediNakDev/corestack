/**
 * tests/unit/test_socket.c
 *
 * Unit tests for the reliable-IO helpers in source/common.c:
 *   unsigned char *read_bytes(int sockfd, uint64_t length)
 *   int            send_all  (int sockfd, const unsigned char *buf, uint64_t length)
 *
 * We use socketpair(AF_UNIX, SOCK_STREAM) so we can drive both ends of a
 * real socket without involving TCP / the loopback interface.
 *
 * Link against source/common.c and tests/unity/unity.c.
 */

#include "unity.h"
#include "common.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

/* SIGPIPE is raised when we write to a closed peer. We don't care; the
 * relevant write returns -1 with errno=EPIPE, which is what we test. */
static void ignore_sigpipe(void)
{
    signal(SIGPIPE, SIG_IGN);
}

static int pair_fds[2];

void setUp(void)
{
    ignore_sigpipe();
    /* socketpair returns 0 on success */
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, pair_fds));
}

void tearDown(void)
{
    /* Close whichever end is still open. close() of an already-closed fd
     * returns -1 with EBADF, which is harmless here. */
    close(pair_fds[0]);
    close(pair_fds[1]);
}

/* ---------- read_bytes ---------- */

static void test_read_bytes_exact_length(void)
{
    const unsigned char payload[] = "hello world";
    const size_t n = sizeof(payload) - 1; /* drop trailing NUL */

    ssize_t sent = send(pair_fds[1], payload, n, 0);
    TEST_ASSERT_EQUAL_INT((int)n, (int)sent);

    unsigned char *got = read_bytes(pair_fds[0], n);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_MEMORY(payload, got, n);
    free(got);
}

static void test_read_bytes_loops_across_chunks(void)
{
    /* Send 9 bytes as three separate sends. read_bytes must keep calling
     * recv() until it has all 9 bytes. */
    const unsigned char *c1 = (const unsigned char *)"foo";
    const unsigned char *c2 = (const unsigned char *)"bar";
    const unsigned char *c3 = (const unsigned char *)"baz";

    TEST_ASSERT_EQUAL_INT(3, (int)send(pair_fds[1], c1, 3, 0));
    TEST_ASSERT_EQUAL_INT(3, (int)send(pair_fds[1], c2, 3, 0));
    TEST_ASSERT_EQUAL_INT(3, (int)send(pair_fds[1], c3, 3, 0));

    unsigned char *got = read_bytes(pair_fds[0], 9);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_MEMORY("foobarbaz", got, 9);
    free(got);
}

static void test_read_bytes_returns_null_on_early_close(void)
{
    /* Peer sends 5 bytes then closes. Asking for 100 must return NULL. */
    const unsigned char *short_payload = (const unsigned char *)"short";
    TEST_ASSERT_EQUAL_INT(5, (int)send(pair_fds[1], short_payload, 5, 0));
    close(pair_fds[1]);

    unsigned char *got = read_bytes(pair_fds[0], 100);
    TEST_ASSERT_NULL(got);
}

/* ---------- send_all ---------- */

static void test_send_all_writes_all_bytes(void)
{
    const unsigned char payload[] = "abcdefghij";
    const uint64_t n = sizeof(payload) - 1;

    int rc = send_all(pair_fds[0], payload, n);
    TEST_ASSERT_EQUAL_INT(0, rc);

    unsigned char buf[16] = {0};
    ssize_t got = recv(pair_fds[1], buf, n, 0);
    TEST_ASSERT_EQUAL_INT((int)n, (int)got);
    TEST_ASSERT_EQUAL_MEMORY(payload, buf, n);
}

static void test_send_all_fails_after_peer_close(void)
{
    /* Close the receiver. Subsequent send_all on the other end should
     * eventually return -1 (after kernel detects the broken pipe). */
    close(pair_fds[1]);

    /* Use a big-enough payload to ensure the kernel actually attempts a
     * write past the socket buffer state change. */
    unsigned char big[8192];
    memset(big, 'X', sizeof(big));

    int rc = send_all(pair_fds[0], big, sizeof(big));
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/* ---------- send_int ---------- */

static void test_send_int_roundtrip(void)
{
    const uint64_t value = 0x0123456789ABCDEFULL;

    int rc = send_int(pair_fds[0], value);
    TEST_ASSERT_EQUAL_INT(0, rc);

    unsigned char buf[INT_BYTES];
    ssize_t got = recv(pair_fds[1], buf, INT_BYTES, 0);
    TEST_ASSERT_EQUAL_INT(INT_BYTES, (int)got);
    TEST_ASSERT_EQUAL_UINT64(value, bytes_to_int(buf));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_read_bytes_exact_length);
    RUN_TEST(test_read_bytes_loops_across_chunks);
    RUN_TEST(test_read_bytes_returns_null_on_early_close);
    RUN_TEST(test_send_all_writes_all_bytes);
    RUN_TEST(test_send_all_fails_after_peer_close);
    RUN_TEST(test_send_int_roundtrip);
    return UNITY_END();
}
