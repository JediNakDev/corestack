/*
 * tetrisctl.c - admin CLI for a running tetrisd, and the door to its console.
 *
 * Speaks HTTTP over the AF_UNIX control socket, never over the public TCP
 * port. That separation is the point: when the game port is saturated, this
 * still gets through, because it shares no socket, no thread and no queue with
 * it.
 *
 *   tetrisctl                             open the live console (tty only)
 *   tetrisctl status                      uptime, session and room counts
 *   tetrisctl shutdown                    graceful stop; alias of stop tetrisd
 *   tetrisctl rooms                       one line per room
 *   tetrisctl players                     one line per connected session
 *   tetrisctl kick <room> <player>        disconnect one player
 *   tetrisctl start <tetrisd|tetrislogd>  spawn via bin/dspawn2 and confirm
 *   tetrisctl stop  <tetrisd|tetrislogd>  SHUTDOWN, or SIGTERM for tetrislogd
 *
 *   --json          print the response body verbatim instead of formatting it
 *   --socket PATH   override the resolved socket path
 *
 * Exit codes are distinct so scripts can tell the cases apart: 0 success,
 * 1 the daemon refused (4xx/5xx), 2 usage error, 3 could not connect.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "libtetrisutil/logmsg.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libhtttp/htttp.h"
#include "tetrisctl/control_plane.h"
#include "tetrisctl/ctl_client.h"
#include "tetrisctl/ctl_lifecycle.h"
#include "tetrisctl/ctl_tui.h"

#define EXIT_OK 0
#define EXIT_REFUSED 1
#define EXIT_USAGE 2
#define EXIT_NOCONN 3

static void usage(void)
{
    fprintf(stderr, "usage: tetrisctl [--json] [--socket PATH] [<command>]\n"
                    "\n"
                    "no command, on a terminal: open the live admin console\n"
                    "\n"
                    "commands:\n"
                    "  status                  uptime, sessions, rooms\n"
                    "  shutdown                stop the daemon gracefully\n"
                    "  rooms                   list active rooms\n"
                    "  players                 list connected players\n"
                    "  kick <room> <player>    disconnect one player\n"
                    "  start <daemon>          tetrisd | tetrislogd\n"
                    "  stop  <daemon>          tetrisd | tetrislogd\n");
}

/* ---- formatters ---------------------------------------------------------- */

static void format_status(const CtlStatus *s)
{
    char up[32];
    ctl_fmt_uptime(up, sizeof up, s->uptime);
    printf("uptime %s  sessions %d  rooms %d\n", up, s->sessions, s->rooms);
}

static void format_rooms(const CtlRoom *r, int n)
{
    if (n == 0)
    {
        printf("no rooms\n");
        return;
    }
    printf("%-5s %-10s %-8s %s\n", "ID", "PHASE", "MEMBERS", "OWNER");
    for (int i = 0; i < n; i++)
    {
        char owner[16];
        if (r[i].owner < 0)
            snprintf(owner, sizeof owner, "-");
        else
            snprintf(owner, sizeof owner, "p%d", r[i].owner);
        printf("%-5d %-10s %-8d %s\n", r[i].id, r[i].phase, r[i].members,
               owner);
    }
}

static void format_players(const CtlPlayer *p, int n)
{
    if (n == 0)
    {
        printf("no players connected\n");
        return;
    }
    printf("%-6s %-7s %-8s %-6s %-8s %s\n", "ROOM", "PLAYER", "PID", "OWNER",
           "SCORE", "NAME");
    for (int i = 0; i < n; i++)
    {
        char room[16];
        if (p[i].room < 0)
            snprintf(room, sizeof room, "-");
        else
            snprintf(room, sizeof room, "%d", p[i].room);
        printf("%-6s %-7d %-8d %-6s %-8d %s\n", room, p[i].player, p[i].pid,
               p[i].is_owner ? "yes" : "no", p[i].score, p[i].name);
    }
}

/* ---- lifecycle from the CLI ---------------------------------------------- */

/* CtlStepFn: the same sequence the TUI draws as a progress panel.
 * Flushed per step, so these interleave correctly with the unbuffered stderr
 * the failure path writes - and so a slow confirm shows progress as it goes. */
static void cli_step(int index, int ok, void *ctx)
{
    const char **names = ctx;
    printf("%-34s ... %s\n", names[index], ok ? "OK" : "FAILED");
    fflush(stdout);
}

