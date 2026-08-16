/*
 * session.c - one ballotu (voter) connection.
 *
 * Its own binary: ballotd's listener_thread forks and execs it per client,
 * passing the accepted socket's fd, this session's end of the admin
 * socketpair, and the server's cert/key paths as argv. Runs until the
 * client disconnects, then exits.
 *
 * Layering (bottom to top):
 *   libtetrissh        - encrypted, authenticated frames
 * (session_accept/recv/send) libhtttp            - frame bytes <->
 * request/response struct libballotclient/codec - HTTTP <->
 * bcl_request_t/bcl_response_t this file            - forward to admin_thread
 * over the socketpair, nothing else
 *
 * Unlike tetriSH's session.c, this file owns no domain state at all: every
 * BallotBox operation needs the daemon's one bb_ctx, so there is nothing
 * left for a per-connection process to compute locally. It is a pure
 * protocol adapter, and it is also where the voter channel's op-class gate
 * lives - JOIN/CAST/UPDATE/RESULTS/CHECK only; CREATE/OPEN/CLOSE/PUBLISH
 * never reach the admin socketpair from here.
 */

#include "ballotd/adminmsg.h"
#include "libballotclient/codec.h"
#include "libhtttp/htttp.h"
#include "libtetrisauth/auth.h"
#include "libtetrissh/tetrissh.h"
#include "libtetrisutil/limits.h"

#include <openssl/pem.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Load the server's RSA private key with plain OpenSSL, not libtetrissh's
 * load_private_key(): that helper lives in core/src/libtetrissh/common.h, which
 * is deliberately not in include/ so the OpenSSL helper surface stays
 * private to that library. session_accept() takes an EVP_PKEY* precisely so
 * the application owns key loading (same rationale as tetrisd/session.c's
 * own load_server_key).
 */
static EVP_PKEY *load_server_key(const char *path) {
  FILE *fp = fopen(path, "r");
  if (fp == NULL)
    return NULL;
  EVP_PKEY *key = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
  fclose(fp);
  return key;
}

/* A bare, bodyless HTTP response for protocol-level rejections (malformed
 * bytes, or an op outside this channel's allowed set) - deliberately not
 * routed through the bb_result_t vocabulary: these are never a ballot rule,
 * only "was this request even well-formed enough, and allowed here". */
static void session_reply_raw(session_t *sh, int status) {
  htttp_response_t res;
  memset(&res, 0, sizeof res);
  res.status = status;

  uint8_t out[512];
  uint32_t len = sizeof out;
  if (htttp_serialize_response(&res, out, &len) == HTTTP_OK)
    session_send(sh, out, len);
}

static bool is_voter_op(bcl_op_t op) {
  return op == BCL_JOIN || op == BCL_CAST || op == BCL_UPDATE ||
         op == BCL_RESULTS || op == BCL_CHECK;
}

/* One client request: decode, forward to admin_thread, wait for the reply,
 * encode, send. Strictly synchronous - one request in flight per
 * connection, so no poll() multiplexing is needed here.
 *
 * cert_name is this connection's auth_login()-verified identity (session_main
 * gets it once via auth_name(), after the pre-auth exchange resolves and
 * before this function is ever called). It overwrites whatever the request
 * itself carries, rather than merely filling a blank field: bcl_decode_request
 * still populates req->cert_name from the wire's Cert-Name header, but that
 * header is client-asserted and unverified, and a JOIN/CAST/etc. deciding
 * eligibility or ownership from an unverified name would make the
 * authentication step above completely decorative. This is what actually
 * closes that gap, not the header. */
