#ifndef BALLOTBOX_FUZZ_SUPPORT_H
#define BALLOTBOX_FUZZ_SUPPORT_H

/*
 * fuzz_support.h - the two things every fuzz target in this directory needs:
 * a check macro that survives -DNDEBUG, and the "is this fixed-size field a
 * C string" predicate the parsers' contracts are written in.
 *
 * Why not assert(): a fuzz target's checks ARE the oracle. assert() compiles
 * away under NDEBUG, and a target built that way still runs, still reports
 * execs/sec, and finds only the bugs that happen to segfault - the worst
 * possible failure mode, because it looks like a clean 24-hour run. FUZZ_CHECK
 * is always live.
 *
 * The distinction that matters when reading a crash: __builtin_trap here means
 * the code under test broke a promise its header makes; an ASan report means it
 * corrupted memory. Both are bugs, but the first one is a contract bug and the
 * second is a memory-safety bug, and they get fixed differently.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * abort(), NOT __builtin_trap(): a trap raises SIGTRAP, which libFuzzer does
 * not install a handler for. The run dies, the message prints, and THE INPUT
 * IS NEVER WRITTEN TO artifact_prefix - so the finding exists only as a line
 * of scrollback and cannot be reproduced, minimised or filed as a regression.
 * SIGABRT is handled (handle_abrt defaults on), so the crashing input lands on
 * disk. ASan reports the same way. Found the hard way: four findings in one
 * smoke run, none of them saved.
 *
 * fflush before aborting for the same reason in miniature - the explanation of
 * WHICH contract broke is worth more than the stack, and a buffered stderr
 * loses it on abort.
 */
#define FUZZ_CHECK(cond)                                                    \
  do {                                                                      \
    if (!(cond)) {                                                          \
      fprintf(stderr, "fuzz: contract broken: %s\n  at %s:%d in %s\n",      \
              #cond, __FILE__, __LINE__, __func__);                         \
      fflush(stderr);                                                       \
      abort();                                                              \
    }                                                                       \
  } while (0)

/* Every fixed-size char field a parser fills must come back NUL-terminated:
 * the caller's next move is printf/strcmp, and an unterminated field turns
 * that into an over-read that ASan would blame on the CALLER, not on the
 * parser that actually broke the contract. Checked at the source instead. */
static inline int fuzz_is_cstr(const char *field, size_t cap) {
  return memchr(field, '\0', cap) != NULL;
}

/* A zero-copy parser (htttp, rows) hands back pointers INTO the input buffer.
 * "Inside" includes one past the end, which is a legal empty slice. */
static inline int fuzz_slice_inside(const void *p, size_t n, const void *base,
                                    size_t base_len) {
  const uint8_t *s = (const uint8_t *)p;
  const uint8_t *b = (const uint8_t *)base;
  if (s == NULL) return n == 0;
  return s >= b && s <= b + base_len && (size_t)(b + base_len - s) >= n;
}

#endif /* BALLOTBOX_FUZZ_SUPPORT_H */
