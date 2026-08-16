#include "libtetrisutil/rc.h"
#include "logger.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

logd_opts_t opts;

static void on_terminate(int sig)
{
    (void)sig;
    logd_stop();
}

static void on_hangup(int sig)
{
    (void)sig;
    rc_reload();
    /* Best-effort: a failed reload leaves the previous opts in place, and a
     * signal handler has nowhere safe to report from anyway. Assigned rather
     * than (void)-cast because GCC does not count the cast as using the result
     * of a warn_unused_result function where clang does - the cast alone
     * builds on macOS and fails the Linux CI job. */
    int cfg_rc = config(&opts);
    (void)cfg_rc;
    logd_reopen();
}

/*
 * Install one handler without SA_RESTART: the receive loop relies on the
 * blocking recvfrom() failing with EINTR to notice the flag a handler set.
 */
static int install(int sig, void (*handler)(int))
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(sig, &sa, NULL) < 0)
    {
        perror("tetrislogd: sigaction");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    memset(&opts, 0, sizeof opts);
    if (config(&opts) < 0)
        return 1;

    /* A sender that vanishes mid-send must not kill us; we never write to a
     * pipe, but a supervisor closing our stdout could otherwise be fatal. */
    if (install(SIGPIPE, SIG_IGN) < 0 || install(SIGINT, on_terminate) < 0 ||
        install(SIGTERM, on_terminate) < 0 || install(SIGHUP, on_hangup) < 0)
        return 1;

    logd_stats_t stats;
    if (logd_run(&opts, &stats) < 0)
        return 1;

    fprintf(stderr,
            "tetrislogd: exiting (received=%lu filtered=%lu malformed=%lu "
            "truncated=%lu dropped=%lu)\n",
            stats.received, stats.filtered, stats.malformed, stats.truncated,
            stats.dropped);
    return 0;
}
