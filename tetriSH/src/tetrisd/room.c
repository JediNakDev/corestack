/*
 * room.c - room table + message routing (admin thread only, no locks).
 *
 * Each Room keeps the list of its members (their session fds), so broadcasts
 * and garbage routing iterate the room's own members instead of scanning every
 * client. A client's room_id/player_id/owner flag live in its SessionState;
 * join/leave keep the two views in sync.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <stdbool.h>
#include <stdio.h> /* snprintf */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tetrisd/room.h"
#include "libtetrisutil/logmsg.h"

/* Admin-side record of one connected session. */
typedef struct
{
    int fd; /* socketpair to the session; -1 = free slot */
    pid_t pid;
    SessionState state; /* phase, room_id, player_id, is_owner       */

    /*
     * Live scoreboard figures, updated by ADMIN_SCORE as the player clears
     * lines - not just the final total at game over. These are what the room
     * publishes to everyone so each client can watch the others' progress.
     */
    int score;
    int lines;

    /*
     * Display name. Today the server assigns "Player N" on join, because no
     * client ever sends one. It is stored per-client rather than formatted at
     * broadcast time so that real usernames later become a change to what
     * writes this field, and nothing else.
     */
    char name[MAX_USER_NAME];
} Client;

/* One active room, owning the list of its members. */
typedef struct
{
    int id;                        /* room number; 0 = free slot   */
    SessionPhase phase;            /* WAITING, then PLAYING         */
    unsigned seed;                 /* shared piece-stream seed      */
    int next_player_id;            /* ids handed out in join order  */
    int members[MAX_ROOM_MEMBERS]; /* member session fds            */
    int count;                     /* number of members             */
    bool standings_dirty;          /* publish after the event batch */
    bool have_standings;           /* last payload is initialized   */
    AdminMsg last_standings;       /* suppress identical snapshots  */
} Room;

static Client g_clients[MAX_SESSIONS];
static Room g_rooms[MAX_ROOMS];
static bool g_inited = false;
static bool g_seeded = false;

/* ---- setup -------------------------------------------------------------- */

static void ensure_init(void)
{
    if (g_inited)
        return;
    for (int i = 0; i < MAX_SESSIONS; i++)
        g_clients[i].fd = -1; /* 0 is a valid fd */
    for (int i = 0; i < MAX_ROOMS; i++)
    {
        g_rooms[i].id = 0;
        g_rooms[i].count = 0;
    }
    g_inited = true;
}

/* Fresh, well-distributed 32-bit seed. Seeded once from time + pid so every
 * server run - and every game - gets a different stream. */
static unsigned gen_seed(void)
{
    if (!g_seeded)
    {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        g_seeded = true;
    }
    /* rand() yields only 15-31 bits; combine calls for a full 32-bit value. */
    return ((unsigned)rand() << 17) ^ ((unsigned)rand() << 2) ^
           (unsigned)rand();
}

/* ---- lookups ------------------------------------------------------------ */

static Client *client_find(int fd)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (g_clients[i].fd == fd)
            return &g_clients[i];
    return NULL;
}

static Client *client_alloc(int fd, pid_t pid)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        if (g_clients[i].fd < 0)
        {
            Client *c = &g_clients[i];
            c->fd = fd;
            c->pid = pid;
            c->state.phase = SESSION_IDLE;
            c->state.room_id = ROOM_NONE;
            c->state.player_id = 0;
            c->state.is_owner = false;
            /* Slots are recycled, so a fresh client would otherwise inherit
             * the previous occupant's final score and could win a round it
             * never played. */
            c->score = 0;
            return c;
        }
    }
    return NULL; /* table full */
}

static Room *room_find(int id)
{
    if (id <= 0)
        return NULL;
    for (int i = 0; i < MAX_ROOMS; i++)
        if (g_rooms[i].id == id)
            return &g_rooms[i];
    return NULL;
}

/* Create a room. id > 0 uses that number; id == 0 picks the smallest free one.
 */
