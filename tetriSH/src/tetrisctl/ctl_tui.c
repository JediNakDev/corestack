/*
 * ctl_tui.c - dashboard above a fixed action list, refreshed once a second.
 *
 * Two hand-drawn windows rather than libtetrisui panes, because frame_win()
 * clears the whole screen before every window (tetrisui.c:73) - two of them
 * cannot coexist. The widgets are still used, as full-screen pages: a page
 * clears, runs modally, returns, and the dashboard repaints.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "libtetrisutil/logmsg.h"
#include "libtetrisutil/rc.h"
#include <limits.h>
#include <ncurses.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libhtttp/htttp.h"
#include "libtetrisui/tetrisui.h"
#include "tetrisctl/ctl_client.h"
#include "tetrisctl/ctl_lifecycle.h"
#include "tetrisctl/ctl_logtail.h"
#include "tetrisctl/ctl_tui.h"
#include "tetrisd/history.h"

#define MIN_ROWS 24
#define MIN_COLS 60

/* Width of the right column (recent rounds + log) */
#define RIGHT_COLS 60
#define MIN_COLS_WIDE (MIN_COLS + RIGHT_COLS)

#define TICK_MS 1000
#define BACKOFF_MAX_MS 5000

#define DB_QUERY_EVERY_TICKS 5
#define CTL_RECENT_ROUNDS 5

#define DASH_LINES 64
#define LINE_LEN 200

/* ---- actions ------------------------------------------------------------- */

typedef enum
{
    ACT_START_ALL = 0,
    ACT_STOP_ALL,
    ACT_START_D,
    ACT_STOP_D,
    ACT_START_L,
    ACT_STOP_L,
    ACT_START_DB,
    ACT_STOP_DB,
    ACT_KICK,
    ACT_RELOAD,
    ACT_QUIT,
    ACT_COUNT,
} Action;

static const char *const action_label[ACT_COUNT] = {
    "Start all",        "Stop all",        "Start tetrisd", "Stop tetrisd",
    "Start tetrislogd", "Stop tetrislogd", "Start DB",      "Stop DB",
    "Kick a player",    "Reload config",   "Quit",
};

/*----- Everything the screen knows between ticks. --------------------*/

typedef struct
{
    const char *sock;
    CtlSnapshot snap;
    bool tetrisd_up;
    bool logd_up;
    bool db_up;
    pid_t tetrisd_pid;
    pid_t logd_pid;
    int last_err;   /* CTL_ERR_* from the most recent refresh, or 0 */
    int backoff_ms; /* 0 while healthy */

    /* right column: recent rounds, and the BEST column's lookup table */
    history_row_t recent[CTL_RECENT_ROUNDS];
    int n_recent;
    bool recent_have;  /* ever successfully populated */
    bool recent_stale; /* the most recent DB refresh failed, or was skipped */
    history_row_t best[HISTORY_BEST_SCORES_MAX];
    int n_best;
    bool best_stale; /* same as recent_stale but for best score */

    /* --- right column: the live log tail. */
    logtail_t *log;
} console_t;

/*
 * Why an action is unavailable, or NULL if it is available.
 *
 * Labels and positions never change; only this does. A label that mutated
 * under a 1 s refresh would let Enter do the opposite of what was read.
 */
static const char *disabled_reason(const console_t *c, Action a)
{
    switch (a)
    {
    case ACT_START_D:
        return c->tetrisd_up ? "already up" : NULL;
    case ACT_STOP_D:
        return c->tetrisd_up ? NULL : "not running";
    case ACT_START_L:
        return c->logd_up ? "already up" : NULL;
    case ACT_STOP_L:
        return c->logd_up ? NULL : "not running";
    case ACT_START_DB:
        return c->db_up ? "already up" : NULL;
    case ACT_STOP_DB:
        return c->db_up ? NULL : "not running";
    case ACT_START_ALL:
        return c->tetrisd_up && c->logd_up && c->db_up ? "all already up"
                                                       : NULL;
    case ACT_STOP_ALL:
        return c->tetrisd_up || c->logd_up || c->db_up ? NULL
                                                       : "nothing running";
    case ACT_KICK:
        return c->tetrisd_up ? NULL : "tetrisd is down";
    case ACT_RELOAD:
        return "not implemented (501)";
    default:
        return NULL;
    }
}

/* ---- dashboard text ------------------------------------------------------ */

typedef struct
{
    char text[DASH_LINES][LINE_LEN];
    int n;
} Lines;

