/* Verify q and SIGINT restore the terminal after ncurses has changed it. */

#define _XOPEN_SOURCE 600

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include "test_output.h"
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "libtetrisui/tetrisui.h"

static void pause_ms(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000,
                          .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void drain_output(int fd)
{
    char buf[4096];
    while (read(fd, buf, sizeof buf) > 0)
    {
    }
}

static int wait_pid(pid_t pid, int master, int timeout_ms, int *status)
{
    for (int waited = 0; waited < timeout_ms; waited += 20)
    {
        drain_output(master);
        pid_t rc = waitpid(pid, status, WNOHANG);
        if (rc == pid)
            return 0;
        if (rc < 0)
            return -1;
        pause_ms(20);
    }
    return -1;
}

static int same_mode(const struct termios *a, const struct termios *b)
{
    const tcflag_t local = ECHO | ECHONL | ICANON | ISIG | IEXTEN;
    return (a->c_lflag & local) == (b->c_lflag & local) &&
           (a->c_oflag & OPOST) == (b->c_oflag & OPOST);
}

static int run_case(int use_signal)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0)
        return -1;
    char *slave_name = ptsname(master);
    int slave = slave_name ? open(slave_name, O_RDWR | O_NOCTTY) : -1;
    if (slave < 0)
        return -1;
    int master_flags = fcntl(master, F_GETFL, 0);
    if (master_flags >= 0)
        (void)fcntl(master, F_SETFL, master_flags | O_NONBLOCK);

    struct winsize ws = {.ws_row = 40, .ws_col = 120};
    (void)ioctl(slave, TIOCSWINSZ, &ws);
    struct termios before, during, after;
    memset(&after, 0, sizeof after);
    if (tcgetattr(master, &before) != 0)
        return -1;

    pid_t pid = fork();
    if (pid == 0)
    {
        close(master);
        setsid();
        (void)ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO)
            close(slave);
        setenv("TERM", "xterm-256color", 1);

        if (use_signal)
        {
            char root[PATH_MAX], app[PATH_MAX];
            if (getcwd(root, sizeof root) == NULL)
                _exit(127);
            snprintf(app, sizeof app, "%s/bin/tetrisu", root);
            execl(app, "tetrisu", (char *)NULL);
            _exit(127);
        }

        const char *items[] = {"stay", "quit"};
        tetrisui_init();
        int choice = tetrisui_menu("terminal restore", items, 2, "q quit");
        tetrisui_shutdown();
        _exit(choice == -1 ? 0 : 1);
    }
    if (pid < 0)
        return -1;

    int changed = 0;
    for (int waited = 0; waited < 2000; waited += 20)
    {
        if (tcgetattr(master, &during) == 0 &&
            (during.c_lflag & (ECHO | ICANON)) !=
                (before.c_lflag & (ECHO | ICANON)))
        {
            changed = 1;
            break;
        }
        pause_ms(20);
    }

    if (use_signal)
        kill(pid, SIGINT);
    else
    {
        ssize_t _long = write(master, "q", 1);
        (void)_long;
    }

    int status = 0;
    int exited = wait_pid(pid, master, 3000, &status) == 0;
    if (!exited)
    {
        kill(pid, SIGKILL);
        (void)wait_pid(pid, master, 1000, &status);
    }
    int restored = tcgetattr(master, &after) == 0 && same_mode(&before, &after);
    close(slave);
    close(master);

    int wanted = use_signal ? 130 : 0;
    int code = exited && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (!changed || !exited || code != wanted || !restored)
        test_output_failure_detailf(
            __FILE__, __LINE__,
            "%s: changed=%d exited=%d code=%d restored=%d "
            "lflag=%lx/%lx oflag=%lx/%lx",
            use_signal ? "SIGINT" : "q", changed, exited, code, restored,
            (unsigned long)before.c_lflag, (unsigned long)after.c_lflag,
            (unsigned long)before.c_oflag, (unsigned long)after.c_oflag);
    return changed && exited && code == wanted && restored ? 0 : -1;
}

int main(void)
{
    test_output_begin("test_tetrisu_terminal");
    int q = run_case(0);
    int sigint = run_case(1);
    if (q == 0)
        test_output_pass("q restores terminal");
    else
        test_output_fail("q restores terminal");
    if (sigint == 0)
        test_output_pass("SIGINT restores terminal");
    else
        test_output_fail("SIGINT restores terminal");
    test_output_summary(2, (q != 0) + (sigint != 0), 0);
    return q == 0 && sigint == 0 ? 0 : 1;
}