static Room *room_create(int id)
{
    if (id == 0)
    {
        for (int cand = 1;; cand++)
            if (!room_find(cand))
            {
                id = cand;
                break;
            }
    }
    for (int i = 0; i < MAX_ROOMS; i++)
    {
        if (g_rooms[i].id == 0)
        {
            Room *r = &g_rooms[i];
            r->id = id;
            r->phase = SESSION_WAITING;
            r->seed = 0;
            r->next_player_id = 0;
            r->count = 0;
            r->standings_dirty = false;
            r->have_standings = false;
            return r;
        }
    }
    return NULL; /* room table full */
}

/* ---- room membership ---------------------------------------------------- */

static bool room_add_member(Room *r, int fd)
{
    if (r->count >= MAX_ROOM_MEMBERS)
        return false; /* room full */
    r->members[r->count++] = fd;
    return true;
}

static void room_del_member(Room *r, int fd)
{
    for (int i = 0; i < r->count; i++)
    {
        if (r->members[i] == fd)
        {
            r->members[i] = r->members[--(r->count)]; /* swap-remove */
            return;
        }
    }
}

/* ---- sending ------------------------------------------------------------ */

/*
 * Sessions whose socketpair filled up, waiting for the admin to close them.
 * room.c cannot close an fd itself: the admin owns the poll set, and freeing
 * the number behind its back lets the listener reuse it immediately.
 */
#define WEDGED_MAX 16
static int g_wedged[WEDGED_MAX];
static int g_wedged_n = 0;

/* Dedup: one broadcast can hit the same bad session several times. */
static void mark_wedged(int fd)
{
    for (int i = 0; i < g_wedged_n; i++)
        if (g_wedged[i] == fd)
            return;
    if (g_wedged_n < WEDGED_MAX)
        g_wedged[g_wedged_n++] = fd;
}

int client_take_wedged(void)
{
    if (g_wedged_n == 0)
        return -1;
    int fd = g_wedged[0];
    for (int i = 1; i < g_wedged_n; i++)
        g_wedged[i - 1] = g_wedged[i];
    g_wedged_n--;
    return fd;
}

static void send_msg(int fd, const AdminMsg *m)
{
    /* A full socketpair means this session is finished, not merely slow. The
     * message cannot just be skipped - a partial write desyncs the stream - so
     * record the fd and let the admin close it. -1 is left alone: that fd is
     * about to report POLLHUP and be dropped by the normal path. */
    if (adminmsg_write(fd, m) == ADMINMSG_WOULDBLOCK)
        mark_wedged(fd);
}

/* Push a session its current SessionState (phase / room / owner). */
static void send_session_ex(const Client *c, bool created);

static void send_session(const Client *c)
{
    send_session_ex(c, false);
}

/* As send_session, but marks the message as answering a room-creating JOIN.
 * Only the creator's own copy carries the flag; the other members' pushes
 * describe the same room but answer nothing. */
static void send_session_ex(const Client *c, bool created)
{
    AdminMsg m;
    memset(&m, 0, sizeof m);
    m.type = ADMIN_SESSION;
    m.session = c->state;
    m.created = created;
    send_msg(c->fd, &m);
}

/*
 * Tell one client its command was refused.
 *
 * Every rejection path below used to be a bare `return`. Combined with the
 * (then missing) UPD_SESSION push, that made a refused JOIN look exactly like a
 * successful one from the client's side - it sent a command and heard nothing
 * either way. `reason` is an htttp status; the session forwards it verbatim.
 */
static void send_reject(const Client *c, int reason)
{
    AdminMsg m;
    memset(&m, 0, sizeof m);
    m.type = ADMIN_REJECT;
    m.reason = reason;
    send_msg(c->fd, &m);
}

static void room_broadcast(const Room *r, const AdminMsg *m)
{
    for (int i = 0; i < r->count; i++)
        send_msg(r->members[i], m);
}

