/* Socketpair tests for libtetrissh: handshake, traffic, failure paths.
 * Each test forks: child = server (session_accept), parent = client.
 * Run from repo root (auth/ paths are relative): make test */
#include <stdio.h>
#include "test_output.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include "libtetrissh/tetrissh.h"

#define CA_PATH "auth/cacsertificate.crt"
#define KEY_PATH "auth/private_key.pem"
#define CERT_PATH "auth/server_signed.crt"

static int tests_run = 0, tests_failed = 0;

#define CHECK(cond, msg)                                                       \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            test_output_failure_detail(msg, __FILE__, __LINE__);               \
            return -1;                                                         \
        }                                                                      \
    } while (0)

static EVP_PKEY *load_priv(void)
{
    FILE *fp = fopen(KEY_PATH, "rb");
    if (!fp)
        return NULL;
    EVP_PKEY *k = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
    fclose(fp);
    return k;
}

/* Child exit codes: 0 = expected behaviour, anything else = which step broke.
 */

/* Handshake + echo both directions. */
static int test_roundtrip(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    pid_t pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid == 0)
    { /* server */
        close(sv[0]);
        alarm(10);
        EVP_PKEY *priv = load_priv();
        if (!priv)
            _exit(10);
        session_t srv;
        if (session_accept(&srv, sv[1], priv, CERT_PATH) != SESSION_OK)
            _exit(11);
        uint8_t buf[64];
        uint32_t len = sizeof(buf);
        if (session_recv(&srv, buf, &len) != SESSION_OK)
            _exit(12);
        if (session_send(&srv, buf, len) != SESSION_OK)
            _exit(13); /* echo */
        session_close(&srv);
        EVP_PKEY_free(priv);
        _exit(0);
    }
    close(sv[1]);
    alarm(10);
    session_t cli;
    CHECK(session_connect(&cli, sv[0], CA_PATH) == SESSION_OK,
          "session_connect");
    const char *msg = "hello, tetriSH";
    CHECK(session_send(&cli, (const uint8_t *)msg, (uint32_t)strlen(msg)) ==
              SESSION_OK,
          "send");
    uint8_t buf[64];
    uint32_t len = sizeof(buf);
    CHECK(session_recv(&cli, buf, &len) == SESSION_OK, "recv echo");
    CHECK(len == strlen(msg) && memcmp(buf, msg, len) == 0, "echo payload");
    session_close(&cli);
    close(sv[0]);
    alarm(0);
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "server exit status");
    return 0;
}

/* Oversized sends rejected before any I/O (fd = -1 would error otherwise).
 * Plaintext of exactly SESSION_MAX_FRAME also rejected: the limit is on the
 * wire frame, and ciphertext overhead pushes it past 64 KiB (A2). */
static int test_send_toobig_no_io(void)
{
    session_t s;
    memset(&s, 0, sizeof(s));
    s.established = 1;
    s.fd = -1;
    uint8_t *big = malloc(SESSION_MAX_FRAME + 1);
    CHECK(big != NULL, "malloc");
    memset(big, 'x', SESSION_MAX_FRAME + 1);
    CHECK(session_send(&s, big, SESSION_MAX_FRAME + 1) == SESSION_ERR_TOOBIG,
          "over-limit plaintext");
    CHECK(session_send(&s, big, SESSION_MAX_FRAME) == SESSION_ERR_TOOBIG,
          "wire frame over limit");
    free(big);
    return 0;
}

/* Peer dies mid-handshake: server must return SESSION_ERR_IO, not crash (S1).
 */
static int test_peer_death_mid_handshake(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    pid_t pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid == 0)
    { /* server */
        close(sv[0]);
        alarm(10);
        EVP_PKEY *priv = load_priv();
        if (!priv)
            _exit(10);
        session_t srv;
        int rc = session_accept(&srv, sv[1], priv, CERT_PATH);
        _exit(rc == SESSION_ERR_IO ? 0 : 20);
    }
    close(sv[1]);
    /* valid nonce_len (32), then vanish before sending the nonce */
    unsigned char hdr[8] = {0, 0, 0, 0, 0, 0, 0, 32};
    CHECK(write(sv[0], hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr),
          "write header");
    close(sv[0]);
    int st;
    waitpid(pid, &st, 0);
    CHECK(!WIFSIGNALED(st),
          "server not killed by signal (segfault regression)");
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "server returned SESSION_ERR_IO");
    return 0;
}

