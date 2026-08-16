/*
 * test_room.c - the session <-> admin protocol, from the admin side.
 *
 * room.c IS the admin thread's logic: client_add / client_handle / client_close
 * are its whole public surface, and it answers by writing AdminMsg structs
 * straight to session fds. So a test can drive it directly - hand it one end of
 * a socketpair and read its replies off the other. No fork, no exec, no crypto,
 * no network, no timing.
 *
 *      client_handle(s->rx, &msg)  ->  room.c  ->  write(s->rx, reply)
 *                                                     |
 *      recv_msg(s, &reply)         <-------------------+  read(s->tx)
 *
 * WHY THERE IS NO CONCURRENCY TEST HERE
 * All three entry points are called from exactly one thread - see tetrisd.c
 * lines 177/193/198, all inside admin_thread. The listener thread never touches
 * room.c; it hands new sessions over the g_notify pipe and the admin registers
 * them itself. Messages from N sessions are therefore polled and processed
 * strictly one at a time and cannot interleave inside client_handle. What
 * actually varies with many clients is ORDER, SCALE and CAPACITY, so that is
 * what the later cases cover.
 *
 * IMPORTANT: room.c keeps its tables in file-static arrays with no reset hook,
 * so state carries across cases. Every session must be released through
 * sess_close(), which calls client_close() before closing the fds - otherwise a
 * later case can collide with a stale entry once the kernel recycles the fd.
 *
 * Build:  make gui   (or: make bin/test_room)
 * Run:    ./bin/test_room        exit code = failing cases
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <stdio.h>
#include "test_output.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include "tetrisd/room.h"
#include "tetrisd/adminmsg.h"
#include "tetrisd/tetrisd.h"

/* ---- tiny test framework ------------------------------------------------ */

static int cases_run = 0, regressions = 0, gaps = 0;

#define CHECK(cond, msg)                                                       \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            test_output_failure_detail(msg, __FILE__, __LINE__);               \
            return -1;                                                         \
        }                                                                      \
    } while (0)