/*
 * Publish the room's roster to every member.
 *
 * Not send_session(c): that reaches one client, which is right for a phase or
 * ownership change because those are personal. A roster is room-wide - when
 * somebody joins, the people ALREADY in the room are the ones who need the new
 * list, and they are exactly who send_session would miss. The symptom of
 * getting this wrong is quiet: the joiner sees everyone, and nobody sees the
 * joiner.
 *
 * Built in join order (player_id ascending) rather than r->members order,
 * because room_del_member swap-removes and so leaves that array unordered. A
 * lobby list that reordered itself when an unrelated player left would be hard
 * to read.
 *
 * Reads the same Client records as room_push_standings, and takes the name and
 * id from the same fields, so the lobby and the scoreboard cannot disagree
 * about who is present or what they are called.
 */
/* creator_fd is the member whose JOIN created this room, or -1. Its own push
 * is the answer to that JOIN and carries created=true; everyone else's is an
 * unsolicited roster update. */
static void room_push_sessions_ex(Room *r, int creator_fd)
{
    if (!r || r->id == 0 || r->count == 0)
        return;

    RoomMember roster[MAX_STANDINGS];
    int n = 0;

    for (int i = 0; i < r->count; i++)
    {
        const Client *c = client_find(r->members[i]);
        if (!c)
            continue;

        RoomMember rm;
        memset(&rm, 0, sizeof rm);
        rm.player_id = c->state.player_id;
        rm.is_owner = c->state.is_owner;
        snprintf(rm.name, sizeof rm.name, "%s", c->name);

        /* Bounded insertion, so an over-large room keeps the lowest ids
         * rather than whichever eight happen to sit first in members[]. */
        int pos = 0;
        while (pos < n && roster[pos].player_id < rm.player_id)
            pos++;
        if (pos >= MAX_STANDINGS)
            continue;

        int last = (n < MAX_STANDINGS) ? n : MAX_STANDINGS - 1;
        for (int k = last; k > pos; k--)
            roster[k] = roster[k - 1];
        roster[pos] = rm;
        if (n < MAX_STANDINGS)
            n++;
    }

    /* Each member gets its OWN SessionState - phase and is_owner differ per
     * client - carrying the one shared roster. */
    for (int i = 0; i < r->count; i++)
    {
        Client *c = client_find(r->members[i]);
        if (!c)
            continue;
        memcpy(c->state.roster, roster, (size_t)n * sizeof roster[0]);
        c->state.roster_count = n;
        send_session_ex(c, c->fd == creator_fd);
    }
}

static void room_push_sessions(Room *r)
{
    room_push_sessions_ex(r, -1);
}

/*
 * Ranking rule for the scoreboard: higher score first, ties broken on the
 * lower player_id.
 *
 * Deliberately the SAME rule room_check_round_end uses to pick the winner. If
 * these two ever diverged, the scoreboard would show one player on top all
 * round and the result screen would then announce someone else - and the
 * scoreboard is the version every player has been watching.
 */
static bool standing_ranks_before(const PlayerStanding *a,
                                  const PlayerStanding *b)
{
    if (a->score != b->score)
        return a->score > b->score;
    return a->player_id < b->player_id;
}

/*
 * Publish the room's scoreboard to every member, ranked.
 *
 * Called whenever anything a client can see changes: a score, or the
 * membership itself. Sessions cannot compute this - each knows only its own
 * board - so the admin thread is the only place it can be assembled.
 *
 * Insertion into a fixed MAX_STANDINGS-sized array, rather than sorting a copy
 * of the whole room: a room may hold up to MAX_ROOM_MEMBERS (254) members, and
 * building a 254-entry array on the admin thread's stack to then throw all but
 * eight away is wasted work on a path that runs on every line clear. This
 * walks every member but never holds more than the eight it will send, so an
 * over-large room still reports the genuine top eight rather than whoever
 * happened to join first.
 */
