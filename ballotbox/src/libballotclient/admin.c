#include "libballotclient/admin.h"
#include "libballotbrain/ballotbrain.h" /* bb_validate_config (authoritative) */
#include "libtetrisutil/name.h"

#include <string.h>

bb_result_t bc_prevalidate_config(const bb_config_t *config) {
  /* Reuse the daemon's authoritative validator so client and server agree on
   * exactly what "valid" means - no drift, no second copy of the rules. */
  return bb_validate_config(config);
}

bb_result_t bc_build_create(const bb_config_t *config, bcl_request_t *out) {
  if (config == NULL || out == NULL) {
    return BB_ERR_CONFIG_TITLE;
  }
  bb_result_t vr = bc_prevalidate_config(config);
  if (vr != BB_OK) {
    return vr;
  }
  memset(out, 0, sizeof(*out));
  out->op = BCL_CREATE;
  out->config = *config;
  return BB_OK;
}

int bc_fold_eligible(char out[][BB_CERT_LEN], int *count, char *bad_entry,
                     size_t bad_cap) {
  if (out == NULL || count == NULL) {
    return -1;
  }
  for (int i = 0; i < *count; i++) {
    char folded[BB_CERT_LEN];
    size_t len = strlen(out[i]);
    if (!user_name_ok(out[i], len) ||
        user_name_fold(folded, sizeof folded, out[i], len) != 0) {
      if (bad_entry != NULL)
        snprintf(bad_entry, bad_cap, "%s", out[i]);
      return -1;
    }
    snprintf(out[i], BB_CERT_LEN, "%s", folded);
  }

  int kept = 0;
  for (int i = 0; i < *count; i++) {
    int dup = 0;
    for (int j = 0; j < kept; j++) {
      if (strcmp(out[j], out[i]) == 0) {
        dup = 1;
        break;
      }
    }
    if (!dup) {
      if (kept != i)
        snprintf(out[kept], BB_CERT_LEN, "%s", out[i]);
      kept++;
    }
  }
  *count = kept;
  return 0;
}

bb_result_t bc_build_transition(bcl_op_t op, const char *election_id,
                                bcl_request_t *out) {
  if (out == NULL || election_id == NULL) {
    return BB_ERR_DB;
  }
  if (op != BCL_OPEN && op != BCL_CLOSE && op != BCL_PUBLISH) {
    return BB_ERR_ILLEGAL_TRANSITION;
  }
  memset(out, 0, sizeof(*out));
  out->op = op;
  snprintf(out->election_id, BB_ID_LEN, "%s", election_id);
  return BB_OK;
}