/* A case that must pass: a failure here is a regression. */
#define RUN(fn)                                                                \
    do                                                                         \
    {                                                                          \
        cases_run++;                                                           \
        if (fn() == 0)                                                         \
            test_output_pass(#fn);                                             \
        else                                                                   \
        {                                                                      \
            test_output_fail(#fn);                                             \
            regressions++;                                                     \
        }                                                                      \
    } while (0)

/*
 * A case asserting behaviour room.c does not implement yet. It still fails -
 * these are real defects, not aspirations - but it is labelled so a genuine
 * regression stays distinguishable from the known to-do list.
 */
#define RUN_GAP(fn, why)                                                       \
    do                                                                         \
    {                                                                          \
        cases_run++;                                                           \
        if (fn() == 0)                                                         \
            test_output_pass(#fn " (known gap fixed - promote to RUN)");       \
        else                                                                   \
        {                                                                      \
            test_output_fail(#fn " (known gap: " why ")");                     \
            gaps++;                                                            \
        }                                                                      \
    } while (0)

/* ---- fake sessions ------------------------------------------------------ */

typedef struct
{
    int rx; /* room.c's end - passed to client_add / client_handle */
    int tx; /* our end - where room.c's replies arrive             */
} Sess;

/* SOCK_DGRAM so one write() is exactly one read(), matching how tetrisd.c
 * creates the real session <-> admin socketpair. */
static int sess_open(Sess *s, pid_t pid)
{
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sp) != 0)
        return -1;
    s->rx = sp[0];
    s->tx = sp[1];
    client_add(s->rx, pid);
    return 0;
}

static void sess_close(Sess *s)
{
    if (s->rx < 0)
        return;
    client_close(s->rx); /* free the table slot BEFORE the fd number */
    room_flush_updates();
    close(s->rx);
    close(s->tx);
    s->rx = s->tx = -1;
}

/* ---- reading replies ---------------------------------------------------- */

/* Read one AdminMsg or give up after `ms`. Never blocks forever: a missing
 * reply must fail the case, not hang the suite. */
static int recv_msg(const Sess *s, AdminMsg *m, int ms)
{
    struct pollfd p = {.fd = s->tx, .events = POLLIN, .revents = 0};
    if (poll(&p, 1, ms) != 1)
        return -1;
    return read(s->tx, m, sizeof *m) == (ssize_t)sizeof *m ? 0 : -1;
}

/*
 * Read until a message of type `want` arrives, discarding others.
 *
 * The scoreboard is broadcast to the whole room whenever a score or the
 * membership changes, so ADMIN_STANDINGS now turns up between almost any two
 * messages a test cares about. Filtering here keeps each case asserting the
 * one thing it is about, instead of every case having to know the exact
 * message stream of every feature added since.
 */
static int recv_type(const Sess *s, AdminMsg *m, AdminMsgType want, int ms)
{
    for (;;)
    {
        if (recv_msg(s, m, ms) != 0)
            return -1;
        if (m->type == want)
            return 0;
    }
}

/*
 * Read every pending message and keep the LAST one of type `want`.
 *
 * The scoreboard is a state broadcast, not an event: each score report
 * republishes the whole board, so after three reports there are three
 * standings messages queued and only the newest describes the room now.
 * recv_type would hand back the first - the board as it looked one report in -
 * which is a real trap when asserting on ranking.
 */
static int recv_latest(const Sess *s, AdminMsg *m, AdminMsgType want, int ms)
{
    AdminMsg cur;
    int found = -1;
    while (recv_msg(s, &cur, ms) == 0)
    {
        if (cur.type == want)
        {
            *m = cur;
            found = 0;
        }
        ms = 40; /* first read may wait; later ones only drain what is there */
    }
    return found;
}

/* Nothing waiting. Asserts the negative half of "everyone EXCEPT the sender"
 * and "owner only", which is most of what those rules actually mean. */
static int quiet(const Sess *s)
{
    struct pollfd p = {.fd = s->tx, .events = POLLIN, .revents = 0};
    return poll(&p, 1, 60) == 0;
}

/*
 * Nothing waiting except scoreboard updates.
 *
 * "This player was not told about X" is still the property under test, but a
 * standings broadcast is not a notification about X - every member gets one
 * whenever anyone's score or the roster changes, by design. Tests that mean
 * "no reply to my command" use this; tests that mean "absolutely silent" keep
 * quiet().
 */
static int quiet_but_standings(const Sess *s)
{
    AdminMsg m;
    while (recv_msg(s, &m, 60) == 0)
        if (m.type != ADMIN_STANDINGS)
            return 0;
    return 1;
}

static int count_type(const Sess *s, AdminMsgType want)
{
    AdminMsg m;
    int count = 0;
    while (recv_msg(s, &m, 0) == 0)
        if (m.type == want)
            count++;
    return count;
}

static void drain(const Sess *s)
{
    AdminMsg m;
    while (recv_msg(s, &m, 0) == 0)
    {
    }
}

/* Queue one op as part of the admin thread's current poll batch. */
static void op_deferred(const Sess *s, AdminMsgType t, int room_id, int lines,
                        int score)
{
    AdminMsg m;
    memset(&m, 0, sizeof m);
    m.type = t;
    m.room_id = room_id;
    m.lines = lines;
    m.score = score;
    client_handle(s->rx, &m);
}

/* Send one op and finish its admin-thread poll batch. */
static void op(const Sess *s, AdminMsgType t, int room_id, int lines, int score)
{
    op_deferred(s, t, room_id, lines, score);
    room_flush_updates();
}

/* Open a session and put it in `room` (0 = make a new one). Returns room id. */
static int join(Sess *s, pid_t pid, int room)
{
    if (sess_open(s, pid) != 0)
        return -1;
    op(s, ADMIN_JOIN, room, 0, 0);
    AdminMsg m;
    if (recv_type(s, &m, ADMIN_SESSION, 200) != 0)
        return -1;
    return m.session.room_id;
}

/* ---- JOIN --------------------------------------------------------------- */

static int t_join_new_room_owns_it(void)
{
    Sess a;
    CHECK(sess_open(&a, 100) == 0, "socketpair");
    op(&a, ADMIN_JOIN, 0, 0, 0);

    AdminMsg m;
    CHECK(recv_type(&a, &m, ADMIN_SESSION, 200) == 0,
          "expected a reply to JOIN");
    CHECK(m.type == ADMIN_SESSION, "reply should be ADMIN_SESSION");
    CHECK(m.session.room_id > 0, "should have been given a real room id");
    CHECK(m.session.is_owner, "first member must own the room");
    CHECK(m.session.player_id == 0, "first member should be player 0");
    CHECK(m.session.phase == SESSION_WAITING, "joining should move to WAITING");

    sess_close(&a);
    return 0;
}

static int t_join_second_is_not_owner(void)
{
    Sess a, b;
    int room = join(&a, 101, 0);
    CHECK(room > 0, "a should get a room");
    CHECK(sess_open(&b, 102) == 0, "socketpair b");

    op(&b, ADMIN_JOIN, room, 0, 0);
    AdminMsg mb;
    CHECK(recv_type(&b, &mb, ADMIN_SESSION, 200) == 0, "b reply");
    CHECK(mb.session.room_id == room, "b should join the same room");
    CHECK(!mb.session.is_owner, "second member must not own the room");
    CHECK(mb.session.player_id == 1, "second member should be player 1");
    /*
     * a must be told too. This is the whole point of the roster broadcast:
     * the people ALREADY in the room are the ones who need to see the new
     * arrival, and they are exactly who a single send_session would miss.
     */
    AdminMsg ma;
    CHECK(recv_latest(&a, &ma, ADMIN_SESSION, 200) == 0,
          "a should be told that b joined");
    CHECK(ma.session.roster_count == 2, "a's roster should now list both");
    CHECK(ma.session.roster[0].player_id == 0, "roster is in join order");
    CHECK(ma.session.roster[1].player_id == 1, "roster is in join order");
    CHECK(ma.session.roster[0].is_owner, "a is the owner");
    CHECK(!ma.session.roster[1].is_owner, "b is not");
    CHECK(ma.session.is_owner, "and a still knows it owns the room");

    sess_close(&a);
    sess_close(&b);
    return 0;
}

/* A second JOIN while already in a room must be refused - and the client has to
 * be TOLD, or a refused join is indistinguishable from a successful one. */
static int t_join_twice_refused(void)
{
    Sess a;
    CHECK(join(&a, 103, 0) > 0, "join");
    op(&a, ADMIN_JOIN, 0, 0, 0);

    AdminMsg m;
    CHECK(recv_type(&a, &m, ADMIN_REJECT, 200) == 0,
          "a second JOIN must get an answer");
    CHECK(m.type == ADMIN_REJECT, "it should be refused, not accepted");
    CHECK(m.reason == REJECT_CONFLICT, "already in a room is a 409");

    sess_close(&a);
    return 0;
}

/* Two clients each asking for "a new room" must not land in the same one. */
static int t_two_new_rooms_are_distinct(void)
{
    Sess a, b;
    int ra = join(&a, 104, 0);
    int rb = join(&b, 105, 0);
    CHECK(ra > 0 && rb > 0, "both should get rooms");
    CHECK(ra != rb, "two JOIN 0 requests must produce different rooms");
    sess_close(&a);
    sess_close(&b);
    return 0;
}

/* ---- LEAVE -------------------------------------------------------------- */

static int t_leave_promotes_new_owner(void)
{
    Sess a, b;
    int room = join(&a, 110, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 111, room) == room, "b join");
    drain(&a);
    drain(&b); /* the join broadcasts are not what this tests */

    op(&a, ADMIN_LEAVE, 0, 0, 0);

    AdminMsg gone;
    CHECK(recv_type(&a, &gone, ADMIN_SESSION, 200) == 0,
          "leaver should be told");
    CHECK(gone.session.room_id == ROOM_NONE, "leaver must be out of the room");
    CHECK(gone.session.phase == SESSION_IDLE, "leaver should return to IDLE");
    CHECK(!gone.session.is_owner, "leaver keeps no ownership");

    AdminMsg promo;
    CHECK(recv_type(&b, &promo, ADMIN_SESSION, 200) == 0,
          "remaining member should be told");
    CHECK(promo.type == ADMIN_SESSION, "promotion is an ADMIN_SESSION push");
    CHECK(promo.session.is_owner, "b should have been promoted to owner");

    sess_close(&a);
    sess_close(&b);
    return 0;
}

static int t_leave_without_room_still_replies(void)
{
    Sess a;
    CHECK(sess_open(&a, 112) == 0, "socketpair");
    op(&a, ADMIN_LEAVE, 0, 0, 0);
    AdminMsg m;
    CHECK(recv_type(&a, &m, ADMIN_SESSION, 200) == 0,
          "LEAVE with no room should still reply");
    CHECK(m.session.room_id == ROOM_NONE, "still in no room");
    sess_close(&a);
    return 0;
}

/* ---- START -------------------------------------------------------------- */

static int t_start_broadcasts_one_seed(void)
{
    Sess a, b, c;
    int room = join(&a, 120, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 121, room) == room, "b join");
    CHECK(join(&c, 122, room) == room, "c join");

    op(&a, ADMIN_START, 0, 0, 0);

    AdminMsg sa, sb, sc;
    CHECK(recv_type(&a, &sa, ADMIN_SEED, 200) == 0,
          "a should receive the seed");
    CHECK(recv_type(&b, &sb, ADMIN_SEED, 200) == 0,
          "b should receive the seed");
    CHECK(recv_type(&c, &sc, ADMIN_SEED, 200) == 0,
          "c should receive the seed");
    CHECK(sa.type == ADMIN_SEED && sb.type == ADMIN_SEED &&
              sc.type == ADMIN_SEED,
          "start signal is ADMIN_SEED");
    CHECK(sa.seed == sb.seed && sb.seed == sc.seed,
          "all members MUST get the identical seed or their boards diverge");

    sess_close(&a);
    sess_close(&b);
    sess_close(&c);
    return 0;
}

