#include "system_program.h"

int main(void) {
  /* -e: every process, -o: choose columns. We print the controlling
     terminal and the command, then drop anything on a tty/pts and
     anything not named dspawn. Works on both Linux and macOS. */
  const char *cmd =
      "ps -eo tty,comm 2>/dev/null | grep '[d]spawn' | grep -Ev 'tty|pts'";

  FILE *fp = popen(cmd, "r");
  if (fp == NULL) {
    perror("popen failed");
    return EXIT_FAILURE;
  }

  int count = 0;
  char line[SHELL_BUFFERSIZE];
  while (fgets(line, sizeof(line), fp) != NULL) {
    count++;
  }

  pclose(fp);

  printf("Number of live daemon(s) spawned from dspawn: " COLOR_GREEN
         "%d\n" COLOR_RESET,
         count);

  return EXIT_SUCCESS;
}
