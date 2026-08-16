/*
 * replay_main.c - a main() for the fuzz targets, so they can run without
 * libFuzzer.
 *
 * WHY THIS EXISTS, and it is not a convenience:
 *
 * libFuzzer only ships with Homebrew's LLVM (Apple's clang has no fuzzer
 * runtime), and Homebrew's LLVM 19 AddressSanitizer HANGS at startup on this
 * macOS - it never returns from __asan::MemoryRangeIsAvailable while it walks
 * the address space looking for a shadow region. Fixed upstream in LLVM 20.
 * Apple's clang has a working ASan and no fuzzer; Homebrew's has a working
 * fuzzer and a hanging ASan. Neither compiler gives us both.
 *
 * So the work is split, and each half uses the compiler that can do it:
 *
 *   bin/fuzz_*    Homebrew clang, libFuzzer + UBSan. Generates inputs.
 *                 Finds the crashes, the hangs and the broken contracts.
 *   bin/replay_*  Apple clang, ASan + UBSan + this main(). Runs the corpus
 *                 those inputs became, and every filed regression, with full
 *                 memory-safety checking.
 *
 * The pipeline that results is better than either half: the fuzzer's job is to
 * find inputs, and every input it keeps lands in the corpus, where the replay
 * binary re-runs it under ASan. A heap overflow the fuzzer only saw as "no
 * crash, interesting coverage" is caught on the next `make fuzz-regress`.
 *
 * It also means the CI gate needs no Homebrew LLVM at all - which is what lets
 * it run on the same macOS runner as `make test-ci`.
 *
 * Same interface as libFuzzer's own StandaloneFuzzTargetMain: pass files
 * and/or directories, every file is one input.
 */

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/*
 * No LLVMFuzzerInitialize hook here. libFuzzer's contract has one, and the
 * obvious move is to declare it weak and call it if present - but on Mach-O a
 * weak DECLARATION of a symbol nothing defines is still undefined at link
 * time, and ld refuses the binary. Weak-importing it would need
 * -undefined dynamic_lookup, which turns every genuine link error in these
 * harnesses into a runtime surprise. Not worth it for a hook no target uses:
 * if one ever needs per-process setup, call it from here explicitly, so the
 * fuzz build and the replay build cannot silently disagree about whether
 * initialisation ran.
 */

static int run_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "replay: cannot open %s\n", path);
    return -1;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return -1;
  }
  long n = ftell(f);
  if (n < 0) {
    fclose(f);
    return -1;
  }
  rewind(f);

  /* Sized exactly, never padded: a target that reads one byte past its input
   * must hit a redzone here, which is the entire reason to replay under ASan
   * rather than just re-run under the fuzzer. */
  uint8_t *buf = (uint8_t *)malloc((size_t)n ? (size_t)n : 1);
  if (!buf) {
    fclose(f);
    return -1;
  }
  size_t got = n > 0 ? fread(buf, 1, (size_t)n, f) : 0;
  fclose(f);

  LLVMFuzzerTestOneInput(buf, got);
  free(buf);
  return 0;
}

static int run_dir(const char *path, int *count) {
  DIR *d = opendir(path);
  if (!d) return -1;

  struct dirent *e;
  int rc = 0;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue; /* skips . .. and .gitkeep */

    char child[4096];
    snprintf(child, sizeof child, "%s/%s", path, e->d_name);

    struct stat st;
    if (stat(child, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) continue; /* corpus dirs are flat */

    if (run_file(child) != 0) rc = -1;
    (*count)++;
  }
  closedir(d);
  return rc;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <file-or-dir> [...]\n", argv[0]);
    return 2;
  }

  int count = 0;
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    struct stat st;
    if (stat(argv[i], &st) != 0) {
      fprintf(stderr, "replay: no such path %s\n", argv[i]);
      rc = 1;
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      if (run_dir(argv[i], &count) != 0) rc = 1;
    } else {
      if (run_file(argv[i]) != 0) rc = 1;
      count++;
    }
  }

  /* Reaching here at all is the result: a target that survives every input is
   * silent, and a target that does not never returns from
   * LLVMFuzzerTestOneInput - ASan and __builtin_trap both abort the process. */
  printf("replayed %d input%s\n", count, count == 1 ? "" : "s");
  return rc;
}
