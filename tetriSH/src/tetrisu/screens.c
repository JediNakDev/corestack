/**
 * @file screens.c
 * @brief Lobby UI: connect, menu, join, wait for start.
 *
 * Shape follows BallotBox's src/ballotu/screens.c (tetrisui_input ->
 * tetrisui_progress -> tetrisui_message, menus built from an array of
 * snprintf'd labels). The difference is that these screens talk to a Client
 * rather than to the transport directly, so the wire format stays in net.c.
 */

#include <errno.h>
#include <ncurses.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libtetrisui/tetrisui.h"
#include "tetrisd/historyview.h"
#include "tetrisd/adminmsg.h" /* REJECT_* status codes, shared with the server */
#include "tetrisu/screens.h"

/* Redraw cadence of the wait screen. Not a deadline - poll() also returns as
 * soon as a key or a frame arrives; this only bounds how long the spinner can
 * sit still when nothing is happening. */
#define WAIT_POLL_MS 250

#define HISTORY_POLL_MS 250 /* matches AUTH_POLL_MS's reasoning */
#define HISTORY_LINES_MAX (HISTORY_VIEW_ROUNDS + 4)
#define HISTORY_LINE_LEN 96

/* --- helpers ------------------------------------------------------------- */

static void status(const Client *c, const char *host)
{
    tetrisui_set_status("tetrisu", host, client_phase_str(c->phase));
}

/* One-line message box. */
static void say(const char *title, const char *line)
{
    const char *lines[] = {line};
    tetrisui_message(title, lines, 1);
}

/* --- connect ------------------------------------------------------------- */

ScreenResult screen_connect(Client *c, const char *host, int port,
                            const char *ca_path)
{
    char host_buf[64];
    char port_buf[16];
    snprintf(host_buf, sizeof host_buf, "%s", host);
    snprintf(port_buf, sizeof port_buf, "%d", port);

    for (;;)
    {
        const char *labels[] = {"Server IP", "Port"};
        char values[2][TETRISUI_FIELD_LEN];
        snprintf(values[0], TETRISUI_FIELD_LEN, "%s", host_buf);
        snprintf(values[1], TETRISUI_FIELD_LEN, "%s", port_buf);

        if (tetrisui_form("tetrisu - connect", labels, values, 2) != 0)
            return SCR_QUIT;

        snprintf(host_buf, sizeof host_buf, "%s", values[0]);
        snprintf(port_buf, sizeof port_buf, "%s", values[1]);
        int p = atoi(port_buf);
        if (p <= 0 || p > 65535)
        {
            say("Connect failed", "Port must be between 1 and 65535.");
            continue;
        }

        /* Real progress, not the scripted animation: the second step genuinely
         * fails on a forged or expired certificate and the player needs to see
         * WHICH step broke. */
        const char *steps[] = {"Opening TCP connection",
                               "tetrissh handshake (verifying server cert)"};
        tetrisui_progress_begin("Connecting", steps, 2);

        int rc = client_connect(c, host_buf, p, ca_path);

        /* Only a genuine socket failure marks step 0 bad. SESSION_ERR_IO is
         * NOT that - it also means "connected fine, then the peer hung up
         * mid-handshake", which would otherwise be reported as an unreachable
         * server the user had in fact just reached. */
        if (rc == CLIENT_ERR_CONNECT)
        {
            tetrisui_progress_step(0, 0);
            tetrisui_progress_end();
            say("Connect failed", "No route to server, or connection refused.");
            continue;
        }
        tetrisui_progress_step(0, 1); /* TCP is up */

        if (rc != 0)
        {
            tetrisui_progress_step(1, 0);
            tetrisui_progress_end();
            if (rc == SESSION_ERR_AUTH)
                say("Handshake rejected",
                    "Server certificate failed verification against the CA.");
            else if (rc == SESSION_ERR_IO)
                say("Handshake failed",
                    "Server closed the connection during the handshake.");
            else
                say("Handshake failed",
                    "Crypto or protocol error during the handshake.");
            continue;
        }
        tetrisui_progress_step(1, 1);
        tetrisui_progress_end();

        status(c, host_buf);
        return SCR_OK;
    }
}

/* --- main menu ----------------------------------------------------------- */