static void room_push_standings(Room *r)
{
    if (!r || r->id == 0 || r->count == 0)
        return;

    AdminMsg m;
    memset(&m, 0, sizeof m);
    m.type = ADMIN_STANDINGS;

    for (int i = 0; i < r->count; i++)
    {
        const Client *c = client_find(r->members[i]);
        if (!c)
            continue;

        PlayerStanding st;
        memset(&st, 0, sizeof st);
        st.player_id = c->state.player_id;
        st.score = c->score;
        st.lines = c->lines;
        st.game_over = (c->state.phase == SESSION_GAME_OVER);
        snprintf(st.name, sizeof st.name, "%s", c->name);

        /* Where does this player belong among the ones kept so far? */
        int pos = 0;
        while (pos < m.standing_count &&
               standing_ranks_before(&m.standings[pos], &st))
            pos++;

        if (pos >= MAX_STANDINGS)
            continue; /* outranked by a full board */

        /* Shift the tail down one, dropping the last if we are already full. */
        int last = (m.standing_count < MAX_STANDINGS) ? m.standing_count
                                                      : MAX_STANDINGS - 1;
        for (int k = last; k > pos; k--)
            m.standings[k] = m.standings[k - 1];

        m.standings[pos] = st;
        if (m.standing_count < MAX_STANDINGS)
            m.standing_count++;
    }

    if (r->have_standings && memcmp(&r->last_standings, &m, sizeof m) == 0)
        return;

    r->last_standings = m;
    r->have_standings = true;
    room_broadcast(r, &m);
}

static void room_mark_standings_dirty(Room *r)
{
    if (r)
        r->standings_dirty = true;
}

void room_flush_updates(void)
{
    ensure_init();
    for (int i = 0; i < MAX_ROOMS; i++)
    {
        Room *r = &g_rooms[i];
        if (!r->standings_dirty)
            continue;
        r->standings_dirty = false;
        if (r->id != 0)
            room_push_standings(r);
    }
}

/* ---- operation handlers ------------------------------------------------- */

/* Defined below with the game-over logic it belongs to; declared here because
 * the leave paths above the definition have to run it too. */
static bool room_check_round_end(Room *r);

/* JOIN(room_id): 0 = create a new room, >0 = join that room (creating it if it
 * does not exist yet). First member of a room becomes its owner. */
static int handle_join(Client *c, const AdminMsg *msg)
{
    if (c->state.room_id != ROOM_NONE)
    { /* already in a room */
        send_reject(c, REJECT_CONFLICT);
        return REJECT_CONFLICT;
    }

    int want = msg->room_id;
    Room *r = (want == 0) ? NULL : room_find(want);
    bool created = r == NULL;

    /*
     * Refuse to join a round already in progress.
     *
     * ADMIN_SEED was broadcast when that round started, so a late joiner never
     * receives it: it would sit in SESSION_WAITING with no board while the
     * others played, unable to do anything until the round ended. Rejecting is
     * honest; silently admitting them was not.
     */
    if (r && r->phase == SESSION_PLAYING)
    {
        send_reject(c, REJECT_CONFLICT);
        return REJECT_CONFLICT;
    }

    if (!r)
        r = room_create(want);
    if (!r)
    { /* room table full */
        send_reject(c, REJECT_FULL);
        return REJECT_FULL;
    }
    if (!room_add_member(r, c->fd))
    { /* room full */
        send_reject(c, REJECT_FULL);
        return REJECT_FULL;
    }

    c->state.room_id = r->id;
    c->state.player_id = r->next_player_id++;
    c->state.is_owner = (r->count == 1); /* first member = owner */
    c->state.phase = SESSION_WAITING;

    /* Use the supplied name if non-empty, else Player %d. Empty is what
     * today's clients send and what a guest sends, so this is unchanged
     * behaviour until msg->name is ever non-empty (libtetrisauth). */
    if (msg->name[0] != '\0')
        snprintf(c->name, sizeof c->name, "%s", msg->name);
    else
        snprintf(c->name, sizeof c->name, "Player %d", c->state.player_id);

    /* This client starts the round on zero regardless of what its slot held. */
    c->score = 0;
    c->lines = 0;
    (void)log_send(
        LOG_INFO,
        "operation=handle_join phase=complete room=%d player=%d pid=%ld "
        "owner=%d members=%d created=%d status=0",
        r->id, c->state.player_id, (long)c->pid, c->state.is_owner, r->count,
        created);

    /*
     * room_push_sessions covers this client's own ADMIN_SESSION as well as
     * everyone else's, so there is no separate send_session(c) here - it would
     * just send the same client a second, roster-less copy first.
     */
    room_push_sessions_ex(r, created ? c->fd : -1);

    /* The new arrival has never received this room's standings, even when
     * joining below the visible top eight leaves the payload unchanged. */
    r->have_standings = false;
    room_mark_standings_dirty(r);
    return 0;
}

