/*
 * fuzz_rc_bind.c - rc_bind() against a whole generated rc file.
 *
 * rc_bind is the typed half of the rc reader: a declared key table, whole
 * string strtol with bounds, fixed-buffer copies, and a defect report naming
 * the first bad line. Three libraries share it (tetrislogd, libtetrisdb's
 * runner, libtetrisauth's session reader), so its bounds checking is the one
 * copy that has to be right - which is the argument rc.h makes for the table
 * existing at all.
 *
 * This target is slower than the rest by design: rc_bind takes a PATH, not a
 * buffer, so every case writes a file. A few thousand execs/sec instead of a
 * few hundred thousand, which is still ample for a 255-line parser, and it is
 * the honest interface - a fake in-memory one would test code that does not
 * exist. The file is written to $TMPDIR and reused, not recreated.
 *
 * The key table below mirrors the SHAPES real callers declare (int with
 * bounds, size_t, bool, capped string, custom parser, check_only), not any
 * one caller's keys, so every branch of bind_one() is reachable.
 */

#include "libtetrisutil/rc.h"
#include "fuzz_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  int level;
  int attempts;
  size_t queue;
  int enabled;
  char path[64];
  char host[32];
  int custom;
} conf_t;

/* Something only the owner could parse - RC_CUSTOM's reason for existing. */
static int parse_facility(const char *value, void *field) {
  static const char *names[] = {"daemon", "user", "local0"};
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
    if (strcmp(names[i], value) == 0) {
      *(int *)field = (int)i;
      return 0;
    }
  return -1;
}

static const rc_key_t KEYS[] = {
    {"fuzz_level", RC_INT, offsetof(conf_t, level), 0, 7, 0, 0, NULL, false},
    {"fuzz_attempts", RC_INT, offsetof(conf_t, attempts), 1, 1000, 0, 0, NULL,
     false},
    {"fuzz_queue", RC_SIZE, offsetof(conf_t, queue), 0, 1 << 20, 0, 0, NULL,
     false},
    {"fuzz_enabled", RC_BOOL, offsetof(conf_t, enabled), 0, 0, 0, 0, NULL,
     false},
    {"fuzz_path", RC_STR, offsetof(conf_t, path), 0, 0, sizeof(((conf_t *)0)->path),
     0, NULL, false},
    /* max_len shorter than cap: the two bounds are separate fields and a
     * parser that checks the wrong one is the bug this key exists to find. */
    {"fuzz_host", RC_STR, offsetof(conf_t, host), 0, 0, sizeof(((conf_t *)0)->host),
     16, NULL, false},
    {"fuzz_facility", RC_CUSTOM, offsetof(conf_t, custom), 0, 0, 0, 0,
     parse_facility, false},
    /* Validated, stored nowhere - off must never be dereferenced for it. */
    {"fuzz_checked", RC_INT, 0, 0, 100, 0, 0, NULL, true},
};
#define N_KEYS (sizeof KEYS / sizeof KEYS[0])

static const char *rc_tmp_path(void) {
  static char path[512];
  if (path[0] == '\0') {
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    snprintf(path, sizeof path, "%s/ballotbox-fuzz-rc-%d.rc", dir, (int)getpid());
  }
  return path;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > 16 * 1024) return 0;

  const char *path = rc_tmp_path();
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  if (size && fwrite(data, 1, size, f) != size) {
    fclose(f);
    return 0;
  }
  fclose(f);

  conf_t conf;
  memset(&conf, 0, sizeof conf);
  /* Seed with defaults, as rc.h instructs - and with a terminator already in
   * place, so a string key that fails to terminate is visible as a change
   * rather than as whatever the stack held. */
  conf.path[0] = '\0';
  conf.host[0] = '\0';

  rc_defect_t defect;
  memset(&defect, 0xBB, sizeof defect); /* poison, see below */

  /* owned_prefix alternates with the input so both namespaces are explored:
   * NULL (the in-process readers, unknown keys skipped) and "fuzz_" (the
   * operator-facing check run, where an unknown key in our namespace is an
   * error). The two take different paths through bind_directive. */
  const char *prefix = (size && (data[0] & 1)) ? "fuzz_" : NULL;

  int rc = rc_bind(path, KEYS, N_KEYS, &conf, prefix, &defect);

  /* rc.h: a directive count, or one of RC_E_OPEN/RC_E_VALUE/RC_E_UNKNOWN.
   * The file exists, so RC_E_OPEN would be wrong here specifically. */
  FUZZ_CHECK(rc >= 0 || rc == RC_E_VALUE || rc == RC_E_UNKNOWN);
  FUZZ_CHECK(rc != RC_E_OPEN);

  if (rc == RC_E_VALUE || rc == RC_E_UNKNOWN) {
    /* The defect names the offending line, and an operator reads it - so it
     * must be two C strings, not the poison and not raw file bytes. */
    FUZZ_CHECK(fuzz_is_cstr(defect.key, sizeof defect.key));
    FUZZ_CHECK(fuzz_is_cstr(defect.value, sizeof defect.value));
    FUZZ_CHECK(defect.key[0] != '\0');
  }

  if (rc >= 0) {
    /* Only bounded values may have landed. Values outside the declared range
     * are the whole point of the table: a log level of 900 or a queue size of
     * 2^31 reaching the struct means the range check did not run. */
    FUZZ_CHECK(conf.level >= 0 && conf.level <= 7);
    FUZZ_CHECK(conf.attempts == 0 ||
               (conf.attempts >= 1 && conf.attempts <= 1000));
    FUZZ_CHECK(conf.queue <= (1u << 20));
    FUZZ_CHECK(conf.enabled == 0 || conf.enabled == 1);
    FUZZ_CHECK(conf.custom >= 0 && conf.custom <= 2);
    FUZZ_CHECK(fuzz_is_cstr(conf.path, sizeof conf.path));
    FUZZ_CHECK(fuzz_is_cstr(conf.host, sizeof conf.host));
    /* max_len 16, so a longer host must have been refused, not truncated -
     * truncation would silently point a reader at the wrong host. */
    FUZZ_CHECK(strlen(conf.host) <= 16);
  }

  /* Reading the same file twice must produce the same struct: the parser
   * keeps no state between loads, and three libraries load it in one
   * process. */
  conf_t again;
  memset(&again, 0, sizeof again);
  int rc2 = rc_bind(path, KEYS, N_KEYS, &again, prefix, NULL);
  FUZZ_CHECK(rc2 == rc);
  if (rc >= 0) FUZZ_CHECK(memcmp(&conf, &again, sizeof conf) == 0);

  return 0;
}
