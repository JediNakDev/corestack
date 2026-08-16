#include "../system_program.h"

static int spawn_daemon(void)
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

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: dspawn <executable> [args...]\n");
        return EXIT_FAILURE;
    }

    if (spawn_daemon() != EXIT_SUCCESS)
    {
        return EXIT_FAILURE;
    }

    /* --- daemon process --- replace the image with the requested
       executable so the daemon runs it (e.g. `dspawn tetrisd`). */
    execvp(argv[1], &argv[1]);

    /* execvp only returns on failure. */
    perror("dspawn: exec failed");
    return EXIT_FAILURE;
}