static void handle_leave(Client *c)
{
    int room_id = c->state.room_id;
    int player_id = c->state.player_id;
    bool was_owner = c->state.is_owner;
    if (room_id == ROOM_NONE)
    {
        send_session(c);
        return;
    }

    Room *r = room_find(room_id);
    if (r)
        room_del_member(r, c->fd);

    c->state.room_id = ROOM_NONE;
    c->state.player_id = 0;
    c->state.is_owner = false;
    c->state.phase = SESSION_IDLE;
    c->score = 0; /* not this room's business any more */
    /* The roster we were carrying belongs to a room we are no longer in; the
     * lobby must not keep showing it. */
    c->state.roster_count = 0;
    send_session(c); /* now idle */
    (void)log_send(LOG_INFO,
                   "operation=handle_leave phase=complete room=%d player=%d "
                   "pid=%ld owner=%d status=0",
                   room_id, player_id, (long)c->pid, was_owner);

    if (r)
    {
        if (r->count == 0)
        {
            r->id = 0; /* free the empty room */
            (void)log_send(LOG_INFO,
                           "operation=handle_leave event=room_removed room=%d "
                           "status=0",
                           room_id);
        }
        else
        {
            if (was_owner) /* promote a new owner */
            {
                Client *o = client_find(r->members[0]);
                if (o)
                {
                    o->state.is_owner = true;
                    (void)log_send(
                        LOG_INFO,
                        "operation=handle_leave event=owner_changed room=%d "
                        "player=%d status=0",
                        r->id, o->state.player_id);
                }
                /* No send_session(o) here: room_push_sessions below tells the
                 * whole room, and the promotion has to reach everyone anyway -
                 * the owner marker moves on every player's roster, not just
                 * the new owner's. */
            }
            /* Walking out mid-round can be what ends it: everyone left behind
             * may already have topped out. */
            bool round_ended = room_check_round_end(r);

            /* One fewer row on everyone's roster and board. Sent after the
             * round-end check. A completed round already published and cached
             * its final board before the reset, so do not overwrite it. */
            room_push_sessions(r);
            if (!round_ended)
                room_mark_standings_dirty(r);
        }
    }
}

/* START: owner only. Roll a fresh seed and hand the SAME seed to every member
 * so all boards generate the identical piece stream. */
static int handle_start(Client *c)
{
    if (c->state.room_id == ROOM_NONE)
    { /* not in a room */
        send_reject(c, REJECT_CONFLICT);
        return REJECT_CONFLICT;
    }
    if (!c->state.is_owner)
    { /* only the owner may start */
        send_reject(c, REJECT_NOT_OWNER);
        return REJECT_NOT_OWNER;
    }

    Room *r = room_find(c->state.room_id);
    if (!r || r->phase == SESSION_PLAYING)
    { /* already running */
        send_reject(c, REJECT_CONFLICT);
        return REJECT_CONFLICT;
    }

    r->seed = gen_seed();
    r->phase = SESSION_PLAYING;
    (void)log_send(LOG_INFO,
                   "operation=handle_start phase=complete room=%d members=%d "
                   "seed=%u status=0",
                   r->id, r->count, r->seed);

    for (int i = 0; i < r->count; i++)
    { /* admin's own view */
        Client *m = client_find(r->members[i]);
        if (m)
            m->state.phase = SESSION_PLAYING;
    }

    AdminMsg m; /* broadcast the shared seed */
    memset(&m, 0, sizeof m);
    m.type = ADMIN_SEED;
    m.seed = r->seed;
    room_broadcast(r, &m);

    /* Everyone is PLAYING again, so every row's game_over flag has just gone
     * false. Without this the previous round's finishers would still show as
     * greyed out for the whole of the new one. */
    room_mark_standings_dirty(r);
    return 0;
}

