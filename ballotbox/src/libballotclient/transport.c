#include "libballotclient/client.h"
#include "internal.h"

#include "libballotclient/codec.h"
#include "libballotclient/ctl_frame.h"
#include "libhtttp/htttp.h"
#include "libtetrissh/tetrissh.h"

#include <openssl/crypto.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/*
 * Transport seam - real implementation. Two shapes, chosen by req->op:
 *
 *   Voter ops (JOIN/CAST/UPDATE/RESULTS/CHECK): a persistent TCP + tetrissh
 *   session, opened once by bcl_connect and reused by every bcl_send until
 *   bcl_disconnect. ctx->transport is NULL until bcl_connect() succeeds;
 *   from then on it points at this file's own private struct, holding the
 *   session and its fd.
 *
 *   Admin ops (CREATE/OPEN/CLOSE/PUBLISH): a one-shot, plaintext AF_UNIX
 *   connection to ballotd's ctl socket (ctx->ctl_path, set by
 *   bcl_set_ctl_path) - dialled, used for exactly one request, and closed,
 *   because that is ballotd's ctl_thread's own model (accept, one frame,
 *   one reply, close).
 *
 * No cert/ballot pairing is logged in either path, preserving wire secrecy
 * - this file never calls bcl_log with request contents.
 *
 * Host is a dotted-quad IPv4 address (inet_pton), not a hostname - same
 * convention tetriSH's tetrisu/net.c uses for the same connection shape.
 */

typedef struct {
  session_t session;
  int fd;
  int connected;
} bcl_transport_t;