static int t_start_owner_only(void)
{
    Sess a, b;
    int room = join(&a, 123, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 124, room) == room, "b join");
    drain(&a);
    drain(&b); /* the join broadcasts are not what this tests */

    op(&b, ADMIN_START, 0, 0, 0); /* b is not the owner */

    AdminMsg m;
    CHECK(recv_type(&b, &m, ADMIN_REJECT, 200) == 0,
          "a non-owner START must get an answer");
    CHECK(m.type == ADMIN_REJECT, "it should be refused");
    CHECK(m.reason == REJECT_NOT_OWNER, "not being the owner is a 403");
    CHECK(quiet_but_standings(&a),
          "and it must not have started the room for anyone else");

    sess_close(&a);
    sess_close(&b);
    return 0;
}

static int t_start_twice_ignored(void)
{
    Sess a;
    CHECK(join(&a, 125, 0) > 0, "join");
    op(&a, ADMIN_START, 0, 0, 0);
    AdminMsg first;
    CHECK(recv_type(&a, &first, ADMIN_SEED, 200) == 0, "first START seeds");
    CHECK(first.type == ADMIN_SEED, "first START should seed the room");

    op(&a, ADMIN_START, 0, 0, 0);
    AdminMsg again;
    CHECK(recv_type(&a, &again, ADMIN_REJECT, 200) == 0,
          "a second START must get an answer");
    CHECK(again.type == ADMIN_REJECT, "it must not re-seed a running room");
    CHECK(again.reason == REJECT_CONFLICT, "already running is a 409");
    sess_close(&a);
    return 0;
}

/* ---- GARBAGE ------------------------------------------------------------ */

/*
 * One attack lands on exactly ONE opponent - not the sender, and not everyone.
 * Fanning out would multiply an attack by the size of the room.
 */
static int t_garbage_hits_one_opponent(void)
{
    Sess a, b, c;
    int room = join(&a, 130, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 131, room) == room, "b join");
    CHECK(join(&c, 132, room) == room, "c join");
    op(&a, ADMIN_START, 0, 0, 0);
    drain(&a);
    drain(&b);
    drain(&c);

    op(&a, ADMIN_GARBAGE, 0, 4, 0);

    AdminMsg gb, gc;
    int hit_b = (recv_type(&b, &gb, ADMIN_RECV_GARBAGE, 0) == 0);
    int hit_c = (recv_type(&c, &gc, ADMIN_RECV_GARBAGE, 0) == 0);
    CHECK(hit_b + hit_c == 1, "exactly one opponent must receive the attack");
    CHECK((hit_b ? gb.lines : gc.lines) == 4,
          "line count must survive the hop");
    CHECK(quiet_but_standings(&a),
          "the attacker must NOT receive its own garbage");

    sess_close(&a);
    sess_close(&b);
    sess_close(&c);
    return 0;
}

/*
 * The target is drawn per attack, so over many attacks both opponents get
 * some. Guards against a "random" pick that is really fixed on whoever joined
 * first. 24 rounds: a one-sided run by chance is about 1 in 8 million.
 */
static int t_garbage_target_varies(void)
{
    enum
    {
        ROUNDS = 24
    };
    Sess a, b, c;
    int room = join(&a, 135, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 136, room) == room, "b join");
    CHECK(join(&c, 137, room) == room, "c join");
    op(&a, ADMIN_START, 0, 0, 0);
    drain(&a);
    drain(&b);
    drain(&c);

    /*
     * Read each attack back before sending the next. Do NOT batch these: the
     * admin's ends are non-blocking, so a burst that outruns the socketpair
     * buffer is wedged and DROPPED, and the count comes up short. The buffer
     * is ~208K on Linux but ~8K on macOS, so batching passes on one and fails
     * on the other. op() is synchronous, so this costs nothing.
     */
    AdminMsg g;
    int hb = 0, hc = 0;
    for (int i = 0; i < ROUNDS; i++)
    {
        op(&a, ADMIN_GARBAGE, 0, 1, 0);
        if (recv_type(&b, &g, ADMIN_RECV_GARBAGE, 0) == 0)
            hb++;
        else if (recv_type(&c, &g, ADMIN_RECV_GARBAGE, 0) == 0)
            hc++;
    }
    CHECK(hb + hc == ROUNDS, "every attack must land on somebody");
    CHECK(hb > 0 && hc > 0, "the target must vary between opponents");

    sess_close(&a);
    sess_close(&b);
    sess_close(&c);
    return 0;
}

/*
 * A player who has already topped out is not a target - an attack routed to
 * them would just evaporate while a live opponent got off free.
 */