/* Hostile nonce_len (1 GiB) must be rejected as PROTO before malloc (S2). */
static int test_malicious_nonce_len(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    pid_t pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid == 0)
    { /* server */
        close(sv[0]);
        alarm(10);
        EVP_PKEY *priv = load_priv();
        if (!priv)
            _exit(10);
        session_t srv;
        int rc = session_accept(&srv, sv[1], priv, CERT_PATH);
        _exit(rc == SESSION_ERR_PROTO ? 0 : 21);
    }
    close(sv[1]);
    unsigned char hdr[8] = {0, 0, 0, 0, 0x40, 0, 0, 0}; /* 2^30 */
    CHECK(write(sv[0], hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr),
          "write header");
    close(sv[0]);
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "server returned SESSION_ERR_PROTO");
    return 0;
}

/* Client verifying against the wrong CA must get SESSION_ERR_AUTH. */
static int test_forged_cert(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    pid_t pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid == 0)
    { /* server: normal, will hit IO error when client bails */
        close(sv[0]);
        alarm(10);
        EVP_PKEY *priv = load_priv();
        if (!priv)
            _exit(10);
        session_t srv;
        int rc = session_accept(&srv, sv[1], priv, CERT_PATH);
        _exit(rc == SESSION_ERR_IO ? 0 : 22);
    }
    close(sv[1]);
    alarm(10);
    session_t cli;
    /* server's own cert is not the CA that signed it */
    int rc = session_connect(&cli, sv[0], CERT_PATH);
    close(sv[0]);
    alarm(0);
    CHECK(rc == SESSION_ERR_AUTH, "connect rejects wrong CA");
    CHECK(cli.established == 0, "session not established");
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "server saw clean IO abort");
    return 0;
}

/* Oversized advertised frame length: TOOBIG, session marked dead (A3),
 * further recv refused as PROTO. */
static int test_oversized_frame(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    pid_t pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid == 0)
    { /* server */
        close(sv[0]);
        alarm(10);
        EVP_PKEY *priv = load_priv();
        if (!priv)
            _exit(10);
        session_t srv;
        if (session_accept(&srv, sv[1], priv, CERT_PATH) != SESSION_OK)
            _exit(11);
        uint8_t buf[64];
        uint32_t len = sizeof(buf);
        if (session_recv(&srv, buf, &len) != SESSION_ERR_TOOBIG)
            _exit(30);
        /* The RECEIVE side dies, not the whole session: we never consumed the
         * oversize body, so what we are fed is out of sync while what we write
         * is fine. That asymmetry is what lets the app answer 413. */
        if (srv.recv_dead == 0)
            _exit(31);
        if (srv.established == 0)
            _exit(33);
        /* The one send the app still owes the peer. */
        if (session_send(&srv, (const uint8_t *)"413", 3) != SESSION_OK)
            _exit(34);
        len = sizeof(buf);
        if (session_recv(&srv, buf, &len) != SESSION_ERR_PROTO)
            _exit(32);
        _exit(0);
    }
    close(sv[1]);
    alarm(10);
    session_t cli;
    CHECK(session_connect(&cli, sv[0], CA_PATH) == SESSION_OK,
          "session_connect");
    unsigned char hdr[4] = {0x00, 0x10, 0x00, 0x00}; /* 1 MiB advertised */
    CHECK(write(sv[0], hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr),
          "write raw frame header");
    /* Read the server's parting frame before closing, so its send has a peer
     * to write to - that send is the point of the test. */
    uint8_t parting[128];
    uint32_t plen = sizeof(parting);
    CHECK(session_recv(&cli, parting, &plen) == SESSION_OK,
          "parting frame arrives");
    close(sv[0]);
    alarm(0);
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "server TOOBIG then PROTO");
    return 0;
}

