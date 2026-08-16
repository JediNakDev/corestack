#ifndef BALLOTD_DISPATCH_H
#define BALLOTD_DISPATCH_H

/*
 * dispatch.h - the one routing table between a decoded request and
 * libballotbrain. Called only from admin_thread, off the single bb_ctx*,
 * regardless of which channel (voter TCP or admin Unix socket) the request
 * arrived on - by the time it reaches here, that distinction has already
 * been enforced by the channel that decoded it.
 */

#include "libballotbrain/ballotbrain.h"
#include "libballotclient/client.h"

/*
 * Run req against ctx and fill resp. Always overwrites *resp (memset first,
 * then whichever fields the matching bb_* call fills in) and returns the
 * same bb_result_t as resp->status, so a caller that only wants the code
 * does not have to reach into resp.
 */
bb_result_t ballotd_dispatch(bb_ctx *ctx, const bcl_request_t *req, bcl_response_t *resp);

#endif /* BALLOTD_DISPATCH_H */