static int t_garbage_skips_finished_players(void)
{
    enum
    {
        ROUNDS = 8
    };
    Sess a, b, c;
    int room = join(&a, 138, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 139, room) == room, "b join");
    CHECK(join(&c, 140, room) == room, "c join");
    op(&a, ADMIN_START, 0, 0, 0);
    op(&b, ADMIN_GAMEOVER, 0, 0, 500); /* b is out; a and c play on */
    drain(&a);
    drain(&b);
    drain(&c);

    /* Drained each round, not batched - see t_garbage_target_varies. */
    AdminMsg g;
    int hc = 0;
    for (int i = 0; i < ROUNDS; i++)
    {
        op(&a, ADMIN_GARBAGE, 0, 2, 0);
        if (recv_type(&c, &g, ADMIN_RECV_GARBAGE, 0) == 0)
            hc++;
    }
    CHECK(hc == ROUNDS, "every attack must reach the one live opponent");
    CHECK(quiet_but_standings(&b),
          "a finished player must not receive garbage");

    sess_close(&a);
    sess_close(&b);
    sess_close(&c);
    return 0;
}

/* Nobody to attack: the send must vanish quietly rather than bounce back. */
static int t_garbage_alone_in_room(void)
{
    Sess a;
    int room = join(&a, 141, 0);
    CHECK(room > 0, "a join");
    op(&a, ADMIN_START, 0, 0, 0);
    drain(&a);

    op(&a, ADMIN_GARBAGE, 0, 3, 0);
    CHECK(quiet_but_standings(&a), "garbage with no opponent must go nowhere");

    sess_close(&a);
    return 0;
}

static int t_garbage_nonpositive_dropped(void)
{
    Sess a, b;
    int room = join(&a, 133, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 134, room) == room, "b join");
    op(&a, ADMIN_START, 0, 0, 0);
    drain(&a);
    drain(&b);

    op(&a, ADMIN_GARBAGE, 0, 0, 0);
    CHECK(quiet_but_standings(&b), "0 lines of garbage must not be forwarded");
    op(&a, ADMIN_GARBAGE, 0, -3, 0);
    CHECK(quiet_but_standings(&b), "negative garbage must not be forwarded");

    sess_close(&a);
    sess_close(&b);
    return 0;
}

/* ---- GAMEOVER ----------------------------------------------------------- */

static int t_gameover_waits_for_all(void)
{
    Sess a, b;
    int room = join(&a, 140, 0);
    CHECK(room > 0, "a join");
    CHECK(sess_open(&b, 141) == 0, "socketpair b");
    op(&b, ADMIN_JOIN, room, 0, 0);
    AdminMsg mb;
    CHECK(recv_type(&b, &mb, ADMIN_SESSION, 200) == 0, "b join");
    int b_player = mb.session.player_id;

    op(&a, ADMIN_START, 0, 0, 0);
    drain(&a);
    drain(&b);

    op(&a, ADMIN_GAMEOVER, 0, 0, 500); /* a finishes first, lower score */
    CHECK(quiet_but_standings(&a), "no result while b is still playing");
    CHECK(quiet_but_standings(&b), "no result while b is still playing");

    op(&b, ADMIN_GAMEOVER, 0, 0, 900); /* b finishes, higher score */

    AdminMsg ra, rb;
    CHECK(recv_type(&a, &ra, ADMIN_RESULT, 200) == 0,
          "a should get the result");
    CHECK(recv_type(&b, &rb, ADMIN_RESULT, 200) == 0,
          "b should get the result");
    CHECK(ra.type == ADMIN_RESULT && rb.type == ADMIN_RESULT, "type RESULT");
    CHECK(ra.winner == rb.winner, "both members must agree on the winner");
    CHECK(ra.winner == b_player, "highest score should win");

    sess_close(&a);
    sess_close(&b);
    return 0;
}

/* A duplicate GAMEOVER (retry, or a buggy session) must not announce twice. */
static int t_gameover_twice_is_safe(void)
{
    Sess a, b;
    int room = join(&a, 142, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 143, room) == room, "b join");
    op(&a, ADMIN_START, 0, 0, 0);
    drain(&a);
    drain(&b);

    op(&a, ADMIN_GAMEOVER, 0, 0, 300);
    op(&a, ADMIN_GAMEOVER, 0, 0, 300); /* same player again */
    CHECK(quiet_but_standings(&b),
          "a repeated GAMEOVER must not resolve the room early");

    op(&b, ADMIN_GAMEOVER, 0, 0, 100);
    AdminMsg r;
    CHECK(recv_type(&b, &r, ADMIN_RESULT, 200) == 0,
          "result once everyone is done");
    CHECK(r.type == ADMIN_RESULT, "type RESULT");
    CHECK(quiet_but_standings(&b), "the result must be announced exactly once");

    sess_close(&a);
    sess_close(&b);
    return 0;
}

/*
 * A finished round must leave everyone still in the room, in WAITING, so the
 * owner can START a rematch without anyone re-joining.
 *
 * The client depends on this: tetrisu returns to the wait screen after a round
 * rather than the menu, precisely because it is still a member. If the server
 * ever dropped members here instead, the client would sit on a wait screen for
 * a room it had been evicted from. (Conversely, when the client used to fall
 * back to the menu, its only command was JOIN - which is correctly refused
 * with 409 for a room you are already in. That was the visible bug.)
 */
static int t_rematch_after_round(void)
{
    Sess a, b;
    int room = join(&a, 240, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 241, room) == room, "b join");

    op(&a, ADMIN_START, 0, 0, 0);
    AdminMsg s1;
    CHECK(recv_type(&a, &s1, ADMIN_SEED, 200) == 0, "first seed");
    CHECK(s1.type == ADMIN_SEED, "first START seeds");
    drain(&b);

    op(&a, ADMIN_GAMEOVER, 0, 0, 100);
    op(&b, ADMIN_GAMEOVER, 0, 0, 900);
    AdminMsg res;
    CHECK(recv_type(&a, &res, ADMIN_RESULT, 200) == 0, "result");
    CHECK(res.type == ADMIN_RESULT, "round should resolve");
    CHECK(res.winner == 1, "b had the higher score");
    drain(&a);
    drain(&b);

    /* Still in the room, still the owner -> a rematch just works. */
    op(&a, ADMIN_START, 0, 0, 0);
    AdminMsg s2a, s2b;
    CHECK(recv_type(&a, &s2a, ADMIN_SEED, 200) == 0,
          "owner can START again without rejoining");
    CHECK(s2a.type == ADMIN_SEED, "a rematch must seed, not reject");
    CHECK(recv_type(&b, &s2b, ADMIN_SEED, 200) == 0,
          "the other member is still in the room");
    CHECK(s2b.type == ADMIN_SEED, "and gets the rematch seed too");
    CHECK(s2a.seed == s2b.seed, "both must get the same new seed");

    /* Scores from the previous round must not decide this one. */
    op(&a, ADMIN_GAMEOVER, 0, 0, 700);
    op(&b, ADMIN_GAMEOVER, 0, 0, 200);
    AdminMsg res2;
    CHECK(recv_type(&a, &res2, ADMIN_RESULT, 200) == 0, "second result");
    CHECK(res2.winner == 0,
          "a won this round; last round's scores must not carry over");

    sess_close(&a);
    sess_close(&b);
    return 0;
}

