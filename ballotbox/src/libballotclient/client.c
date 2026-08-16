#include "libballotclient/client.h"
#include "internal.h"

#include <stdarg.h>
#include <stdlib.h>

/* struct bcl_ctx itself lives in internal.h, shared with transport.c.
 * Instance-scoped, no file-scope state. */

bcl_ctx *bcl_create(void) {
  bcl_ctx *ctx = calloc(1, sizeof(*ctx));
  if (ctx == NULL) {
    return NULL;
  }
  ctx->log = stderr;
  return ctx;
}

void bcl_destroy(bcl_ctx *ctx) {
  /* Does NOT call bcl_disconnect(): that would pull transport.o into every
   * binary that links libballotclient, including unit tests that define
   * their own bcl_send precisely to keep transport.o's real member out of
   * the archive pull (see fake_client_seams.h) - a test's bcl_send would
   * then collide with transport.o's. A caller that connected is
   * responsible for bcl_disconnect() first; ballotu does. */
  free(ctx);
}

void bcl_set_log(bcl_ctx *ctx, FILE *sink) {
  if (ctx != NULL) {
    ctx->log = sink;
  }
}

void bcl_log(bcl_ctx *ctx, const char *fmt, ...) {
  if (ctx == NULL || ctx->log == NULL) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  vfprintf(ctx->log, fmt, ap);
  va_end(ap);
  fputc('\n', ctx->log);
}
