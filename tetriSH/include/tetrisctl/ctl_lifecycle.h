#ifndef TETRISCTL_CTL_LIFECYCLE_H
#define TETRISCTL_CTL_LIFECYCLE_H

/*
 * ctl_lifecycle.h - starting and stopping the daemons.
 *
 * Separate from ctl_client because the failures are different in kind: that
 * file talks to a running daemon over a socket, this one forks processes and
 * signals pids. No ncurses here - progress is reported through a callback, so
 * the CLI prints lines and the TUI drives tetrisui_progress_step from the same
 * sequence.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef enum
{
    DAEMON_TETRISD = 0,
    DAEMON_TETRISLOGD,
} Daemon;

/* Reports step `index` finished, ok or not. Called in order, once per step. */
typedef void (*CtlStepFn)(int index, int ok, void *ctx);

#define CTL_START_STEPS 2
#define CTL_STOP_LOGD_STEPS 3
/* Not const-qualified inside: tetrisui_progress_begin takes `const char *[]`,
 * and a `const char *const[]` will not convert to it. */
extern const char *ctl_start_step_names[CTL_START_STEPS];
extern const char *ctl_stop_logd_step_names[CTL_STOP_LOGD_STEPS];

const char *ctl_daemon_name(Daemon d);
int ctl_daemon_parse(const char *name, Daemon *out);

/*
 * The project root, and paths under it.
 *
 * $TETRISD_ROOT, else the working directory - the ctl_socket_path() idiom from
 * control_plane.h, factored so this file agrees with it about where the
 * project is. ADR 0003's tetrish_root() would replace both; it does not exist
 * yet, and implementing it does not belong in this change.
 */
int ctl_root(char *out, size_t cap);
int ctl_root_path(char *out, size_t cap, const char *rel);

/*
 * Is it up, and as what pid?
 *
 * tetrisd: whether STATUS answers on `sock` - more authoritative than a
 * pidfile nobody unlinks. tetrislogd: its pidfile corroborated by a live
 * var/run/tetrislogd.sock, so the answer means "logd is there", not "some
 * process once had this number".
 *
 * pid_out may be NULL, and is left untouched when the pid is unknown.
 */
bool ctl_probe(Daemon d, const char *sock, pid_t *pid_out);

/* The account database is managed by bin/tetrisdb rather than dspawn2. */
bool ctl_probe_db(void);
int ctl_db_command(const char *command, char *err, size_t err_cap);

/*
 * Spawn via bin/dspawn2 and confirm by observation.
 *
 * dspawn2's parent exits after its FIRST fork, before execvp, so its status
 * proves only that forking worked - the confirm step is what actually
 * establishes that the daemon is alive.
 *
 * Returns CTL_START_*. Already-up is reported rather than papered over: the
 * confirm step would otherwise pass on its first poll against the daemon that
 * was already there, and claim to have spawned something it did not.
 */
enum
{
    CTL_START_OK = 0,
    CTL_START_ALREADY = 1,
    CTL_START_FAILED = -1,
};

int ctl_start(Daemon d, const char *sock, CtlStepFn step, void *ctx, char *err,
              size_t err_cap);

/*
 * SIGTERM tetrislogd, which has no control plane.
 *
 * Refuses unless the pidfile pid is corroborated by a live logd socket: a
 * pidfile nobody cleans plus a recycled pid would otherwise signal a stranger.
 * Returns 0, or -1 with `err` filled.
 */
int ctl_stop_logd(CtlStepFn step, void *ctx, char *err, size_t err_cap);

#endif /* TETRISCTL_CTL_LIFECYCLE_H */
