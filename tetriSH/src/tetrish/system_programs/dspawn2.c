#include "libtetrisutil/rc.h"
#include "../system_program.h"

#define DEFAULT_PID_DIR "var/run"
#define DEFAULT_ERR_DIR "var/log"

static int already_running(const char *pidfile)
{
    long pid = 0;
    FILE *f = fopen(pidfile, "r");

    if (f == NULL)
        return 0; /* no pidfile: nothing claims to be running */

    int got = fscanf(f, "%ld", &pid);
    fclose(f);
    if (got != 1 || pid <= 0)
        return 0;

    /* kill(0) tests for a signalable process without sending anything; EPERM
       means it is alive but someone else's, which still counts as running. */
    if (kill((pid_t)pid, 0) == 0 || errno == EPERM)
    {
        fprintf(
            stderr,
            "dspawn2: already running (pid %ld); a second copy would fight it "
            "over its files\n",
            pid);
        return 1;
    }
    return 0;
}

static int spawn_daemon(const char *pidfile, const char *errfile)
{
    pid_t pid;
    int fd0, fd1, fd2;
    int x;

    pid = fork();
    if (pid < 0)
    {
        perror("first fork failed");
        return EXIT_FAILURE;
    }
    if (pid > 0)
    {
        exit(EXIT_SUCCESS);
    }

    /* --- intermediate process from here on --- */

    /* become session leader and lose the controlling TTY. */
    if (setsid() < 0)
    {
        perror("setsid failed");
        return EXIT_FAILURE;
    }

    /* ignore SIGCHLD (no zombies) and SIGHUP (so the daemon
       isn't killed when this session leader terminates). */
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0)
    {
        perror("second fork failed");
        return EXIT_FAILURE;
    }
    if (pid > 0)
    {
        exit(EXIT_SUCCESS);
    }

    /* --- daemon process from here on --- */

    //* no umask(0) - keep file permission */

    //* no chdir("/") - keep the project root */

    /* close all open file descriptors and redirect fd 0,1,2 */
    for (x = sysconf(_SC_OPEN_MAX); x >= 0; x--)
        close(x);

    fd0 = open("/dev/null", O_RDONLY); /* stdin  -> fd 0 */

    //* stdout and stderr go to the daemon's error log instead of /dev/null */
    fd1 = open(errfile, O_WRONLY | O_CREAT | O_APPEND, 0640);
    if (fd1 < 0)
        fd1 = open("/dev/null", O_WRONLY);
    fd2 = dup(fd1);
    (void)fd0;
    (void)fd2;

    //* record the pid to prevent spawning multiple daemon. */
    FILE *f = fopen(pidfile, "w");
    if (f == NULL)
        return EXIT_FAILURE; /* a daemon nobody can signal is worse than none */
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);

    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    char pidfile[PATH_MAX], errfile[PATH_MAX];
    const char *slash, *name;

    if (argc < 2)
    {
        fprintf(stderr, "usage: dspawn2 <executable> [args...]\n");
        return EXIT_FAILURE;
    }

    char pid_dir[PATH_MAX], err_dir[PATH_MAX];
    (void)rc_get("dspawn_pid_dir", DEFAULT_PID_DIR, pid_dir, sizeof pid_dir);
    (void)rc_get("dspawn_err_dir", DEFAULT_ERR_DIR, err_dir, sizeof err_dir);

    slash = strrchr(argv[1], '/');
    name = slash != NULL ? slash + 1 : argv[1];
    snprintf(pidfile, sizeof(pidfile), "%s/%s.pid", pid_dir, name);
    snprintf(errfile, sizeof(errfile), "%s/%s.err", err_dir, name);

    if (already_running(pidfile))
    {
        return EXIT_FAILURE;
    }

    if (spawn_daemon(pidfile, errfile) != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    /* --- daemon process --- replace the image with the requested
       executable so the daemon runs it (e.g. `dspawn2 tetrislogd`). */
    execvp(argv[1], &argv[1]);

    /* execvp only returns on failure. Remove the pidfile: it names a process
       that is about to stop existing. */
    perror("dspawn2: exec failed");
    unlink(pidfile);
    return EXIT_FAILURE;
}