/* ---- a session that stops draining -------------------------------------- */

/*
 * A client that never reads its socketpair must be reported, not waited for.
 *
 * tetrisd makes the admin's ends non-blocking so that one stuck session cannot
 * stall the single thread routing every room; room.c then records the fd for
 * the admin to close. This case reproduces that by opening a session the same
 * way tetrisd does - non-blocking on room.c's side - and never reading it.
 *
 * If the fix regressed, this case does not fail: it HANGS, which is precisely
 * the bug. That is why it is worth having.
 */
static int t_wedged_session_is_reported(void)
{
    int sp[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair");
    CHECK(fcntl(sp[0], F_SETFL, O_NONBLOCK) == 0,
          "non-blocking, as tetrisd does");

    Sess w = {.rx = sp[0], .tx = sp[1]};
    client_add(w.rx, 400);
    op(&w, ADMIN_JOIN, 0, 0, 0);

    /*
     * Never read w.tx. Each score change republishes the scoreboard to the
     * room, so the buffer fills after a few hundred messages - the socketpair
     * holds ~400 of them.
     */
    int wedged = -1;
    for (int i = 1; i <= 4000 && wedged < 0; i++)
    {
        op(&w, ADMIN_SCORE, 0, i, i * 10);
        wedged = client_take_wedged();
    }

    CHECK(wedged == w.rx, "the stuck session should have been reported");
    CHECK(client_take_wedged() == -1, "and reported only once");

    client_close(w.rx);
    close(w.rx);
    close(w.tx);
    return 0;
}

/* ---- lobby roster ------------------------------------------------------- */

/*
 * The roster reaches everyone, in join order, with the owner marked.
 *
 * The failure this guards against is quiet: if the roster were sent only to
 * the client whose state changed, the joiner would see a full list and the
 * players already in the room would never see them arrive. So the assertions
 * that matter are on the EXISTING members, not on the newcomer.
 */
static int t_roster_reaches_everyone(void)
{
    Sess a, b, c;
    int room = join(&a, 280, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 281, room) == room, "b join");
    drain(&a);
    drain(&b);

    CHECK(join(&c, 282, room) == room, "c join");

    AdminMsg m;
    /* The player who was there first must have been told about the newcomer. */
    CHECK(recv_latest(&a, &m, ADMIN_SESSION, 300) == 0, "a should be told");
    CHECK(m.session.roster_count == 3, "a's roster should list all three");
    CHECK(m.session.roster[0].player_id == 0,
          "join order, not members[] order");
    CHECK(m.session.roster[1].player_id == 1, "join order");
    CHECK(m.session.roster[2].player_id == 2, "join order");
    CHECK(strcmp(m.session.roster[2].name, "Player 2") == 0,
          "names travel too");
    CHECK(m.session.roster[0].is_owner, "the first joiner owns the room");
    CHECK(!m.session.roster[1].is_owner, "and nobody else does");
    CHECK(!m.session.roster[2].is_owner, "and nobody else does");

    /* So must the one who joined second. */
    CHECK(recv_latest(&b, &m, ADMIN_SESSION, 300) == 0, "b should be told");
    CHECK(m.session.roster_count == 3, "b sees the same three");
    CHECK(!m.session.is_owner,
          "b's own flag is still its own, not the roster's");

    sess_close(&a);
    sess_close(&b);
    sess_close(&c);
    return 0;
}

/* Ownership moving must move the marker on EVERY roster, not just the new
 * owner's - otherwise the others keep showing a player who has left as the one
 * who can start. */
static int t_roster_follows_ownership(void)
{
    Sess a, b, c;
    int room = join(&a, 290, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 291, room) == room, "b join");
    CHECK(join(&c, 292, room) == room, "c join");
    drain(&a);
    drain(&b);
    drain(&c);

    op(&a, ADMIN_LEAVE, 0, 0, 0); /* the owner walks out */

    /* c may be neither the leaver nor the promoted player - the least likely
     * to be told, and the one whose UI would otherwise be stale. */
    AdminMsg m;
    CHECK(recv_latest(&c, &m, ADMIN_SESSION, 300) == 0, "c should be told");
    CHECK(m.session.roster_count == 2, "a is gone from the roster");
    CHECK(m.session.roster[0].player_id == 1, "roster stays in join order");
    CHECK(m.session.roster[1].player_id == 2, "roster stays in join order");

    /*
     * Exactly one owner, and it is somebody still here.
     *
     * Deliberately NOT asserting which one. room_del_member swap-removes, so
     * the successor is whoever lands in members[0] - player 2 in a room of
     * three. That is the admin's choice to make; this test's job is to prove
     * the roster reports it faithfully to a third party, not to dictate it.
     */
    int owners = 0, mine = -1;
    for (int i = 0; i < m.session.roster_count; i++)
    {
        if (m.session.roster[i].is_owner)
            owners++;
        if (m.session.roster[i].player_id == m.session.player_id)
            mine = i;
    }
    CHECK(owners == 1, "the room must have exactly one owner after a handover");

    /*
     * SessionState carries ownership twice - "am I the owner" and "is this row
     * the owner" - and a client that disagreed with itself would draw the
     * marker beside one player while offering the START key to another. This
     * is the assertion that keeps the two honest.
     */
    CHECK(mine >= 0, "c must appear in its own roster");
    CHECK(m.session.roster[mine].is_owner == m.session.is_owner,
          "own is_owner must match own row in the roster");

    sess_close(&a);
    sess_close(&b);
    sess_close(&c);
    return 0;
}

