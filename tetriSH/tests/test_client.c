/*
 * test_client.c - headless integration check against a running tetrisd.
 *
 * The default integration mode needs a live server and real certificates.
 * `--history-frames` is the exception: it uses a paired in-memory transport
 * and runs under `make test` to pin malformed UPD_HISTORY handling. This file
 * exists because the interactive client
 * cannot prove the push path on its own - tetrisu leaves the wait screen the
 * instant UPD_SESSION says PLAYING, which is before any UPD_GAME has arrived.
 *
 * Drives the same client_* API the UI uses, with no terminal:
 *   connect -> handshake -> JOIN -> START -> count UPD_GAME frames
 *
 * argv[4] "history" is a fourth mode alongside "force"/"nostart": skips JOIN
 * entirely and instead proves the HISTORY / UPD_HISTORY leg end to end,
 * which nothing else in the suite can - it needs a live tetrisd and a real
 * session process, exactly like the frame-counting path above (issue #79).
 *
 * Usage: test_client [ip] [port]      exits 0 only if frames actually flowed.
 */

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "libhtttp/htttp.h"
#include "tetrisu/client.h"

#define WATCH_MS 2000 /* how long to count frames for */

static int send_history_push(session_t *server, Client *client,
                             const void *body, uint32_t body_len)
{
    htttp_request_t req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "UPD_HISTORY");
    snprintf(req.path, sizeof req.path, "/player/history");
    req.body = body;
    req.body_len = body_len;

    uint8_t frame[SESSION_MAX_FRAME];
    uint32_t frame_len = sizeof frame;
    if (htttp_serialize_request(&req, frame, &frame_len) != HTTTP_OK ||
        session_send(server, frame, frame_len) != SESSION_OK)
        return -1;
    return client_service(client);
}