/* Undersized recv buffer: NOSPACE, frame discarded, session stays usable. */
static int test_nospace_recoverable(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    pid_t pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid == 0)
    { /* server */
        close(sv[0]);
        alarm(10);
        EVP_PKEY *priv = load_priv();
        if (!priv)
            _exit(10);
        session_t srv;
        if (session_accept(&srv, sv[1], priv, CERT_PATH) != SESSION_OK)
            _exit(11);
        uint8_t small[8];
        uint32_t len = sizeof(small);
        if (session_recv(&srv, small, &len) != SESSION_ERR_NOSPACE)
            _exit(40);
        uint8_t buf[64];
        len = sizeof(buf);
        if (session_recv(&srv, buf, &len) != SESSION_OK)
            _exit(41);
        if (len != 6 || memcmp(buf, "second", 6) != 0)
            _exit(42);
        if (session_send(&srv, (const uint8_t *)"ok", 2) != SESSION_OK)
            _exit(43);
        _exit(0);
    }
    close(sv[1]);
    alarm(10);
    session_t cli;
    CHECK(session_connect(&cli, sv[0], CA_PATH) == SESSION_OK,
          "session_connect");
    const char *big = "0123456789abcdef"; /* 16 > server's 8-byte buffer */
    CHECK(session_send(&cli, (const uint8_t *)big, 16) == SESSION_OK,
          "send big");
    CHECK(session_send(&cli, (const uint8_t *)"second", 6) == SESSION_OK,
          "send second");
    uint8_t buf[16];
    uint32_t len = sizeof(buf);
    CHECK(session_recv(&cli, buf, &len) == SESSION_OK, "recv ok-ack");
    CHECK(len == 2 && memcmp(buf, "ok", 2) == 0, "ack payload");
    close(sv[0]);
    alarm(0);
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "server NOSPACE then recovered");
    return 0;
}

/*
 * A byte-identical frame delivered twice decrypts and authenticates twice.
 *
 * See the "NO replay defence lives here" comment on session_t in
 * tetrissh.h: frames carry no counter, so a captured frame is bit-for-bit
 * valid on replay and this layer hands it to the caller as if it were new.
 * That is not an oversight to close here - it is documented as someone
 * else's job, one layer up. This case is the proof the documentation is
 * honest: it is the same capture-and-redeliver a real replay attack would
 * use, and it goes through clean.
 *
 * The two session_t are built by hand with a shared key: this exercises the
 * traffic layer, and a real handshake puts the ends in separate processes.
 */
static int test_replay_is_not_rejected_by_design(void)
{
    unsigned char key[32];
    CHECK(RAND_bytes(key, sizeof key) == 1, "key");

    int cap[2], feed[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, cap) == 0, "capture pair");
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, feed) == 0, "feed pair");

    session_t tx, rx;
    memset(&tx, 0, sizeof tx);
    memset(&rx, 0, sizeof rx);
    tx.established = rx.established = 1;
    memcpy(tx.key, key, sizeof key);
    memcpy(rx.key, key, sizeof key);
    tx.fd = cap[0];  /* tx writes; we read the raw frame off cap[1] */
    rx.fd = feed[0]; /* rx reads; we choose what lands on feed[1]   */

    CHECK(session_send(&tx, (const uint8_t *)"MOVE LEFT", 9) == SESSION_OK,
          "send");

    /* Capture the whole wire frame: 4-byte BE length, then that many bytes. */
    unsigned char hdr[4];
    CHECK(read(cap[1], hdr, 4) == 4, "capture length prefix");
    uint32_t n = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                 ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
    CHECK(n > 0 && n < 4096, "sane frame length");
    unsigned char body[4096];
    CHECK(read(cap[1], body, n) == (ssize_t)n, "capture body");

    uint8_t out[4096];
    uint32_t out_len = sizeof out;

    /* First delivery: ordinary traffic. */
    CHECK(write(feed[1], hdr, 4) == 4 && write(feed[1], body, n) == (ssize_t)n,
          "deliver once");
    CHECK(session_recv(&rx, out, &out_len) == SESSION_OK, "first delivery ok");
    CHECK(out_len == 9 && memcmp(out, "MOVE LEFT", 9) == 0, "payload intact");
    CHECK(rx.recv_dead == 0, "session still alive");

    /* Second delivery is an authenticated, byte-identical replay. Nothing at
     * this layer distinguishes it from the original: it decrypts, its HMAC
     * verifies, and it comes back exactly as the first delivery did. */
    out_len = sizeof out;
    CHECK(write(feed[1], hdr, 4) == 4 && write(feed[1], body, n) == (ssize_t)n,
          "deliver again");
    CHECK(session_recv(&rx, out, &out_len) == SESSION_OK,
          "replayed traffic decrypts exactly like the original");
    CHECK(out_len == 9 && memcmp(out, "MOVE LEFT", 9) == 0,
          "replayed payload intact");
    CHECK(rx.recv_dead == 0, "no replay poisons the stream at this layer");

    close(cap[0]);
    close(cap[1]);
    close(feed[0]);
    close(feed[1]);
    return 0;
}