static void emit(Lines *l, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void emit(Lines *l, const char *fmt, ...)
{
    if (l->n >= DASH_LINES)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l->text[l->n], LINE_LEN, fmt, ap);
    va_end(ap);
    l->n++;
}

static bool get_individual_best_score(const console_t *c, const char *name,
                                      int *out)
{
    for (int i = 0; i < c->n_best; i++)
    {
        if (strcmp(c->best[i].user_name, name) == 0)
        {
            *out = c->best[i].score;
            return true;
        }
    }
    return false;
}

static void build_dashboard(const console_t *c, Lines *l)
{
    char up[32];
    l->n = 0;

    /*----- dashboard - print error ---------------*/

    if (c->last_err != 0)
    {
        emit(l, "! %s", ctl_strerror(c->last_err));
        emit(l, "  %s - retrying in %ds", c->sock, c->backoff_ms / 1000);
        emit(l, " ");
    }

    /*----- dashboard -- print process status ---------------*/

    emit(l, "%-12s %-6s %-7s %s", "PROCESS", "STATE", "PID", "UPTIME");

    ctl_fmt_uptime(up, sizeof up, c->snap.status.uptime);
    emit(l, "%-12s %-6s %-7ld %s", "tetrisd", c->tetrisd_up ? "up" : "down",
         (long)c->tetrisd_pid, c->tetrisd_up && c->snap.have ? up : "-");
    emit(l, "%-12s %-6s %-7ld %s", "tetrislogd", c->logd_up ? "up" : "down",
         (long)c->logd_pid, "-");
    emit(l, "%-12s %-6s %-7s %s", "tetrisdb", c->db_up ? "up" : "down", "-",
         "-");
    emit(l, " ");

    if (!c->snap.have)
    {
        emit(l, "no data from tetrisd yet");
        return;
    }

    emit(l, "sessions %d   rooms %d%s", c->snap.status.sessions,
         c->snap.status.rooms, c->snap.stale ? "   (stale)" : "");
    emit(l, " ");

    /*----- dashboard -- print rooms ---------------*/

    emit(l, "%-5s %-10s %-8s %s", "ID", "PHASE", "MEMBERS", "OWNER");
    if (c->snap.n_rooms == 0)
        emit(l, "  no rooms");
    for (int i = 0; i < c->snap.n_rooms; i++)
    {
        const CtlRoom *r = &c->snap.rooms[i];
        char owner[16];
        if (r->owner < 0)
            snprintf(owner, sizeof owner, "-");
        else
            snprintf(owner, sizeof owner, "p%d", r->owner);
        emit(l, "%-5d %-10s %-8d %s", r->id, r->phase, r->members, owner);
    }
    emit(l, " ");

    /*----- dashboard -- print player ---------------*/

    emit(l, "%-6s %-7s %-8s %-6s %-8s %-6s %s", "ROOM", "PLAYER", "PID",
         "OWNER", "SCORE", "BEST", "NAME");
    if (c->snap.n_players == 0)
        emit(l, "  no players connected");
    for (int i = 0; i < c->snap.n_players; i++)
    {
        const CtlPlayer *p = &c->snap.players[i];
        char room[16];
        if (p->room < 0)
            snprintf(room, sizeof room, "-");
        else
            snprintf(room, sizeof room, "%d", p->room);

        char best[16];
        int best_score;
        if (get_individual_best_score(c, p->name, &best_score))
            snprintf(best, sizeof best, "%d", best_score);
        else
            snprintf(best, sizeof best, "-");

        emit(l, "%-6s %-7d %-8d %-6s %-8d %-6s %s", room, p->player, p->pid,
             p->is_owner ? "yes" : "no", p->score, best, p->name);
    }
    if (c->best_stale)
        emit(l, "  BEST figures stale - database unreachable");
}

/* ---- drawing ------------------------------------------------------------- */

static WINDOW *w_dash;
static WINDOW *w_act;
static WINDOW *w_right; /* recent rounds + log; NULL below MIN_COLS_WIDE */

static void windows_free(void)
{
    if (w_dash)
        delwin(w_dash);
    if (w_act)
        delwin(w_act);
    if (w_right)
        delwin(w_right);
    w_dash = w_act = w_right = NULL;
}

/* Returns 0, or -1 if the terminal is currently too small to draw in. */
/*----- check if window is wide enough to draw log (w_right) -----*/