/* A player who leaves must not be left holding the room's roster. */
static int t_roster_cleared_on_leave(void)
{
    Sess a, b;
    int room = join(&a, 300, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 301, room) == room, "b join");
    drain(&a);
    drain(&b);

    op(&b, ADMIN_LEAVE, 0, 0, 0);

    AdminMsg m;
    CHECK(recv_latest(&b, &m, ADMIN_SESSION, 300) == 0,
          "leaver should be told");
    CHECK(m.session.room_id == ROOM_NONE, "out of the room");
    CHECK(m.session.roster_count == 0,
          "and holding no roster - the lobby must not keep showing it");

    sess_close(&a);
    sess_close(&b);
    return 0;
}

/* ---- scoreboard --------------------------------------------------------- */

/*
 * ADMIN_SCORE updates the sender's row and republishes the board to everyone,
 * ranked. The ranking is the interesting part: it must match the rule that
 * later picks the winner, or players watch one leader all round and are told
 * about a different one at the end.
 */
static int t_standings_ranked_and_broadcast(void)
{
    Sess a, b, c;
    int room = join(&a, 250, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 251, room) == room, "b join");
    CHECK(join(&c, 252, room) == room, "c join");
    op(&a, ADMIN_START, 0, 0, 0);
    drain(&a);
    drain(&b);
    drain(&c);

    /* c pulls ahead, a is second, b last. */
    op(&a, ADMIN_SCORE, 0, 2, 300);
    op(&b, ADMIN_SCORE, 0, 1, 100);
    op(&c, ADMIN_SCORE, 0, 5, 800);

    /* Every member sees the same board; check it on someone who sent nothing
     * last, so this is a broadcast and not an echo to the reporter. */
    AdminMsg m;
    CHECK(recv_latest(&b, &m, ADMIN_STANDINGS, 300) == 0,
          "b should get standings");
    CHECK(m.standing_count == 3, "all three players should appear");

    CHECK(m.standings[0].player_id == 2, "highest score should rank first");
    CHECK(m.standings[0].score == 800, "c's score");
    CHECK(m.standings[0].lines == 5, "c's lines");
    CHECK(m.standings[1].player_id == 0, "a should be second");
    CHECK(m.standings[2].player_id == 1, "b should be last");

    /* The name is assigned by the server from the id, and travels as text so
     * that real usernames later need no protocol change. */
    CHECK(strcmp(m.standings[0].name, "Player 2") == 0,
          "name follows player_id");

    /* Nobody is finished yet. */
    for (int i = 0; i < m.standing_count; i++)
        CHECK(!m.standings[i].game_over, "nobody has topped out yet");

    /* Topping out flags the row without removing it - the others keep watching
     * the final figure. */
    op(&c, ADMIN_GAMEOVER, 0, 0, 800);
    CHECK(recv_latest(&a, &m, ADMIN_STANDINGS, 300) == 0,
          "standings after a top-out");
    CHECK(m.standings[0].player_id == 2, "c still leads");
    CHECK(m.standings[0].game_over, "c's row should be marked finished");

    sess_close(&a);
    sess_close(&b);
    sess_close(&c);
    return 0;
}

/* A duplicate report changes nothing, so it must not cost a broadcast. */
static int t_standings_no_broadcast_without_change(void)
{
    Sess a, b;
    int room = join(&a, 260, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 261, room) == room, "b join");
    op(&a, ADMIN_START, 0, 0, 0);
    drain(&a);
    drain(&b);

    op(&a, ADMIN_SCORE, 0, 3, 500);
    AdminMsg m;
    CHECK(recv_type(&b, &m, ADMIN_STANDINGS, 300) == 0,
          "first report broadcasts");
    drain(&b);

    op(&a, ADMIN_SCORE, 0, 3, 500); /* identical figures */
    CHECK(quiet(&b), "a report that changes nothing must not be broadcast");

    sess_close(&a);
    sess_close(&b);
    return 0;
}

/* Someone in another room must never see this room's scoreboard. */
static int t_standings_stay_in_the_room(void)
{
    Sess a, outsider;
    int room = join(&a, 270, 0);
    CHECK(room > 0, "a join");
    int other = join(&outsider, 271, 0);
    CHECK(other > 0 && other != room, "outsider in a different room");
    drain(&a);
    drain(&outsider);

    op(&a, ADMIN_SCORE, 0, 4, 900);
    CHECK(quiet(&outsider), "a different room's score must not leak");

    sess_close(&a);
    sess_close(&outsider);
    return 0;
}

/* A batch of simultaneous top-outs publishes the final room state once. */
static int t_gameover_batch_coalesces_standings(void)
{
    Sess a, b, c;
    int room = join(&a, 280, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 281, room) == room, "b join");
    CHECK(join(&c, 282, room) == room, "c join");
    op(&a, ADMIN_START, 0, 0, 0);
    drain(&a);
    drain(&b);
    drain(&c);

    op_deferred(&a, ADMIN_GAMEOVER, 0, 0, 300);
    op_deferred(&b, ADMIN_GAMEOVER, 0, 0, 200);
    op_deferred(&c, ADMIN_GAMEOVER, 0, 0, 100);
    room_flush_updates();

    AdminMsg standings, result;
    CHECK(recv_msg(&a, &standings, 300) == 0,
          "the final standings are published");
    CHECK(standings.type == ADMIN_STANDINGS,
          "final standings precede the result");
    CHECK(standings.standings[0].score == 300,
          "final standings preserve the winning score");
    CHECK(recv_msg(&a, &result, 300) == 0, "the result follows standings");
    CHECK(result.type == ADMIN_RESULT && result.winner == 0,
          "the result names the standings leader");
    CHECK(count_type(&a, ADMIN_STANDINGS) == 0,
          "no reset snapshot overwrites the final standings");
    CHECK(count_type(&b, ADMIN_STANDINGS) == 1,
          "every member gets one standings update");
    CHECK(count_type(&c, ADMIN_STANDINGS) == 1,
          "the final reporter also gets one standings update");

    sess_close(&a);
    sess_close(&b);
    sess_close(&c);
    return 0;
}

/* A top-out below the visible top eight does not change the wire payload. */
static int t_hidden_gameover_skips_duplicate_standings(void)
{
    Sess players[9];
    int room = join(&players[0], 290, 0);
    CHECK(room > 0, "first player joins");
    for (int i = 1; i < 9; i++)
        CHECK(join(&players[i], 290 + i, room) == room,
              "remaining player joins");
    AdminMsg standings;
    CHECK(recv_type(&players[8], &standings, ADMIN_STANDINGS, 300) == 0,
          "a hidden joiner still receives the visible standings");
    op(&players[0], ADMIN_START, 0, 0, 0);
    for (int i = 0; i < 9; i++)
        drain(&players[i]);

    op(&players[8], ADMIN_GAMEOVER, 0, 0, 0);
    CHECK(quiet(&players[0]),
          "an unchanged visible scoreboard must not be rebroadcast");

    for (int i = 0; i < 9; i++)
        sess_close(&players[i]);
    return 0;
}