/* GARBAGE(lines): forward as RECV_GARBAGE to one random other member. */
static void handle_garbage(Client *c, const AdminMsg *msg)
{
    if (c->state.room_id == ROOM_NONE)
        return;
    if (msg->lines <= 0)
        return;

    Room *r = room_find(c->state.room_id);
    if (!r)
        return;

    /* Gather the eligible targets before choosing. Picking an index into
     * r->members and re-rolling on a skip would bias toward whoever follows a
     * skipped slot. Players who already topped out are not targets. */
    int cand[MAX_ROOM_MEMBERS];
    int n = 0;

    int candidate_real=-1;
    int score_m=-1;

    for (int i = 0; i < r->count; i++)
    {
        if (r->members[i] == c->fd)
            continue; /* never the sender */
        Client *t = client_find(r->members[i]);
        if (!t || t->state.phase != SESSION_PLAYING)
            continue;

        cand[n++] = r->members[i];

        if(score_m==-1 || t->score > score_m)
        {
            candidate_real=r->members[i];
            score_m=t->score;
        }

    }
    if (n == 0)
        return; /* nobody left to hit */

    AdminMsg out;
    memset(&out, 0, sizeof out);
    out.type = ADMIN_RECV_GARBAGE;
    out.lines = msg->lines;

    //send_msg(cand[gen_seed() % (unsigned)n], &out);

    send_msg(candidate_real, &out);
}

/*
 * Has this round finished? If so, announce the result and reset the room.
 *
 * "Finished" means every REMAINING member has topped out - so this must be
 * re-checked whenever the membership shrinks, not only when a player reports a
 * game over. A player who leaves or drops mid-round is the last one still
 * playing as far as this test is concerned; without the re-check the members
 * who already topped out sat in SESSION_GAME_OVER forever, waiting for a result
 * that could no longer arrive. That is the "a round can stop ending" bug: the
 * board stops updating (the session only pushes UPD_GAME while PLAYING) and no
 * UPD_RESULT ever follows.
 *
 * The room phase guard keeps it honest: a room in WAITING has no round to end,
 * so a departure from the lobby announces nothing.
 */
static bool room_check_round_end(Room *r)
{
    if (!r || r->phase != SESSION_PLAYING)
        return false;
    if (r->count == 0)
        return false; /* nobody left to tell */

    /* still someone playing? then the room is not over yet. */
    for (int i = 0; i < r->count; i++)
    {
        Client *m = client_find(r->members[i]);
        if (m && m->state.phase != SESSION_GAME_OVER)
            return false;
    }

    /* everyone finished -> highest score wins */
    int winner = -1, best = -1;
    for (int i = 0; i < r->count; i++)
    {
        Client *m = client_find(r->members[i]);
        if (m && (m->score > best ||
                  (m->score == best && m->state.player_id < winner)))
        {
            best = m->score;
            winner = m->state.player_id;
        }
    }
    /*
     * Tie-break on the lowest player_id, which is join order.
     *
     * `>` alone made the winner depend on the order members happen to sit in
     * r->members[], which room_del_member reshuffles - so two identical games
     * could name different winners. Join order is arbitrary but stable and
     * explainable ("first to join wins a tie").
     */

    /* Preserve the table that decided the winner before resetting internal
     * figures for the next round. This is the one game-over update that must
     * not wait for the end-of-batch flush. */
    r->standings_dirty = false;
    room_push_standings(r);

    AdminMsg res; /* announce the result after its supporting standings */
    memset(&res, 0, sizeof res);
    res.type = ADMIN_RESULT;
    res.winner = winner;
    room_broadcast(r, &res);
    (void)log_send(
        LOG_INFO,
        "operation=room_check_round_end event=ended room=%d winner=%d "
        "score=%d members=%d status=0",
        r->id, winner, best, r->count);
    /* reset the room so the owner can START another round */
    r->phase = SESSION_WAITING;
    for (int i = 0; i < r->count; i++)
    {
        Client *m = client_find(r->members[i]);
        if (m)
        {
            m->state.phase = SESSION_WAITING;
            /* Stale figures must not decide the next round, nor linger on
             * anyone's scoreboard once it starts. */
            m->score = 0;
            m->lines = 0;
        }
    }
    (void)log_send(LOG_INFO,
                   "operation=room_check_round_end event=reset room=%d "
                   "status=0",
                   r->id);

    /* Keep the published final table visible while the room waits. START
     * marks standings dirty and publishes the reset figures for the rematch. */
    return true;
}