/* An attacker who changes a captured ciphertext or HMAC must not obtain
 * plaintext. The complete frame is queued before recv, so this test has no
 * dependency on scheduling or a peer that can stall it. */
static int test_tampered_frame_is_rejected(void)
{
    unsigned char key[32];
    int cap[2], feed[2];
    session_t tx, rx;
    unsigned char hdr[4], body[4096];
    uint8_t out[4096];
    uint32_t out_len = sizeof out;

    CHECK(RAND_bytes(key, sizeof key) == 1, "key");
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, cap) == 0, "capture pair");
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, feed) == 0, "feed pair");
    memset(&tx, 0, sizeof tx);
    memset(&rx, 0, sizeof rx);
    tx.established = rx.established = 1;
    memcpy(tx.key, key, sizeof key);
    memcpy(rx.key, key, sizeof key);
    tx.fd = cap[0];
    rx.fd = feed[0];

    CHECK(session_send(&tx, (const uint8_t *)"MOVE RIGHT", 10) == SESSION_OK,
          "send");
    CHECK(read(cap[1], hdr, sizeof hdr) == (ssize_t)sizeof hdr,
          "capture length prefix");
    uint32_t n = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                 ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
    CHECK(n > 0 && n < sizeof body, "sane frame length");
    CHECK(read(cap[1], body, n) == (ssize_t)n, "capture body");
    body[n - 1] ^= 0x01; /* HMAC byte: malformed but length remains valid. */

    CHECK(write(feed[1], hdr, sizeof hdr) == (ssize_t)sizeof hdr &&
              write(feed[1], body, n) == (ssize_t)n,
          "deliver tampered frame");
    CHECK(session_recv(&rx, out, &out_len) == SESSION_ERR_CRYPTO,
          "tampered frame is rejected before plaintext reaches the caller");

    close(cap[0]);
    close(cap[1]);
    close(feed[0]);
    close(feed[1]);
    return 0;
}

/* A replay fix must still accept a long run of distinct, in-order frames. */
static int test_sequence_advances(void)
{
    unsigned char key[32];
    CHECK(RAND_bytes(key, sizeof key) == 1, "key");
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");

    session_t a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.established = b.established = 1;
    memcpy(a.key, key, sizeof key);
    memcpy(b.key, key, sizeof key);
    a.fd = sv[0];
    b.fd = sv[1];

    for (int i = 0; i < 50; i++)
    {
        uint8_t msg[16];
        snprintf((char *)msg, sizeof msg, "frame-%d", i);
        CHECK(session_send(&a, msg, (uint32_t)strlen((char *)msg)) ==
                  SESSION_OK,
              "send");
        uint8_t got[64];
        uint32_t len = sizeof got;
        CHECK(session_recv(&b, got, &len) == SESSION_OK, "recv");
        CHECK(len == strlen((char *)msg) && memcmp(got, msg, len) == 0,
              "payload");
    }
    close(sv[0]);
    close(sv[1]);
    return 0;
}

#define RUN(fn)                                                                \
    do                                                                         \
    {                                                                          \
        tests_run++;                                                           \
        if (fn() == 0)                                                         \
            test_output_pass(#fn);                                             \
        else                                                                   \
        {                                                                      \
            test_output_fail(#fn);                                             \
            tests_failed++;                                                    \
        }                                                                      \
    } while (0)

int main(void)
{
    test_output_begin("test_session");
    if (access(KEY_PATH, R_OK) != 0 || access(CERT_PATH, R_OK) != 0 ||
        access(CA_PATH, R_OK) != 0)
    {
        test_output_failure_detailf(
            __FILE__, __LINE__,
            "missing key material: need %s, %s, %s (run from repo root)",
            KEY_PATH, CERT_PATH, CA_PATH);
        test_output_fail("session key material is available");
        test_output_summary(1, 1, 0);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN); /* broken-pipe paths must return -1, not die */

    RUN(test_roundtrip);
    RUN(test_send_toobig_no_io);
    RUN(test_peer_death_mid_handshake);
    RUN(test_malicious_nonce_len);
    RUN(test_forged_cert);
    RUN(test_oversized_frame);
    RUN(test_nospace_recoverable);
    RUN(test_replay_is_not_rejected_by_design);
    RUN(test_tampered_frame_is_rejected);
    RUN(test_sequence_advances);

    test_output_summary(tests_run, tests_failed, 0);
    return tests_failed ? 1 : 0;
}
