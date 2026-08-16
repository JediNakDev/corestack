#include "../system_program.h"
#include "libtetrisutil/rc.h"

/* Matches dspawn2.c's own default exactly - dspawn2 writes one <name>.pid
 * file per daemon here, and this is where we look for them. */
#define DEFAULT_PID_DIR "var/run"

/* How many *.pid files in pid_dir name a process that is still alive.
 *
 * dspawn2 execvp()s into the daemon it starts, so the resulting process's own
 * name is the daemon's (e.g. "tetrislogd"), never "dspawn2" - a ps grep on
 * the command name, which is all the loop below this one can see, cannot
 * find it. The pidfile dspawn2 writes for its own already_running() check
 * (src/tetrish/system_programs/dspawn2.c) is the one place that identity
 * survives, so this counts the same way that check does: read the pid, ask
 * the kernel with kill(pid, 0) rather than trust the file blindly.
 */
static int count_dspawn2_daemons(void)
{
    char pid_dir[PATH_MAX];
    (void)rc_get("dspawn_pid_dir", DEFAULT_PID_DIR, pid_dir, sizeof pid_dir);

    DIR *d = opendir(pid_dir);
    if (d == NULL)
        return 0; /* no directory yet: nothing has spawned through it */

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
    {
        size_t len = strlen(ent->d_name);
        if (len <= 4 || strcmp(ent->d_name + len - 4, ".pid") != 0)
            continue;

        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", pid_dir, ent->d_name);

        FILE *f = fopen(path, "r");
        if (f == NULL)
            continue;
        long pid = 0;
        int got = fscanf(f, "%ld", &pid);
        fclose(f);

        /* EPERM means it is alive but owned by someone else, which still
         * counts as running - the same reading dspawn2's own check uses. */
        if (got == 1 && pid > 0 &&
            (kill((pid_t)pid, 0) == 0 || errno == EPERM))
            count++;
    }
    closedir(d);
    return count;
}

int main(void)
{
    /* -e: every process, -o: choose columns. We print the controlling
       terminal and the command, then drop anything on a tty/pts and
       anything not named dspawn. Works on both Linux and macOS.

       This only ever matches dspawn(1), the original, non-daemonising
       spawn: dspawn2 execvp()s into its target and is counted separately,
       below, by count_dspawn2_daemons(). */
    const char *cmd =
        "ps -eo tty,comm 2>/dev/null | grep '[d]spawn' | grep -Ev 'tty|pts'";

    FILE *fp = popen(cmd, "r");
    if (fp == NULL)
    {
        perror("popen failed");
        return EXIT_FAILURE;
    }

    int count = 0;
    char line[SHELL_BUFFERSIZE];
    while (fgets(line, sizeof(line), fp) != NULL)
    {
        count++;
    }

    pclose(fp);

    count += count_dspawn2_daemons();

    printf("Number of live daemon(s) spawned from dspawn: " COLOR_GREEN
           "%d\n" COLOR_RESET,
           count);

    return EXIT_SUCCESS;
}