ScreenResult screen_main_menu(Client *c)
{
    for (;;)
    {
        const char *items[4];
        int n = 0;
        items[n++] = "Create a new room  (JOIN 0)";
        items[n++] = "Join a room by id";
        int history_idx = n;
        items[n++] = "See my history";
        int quit_idx = n;
        items[n++] = "Quit";

        int sel = tetrisui_menu("tetrisu - lobby", items, n,
                                "Up/Down move  Enter select  q quit");
        if (sel < 0 || sel == quit_idx)
            return SCR_QUIT;
        if (sel == 0)
        {
            if (client_join(c, 0) != 0)
            {
                say("Join failed", "Could not send JOIN to the server.");
                return SCR_DISCONNECTED;
            }
            return SCR_OK;
        }
        if (sel == 1)
            return screen_join_room(c) == SCR_OK ? SCR_OK : SCR_BACK;
        if (sel == history_idx)
        {
            if (screen_history(c) == SCR_DISCONNECTED)
                return SCR_DISCONNECTED;
            continue; /* redraw the lobby menu */
        }
    }
}

static ClientEvent wait_history_reply(Client *c)
{
    const char *steps[] = {"Waiting for your history"};
    tetrisui_progress_begin("Checking", steps, 1);

    for (;;)
    {
        struct pollfd pfd;
        pfd.fd = client_fd(c);
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, HISTORY_POLL_MS);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            tetrisui_progress_step(0, 0);
            tetrisui_progress_end();
            return CLI_EV_DISCONNECT;
        }

        if (pfd.revents & (POLLIN | POLLHUP | POLLERR))
        {
            ClientEvent ev = client_service(c);
            if (ev == CLI_EV_HISTORY || ev == CLI_EV_REJECT ||
                ev == CLI_EV_DISCONNECT)
            {
                tetrisui_progress_step(0, ev == CLI_EV_HISTORY);
                tetrisui_progress_end();
                return ev;
            }
            /* CLI_EV_NONE and the room events are not possible pre-JOIN,
             * but nothing here assumes that - just keep waiting. */
        }
    }
}

/* Renders h into buf/lines, and sets *n to how many of them are used. buf
 * backs lines - tetrisui_list_view takes pointers, and nothing else here
 * outlives this call, so the two must be sized and passed together. */
static void build_history_lines(const player_history_t *h,
                                char buf[][HISTORY_LINE_LEN],
                                const char *lines[], int *n)
{
    int i = 0;

    switch (h->status)
    {
    case HISTORY_VIEW_GUEST:
        snprintf(buf[i], HISTORY_LINE_LEN, "History is kept per account.");
        lines[i] = buf[i];
        i++;
        snprintf(buf[i], HISTORY_LINE_LEN, "Log in or register to start one.");
        lines[i] = buf[i];
        i++;
        break;

    case HISTORY_VIEW_UNAVAILABLE:
        snprintf(buf[i], HISTORY_LINE_LEN,
                 "Could not reach the history service.");
        lines[i] = buf[i];
        i++;
        snprintf(buf[i], HISTORY_LINE_LEN, "Try again in a moment.");
        lines[i] = buf[i];
        i++;
        break;

    case HISTORY_VIEW_EMPTY:
        snprintf(buf[i], HISTORY_LINE_LEN,
                 "You have not finished a round yet.");
        lines[i] = buf[i];
        i++;
        break;

    case HISTORY_VIEW_OK:
        snprintf(buf[i], HISTORY_LINE_LEN,
                 "Best score %-6d  Best lines %-4d  Games played %d",
                 (int)h->best_score, (int)h->best_lines, (int)h->games_played);
        lines[i] = buf[i];
        i++;
        snprintf(buf[i], HISTORY_LINE_LEN, " ");
        lines[i] = buf[i];
        i++;
        snprintf(buf[i], HISTORY_LINE_LEN, "Recent games (newest first):");
        lines[i] = buf[i];
        i++;
        for (int r = 0; r < h->recent_count && i < HISTORY_LINES_MAX; r++)
        {
            const history_round_t *rd = &h->recent[r];
            long survived = history_survived_secs(rd->ts_start, rd->ts_end);
            snprintf(buf[i], HISTORY_LINE_LEN,
                     "  %d. score %-6d  lines %-4d  survived %lds", r + 1,
                     (int)rd->score, (int)rd->lines, survived);
            lines[i] = buf[i];
            i++;
        }
        break;
    }

    *n = i;
}

ScreenResult screen_history(Client *c)
{
    if (client_history(c) != 0)
    {
        say("History unavailable", "Could not send the request.");
        return SCR_OK;
    }

    ClientEvent ev = wait_history_reply(c);
    if (ev == CLI_EV_DISCONNECT)
        return SCR_DISCONNECTED;
    if (ev != CLI_EV_HISTORY)
    {
        say("History unavailable", "Unexpected server state. Try again.");
        return SCR_OK;
    }

    char buf[HISTORY_LINES_MAX][HISTORY_LINE_LEN];
    const char *lines[HISTORY_LINES_MAX];
    int n = 0;
    build_history_lines(&c->history, buf, lines, &n);

    tetrisui_list_view("tetrisu - my history", lines, n);
    return SCR_OK;
}

