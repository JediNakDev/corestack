/*
 * test_ctl.c - the control plane's verbs, driven directly.
 *
 * Drives ctl_dispatch over socketpairs the same way test_room.c drives the
 * room module: no fork, no TCP, no handshake. The daemon plumbing around it
 * (the accepting thread, the notify pipe, the poll set) is deliberately out of
 * scope here - what this pins down is that each verb reads the right state and
 * answers with the right status and body.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include "test_output.h"
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "libhtttp/htttp.h"
#include "tetrisctl/control_plane.h"
#include "tetrisd/room.h"

static int failures = 0;
static int tests_run = 0;

static void check(const char *what, int ok)
{
    test_output_check(&tests_run, &failures, what, ok);
}

/*
 * Run one verb and hand back its status and body.
 *
 * ctl_dispatch always closes the fd it is given, so each call gets a fresh
 * socketpair: our end is the "control client", and we read the reply off it
 * after dispatch has finished with the other.
 */
static int run_verb(int verb, int room, int player, char *body, size_t cap,
                    CtlAfter *after, int *kick_fd)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    CtlReq req = {.fd = sv[1], .verb = verb, .room = room, .player = player};

    int local_kick = -1;
    CtlAfter a = ctl_dispatch(&req, &local_kick);
    if (after)
        *after = a;
    if (kick_fd)
        *kick_fd = local_kick;

    static uint8_t frame[CTL_MAX_FRAME];
    uint32_t len = 0;
    if (ctl_frame_read(sv[0], frame, sizeof frame, &len) != 0)
    {
        close(sv[0]);
        return -1;
    }
    close(sv[0]);

    htttp_response_t res;
    if (htttp_parse_response(frame, len, &res) != HTTTP_OK)
        return -1;

    size_t n = res.body_len < cap - 1 ? res.body_len : cap - 1;
    if (res.body && n > 0)
        memcpy(body, res.body, n);
    body[n] = '\0';

    return res.status;
}

int main(void)
{
    test_output_begin("test_ctl");
    /* Sets the start time ctl reports as uptime, and exercises bind + the
     * stale-socket guard on a path that is nobody else's. */
    int rc = ctl_open("/tmp/tetrisd-test.ctl", -1, -1, -1);
    check("ctl_open binds", rc == 0);
    unlink("/tmp/tetrisd-test.ctl");

    char body[8192];
    int status;

    /* --- empty server ---------------------------------------------------- */
    status = run_verb(CTL_VERB_STATUS, 0, 0, body, sizeof body, NULL, NULL);
    check("STATUS on empty server -> 200", status == 200);
    check("STATUS reports no sessions", strstr(body, "\"sessions\":0") != NULL);
    check("STATUS reports no rooms", strstr(body, "\"rooms\":0") != NULL);

    status = run_verb(CTL_VERB_ROOMS, 0, 0, body, sizeof body, NULL, NULL);
    check("ROOMS on empty server -> 200", status == 200);
    check("ROOMS is an empty array", strcmp(body, "[]") == 0);

    /* --- two sessions, both in room 1 ------------------------------------ */
    int a[2], b[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, a) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, b) == 0);

    client_add(a[0], 4242);
    client_add(b[0], 4243);

    AdminMsg join = {.type = ADMIN_JOIN, .room_id = 1};
    client_handle(a[0], &join);
    client_handle(b[0], &join);

    status = run_verb(CTL_VERB_STATUS, 0, 0, body, sizeof body, NULL, NULL);
    check("STATUS counts both sessions",
          strstr(body, "\"sessions\":2") != NULL);
    check("STATUS counts the room", strstr(body, "\"rooms\":1") != NULL);

    status = run_verb(CTL_VERB_ROOMS, 0, 0, body, sizeof body, NULL, NULL);
    check("ROOMS -> 200", status == 200);
    check("ROOMS names room 1", strstr(body, "\"id\":1") != NULL);
    check("ROOMS counts 2 members", strstr(body, "\"members\":2") != NULL);
    check("ROOMS reports an owner", strstr(body, "\"owner\":0") != NULL);

    status = run_verb(CTL_VERB_PLAYERS, 0, 0, body, sizeof body, NULL, NULL);
    check("PLAYERS -> 200", status == 200);
    check("PLAYERS shows the first pid", strstr(body, "\"pid\":4242") != NULL);
    check("PLAYERS shows the second pid", strstr(body, "\"pid\":4243") != NULL);
    check("PLAYERS marks the owner", strstr(body, "\"owner\":true") != NULL);
    check("PLAYERS marks the non-owner",
          strstr(body, "\"owner\":false") != NULL);

    /* --- kick ------------------------------------------------------------ */
    CtlAfter after = CTL_AFTER_NONE;
    int kicked = -1;

    status = run_verb(CTL_VERB_KICK, 1, 99, body, sizeof body, &after, &kicked);
    check("KICK of an unknown player -> 404", status == 404);
    check("KICK of an unknown player asks for nothing",
          after == CTL_AFTER_NONE);

    status = run_verb(CTL_VERB_KICK, 9, 0, body, sizeof body, &after, &kicked);
    check("KICK in an unknown room -> 404", status == 404);

    status = run_verb(CTL_VERB_KICK, 1, 1, body, sizeof body, &after, &kicked);
    check("KICK -> 200", status == 200);
    check("KICK asks the caller to drop the session", after == CTL_AFTER_KICK);
    check("KICK names the victim's fd", kicked == b[0]);

    /*
     * The victim is told before it is dropped. A 403 on its admin socket is
     * what the session forwards to the wire, so the player sees a refusal
     * rather than an unexplained disconnect.
     */
    AdminMsg got;
    int drained = 0;
    while (adminmsg_read(b[1], &got) == 1)
    {
        if (got.type == ADMIN_REJECT && got.reason == 403)
        {
            drained = 1;
            break;
        }
    }
    check("victim receives a 403 reject", drained == 1);

    /*
     * --- reload raises SIGHUP in this process and replies 200 -------------
     *
     * The real reload work runs on tetrisd's admin thread, off its own
     * self-pipe (reload_config() in tetrisd.c) - out of reach of this
     * direct-dispatch unit test, which has no admin thread and installs no
     * SIGHUP handler of its own. What this pins down is the contract this
     * test CAN see: the verb succeeds and really does raise the signal.
     * SIG_IGN stands in for tetrisd's on_reload so the default disposition
     * (terminate) does not take this test process down with it.
     */
    struct sigaction old_hup;
    struct sigaction ign = {.sa_handler = SIG_IGN};
    sigemptyset(&ign.sa_mask);
    sigaction(SIGHUP, &ign, &old_hup);
    status = run_verb(CTL_VERB_RELOAD, 0, 0, body, sizeof body, NULL, NULL);
    sigaction(SIGHUP, &old_hup, NULL);
    check("RELOAD -> 200", status == 200);

    /* --- shutdown replies before anything comes down --------------------- */
    status = run_verb(CTL_VERB_SHUTDOWN, 0, 0, body, sizeof body, &after, NULL);
    check("SHUTDOWN -> 200", status == 200);
    check("SHUTDOWN asks the caller to tear down", after == CTL_AFTER_SHUTDOWN);

    close(a[0]);
    close(a[1]);
    close(b[0]);
    close(b[1]);

    test_output_summary(tests_run, failures, 0);
    return failures == 0 ? 0 : 1;
}