static int test_history_frame_lengths(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return 1;

    Client client;
    memset(&client, 0, sizeof client);
    client.fd = sv[0];
    client.sh.fd = sv[0];
    client.sh.established = 1;

    session_t server;
    memset(&server, 0, sizeof server);
    server.fd = sv[1];
    server.established = 1;
    for (size_t i = 0; i < sizeof server.key; i++)
        client.sh.key[i] = server.key[i] = (uint8_t)(i + 1);

    memset(&client.history, 0x5a, sizeof client.history);
    player_history_t before = client.history;
    uint8_t body[sizeof(player_history_t) + 1];
    memset(body, 0, sizeof body);

    int short_ev = send_history_push(&server, &client, body,
                                     (uint32_t)sizeof(player_history_t) - 1);
    int long_ev = send_history_push(&server, &client, body,
                                    (uint32_t)sizeof(player_history_t) + 1);
    int ok = short_ev == CLI_EV_NONE && long_ev == CLI_EV_NONE &&
             !client.have_history &&
             memcmp(&client.history, &before, sizeof before) == 0;

    session_close(&server);
    session_close(&client.sh);
    close(sv[0]);
    close(sv[1]);
    if (!ok)
    {
        fprintf(stderr, "FAIL: malformed UPD_HISTORY changed client state\n");
        return 1;
    }
    printf("PASS: short and oversized UPD_HISTORY frames were ignored\n");
    return 0;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static const char *ev_name(ClientEvent e)
{
    switch (e)
    {
    case CLI_EV_NONE:
        return "none";
    case CLI_EV_GAME:
        return "UPD_GAME";
    case CLI_EV_SESSION:
        return "UPD_SESSION";
    case CLI_EV_RESULT:
        return "UPD_RESULT";
    case CLI_EV_REJECT:
        return "reject";
    case CLI_EV_DISCONNECT:
        return "disconnect";
    case CLI_EV_HISTORY:
        return "UPD_HISTORY";
    case CLI_EV_AUTH_OK:
        return "auth ok";
    case CLI_EV_AUTH_REPLY:
        return "auth reply"; /* never actually escapes client_service() (#62) */
    }
    return "?";
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--history-frames") == 0)
        return test_history_frame_lengths();

    const char *ip = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 5555;
    /* Room to ask for (0 = create a new one) and whether to send START even
     * when the server has not said we own the room - that is how the 403 and
     * 409 refusal paths get exercised. */
    int room = argc > 3 ? atoi(argv[3]) : 0;
    /* argv[4]: "force"   = send START without being told we own the room
     *          "nostart" = join and idle, so another client can be the
     *                      non-owner that gets refused.
     *          "history" = skip JOIN; send HISTORY and check the reply. */
    const char *mode = argc > 4 ? argv[4] : "";
    bool force = strcmp(mode, "force") == 0;
    bool nostart = strcmp(mode, "nostart") == 0;
    bool history_mode = strcmp(mode, "history") == 0;

    static Client c; /* static: Client carries a 64 KiB rx buffer */

    int rc = client_connect(&c, ip, port, "auth/cacsertificate.crt");
    if (rc != 0)
    {
        fprintf(stderr, "connect/handshake failed: %d\n", rc);
        return 1;
    }
    printf("handshake OK\n");

    /*
     * Authenticate before anything else.
     *
     * The pre-auth gate (#50) refuses every game method with 401 until one of
     * LOGIN/REGISTER/GUEST has succeeded, so a harness that skipped this got a
     * 401 on JOIN and never saw a single frame. GUEST because it needs no
     * fixture in the database.
     */
    if (client_guest(&c) != 0)
    {
        fprintf(stderr, "GUEST failed\n");
        return 1;
    }
    long auth_deadline = now_ms() + 3000;
    bool authed = false;
    while (!authed && now_ms() < auth_deadline)
    {
        struct pollfd p = {client_fd(&c), POLLIN, 0};
        if (poll(&p, 1, 100) <= 0)
            continue;
        ClientEvent ev = client_service(&c);
        if (ev == CLI_EV_AUTH_OK)
            authed = true;
        else if (ev == CLI_EV_REJECT || ev == CLI_EV_DISCONNECT)
        {
            fprintf(stderr, "GUEST refused: status %d\n", c.last_reject);
            return 1;
        }
    }
    if (!authed)
    {
        fprintf(stderr, "GUEST got no reply\n");
        return 1;
    }
    printf("GUEST ok\n");

    if (history_mode)
    {
        if (client_history(&c) != 0)
        {
            fprintf(stderr, "HISTORY failed to send\n");
            return 1;
        }
        printf("HISTORY sent\n");

        long hist_deadline = now_ms() + 3000;
        bool got = false;
        while (!got && now_ms() < hist_deadline)
        {
            struct pollfd p = {client_fd(&c), POLLIN, 0};
            if (poll(&p, 1, 100) <= 0)
                continue;
            ClientEvent ev = client_service(&c);
            if (ev == CLI_EV_HISTORY)
                got = true;
            else if (ev == CLI_EV_DISCONNECT)
            {
                fprintf(stderr, "disconnected waiting for UPD_HISTORY\n");
                return 1;
            }
        }
        client_disconnect(&c);

        if (!got)
        {
            fprintf(stderr, "FAIL: no UPD_HISTORY reply\n");
            return 1;
        }
        printf("UPD_HISTORY: status=%d recent_count=%d best_score=%d "
               "best_lines=%d games_played=%d\n",
               c.history.status, c.history.recent_count, c.history.best_score,
               c.history.best_lines, c.history.games_played);

        /* This client authenticated as GUEST, so history_offer() must answer
         * GUEST without ever touching the database (issue #52's Invariant A)
         * - the one status this leg can assert with no fixture at all. */
        if (c.history.status != HISTORY_VIEW_GUEST)
        {
            fprintf(stderr, "FAIL: expected HISTORY_VIEW_GUEST, got %d\n",
                    c.history.status);
            return 1;
        }
        printf("PASS (guest history correctly reports GUEST)\n");
        return 0;
    }

    int sessions = 0, games = 0, rejects = 0;
    bool started = false;
    long deadline =
        now_ms() + (argc > 4 && strcmp(argv[4], "nostart") == 0 ? 8000 : 1500);

    if (client_join(&c, (uint8_t)room) != 0)
    {
        fprintf(stderr, "JOIN failed\n");
        return 1;
    }
    printf("JOIN sent (room %d)\n", room);

    if (force)
    {
        /* Deliberately start without waiting to be told we are the owner. */
        client_start(&c);
        printf("START sent unconditionally (expecting a refusal)\n");
        started = true;
    }

    while (now_ms() < deadline)
    {
        struct pollfd p = {client_fd(&c), POLLIN, 0};
        if (poll(&p, 1, 100) <= 0)
            continue;

        ClientEvent ev = client_service(&c);
        if (ev == CLI_EV_SESSION)
        {
            sessions++;
            printf("  UPD_SESSION: room=%d player=%d owner=%d phase=%d\n",
                   c.session.room_id, c.session.player_id, c.session.is_owner,
                   c.session.phase);
            /* Only start once we are told we own the room - starting blind
             * would make a 403 indistinguishable from a lost frame. */
            if (!started && !nostart && c.session.is_owner)
            {
                if (client_start(&c) != 0)
                {
                    fprintf(stderr, "START failed\n");
                    return 1;
                }
                printf("START sent\n");
                started = true;
                deadline = now_ms() + WATCH_MS; /* now count frames */
            }
        }
        else if (ev == CLI_EV_GAME)
        {
            games++;
        }
        else if (ev == CLI_EV_REJECT)
        {
            rejects++;
            printf("  rejected with status %d\n", c.last_reject);
        }
        else if (ev == CLI_EV_DISCONNECT)
        {
            fprintf(stderr, "disconnected\n");
            break;
        }
        else if (ev != CLI_EV_NONE)
        {
            printf("  %s\n", ev_name(ev));
        }
    }

    client_disconnect(&c);

    printf("\nUPD_SESSION: %d   UPD_GAME: %d   rejects: %d\n", sessions, games,
           rejects);
    if (c.have_game)
        printf("last board: score=%d lines=%d level=%d game_over=%d\n",
               c.game.score, c.game.lines, c.game.level, c.game.game_over);

    /* ~20 frames/sec for WATCH_MS; require clearly more than a couple so a
     * single stray frame cannot pass for a working stream. */
    if (nostart)
    {
        if (sessions == 0)
        {
            fprintf(stderr, "FAIL: no UPD_SESSION\n");
            return 1;
        }
        printf("PASS (joined and idled)\n");
        return 0;
    }
    if (force)
    {
        /* This mode is only about the refusal, so success is a reject. */
        if (rejects == 0)
        {
            fprintf(stderr, "FAIL: expected a refusal\n");
            return 1;
        }
        printf("PASS (refused as expected)\n");
        return 0;
    }
    int want = (WATCH_MS / 50) / 2;
    if (sessions == 0)
    {
        fprintf(stderr, "FAIL: no UPD_SESSION\n");
        return 1;
    }
    if (games < want)
    {
        fprintf(stderr, "FAIL: %d UPD_GAME frames, wanted >= %d\n", games,
                want);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