/* --- join by id ---------------------------------------------------------- */

ScreenResult screen_join_room(Client *c)
{
    for (;;)
    {
        char buf[16] = "";
        if (tetrisui_input("Join room", "Room id (1-255, 0 = new room):", buf,
                           sizeof buf) != 0)
            return SCR_BACK;
        if (buf[0] == '\0')
            continue;

        int id = atoi(buf);
        /* The wire field is a single byte (server: body_byte). Reject locally
         * rather than silently truncating 256 to 0, which would create a new
         * room instead of joining room 256 and look like the server lied. */
        if (id < 0 || id > 255)
        {
            say("Invalid room", "Room id must be between 0 and 255.");
            continue;
        }

        if (client_join(c, (uint8_t)id) != 0)
        {
            say("Join failed", "Could not send JOIN to the server.");
            return SCR_DISCONNECTED;
        }
        return SCR_OK;
    }
}

/* --- wait for start (the one poll-aware screen) -------------------------- */

/* Draw the waiting panel. Called on every poll wake-up, so it must be cheap
 * and must not allocate a window per frame. */
static void draw_wait(const Client *c, int spin)
{
    static const char frames[] = "|/-\\";

    erase();

    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    (void)rows;

    char line[128];

    /*
     * Prefer the id the server assigned, exactly like the player line below.
     *
     * room_req is only what we ASKED for, and for "create a new room" that is
     * 0 - a request, not an id. Printing it left the room's creator staring at
     * "Room: (new)" with no way to tell anyone else which room to join, even
     * though UPD_SESSION had already reported the real number.
     */
    if (c->have_session && c->session.room_id != ROOM_NONE)
        snprintf(line, sizeof line, "Room: %d", c->session.room_id);
    else if (c->room_req == 0)
        snprintf(line, sizeof line, "Room: (waiting for the server)");
    else
        snprintf(line, sizeof line, "Room: %d  (requested)", c->room_req);
    mvprintw(2, 4, "%.*s", cols - 8, line);

    /*
     * Prefer what the server said. have_session goes true on the first
     * UPD_SESSION, which now arrives right after JOIN; assume_owner only
     * covers the brief window before it, and is ignored afterwards.
     */
    if (c->have_session)
    {
        snprintf(line, sizeof line, "Player %d%s   (from server)",
                 c->session.player_id, c->session.is_owner ? "  [owner]" : "");
    }
    else if (c->assume_owner)
    {
        snprintf(line, sizeof line,
                 "You created this room, so you can start it.");
    }
    else
    {
        snprintf(line, sizeof line, "Waiting for the room owner to start.");
    }
    mvprintw(3, 4, "%.*s", cols - 8, line);

    snprintf(line, sizeof line, "%c  waiting for the game to begin...",
             frames[spin & 3]);
    mvprintw(5, 4, "%.*s", cols - 8, line);

    /* Same precedence again: once the server has spoken, is_owner decides.
     * Keying this off have_session alone offered "[S] start game" to every
     * player in the room, including the ones the server would refuse with a
     * 403. */
    bool can_start = c->have_session ? c->session.is_owner : c->assume_owner;
    if (can_start)
        mvprintw(7, 4, "[S] start game    [Q] leave room");
    else
        mvprintw(
            7, 4,
            "[S] start (ignored unless you own the room)    [Q] leave room");

    /*
     * Who else is here.
     *
     * The server pushes this with every UPD_SESSION, so it stays current as
     * people arrive and leave without the lobby polling for anything. Drawn in
     * the order the server sent (join order), which is stable - a list that
     * reordered itself while people waited would be hard to read.
     *
     * Iterating to roster_count and never to MAX_STANDINGS matters: a larger
     * room sends only its first few, and the rest of the array is stale.
     */
    if (c->have_session && c->session.roster_count > 0)
    {
        mvprintw(9, 4, "Players in this room (%d)", c->session.roster_count);

        for (int i = 0; i < c->session.roster_count; i++)
        {
            const RoomMember *rm = &c->session.roster[i];
            bool me = (rm->player_id == c->session.player_id);

            /* "you" is worth marking explicitly: every name is "Player N"
             * until real usernames exist, so the id alone is easy to lose
             * track of in a room of four. */
            snprintf(line, sizeof line, "%c %-2d %-10.10s%s%s", me ? '>' : ' ',
                     rm->player_id, rm->name, rm->is_owner ? "  [owner]" : "",
                     me ? "  (you)" : "");

            if (me)
                attron(A_BOLD);
            mvprintw(10 + i, 4, "%.*s", cols - 8, line);
            if (me)
                attroff(A_BOLD);
        }
    }

    tetrisui_draw_status_bar("waiting");
    refresh();
}