bb_result_t bcl_connect(bcl_ctx *ctx, const char *host, int port, const char *ca_path) {
  if (ctx == NULL || host == NULL || ca_path == NULL) {
    return BB_ERR_DB;
  }

  bcl_transport_t *t = calloc(1, sizeof(*t));
  if (t == NULL) {
    return BB_ERR_DB;
  }

  t->fd = socket(AF_INET, SOCK_STREAM, 0);
  if (t->fd < 0) {
    free(t);
    return BB_ERR_DB;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    close(t->fd);
    free(t);
    return BB_ERR_DB;
  }

  if (connect(t->fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(t->fd);
    free(t);
    return BB_ERR_DB;
  }

  if (session_connect(&t->session, t->fd, ca_path) != SESSION_OK) {
    close(t->fd);
    free(t);
    return BB_ERR_DB;
  }

  /* Any previous transport (e.g. a stale one bcl_send already marked dead)
   * is replaced, not leaked. */
  bcl_disconnect(ctx);

  t->connected = 1;
  ctx->transport = t;
  bcl_log(ctx, "[transport] connected");
  return BB_OK;
}

void bcl_disconnect(bcl_ctx *ctx) {
  if (ctx == NULL || ctx->transport == NULL) {
    return;
  }
  bcl_transport_t *t = ctx->transport;
  if (t->connected) {
    session_close(&t->session);
    close(t->fd);
  }
  free(t);
  ctx->transport = NULL;
}

int bcl_auth(bcl_ctx *ctx, const char *method, const char *username, const char *password,
             int *out_status) {
  if (ctx == NULL || ctx->transport == NULL || method == NULL || username == NULL ||
      password == NULL || out_status == NULL) {
    return -1;
  }
  bcl_transport_t *t = ctx->transport;
  if (!t->connected) {
    return -1;
  }

  /* "username\npassword", matching credential.c's cred_split (splits at the
   * first LF only, so the password is unconstrained - only the username has
   * to avoid one). One stack buffer, scrubbed on every exit below, same
   * discipline tauth.c's own credential_flow keeps server-side. */
  char body[256];
  int blen = snprintf(body, sizeof body, "%s\n%s", username, password);
  int ret = -1;
  if (blen < 0 || (size_t)blen >= sizeof body) {
    goto out;
  }

  htttp_request_t req;
  memset(&req, 0, sizeof req);
  if (snprintf(req.method, sizeof req.method, "%s", method) >= (int)sizeof req.method) {
    goto out;
  }
  snprintf(req.path, sizeof req.path, "/");
  req.body = (const uint8_t *)body;
  req.body_len = (uint32_t)blen;

  uint8_t wire[SESSION_MAX_FRAME];
  uint32_t wlen = sizeof wire;
  if (htttp_serialize_request(&req, wire, &wlen) != HTTTP_OK) {
    goto out;
  }

  if (session_send(&t->session, wire, wlen) != SESSION_OK) {
    t->connected = 0;
    goto out;
  }

  uint8_t rbuf[SESSION_MAX_FRAME];
  uint32_t rlen = sizeof rbuf;
  int rc = session_recv(&t->session, rbuf, &rlen);
  if (rc != SESSION_OK) {
    if (rc == SESSION_ERR_IO || rc == SESSION_ERR_TOOBIG) {
      t->connected = 0;
    }
    goto out;
  }

  htttp_response_t resp;
  if (htttp_parse_response(rbuf, rlen, &resp) != HTTTP_OK) {
    goto out;
  }
  *out_status = resp.status;
  ret = 0;

out:
  OPENSSL_cleanse(body, sizeof body);
  return ret;
}

void bcl_set_ctl_path(bcl_ctx *ctx, const char *path) {
  if (ctx == NULL || path == NULL) {
    return;
  }
  snprintf(ctx->ctl_path, sizeof(ctx->ctl_path), "%s", path);
}

static int is_admin_op(bcl_op_t op) {
  return op == BCL_CREATE || op == BCL_OPEN || op == BCL_CLOSE || op == BCL_PUBLISH ||
         op == BCL_ADMIN_RESULTS || op == BCL_ADMIN_CHECK || op == BCL_ADMIN_NEXT_ID;
}

/* Voter ops: the persistent tetrissh session opened by bcl_connect. */
static bb_result_t send_via_session(bcl_ctx *ctx, const bcl_request_t *req, bcl_response_t *resp) {
  if (ctx->transport == NULL) {
    return BB_ERR_DB;
  }
  bcl_transport_t *t = ctx->transport;
  if (!t->connected) {
    return BB_ERR_DB;
  }

  uint8_t wire[SESSION_MAX_FRAME];
  uint32_t wlen = sizeof wire;
  if (bcl_encode_request(req, wire, &wlen) != 0) {
    bcl_log(ctx, "[transport] encode failed for op=%d", (int)req->op);
    return BB_ERR_DB;
  }

  if (session_send(&t->session, wire, wlen) != SESSION_OK) {
    t->connected = 0; /* stream state is unknown; do not reuse it */
    return BB_ERR_DB;
  }

  uint8_t rbuf[SESSION_MAX_FRAME];
  uint32_t rlen = sizeof rbuf;
  int rc = session_recv(&t->session, rbuf, &rlen);
  if (rc != SESSION_OK) {
    if (rc == SESSION_ERR_IO || rc == SESSION_ERR_TOOBIG) {
      t->connected = 0;
    }
    return BB_ERR_DB;
  }

  htttp_response_t http;
  if (htttp_parse_response(rbuf, rlen, &http) != HTTTP_OK) {
    return BB_ERR_DB;
  }
  if (bcl_decode_response(&http, resp) != 0) {
    return BB_ERR_DB;
  }
  return resp->status;
}

/* Admin ops: one AF_UNIX connection to ballotd's ctl socket, one request,
 * one reply, closed - mirrors ctl_thread's own model on the daemon side. */
static bb_result_t send_via_ctl(bcl_ctx *ctx, const bcl_request_t *req, bcl_response_t *resp) {
  if (ctx->ctl_path[0] == '\0') {
    return BB_ERR_DB;
  }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return BB_ERR_DB;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  if (snprintf(addr.sun_path, sizeof addr.sun_path, "%s", ctx->ctl_path) >=
      (int)sizeof addr.sun_path) {
    close(fd);
    return BB_ERR_DB;
  }

  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return BB_ERR_DB;
  }

  uint8_t wire[CTL_MAX_FRAME];
  uint32_t wlen = sizeof wire;
  if (bcl_encode_request(req, wire, &wlen) != 0) {
    bcl_log(ctx, "[transport] encode failed for op=%d", (int)req->op);
    close(fd);
    return BB_ERR_DB;
  }

  if (ctl_frame_write(fd, wire, wlen) != 0) {
    close(fd);
    return BB_ERR_DB;
  }

  uint8_t rbuf[CTL_MAX_FRAME];
  uint32_t rlen = 0;
  int rc = ctl_frame_read(fd, rbuf, sizeof rbuf, &rlen);
  close(fd); /* one request per connection either way - ctl_thread already closed its end */
  if (rc != 0) {
    return BB_ERR_DB;
  }

  htttp_response_t http;
  if (htttp_parse_response(rbuf, rlen, &http) != HTTTP_OK) {
    return BB_ERR_DB;
  }
  if (bcl_decode_response(&http, resp) != 0) {
    return BB_ERR_DB;
  }
  return resp->status;
}

bb_result_t bcl_send(bcl_ctx *ctx, const bcl_request_t *req, bcl_response_t *resp) {
  if (resp != NULL) {
    memset(resp, 0, sizeof(*resp));
  }
  if (ctx == NULL || req == NULL || resp == NULL) {
    return BB_ERR_DB;
  }

  return is_admin_op(req->op) ? send_via_ctl(ctx, req, resp) : send_via_session(ctx, req, resp);
}