static int windows_build(void)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (rows < MIN_ROWS || cols < MIN_COLS)
        return -1;

    windows_free();
    int act_h = ACT_COUNT + 3;     /* entries, two borders, one pad */
    int dash_h = rows - 1 - act_h; /* last row belongs to the status bar */

    bool wide = cols >= MIN_COLS_WIDE;
    int right_w = wide ? RIGHT_COLS : 0;
    int left_w = cols - right_w;

    w_dash = newwin(dash_h, left_w, 0, 0);
    w_act = newwin(act_h, cols, dash_h, 0);
    w_right = wide ? newwin(dash_h, right_w, 0, left_w) : NULL;
    if (!w_dash || !w_act || (wide && !w_right))
    {
        windows_free();
        return -1;
    }
    keypad(w_act, TRUE);
    wtimeout(w_act, TICK_MS);
    return 0;
}

/*----- log -- print recent ---------------*/

static void build_recent(const console_t *c, Lines *l)
{
    l->n = 0;

    emit(l, "%-6s %-5s %-5s %s", "SCORE", "LINES", "SECS", "PLAYER");
    if (!c->recent_have)
    {
        emit(l, "  no data from the database yet");
        return;
    }
    if (c->recent_stale)
        emit(l, "  (stale - database unreachable)");
    if (c->n_recent == 0)
        emit(l, "  no rounds finished yet");
    for (int i = 0; i < c->n_recent; i++)
    {
        const history_row_t *r = &c->recent[i];
        long survived = history_survived_secs(r->ts_start, r->ts_end);
        emit(l, "%-6d %-5d %-5ld %s", r->score, r->lines, survived,
             r->user_name[0] ? r->user_name : "(guest)");
    }
}

/*----- window right -- recent/log page ---------------*/

static void draw_right(const console_t *c)
{
    if (!w_right)
        return;

    werase(w_right);
    box(w_right, 0, 0);
    mvwprintw(w_right, 0, 2, " recent rounds / log ");

    int dh, dw;
    getmaxyx(w_right, dh, dw);

    Lines recent;
    build_recent(c, &recent);

    int row = 1;
    for (int i = 0; i < recent.n && row < dh - 1; i++)
        mvwprintw(w_right, row++, 2, "%.*s", dw - 4, recent.text[i]);
    row++; /* blank separator */

    if (row < dh - 1)
    {
        mvwprintw(w_right, row++, 2, "log:%s",
                  is_logtail_missing(c->log) ? "  (file not found)" : "");

        int log_room = dh - 1 - row;
        if (log_room > 0)
        {
            const char *log_lines[LOGTAIL_RING_LINES];
            int cap =
                log_room < LOGTAIL_RING_LINES ? log_room : LOGTAIL_RING_LINES;
            int n = logtail_lines(c->log, log_lines, cap);
            for (int i = 0; i < n; i++)
                mvwprintw(w_right, row++, 2, "%.*s", dw - 4, log_lines[i]);
        }
    }
}

/*----- draw the whole ctl -------------------------*/

static void draw(const console_t *c, int sel)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    (void)rows;

    Lines l;
    build_dashboard(c, &l);

    werase(w_dash);
    box(w_dash, 0, 0);
    mvwprintw(w_dash, 0, 2, " tetrisctl - %s ",
              c->last_err ? "UNREACHABLE" : (c->snap.stale ? "stale" : "live"));

    int dh, dw;
    getmaxyx(w_dash, dh, dw);
    int room_for = dh - 2;
    int shown = l.n < room_for ? l.n : room_for;
    for (int i = 0; i < shown; i++)
        mvwprintw(w_dash, i + 1, 2, "%.*s", dw - 4, l.text[i]);
    /* Truncation is stated rather than silent: a list that just stops looks
     * like the server has fewer players than it does.
     *
     * Padded to the full width because it overwrites a row that was already
     * drawn and is shorter than it - otherwise the tail of that row survives
     * and reads as part of this message. */
    if (l.n > room_for && room_for > 0)
    {
        char note[LINE_LEN];
        snprintf(note, sizeof note, "... %d more lines (enlarge terminal)",
                 l.n - room_for + 1);
        mvwprintw(w_dash, dh - 2, 2, "%-*.*s", dw - 4, dw - 4, note);
    }

    draw_right(c);

    werase(w_act);
    box(w_act, 0, 0);
    mvwprintw(w_act, 0, 2, " actions ");
    for (int i = 0; i < ACT_COUNT; i++)
    {
        const char *why = disabled_reason(c, (Action)i);
        char row[LINE_LEN];
        if (why)
            snprintf(row, sizeof row, "%-30s (%s)", action_label[i], why);
        else
            snprintf(row, sizeof row, "%s", action_label[i]);

        int aw = getmaxx(w_act);
        if (why)
            wattron(w_act, A_DIM);
        if (i == sel)
            wattron(w_act, A_REVERSE);
        mvwprintw(w_act, i + 1, 2, " %-*.*s", aw - 6, aw - 6, row);
        if (i == sel)
            wattroff(w_act, A_REVERSE);
        if (why)
            wattroff(w_act, A_DIM);
    }

    char hint[128];
    if (c->snap.have)
    {
        long age = (long)(time(NULL) - c->snap.taken);
        snprintf(hint, sizeof hint,
                 "Up/Down move  Enter select  q quit    "
                 "refreshed %lds ago",
                 age);
    }
    else
    {
        snprintf(hint, sizeof hint, "Up/Down move  Enter select  q quit");
    }
    tetrisui_set_status("tetrisctl", c->sock,
                        c->last_err ? "unreachable" : "live");
    tetrisui_draw_status_bar(hint);

    wnoutrefresh(stdscr);
    wnoutrefresh(w_dash);
    if (w_right)
        wnoutrefresh(w_right);
    wnoutrefresh(w_act);
    doupdate();
}

