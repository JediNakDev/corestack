#include "system_program.h"

static char output_file_path[PATH_MAX];

/* example daemon work from pa1 */
static int daemon_work(void) {
  int num = 0;
  FILE *fptr;
  char buffer[PATH_MAX];
  char *cwd;

  /* Record identity once at startup so we can confirm the daemon
     was adopted by init and is running from root. */
  fptr = fopen(output_file_path, "a");
  if (fptr == NULL) {
    return EXIT_FAILURE;
  }
  fprintf(fptr,
          "Daemon process running with PID: %d, PPID: %d, opening logfile with "
          "FD %d\n",
          getpid(), getppid(), fileno(fptr));

  cwd = getcwd(buffer, sizeof(buffer));
  if (cwd != NULL) {
    fprintf(fptr, "Current working directory: %s\n", cwd);
  }
  fclose(fptr);

  while (1) {
    fptr = fopen(output_file_path, "a");
    if (fptr == NULL) {
      return EXIT_FAILURE;
    }
    fprintf(fptr, "PID %d Daemon writing line %d to the file.\n", getpid(),
            num);
    num++;
    fclose(fptr);

    sleep(10);

    if (num == 10) /* terminate after 10 counts */
    {
      break;
    }
  }

  return EXIT_SUCCESS;
}

static int spawn_daemon(void) {
  pid_t pid;
  int fd0, fd1, fd2;
  int x;

  pid = fork();
  if (pid < 0) {
    perror("first fork failed");
    return EXIT_FAILURE;
  }
  if (pid > 0) {
    exit(EXIT_SUCCESS);
  }

  /* --- intermediate process from here on --- */

  /* become session leader and lose the controlling TTY. */
  if (setsid() < 0) {
    perror("setsid failed");
    return EXIT_FAILURE;
  }

  /* ignore SIGCHLD (no zombies) and SIGHUP (so the daemon
     isn't killed when this session leader terminates). */
  signal(SIGCHLD, SIG_IGN);
  signal(SIGHUP, SIG_IGN);

  pid = fork();
  if (pid < 0) {
    perror("second fork failed");
    return EXIT_FAILURE;
  }
  if (pid > 0) {
    exit(EXIT_SUCCESS);
  }

  /* --- daemon process from here on --- */

  /* allowed daemon created file to be freely accessed */
  umask(0);

  if (chdir("/") < 0)
    return EXIT_FAILURE;

  /* close all open file descriptors and redirect fd 0,1,2 to /dev/null*/
  for (x = sysconf(_SC_OPEN_MAX); x >= 0; x--)
    close(x);

  fd0 = open("/dev/null", O_RDWR); /* stdin  -> fd 0 */
  fd1 = dup(0);                    /* stdout -> fd 1 */
  fd2 = dup(0);                    /* stderr -> fd 2 */
  (void)fd0;
  (void)fd1;
  (void)fd2;

  return EXIT_SUCCESS;
}

int main(void) {
  if (getcwd(output_file_path, sizeof(output_file_path)) == NULL) {
    perror("getcwd() error, exiting now.");
    return EXIT_FAILURE;
  }
  strncat(output_file_path, "/dspawn.log",
          sizeof(output_file_path) - strlen(output_file_path) - 1);

  if (spawn_daemon() != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }

  return daemon_work();
}