/*
 * SCORE: a player's score/lines moved. Record it and republish the board.
 *
 * Both figures change only when a line is cleared, so sessions send this a
 * handful of times per round rather than per tick - which is what keeps this
 * fan-out (one message to every member, per change) affordable.
 */
static void handle_score(Client *c, const AdminMsg *msg)
{
    if (c->state.room_id == ROOM_NONE)
        return;

    /* Ignore a report that changes nothing: a duplicate would otherwise cost
     * a broadcast to the whole room for no visible difference. */
    if (c->score == msg->score && c->lines == msg->lines)
        return;

    c->score = msg->score;
    c->lines = msg->lines;

    room_mark_standings_dirty(room_find(c->state.room_id));
}

/* GAMEOVER: a player topped out and reported its final score. The room is only
 * "over" once EVERY member has finished; the highest score then wins (this is a
 * score race, not last-survivor). */
static void handle_gameover(Client *c, const AdminMsg *msg)
{
    if (c->state.room_id == ROOM_NONE)
        return;

    c->state.phase = SESSION_GAME_OVER;
    c->score = msg->score; /* remember the reported score */
    (void)log_send(LOG_INFO,
                   "operation=handle_gameover phase=complete room=%d "
                   "player=%d score=%d status=0",
                   c->state.room_id, c->state.player_id, msg->score);

    Room *r = room_find(c->state.room_id);
    room_mark_standings_dirty(r);
    (void)room_check_round_end(r);
}

/* ---- public API --------------------------------------------------------- */

void client_add(int fd, pid_t pid)
{
    ensure_init();
    if (client_alloc(fd, pid) != NULL)
    { /* starts IDLE; the session knows */
        log_send(LOG_INFO,
                 "operation=client_add phase=complete session_pid=%ld status=0",
                 (long)pid);
        return;
    }

    /*
     * Session table full.
     *
     * This used to be a bare drop, and it was the worst kind: the session had
     * completed its handshake and looked connected, but was invisible to
     * client_handle, so every command it ever sent was silently ignored. The
     * player saw a lobby that never responded.
     *
     * Written straight to the fd rather than through send_reject(), which
     * needs a Client we could not allocate - that is the whole problem here.
     */
    AdminMsg m;
    memset(&m, 0, sizeof m);
    m.type = ADMIN_REJECT;
    m.reason = REJECT_FULL;
    send_msg(fd, &m);
    log_send(LOG_WARN,
             "operation=client_add phase=complete session_pid=%ld status=%d",
             (long)pid, REJECT_FULL);
}

void client_handle(int fd, const AdminMsg *msg)
{
    Client *c = client_find(fd);
    if (!c)
    {
        (void)log_send(LOG_WARN,
                       "operation=client_handle phase=complete status=404");
        return;
    }

    int status = 0;
    switch (msg->type)
    {
    case ADMIN_JOIN:
        status = handle_join(c, msg);
        break;
    case ADMIN_LEAVE:
        handle_leave(c);
        break;
    case ADMIN_START:
        status = handle_start(c);
        break;
    case ADMIN_GARBAGE:
        handle_garbage(c, msg);
        break;
    case ADMIN_GAMEOVER:
        handle_gameover(c, msg);
        break;
    case ADMIN_SCORE:
        handle_score(c, msg);
        break;
    default:
        status = 400;
        break; /* admin->session types shouldn't arrive here */
    }
    if (status != 0)
    {
        (void)log_send(LOG_WARN,
                       "operation=client_handle phase=complete status=%d",
                       status);
        return;
    }
}