/* A libtetrisui page has just cleared and repainted the screen; take it back.
 */
static void after_page(void)
{
    clear();
    refresh();
    if (w_dash)
        redrawwin(w_dash);
    if (w_right)
        redrawwin(w_right);
    if (w_act)
        redrawwin(w_act);
}

static void say(const char *title, const char *line)
{
    const char *lines[] = {line};
    tetrisui_message(title, lines, 1);
    after_page();
}

/* ---- actions ------------------------------------------------------------- */

/* CtlStepFn: the same sequence the CLI prints, drawn as a progress panel. */
static void tui_step(int index, int ok, void *ctx)
{
    (void)ctx;
    tetrisui_progress_step(index, ok);
}

static int do_start(console_t *c, Daemon d)
{
    char err[512];
    tetrisui_progress_begin("Starting", ctl_start_step_names, CTL_START_STEPS);
    int rc = ctl_start(d, c->sock, tui_step, NULL, err, sizeof err);
    tetrisui_progress_end();
    after_page();
    /* ALREADY is only reachable if the daemon came up between the last
     * refresh dimming this entry and the keypress. Not an error, worth
     * saying. */
    if (rc == CTL_START_ALREADY)
        say("Already running", err);
    else if (rc != CTL_START_OK)
        say("Start failed", err[0] ? err : "unknown error");
    return rc == CTL_START_OK || rc == CTL_START_ALREADY ? 0 : -1;
}

static void do_stop_tetrisd(console_t *c)
{
    if (!tetrisui_confirm("Confirm", "Shut down tetrisd?"))
    {
        after_page();
        return;
    }
    after_page();

    int rc = ctl_request(c->sock, "SHUTDOWN", "/", CTL_TIMEOUT_CLI_MS, NULL, 0);
    if (rc != 200)
        say("Shutdown failed", ctl_strerror(rc));
    /* No exit on success: the dashboard stays up, goes UNREACHABLE, and
     * reconnects by itself if tetrisd is started again. */
}

static int stop_logd_now(void)
{
    char err[512];
    tetrisui_progress_begin("Stopping tetrislogd", ctl_stop_logd_step_names,
                            CTL_STOP_LOGD_STEPS);
    int rc = ctl_stop_logd(tui_step, NULL, err, sizeof err);
    tetrisui_progress_end();
    after_page();
    if (rc != 0)
        say("Stop failed", err[0] ? err : "unknown error");
    return rc;
}

static void do_stop_logd(console_t *c)
{
    (void)c;
    if (!tetrisui_confirm("Confirm", "Stop tetrislogd?"))
    {
        after_page();
        return;
    }
    after_page();
    (void)stop_logd_now();
}

static int do_db(const char *command)
{
    char err[512];
    int rc = ctl_db_command(command, err, sizeof err);
    after_page();
    if (rc != 0)
        say("Database command failed", err[0] ? err : "unknown error");
    return rc;
}

static void do_start_all(console_t *c)
{
    if (!c->logd_up)
    {
        if (do_start(c, DAEMON_TETRISLOGD) != 0)
            return;
    }
    if (!ctl_probe_db() && do_db("start") != 0)
        return;
    if (!ctl_probe(DAEMON_TETRISD, c->sock, NULL))
        (void)do_start(c, DAEMON_TETRISD);
}