static int cmd_start(const char *who, const char *sock)
{
    Daemon d;
    if (ctl_daemon_parse(who, &d) != 0)
    {
        fprintf(stderr, "tetrisctl: unknown daemon '%s'\n", who);
        log_send(LOG_WARN,
                 "operation=cmd_start phase=complete daemon=%s status=%d", who,
                 EXIT_USAGE);
        return EXIT_USAGE;
    }

    char err[512];
    int rc =
        ctl_start(d, sock, cli_step, ctl_start_step_names, err, sizeof err);

    /* Already up is success: `start` asks for a running daemon, and there is
     * one. Said out loud so a script's log does not imply it spawned it. */
    if (rc == CTL_START_ALREADY)
    {
        printf("%s\n", err);
        log_send(LOG_INFO,
                 "operation=cmd_start phase=complete daemon=%s status=%d", who,
                 EXIT_OK);
        return EXIT_OK;
    }
    if (rc != CTL_START_OK)
    {
        fprintf(stderr, "tetrisctl: %s\n", err);
        log_send(LOG_ERROR,
                 "operation=cmd_start phase=complete daemon=%s status=%d", who,
                 EXIT_NOCONN);
        return EXIT_NOCONN;
    }
    printf("%s is up\n", ctl_daemon_name(d));
    log_send(LOG_INFO, "operation=cmd_start phase=complete daemon=%s status=%d",
             who, EXIT_OK);
    return EXIT_OK;
}