ScreenResult screen_wait_start(Client *c)
{
    ScreenResult result = SCR_BACK;
    int spin = 0;
    int reject = 0;

    /*
     * nodelay: getch() must return ERR rather than block, because poll() - not
     * ncurses - owns the waiting here. Cleared on every exit path below; if it
     * leaked out, the next modal screen would spin at 100% CPU.
     */
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    draw_wait(c, spin);

    for (;;)
    {
        struct pollfd pfds[2];
        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = client_fd(c);
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int pr = poll(pfds, 2, WAIT_POLL_MS);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            result = SCR_DISCONNECTED;
            break;
        }

        if (pfds[0].revents & POLLIN)
        {
            int ch;
            /* Drain: one poll wake-up can cover several buffered keys, and a
             * single getch() would leave the rest until the next wake-up. */
            while ((ch = getch()) != ERR)
            {
                if (ch == 's' || ch == 'S')
                {
                    client_start(c);
                }
                else if (ch == 'q' || ch == 'Q' || ch == 27)
                {
                    client_leave(c);
                    result = SCR_BACK;
                    goto done;
                }
            }
        }

        if (pfds[1].revents & POLLIN)
        {
            ClientEvent ev = client_service(c);
            if (ev == CLI_EV_DISCONNECT)
            {
                result = SCR_DISCONNECTED;
                goto done;
            }
            if (ev == CLI_EV_REJECT)
            {
                int why = c->last_reject;
                c->last_reject = 0;

                if (why != REJECT_NOT_OWNER)
                {
                    /* The room is unusable to us - full, already playing, or
                     * we are already in one. The wait is pointless, so leave
                     * and let the exit path report it. */
                    reject = why;
                    result = SCR_BACK;
                    goto done;
                }

                /*
                 * 403 only means WE may not start; the owner still can, so the
                 * wait continues.
                 *
                 * Reported HERE rather than at done:, because done: is reached
                 * when the round starts - so deferring it meant the player
                 * pressed 's', saw nothing at all, and then got "Not the
                 * owner" popped in their face at the exact moment the game
                 * began. The refusal has to answer the keypress that caused
                 * it, while it still makes sense.
                 *
                 * say() is modal and needs blocking input, so nodelay is
                 * lifted for its duration and the wait screen redrawn after.
                 * Frames arriving meanwhile just queue on the socket and are
                 * picked up on the next pass.
                 */
                nodelay(stdscr, FALSE);
                say("Not the owner",
                    "Only the player who created the room can start it.");
                nodelay(stdscr, TRUE);
                draw_wait(c, spin);
            }
            if (c->phase == CLI_PLAYING)
            {
                result = SCR_GAME_START;
                goto done;
            }
        }

        if (pfds[1].revents & (POLLHUP | POLLERR))
        {
            result = SCR_DISCONNECTED;
            goto done;
        }

        draw_wait(c, ++spin);
    }

done:
    nodelay(stdscr, FALSE); /* restore blocking input for the modal screens */

    /*
     * Going back to the lobby means we are no longer in a room - so make sure
     * the SERVER agrees, whatever brought us here.
     *
     * Only the explicit 'q' path used to send LEAVE. Every other route out
     * (a refusal, or anything that ends the wait without starting a game) left
     * the server still holding us as a member while the client showed the
     * lobby. From then on the lobby's only command is JOIN, which the server
     * correctly refuses with 409 "already in a room" - permanently, because
     * the client never sends the LEAVE that would fix it. That is the
     * "first attempt drops me to the lobby, every one after says already in a
     * room" behaviour.
     *
     * Idempotent: the 'q' path has already sent one, and a LEAVE while in no
     * room is a no-op the server answers with an IDLE session update. Sending
     * a redundant one is far cheaper than the stuck state it prevents.
     */
    if (result == SCR_BACK)
        client_leave(c);

    /*
     * Reported after nodelay is cleared, because tetrisui_message blocks.
     *
     * Only refusals that END the wait land here. REJECT_NOT_OWNER is handled
     * inline above and never reaches this point - it must not, because this
     * path also runs on a successful start, which is the worst possible time
     * to announce that an earlier keypress was refused.
     */
    if (reject == REJECT_CONFLICT)
        say("Cannot join",
            "Room is already playing, or you are already in one.");
    else if (reject == REJECT_FULL)
        say("Cannot join", "That room is full, or the server is out of slots.");

    return result;
}