static void handle_request(session_t *sh, int admin_fd, const char *cert_name,
                           const uint8_t *buf, uint32_t len) {
  htttp_request_t http;
  if (htttp_parse_request(buf, len, &http) != HTTTP_OK) {
    session_reply_raw(sh, 400);
    return;
  }

  BallotdReq creq;
  memset(&creq, 0, sizeof creq);
  if (bcl_decode_request(&http, &creq.req) != 0) {
    session_reply_raw(sh, 400);
    return;
  }

  /* Only the voter ops belong on this channel; CREATE/OPEN/CLOSE/PUBLISH
   * must come in over ballotd's local admin socket instead. This is the
   * actual enforcement of "a voter can only join/vote, never manage
   * elections" - the channel decides what it is allowed to ask. */
  if (!is_voter_op(creq.req.op)) {
    session_reply_raw(sh, 400);
    return;
  }

  snprintf(creq.req.cert_name, sizeof creq.req.cert_name, "%s", cert_name);
  snprintf(creq.req.ballot.cert_name, sizeof creq.req.ballot.cert_name, "%s",
           cert_name);

  BallotdResp cresp;
  if (ballotmsg_write_req(admin_fd, &creq) != 0)
    return; /* admin gone: no reply, disconnect */
  if (ballotmsg_read_resp(admin_fd, &cresp) != 1)
    return; /* admin gone mid-request */

  uint8_t out[SESSION_MAX_FRAME];
  uint32_t out_len = sizeof out;
  if (bcl_encode_response(creq.req.op, &cresp.resp, out, &out_len) == 0)
    session_send(sh, out, out_len);
}

static void session_main(int client_fd, int admin_fd, const char *cert_path,
                         const char *key_path) {
  EVP_PKEY *priv = load_server_key(key_path);
  if (priv == NULL) {
    fprintf(stderr, "ballot_session: cannot load private key %s\n", key_path);
    return;
  }

  session_t sh;
  int hs = session_accept(&sh, client_fd, priv, cert_path);
  /* The key is only needed to sign the handshake nonce; free it immediately
   * so a long-lived connection is not sitting on private key material it
   * can no longer use. */
  EVP_PKEY_free(priv);

  if (hs != SESSION_OK)
    return;

  /* The pre-auth exchange, owned end to end by libtetrisauth (unmodified -
   * same library tetriSH's own session.c calls). Blocks until the client is
   * a real account; there is no guest path here; every BallotBox voter op
   * needs a cert_name it can check eligibility against, so "accepted as
   * anonymous" is not a state this daemon has any use for. Nothing below
   * this line may run until it returns AUTH_OK - that return is the proof
   * of authentication, not a flag anywhere on this connection. */
  if (auth_retry_handler(&sh) != AUTH_OK) {
    session_close(&sh);
    return;
  }

  char cert_name[MAX_USER_NAME];
  auth_get_name(cert_name, sizeof cert_name);

  uint8_t buf[SESSION_MAX_FRAME];
  while (true) {
    uint32_t len = sizeof buf;
    int rc = session_recv(&sh, buf, &len);

    if (rc == SESSION_OK) {
      handle_request(&sh, admin_fd, cert_name, buf, len);
    } else if (rc == SESSION_ERR_IO || rc == SESSION_ERR_TOOBIG) {
      break; /* client closed, or the stream desynced */
    }
    /* other recoverable errors (CRYPTO/PROTO/NOSPACE): skip this frame,
     * keep going - the session stays usable. */
  }

  session_close(&sh);
}

/*
 * Entry point of the ballot_session binary. ballotd's listener_thread execs
 * it as:
 *     ballot_session <client_fd> <admin_fd> <cert_path> <key_path>
 */
int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: %s <client_fd> <admin_fd> <cert_path> <key_path>\n",
            argv[0]);
    return 1;
  }

  /* A dead client socket must not kill us with SIGPIPE on session_send. */
  signal(SIGPIPE, SIG_IGN);

  int client_fd = atoi(argv[1]);
  int admin_fd = atoi(argv[2]);
  session_main(client_fd, admin_fd, argv[3], argv[4]);

  close(client_fd);
  close(admin_fd);
  return 0;
}