static void do_stop_all(console_t *c)
{
    if (!tetrisui_confirm("Confirm", "Stop tetrisd, DB, and tetrislogd?"))
    {
        after_page();
        return;
    }
    after_page();

    if (ctl_probe(DAEMON_TETRISD, c->sock, NULL))
        (void)ctl_request(c->sock, "SHUTDOWN", "/", CTL_TIMEOUT_CLI_MS, NULL,
                          0);
    if (ctl_probe_db())
        (void)do_db("stop");
    if (ctl_probe(DAEMON_TETRISLOGD, NULL, NULL))
        (void)stop_logd_now();
}

static void do_kick(console_t *c)
{
    const char *labels[] = {"Room", "Player"};
    char values[2][TETRISUI_FIELD_LEN] = {"", ""};

    /* Blank, never prefilled: prefill plus habit turns Enter-Enter into
     * kicking whoever happens to be first. */
    if (tetrisui_form("Kick a player", labels, values, 2) != 0)
    {
        after_page();
        return;
    }
    after_page();

    char *end;
    long room = strtol(values[0], &end, 10);
    bool room_ok = values[0][0] != '\0' && *end == '\0';
    long player = strtol(values[1], &end, 10);
    bool player_ok = values[1][0] != '\0' && *end == '\0';

    /* The wire fields are single bytes; reject here rather than let 256
     * silently become 0 and kick the wrong player. */
    if (!room_ok || !player_ok || room < 0 || room > 255 || player < 0 ||
        player > 255)
    {
        say("Invalid target", "Room and player must both be 0-255.");
        return;
    }

    char q[128];
    snprintf(q, sizeof q, "Disconnect player %ld from room %ld?", player, room);
    if (!tetrisui_confirm("Confirm", q))
    {
        after_page();
        return;
    }
    after_page();

    char path[HTTTP_MAX_PATH];
    snprintf(path, sizeof path, "/room/%ld/player/%ld", room, player);
    int rc = ctl_request(c->sock, "KICK", path, CTL_TIMEOUT_CLI_MS, NULL, 0);
    if (rc != 200)
        say("Kick failed", ctl_strerror(rc));
}

/* ---- loop ---------------------------------------------------------------- */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void refresh_db(console_t *c)
{
    if (!c->db_up)
    {
        if (c->recent_have)
            c->recent_stale = true;
        c->best_stale = true;
        return;
    }

    int n = history_db_read_recent(c->recent, CTL_RECENT_ROUNDS);
    if (n >= 0)
    {
        c->n_recent = n;
        c->recent_have = true;
        c->recent_stale = false;
    }
    else if (c->recent_have)
    {
        c->recent_stale = true;
    }

    int nb = history_db_read_best_scores(c->best, HISTORY_BEST_SCORES_MAX);
    if (nb >= 0)
    {
        c->n_best = nb;
        c->best_stale = false;
    }
    else
    {
        c->best_stale = true;
    }
}

static void refresh_all(console_t *c)
{
    /* Counts calls across this process's whole lifetime, unaffected by
     * backoff or a forced post-action refresh - a plain gate, not a
     * scheduler. */
    static int db_tick;

    int rc = ctl_refresh(c->sock, CTL_TIMEOUT_TUI_MS, &c->snap);
    c->last_err = rc == 0 ? 0 : rc;

    if (rc == 0)
        c->backoff_ms = 0;
    else if (c->backoff_ms == 0)
        c->backoff_ms = 1000;
    else if (c->backoff_ms < BACKOFF_MAX_MS)
        c->backoff_ms = c->backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS
                                                           : c->backoff_ms * 2;

    c->tetrisd_pid = 0;
    c->logd_pid = 0;
    c->tetrisd_up = rc == 0;
    if (c->tetrisd_up)
        ctl_probe(DAEMON_TETRISD, c->sock, &c->tetrisd_pid);
    c->logd_up = ctl_probe(DAEMON_TETRISLOGD, c->sock, &c->logd_pid);
    c->db_up = ctl_probe_db();

    if (db_tick % DB_QUERY_EVERY_TICKS == 0)
        refresh_db(c);
    db_tick++;

    (void)log_send(LOG_DEBUG,
                   "operation=refresh_all phase=complete status=%d "
                   "tetrisd_up=%d logd_up=%d db_up=%d backoff_ms=%d",
                   rc, c->tetrisd_up, c->logd_up, c->db_up, c->backoff_ms);
}