static int cmd_stop(const char *who, const char *sock)
{
    Daemon d;
    if (ctl_daemon_parse(who, &d) != 0)
    {
        fprintf(stderr, "tetrisctl: unknown daemon '%s'\n", who);
        log_send(LOG_WARN,
                 "operation=cmd_stop phase=complete daemon=%s status=%d", who,
                 EXIT_USAGE);
        return EXIT_USAGE;
    }

    if (d == DAEMON_TETRISLOGD)
    {
        char err[512];
        if (ctl_stop_logd(cli_step, ctl_stop_logd_step_names, err,
                          sizeof err) != 0)
        {
            fprintf(stderr, "tetrisctl: %s\n", err);
            log_send(LOG_ERROR,
                     "operation=cmd_stop phase=complete daemon=%s status=%d",
                     who, EXIT_REFUSED);
            return EXIT_REFUSED;
        }
        printf("tetrislogd stopped\n");
        log_send(LOG_INFO,
                 "operation=cmd_stop phase=complete daemon=%s status=%d", who,
                 EXIT_OK);
        return EXIT_OK;
    }

    /* stop tetrisd and shutdown are one call site with two spellings. */
    int rc = ctl_request(sock, "SHUTDOWN", "/", CTL_TIMEOUT_CLI_MS, NULL, 0);
    if (rc != 200)
    {
        fprintf(stderr, "tetrisctl: %s\n", ctl_strerror(rc));
        int outcome = rc < 0 ? EXIT_NOCONN : EXIT_REFUSED;
        log_send(LOG_INFO,
                 "operation=cmd_stop phase=complete daemon=%s status=%d", who,
                 outcome);
        return outcome;
    }
    printf("tetrisd shutting down\n");
    log_send(LOG_INFO, "operation=cmd_stop phase=complete daemon=%s status=%d",
             who, EXIT_OK);
    return EXIT_OK;
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    bool as_json = false;
    char sock[PATH_MAX] = "";
    const char *args[8];
    int n_args = 0;

    /* ------- log open ----------------------------------------------- */
    (void)log_open_configured();
    atexit(log_close);
    log_send(LOG_DEBUG, "tetrisctl invoked argc=%d", argc);

    /* ------- check args ----------------------------------------------------*/
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--json") == 0)
        {
            as_json = true;
        }
        else if (strcmp(argv[i], "--socket") == 0)
        {
            if (++i >= argc)
            {
                usage();
                (void)log_send(
                    LOG_INFO,
                    "operation=main phase=complete program=tetrisctl status=%d",
                    EXIT_USAGE);
                return EXIT_USAGE;
            }
            snprintf(sock, sizeof sock, "%s", argv[i]);
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            usage();
            (void)log_send(
                LOG_INFO,
                "operation=main phase=complete program=tetrisctl status=%d",
                EXIT_OK);
            return EXIT_OK;
        }
        else if (n_args < (int)(sizeof args / sizeof args[0]))
        {
            args[n_args++] = argv[i];
        }
    }

    /* -------- resolve sock path ----------------------------------------- */

    if (sock[0] == '\0')
        ctl_socket_path(sock, sizeof sock);

    /* enter tui */
    if (n_args == 0)
    {
        if (as_json || !isatty(STDOUT_FILENO))
        {
            usage();
            (void)log_send(
                LOG_INFO,
                "operation=main phase=complete program=tetrisctl status=%d",
                EXIT_USAGE);
            return EXIT_USAGE;
        }
        int status = ctl_tui_run(sock);
        (void)log_send(
            LOG_INFO,
            "operation=main phase=complete program=tetrisctl status=%d",
            status);
        return status;
    }

    /*---- command handler --------------------------------------*/

    /*------ start-stop tetrisd/logd ---------------*/
    const char *cmd = args[0];
    log_send(LOG_DEBUG, "tetrisctl command=%s socket=%s json=%d", cmd, sock,
             as_json);

    if (strcmp(cmd, "start") == 0 || strcmp(cmd, "stop") == 0)
    {
        if (n_args != 2)
        {
            fprintf(stderr, "tetrisctl: %s needs <tetrisd|tetrislogd>\n", cmd);
            (void)log_send(
                LOG_INFO,
                "operation=main phase=complete program=tetrisctl status=%d",
                EXIT_USAGE);
            return EXIT_USAGE;
        }
        int status = strcmp(cmd, "start") == 0 ? cmd_start(args[1], sock)
                                               : cmd_stop(args[1], sock);
        (void)log_send(
            LOG_INFO,
            "operation=main phase=complete program=tetrisctl status=%d",
            status);
        return status;
    }

    /*----- other cli method ---------------*/

    const char *method;
    char path[HTTTP_MAX_PATH] = "/";

    if (strcmp(cmd, "status") == 0)
        method = "STATUS";
    else if (strcmp(cmd, "shutdown") == 0)
    {
        int status = cmd_stop("tetrisd", sock);
        (void)log_send(
            LOG_INFO,
            "operation=main phase=complete program=tetrisctl status=%d",
            status);
        return status; /* alias */
    }
    else if (strcmp(cmd, "rooms") == 0)
        method = "ROOMS";
    else if (strcmp(cmd, "players") == 0)
        method = "PLAYERS";
    else if (strcmp(cmd, "reload") == 0)
        method = "RELOAD";
    else if (strcmp(cmd, "kick") == 0)
    {
        if (n_args != 3)
        {
            fprintf(stderr, "tetrisctl: kick needs <room> <player>\n");
            (void)log_send(
                LOG_INFO,
                "operation=main phase=complete program=tetrisctl status=%d",
                EXIT_USAGE);
            return EXIT_USAGE;
        }
        method = "KICK";
        snprintf(path, sizeof path, "/room/%s/player/%s", args[1], args[2]);
    }
    else
    {
        fprintf(stderr, "tetrisctl: unknown command '%s'\n", cmd);
        usage();
        (void)log_send(
            LOG_INFO,
            "operation=main phase=complete program=tetrisctl status=%d",
            EXIT_USAGE);
        return EXIT_USAGE;
    }

    /*----- ctl_request -> connect to tetrisd -> request out -> recv response */

    static char body[CTL_BODY_MAX];
    int rc =
        ctl_request(sock, method, path, CTL_TIMEOUT_CLI_MS, body, sizeof body);

    if (rc < 0)
    {
        fprintf(stderr, "tetrisctl: %s (%s)\n", ctl_strerror(rc), sock);
        (void)log_send(
            LOG_INFO,
            "operation=main phase=complete program=tetrisctl status=%d",
            EXIT_NOCONN);
        return EXIT_NOCONN;
    }

    /*----- print error ---------------*/

    if (rc < 200 || rc >= 300)
    {
        /* The daemon's own words when it has any, its status line otherwise. */
        const char *p = strstr(body, "\"error\":\"");
        if (p != NULL)
        {
            char msg[256];
            p += strlen("\"error\":\"");
            size_t n = strcspn(p, "\"");
            if (n >= sizeof msg)
                n = sizeof msg - 1;
            memcpy(msg, p, n);
            msg[n] = '\0';
            fprintf(stderr, "tetrisctl: %d %s\n", rc, msg);
        }
        else
        {
            fprintf(stderr, "tetrisctl: %d %s\n", rc, ctl_strerror(rc));
        }
        (void)log_send(
            LOG_INFO,
            "operation=main phase=complete program=tetrisctl status=%d",
            EXIT_REFUSED);
        return EXIT_REFUSED;
    }

    /*----- print response ---------------*/

    if (as_json)
    {
        printf("%s\n", body);
        (void)log_send(
            LOG_INFO,
            "operation=main phase=complete program=tetrisctl status=%d",
            EXIT_OK);
        return EXIT_OK;
    }

    if (strcmp(cmd, "status") == 0)
    {
        CtlStatus s;
        if (ctl_decode_status(body, &s) != 0)
        {
            (void)log_send(
                LOG_INFO,
                "operation=main phase=complete program=tetrisctl status=%d",
                EXIT_REFUSED);
            return EXIT_REFUSED;
        }
        format_status(&s);
    }
    else if (strcmp(cmd, "rooms") == 0)
    {
        static CtlRoom rooms[MAX_ROOMS];
        int n = ctl_decode_rooms(body, rooms, MAX_ROOMS);
        if (n < 0)
        {
            (void)log_send(
                LOG_INFO,
                "operation=main phase=complete program=tetrisctl status=%d",
                EXIT_REFUSED);
            return EXIT_REFUSED;
        }
        format_rooms(rooms, n);
    }
    else if (strcmp(cmd, "players") == 0)
    {
        static CtlPlayer players[MAX_SESSIONS];
        int n = ctl_decode_players(body, players, MAX_SESSIONS);
        if (n < 0)
        {
            (void)log_send(
                LOG_INFO,
                "operation=main phase=complete program=tetrisctl status=%d",
                EXIT_REFUSED);
            return EXIT_REFUSED;
        }
        format_players(players, n);
    }
    else if (strcmp(cmd, "kick") == 0)
        printf("kicked room %s player %s\n", args[1], args[2]);
    else
        printf("%s\n", body);

    (void)log_send(LOG_INFO,
                   "operation=main phase=complete program=tetrisctl status=%d",
                   EXIT_OK);
    return EXIT_OK;
}