/* ---- disconnect --------------------------------------------------------- */

static int t_close_promotes_new_owner(void)
{
    Sess a, b;
    int room = join(&a, 150, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 151, room) == room, "b join");

    client_close(a.rx); /* a's process died */
    close(a.rx);
    close(a.tx);
    a.rx = a.tx = -1;

    AdminMsg promo;
    CHECK(recv_type(&b, &promo, ADMIN_SESSION, 200) == 0,
          "b should be told it is now owner");
    CHECK(promo.type == ADMIN_SESSION, "promotion is an ADMIN_SESSION push");
    CHECK(promo.session.is_owner, "b must be promoted when the owner dies");

    op(&b, ADMIN_START, 0, 0, 0); /* the point of promoting it */
    AdminMsg seed;
    CHECK(recv_type(&b, &seed, ADMIN_SEED, 200) == 0,
          "promoted owner should be able to START");
    CHECK(seed.type == ADMIN_SEED, "START should produce a seed");

    sess_close(&b);
    return 0;
}

/* ---- scale -------------------------------------------------------------- */

/* Many clients in one room. Not a concurrency test - the admin is single
 * threaded - but it does check the bookkeeping holds up: ids stay unique, only
 * the first member owns the room, and the seed reaches every single member. */
#define CROWD 8

static int t_many_clients_one_room(void)
{
    Sess s[CROWD];
    int room = join(&s[0], 200, 0);
    CHECK(room > 0, "first join");

    for (int i = 1; i < CROWD; i++)
        CHECK(join(&s[i], 200 + i, room) == room,
              "everyone joins the same room");

    /* player ids must be unique and handed out in join order */
    for (int i = 0; i < CROWD; i++)
        drain(&s[i]);

    op(&s[0], ADMIN_START, 0, 0, 0);

    unsigned seed0 = 0;
    for (int i = 0; i < CROWD; i++)
    {
        AdminMsg m;
        CHECK(recv_type(&s[i], &m, ADMIN_SEED, 300) == 0,
              "every member must receive the seed");
        CHECK(m.type == ADMIN_SEED, "should be ADMIN_SEED");
        if (i == 0)
            seed0 = m.seed;
        CHECK(m.seed == seed0, "every member must get the SAME seed");
    }

    for (int i = 0; i < CROWD; i++)
        sess_close(&s[i]);
    return 0;
}

/* The full room tops out as one batch without filling a session socketpair. */
static int t_full_room_gameover_batch_stays_connected(void)
{
    static Sess players[MAX_SESSIONS];
    int room = join(&players[0], 300, 0);
    CHECK(room > 0, "first full-room player joins");
    drain(&players[0]);
    for (int i = 1; i < MAX_SESSIONS; i++)
    {
        CHECK(join(&players[i], 300 + i, room) == room,
              "full-room player joins");
        for (int j = 0; j <= i; j++)
            drain(&players[j]);
    }

    op(&players[0], ADMIN_START, 0, 0, 0);
    for (int i = 0; i < MAX_SESSIONS; i++)
        drain(&players[i]);

    for (int i = 0; i < MAX_SESSIONS; i++)
        op_deferred(&players[i], ADMIN_GAMEOVER, 0, 0,
                    MAX_SESSIONS - i);
    room_flush_updates();

    AdminMsg standings, result;
    CHECK(recv_msg(&players[0], &standings, 300) == 0 &&
              standings.type == ADMIN_STANDINGS,
          "full room receives final standings");
    CHECK(standings.standings[0].player_id == 0 &&
              standings.standings[0].score == MAX_SESSIONS,
          "full-room standings preserve the winner");
    CHECK(recv_msg(&players[0], &result, 300) == 0 &&
              result.type == ADMIN_RESULT && result.winner == 0,
          "full room receives the matching result");
    CHECK(client_take_wedged() == -1,
          "the coalesced full-room batch wedges no session");

    for (int i = MAX_SESSIONS - 1; i >= 0; i--)
    {
        sess_close(&players[i]);
        for (int j = 0; j < i; j++)
            drain(&players[j]);
    }
    return 0;
}

/* ---- ordering: the cases that worry me ---------------------------------- */

/*
 * A client that joins AFTER the room started is added with phase WAITING and
 * never receives the seed - handle_join does not look at r->phase. It is now a
 * member who can never reach GAME_OVER, and handle_gameover only fires once
 * EVERY member is GAME_OVER. So one late joiner wedges the room permanently.
 *
 * Correct behaviour is either to refuse the join, or to admit them in a way
 * that does not block the running match. Either way the result must arrive.
 */
static int t_late_joiner_does_not_wedge_room(void)
{
    Sess a, b, late;
    int room = join(&a, 210, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 211, room) == room, "b join");
    op(&a, ADMIN_START, 0, 0, 0);
    drain(&a);
    drain(&b);

    (void)join(&late, 212, room); /* arrives mid-match */
    drain(&late);

    op(&a, ADMIN_GAMEOVER, 0, 0, 400);
    op(&b, ADMIN_GAMEOVER, 0, 0, 700);

    AdminMsg r;
    CHECK(recv_type(&a, &r, ADMIN_RESULT, 300) == 0,
          "the match must still resolve once the real players finish");
    CHECK(r.type == ADMIN_RESULT, "type RESULT");

    sess_close(&a);
    sess_close(&b);
    sess_close(&late);
    return 0;
}

/*
 * Everyone else has topped out and the last player quits instead of finishing.
 * Nothing re-runs the "is everyone done?" check on LEAVE, so the room never
 * announces a result and the survivors sit in GAME_OVER forever.
 */