/*
 * Move the cursor, skipping entries Enter could not act on anyway.
 *
 * delta 0 means "stay unless the refresh just disabled us", which happens
 * whenever a daemon changes state under the cursor. Quit is never disabled, so
 * the search always terminates somewhere sensible.
 */
static int move_sel(const console_t *c, int sel, int delta)
{
    if (delta == 0)
    {
        if (disabled_reason(c, (Action)sel) == NULL)
            return sel;
        delta = 1;
    }
    for (int i = 0; i < ACT_COUNT; i++)
    {
        sel = (sel + delta + ACT_COUNT) % ACT_COUNT;
        if (disabled_reason(c, (Action)sel) == NULL)
            break;
    }
    return sel;
}

/*----- main() for the ctl tui --------------------*/

int ctl_tui_run(const char *sock)
{
    log_send(LOG_DEBUG, "opening admin console socket=%s", sock);
    console_t c;
    memset(&c, 0, sizeof c);
    c.sock = sock;

    c.log = logtail_init();
    if (c.log == NULL)
    {
        fprintf(stderr, "tetrisctl: out of memory opening the log pane\n");
        (void)log_send(LOG_ERROR,
                       "operation=ctl_tui_run phase=complete status=-1 "
                       "reason=logtail_oom");
        return 2;
    }

    tetrisui_init();
    if (windows_build() != 0)
    {
        logtail_close(c.log);
        tetrisui_shutdown();
        fprintf(stderr,
                "tetrisctl: terminal is too small for the console "
                "(need at least %dx%d)\n",
                MIN_COLS, MIN_ROWS);
        (void)log_send(LOG_INFO,
                       "operation=ctl_tui_run phase=complete status=2");
        return 2;
    }

    refresh_all(&c);
    int sel = move_sel(&c, ACT_COUNT - 1, 1);
    long next = now_ms() + (c.backoff_ms ? c.backoff_ms : TICK_MS);

    for (;;)
    {
        logtail_poll(c.log);
        draw(&c, sel);

        int ch = wgetch(w_act);

        if (ch == KEY_RESIZE)
        {
            /* Too small mid-session is reported in place, not exited on -
             * the terminal may well grow again. */
            if (windows_build() != 0)
            {
                clear();
                mvprintw(0, 0, "terminal too small (need %dx%d)", MIN_COLS,
                         MIN_ROWS);
                refresh();
                napms(200);
                continue;
            }
            after_page();
        }
        else if (ch == KEY_UP)
        {
            sel = move_sel(&c, sel, -1);
        }
        else if (ch == KEY_DOWN)
        {
            sel = move_sel(&c, sel, +1);
        }
        else if (ch == 'q' || ch == 'Q')
        {
            break;
        }
        else if (ch == '\n' || ch == KEY_ENTER || ch == '\r')
        {
            if (disabled_reason(&c, (Action)sel) != NULL)
                continue;

            switch ((Action)sel)
            {
            case ACT_START_D:
                (void)do_start(&c, DAEMON_TETRISD);
                break;
            case ACT_START_L:
                (void)do_start(&c, DAEMON_TETRISLOGD);
                break;
            case ACT_STOP_D:
                do_stop_tetrisd(&c);
                break;
            case ACT_STOP_L:
                do_stop_logd(&c);
                break;
            case ACT_START_DB:
                (void)do_db("start");
                break;
            case ACT_STOP_DB:
                if (tetrisui_confirm("Confirm", "Stop the account DB?"))
                {
                    after_page();
                    (void)do_db("stop");
                }
                else
                    after_page();
                break;
            case ACT_START_ALL:
                do_start_all(&c);
                break;
            case ACT_STOP_ALL:
                do_stop_all(&c);
                break;
            case ACT_KICK:
                do_kick(&c);
                break;
            case ACT_QUIT:
                goto done;
            default:
                break;
            }

            /* Forced refresh, so the dashboard the user comes back to already
             * reflects what they just did. */
            refresh_all(&c);
            sel = move_sel(&c, sel, 0);
            next = now_ms() + (c.backoff_ms ? c.backoff_ms : TICK_MS);
            continue;
        }

        if (now_ms() >= next)
        {
            refresh_all(&c);
            sel = move_sel(&c, sel, 0);
            next = now_ms() + (c.backoff_ms ? c.backoff_ms : TICK_MS);
        }
    }

done:
    windows_free();
    logtail_close(c.log);
    tetrisui_shutdown();
    (void)log_send(LOG_INFO, "operation=ctl_tui_run phase=complete status=0");
    return 0;
}