void client_close(int fd)
{
    Client *c = client_find(fd);
    if (!c)
    {
        log_send(LOG_WARN, "operation=client_close phase=complete status=404");
        return;
    }

    int room_id = c->state.room_id;
    bool was_owner = c->state.is_owner;

    if (room_id != ROOM_NONE)
    {
        Room *r = room_find(room_id);
        if (r)
        {
            room_del_member(r, fd);
            if (r->count == 0)
            {
                r->id = 0; /* free the empty room */
                (void)log_send(
                    LOG_INFO,
                    "operation=client_close event=room_removed room=%d "
                    "status=0",
                    room_id);
            }
            else
            {
                if (was_owner)
                { /* promote a new owner */
                    Client *o = client_find(r->members[0]);
                    if (o)
                    {
                        o->state.is_owner = true;
                        (void)log_send(
                            LOG_INFO,
                            "operation=client_close event=owner_changed "
                            "room=%d player=%d status=0",
                            r->id, o->state.player_id);
                    }
                    /* Announced by room_push_sessions below: the owner marker
                     * moves on everyone's roster, not just the new owner's. */
                }
                /* A player who drops mid-round is gone for good: it has just
                 * been taken out of r->members, so if everyone still in the
                 * room has topped out, this is what ends the round. */
                bool round_ended = room_check_round_end(r);

                /* Their row goes with them - the survivors should not be left
                 * watching a name and a score that can no longer change. A
                 * completed round already published its final board. */
                room_push_sessions(r);
                if (!round_ended)
                    room_mark_standings_dirty(r);
            }
        }
    }

    c->fd = -1; /* free the slot (admin thread closes the fd) */
    log_send(LOG_INFO,
             "operation=client_close phase=complete room_id=%d status=0",
             room_id);
}

/* ---- read-only views, for the control plane ----------------------------- */

int client_count(void)
{
    ensure_init();
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (g_clients[i].fd >= 0)
            n++;
    return n;
}

int room_count(void)
{
    ensure_init();
    int n = 0;
    for (int i = 0; i < MAX_ROOMS; i++)
        if (g_rooms[i].id != 0)
            n++;
    return n;
}

/*
 * Owner reported as a player_id rather than a slot index, because that is what
 * an operator sees in `tetrisctl players` and what a kick has to be addressed
 * with. r->members[0] is not the owner after a swap-remove reorders that
 * array, so the flag on the Client is the only trustworthy source.
 */
int room_snapshot(RoomInfo *out, int max)
{
    ensure_init();
    int n = 0;

    for (int i = 0; i < MAX_ROOMS && n < max; i++)
    {
        const Room *r = &g_rooms[i];
        if (r->id == 0)
            continue;

        out[n].id = r->id;
        out[n].phase = (int)r->phase;
        out[n].members = r->count;
        out[n].owner_player = -1;

        for (int m = 0; m < r->count; m++)
        {
            const Client *c = client_find(r->members[m]);
            if (c && c->state.is_owner)
            {
                out[n].owner_player = c->state.player_id;
                break;
            }
        }
        n++;
    }
    return n;
}

int player_snapshot(PlayerInfo *out, int max)
{
    ensure_init();
    int n = 0;

    for (int i = 0; i < MAX_SESSIONS && n < max; i++)
    {
        const Client *c = &g_clients[i];
        if (c->fd < 0)
            continue;

        out[n].room = c->state.room_id;
        out[n].player = c->state.player_id;
        out[n].pid = c->pid;
        out[n].is_owner = c->state.is_owner ? 1 : 0;
        out[n].score = c->score;
        out[n].lines = c->lines;
        snprintf(out[n].name, sizeof out[n].name, "%s", c->name);
        n++;
    }
    return n;
}

int client_fd_by_player(int room_id, int player_id)
{
    ensure_init();

    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        const Client *c = &g_clients[i];
        if (c->fd >= 0 && c->state.room_id == room_id &&
            c->state.player_id == player_id)
            return c->fd;
    }
    return -1;
}

pid_t client_pid_of(int fd)
{
    ensure_init();
    const Client *c = client_find(fd);
    return c ? c->pid : (pid_t)-1;
}