static int t_leave_during_game_resolves_room(void)
{
    Sess a, b;
    int room = join(&a, 220, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 221, room) == room, "b join");
    op(&a, ADMIN_START, 0, 0, 0);
    drain(&a);
    drain(&b);

    op(&a, ADMIN_GAMEOVER, 0, 0, 600); /* a is done */
    drain(&a);
    op(&b, ADMIN_LEAVE, 0, 0, 0); /* b quits rather than topping out */
    drain(&b);

    AdminMsg standings, result;
    CHECK(recv_msg(&a, &standings, 300) == 0 &&
              standings.type == ADMIN_STANDINGS &&
              standings.standings[0].score == 600,
          "leave publishes the final standings first");
    CHECK(recv_msg(&a, &result, 300) == 0 &&
              result.type == ADMIN_RESULT,
          "the last player leaving then resolves the room");
    AdminMsg session;
    CHECK(recv_type(&a, &session, ADMIN_SESSION, 300) == 0,
          "leave publishes the updated roster");
    CHECK(count_type(&a, ADMIN_STANDINGS) == 0,
          "leave does not overwrite the final standings");

    sess_close(&a);
    sess_close(&b);
    return 0;
}

/*
 * Same situation, but the last player's PROCESS DIES rather than leaving
 * cleanly. That is a different code path (client_close, not handle_leave) and
 * it is the likelier one in practice - a crash, a killed terminal, a dropped
 * connection. The survivors must still get their result.
 */
static int t_disconnect_during_game_resolves_room(void)
{
    Sess a, b;
    int room = join(&a, 230, 0);
    CHECK(room > 0, "a join");
    CHECK(join(&b, 231, room) == room, "b join");
    op(&a, ADMIN_START, 0, 0, 0);
    drain(&a);
    drain(&b);

    op(&a, ADMIN_GAMEOVER, 0, 0, 250); /* a topped out */
    drain(&a);

    client_close(b.rx); /* b's process died mid-game */
    room_flush_updates();
    close(b.rx);
    close(b.tx);
    b.rx = b.tx = -1;

    AdminMsg standings, result;
    CHECK(recv_msg(&a, &standings, 300) == 0 &&
              standings.type == ADMIN_STANDINGS &&
              standings.standings[0].score == 250,
          "disconnect publishes the final standings first");
    CHECK(recv_msg(&a, &result, 300) == 0 &&
              result.type == ADMIN_RESULT && result.winner == 0,
          "the last player disconnecting then resolves the room");
    AdminMsg session;
    CHECK(recv_type(&a, &session, ADMIN_SESSION, 300) == 0,
          "disconnect publishes the updated roster");
    CHECK(count_type(&a, ADMIN_STANDINGS) == 0,
          "disconnect does not overwrite the final standings");

    sess_close(&a);
    return 0;
}

/* ---- capacity ----------------------------------------------------------- */

/*
 * MAX_SESSIONS clients are already registered, so client_alloc returns NULL for
 * the next one and client_add drops it silently. That session is then invisible
 * to client_handle, so its JOIN produces no reply at all and its client waits
 * forever. A server at capacity has to say so.
 *
 * Note MAX_ROOM_MEMBERS == MAX_SESSIONS, so "room full" is unreachable - the
 * session table always fills first. That is worth fixing or documenting.
 */
static int t_full_server_answers_anyway(void)
{
    static Sess pool[MAX_SESSIONS];
    int opened = 0;
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        if (sess_open(&pool[i], 1000 + i) != 0)
            break;
        opened++;
    }

    Sess over;
    int rc = sess_open(&over, 9999); /* one too many */

    int ok = 0, right_reason = 0;
    if (rc == 0)
    {
        /* The refusal comes at registration time, not on the first command:
         * a session that cannot be registered has nothing to command with. */
        AdminMsg m;
        ok = (recv_msg(&over, &m, 200) == 0);
        right_reason = ok && m.type == ADMIN_REJECT && m.reason == REJECT_FULL;
        sess_close(&over);
    }

    for (int i = 0; i < opened; i++)
        sess_close(&pool[i]);

    CHECK(opened == MAX_SESSIONS, "should have been able to open MAX_SESSIONS");
    CHECK(ok, "a client arriving at a full server must get some reply");
    CHECK(right_reason, "and it should be ADMIN_REJECT with REJECT_FULL");
    return 0;
}

/* ---- main --------------------------------------------------------------- */

/*
 * t_full_server_answers_anyway opens MAX_SESSIONS socketpairs (2 fds each) in
 * one case. macOS's default per-process limit is 256, well under that, so a
 * plain shell fails this case on an OS fd limit, not a room.c defect. Raise
 * the soft limit to the hard limit up front so the suite does not depend on
 * the caller's ulimit. Best effort: a sandbox that caps the hard limit too
 * just leaves the limit as found, and the case fails as it did before.
 */
static void raise_fd_limit(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
        return;
    rl.rlim_cur = rl.rlim_max;
    (void)setrlimit(RLIMIT_NOFILE, &rl);
}

int main(void)
{
    raise_fd_limit();
    test_output_begin("test_room");

    RUN(t_join_new_room_owns_it);
    RUN(t_join_second_is_not_owner);
    RUN(t_join_twice_refused);
    RUN(t_two_new_rooms_are_distinct);

    RUN(t_leave_promotes_new_owner);
    RUN(t_leave_without_room_still_replies);

    RUN(t_start_broadcasts_one_seed);
    RUN(t_start_owner_only);
    RUN(t_start_twice_ignored);

    RUN(t_garbage_hits_one_opponent);
    RUN(t_garbage_target_varies);
    RUN(t_garbage_skips_finished_players);
    RUN(t_garbage_alone_in_room);
    RUN(t_garbage_nonpositive_dropped);

    RUN(t_gameover_waits_for_all);
    RUN(t_gameover_twice_is_safe);
    RUN(t_rematch_after_round);

    RUN(t_wedged_session_is_reported);

    RUN(t_roster_reaches_everyone);
    RUN(t_roster_follows_ownership);
    RUN(t_roster_cleared_on_leave);

    RUN(t_standings_ranked_and_broadcast);
    RUN(t_standings_no_broadcast_without_change);
    RUN(t_standings_stay_in_the_room);
    RUN(t_gameover_batch_coalesces_standings);
    RUN(t_hidden_gameover_skips_duplicate_standings);

    RUN(t_close_promotes_new_owner);

    RUN(t_many_clients_one_room);
    RUN(t_full_room_gameover_batch_stays_connected);

    RUN(t_late_joiner_does_not_wedge_room);
    RUN(t_leave_during_game_resolves_room);
    RUN(t_disconnect_during_game_resolves_room);
    RUN(t_full_server_answers_anyway);

    test_output_summary(cases_run, regressions + gaps, 0);
    return regressions + gaps;
}
