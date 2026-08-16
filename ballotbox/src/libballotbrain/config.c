#include "libballotbrain/ballotbrain.h"

#include <string.h>

/*
 * UC-1 config validation. Pure predicate, no side effects. Returns a specific
 * BB_ERR_CONFIG_* code so the client can show the exact reason (README error
 * state 2a). Checked in order: title, option count, time window.
 *
 * Time comparison is lexicographic on the ISO-8601 strings, which orders
 * correctly for fixed-format timestamps; a real clock/parse lands with the
 * daemon's time source.
 */
bb_result_t bb_validate_config(const bb_config_t *config) {
  if (config == NULL) {
    return BB_ERR_CONFIG_TITLE;
  }
  if (config->title[0] == '\0') {
    return BB_ERR_CONFIG_TITLE;
  }
  if (config->option_count < 2) {
    return BB_ERR_CONFIG_OPTIONS;
  }
  if (strcmp(config->close_time, config->open_time) <= 0) {
    return BB_ERR_CONFIG_TIME;
  }
  return BB_OK;
}
