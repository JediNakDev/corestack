/**
 * @file test_load.c
 * @brief End-to-end load tests for concurrent encrypted game clients.
 *
 * Each scenario fills the daemon to MAX_SESSIONS clients and hammers it with
 * moves, watching for the three ways a server fails under a cohort-scale
 * playtest:
 *
 * - crash: a client sees a disconnect, or the daemon tree loses a process;
 * - deadlock: joins, game states or broadcasts stop arriving before a deadline;
 * - leak: resident memory of the daemon tree climbs while the client
 *   population is fixed (see sample_tree() and check_memory()).
 */

// * ===== Where each failure mode is caught =================================
// * CRASH    -> service_once(): a session that died takes its TLS socket with
// *             it, so the client reads a disconnect and the scenario stops.
// *             check_memory() catches the quieter case: a session died but
// *             the client never noticed, so the tree lost a process.
// * DEADLOCK -> service_until(): every phase (join acks, first game state,
// *             post-load broadcast) must complete inside SETTLE_MS. A daemon
// *             wedged on a lock still holds the socket open, so nothing
// *             disconnects - only the missing progress gives it away.
// * LEAK     -> sample_tree() + check_memory(): ps(1) sums resident memory
// *             over the daemon and every session it forked. The client
// *             population is fixed after warm-up, so memory that keeps
// *             climbing is memory nobody gave back.
// * =========================================================================

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "libtetrisutil/limits.h"
#include "load_harness.h"
#include "test_output.h"
#include "tetrisu/client.h"

#define DEFAULT_SETTLE_MS 5000
#define DEFAULT_LOAD_IN_S 2000
#define DEFAULT_COMMAND_INTERVAL_MS 100
#define DEFAULT_ONE_ROOM_ID 42
#define DEFAULT_MAX_RSS_GROWTH_PCT 10
#define DEFAULT_MAX_RSS_GROWTH_KB 8192

/** How often the load loop re-measures the daemon tree, in milliseconds. */
#define RSS_SAMPLE_INTERVAL_MS 5000

/** Upper bound on the process table read from ps(1) in one sample. */
#define MAX_PROC_TABLE 16384

/** Share of the load window treated as warm-up; 4 means the first quarter. */
#define WARMUP_FRACTION 4

static const int distributed_room_sizes[] = {
    9, 21, 4, 16, 13, 1, 27, 7, 18, 11, 4, 23, 10, 14, 1, 20, 8, 17, 6, 24,
};

typedef enum
{
    ONE_ROOM,
    ONE_PLAYER_PER_ROOM,
    DISTRIBUTED_ROOMS,
} ScenarioLayout;

typedef struct
{
    const char *name;
    ScenarioLayout layout;
} Scenario;

typedef struct
{
    int settle_ms;
    int load_in_s;
    int command_interval_ms;
    int one_room_id;
    int max_rss_growth_pct;
    int max_rss_growth_kb;
} TestConfig;

static TestConfig config = {
    .settle_ms = DEFAULT_SETTLE_MS,
    .load_in_s = DEFAULT_LOAD_IN_S,
    .command_interval_ms = DEFAULT_COMMAND_INTERVAL_MS,
    .one_room_id = DEFAULT_ONE_ROOM_ID,
    .max_rss_growth_pct = DEFAULT_MAX_RSS_GROWTH_PCT,
    .max_rss_growth_kb = DEFAULT_MAX_RSS_GROWTH_KB,
};

/** One measurement of the daemon and every process it forked. */
typedef struct
{
    long rss_kb; /**< Resident memory summed over the tree, in kilobytes. */
    int procs;   /**< Processes alive in the tree; drops when a session dies. */
} MemSample;

static int read_config_int(const char *name, int current, int min, int max,
                           int *result)
{
    const char *value = getenv(name);
    if (value == NULL)
    {
        *result = current;
        return 0;
    }

    char *end;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < min ||
        parsed > max)
    {
        fprintf(stderr, "%s must be an integer from %d to %d\n", name, min,
                max);
        return -1;
    }
    *result = (int)parsed;
    return 0;
}

static int read_config(void)
{
    return read_config_int("SETTLE_MS", config.settle_ms, 1, INT_MAX,
                           &config.settle_ms) == 0 &&
                   read_config_int("LOAD_IN_S", config.load_in_s, 1, INT_MAX,
                                   &config.load_in_s) == 0 &&
                   read_config_int("COMMAND_INTERVAL_MS",
                                   config.command_interval_ms, 1, INT_MAX,
                                   &config.command_interval_ms) == 0 &&
                   read_config_int("ONE_ROOM_ID", config.one_room_id, 1,
                                   UINT8_MAX, &config.one_room_id) == 0 &&
                   read_config_int("MAX_RSS_GROWTH_PCT",
                                   config.max_rss_growth_pct, 0, INT_MAX,
                                   &config.max_rss_growth_pct) == 0 &&
                   read_config_int("MAX_RSS_GROWTH_KB",
                                   config.max_rss_growth_kb, 0, INT_MAX,
                                   &config.max_rss_growth_kb) == 0
               ? 0
               : -1;
}

/** One row of ps(1) output. Filled by read_proc_table() per sample. */
typedef struct
{
    pid_t pid;
    pid_t ppid;
    long rss_kb;
} ProcRow;

/** Process table of the whole machine; reused by every sample_tree() call. */
static ProcRow proc_table[MAX_PROC_TABLE];

/**
 * Reads pid, parent pid and resident size for every process on the machine.
 *
 * Called by sample_tree(). Uses ps(1) because the daemon's session children
 * have to be discovered by parentage, and neither /proc nor a kernel call for
 * that is portable across the platforms this test runs on.
 *
 * @param count  Receives the number of rows stored in proc_table.
 * @returns 0 on success, -1 if ps could not be run or produced nothing.
 */
static int read_proc_table(int *count)
{
    FILE *ps = popen("ps -Ao pid=,ppid=,rss=", "r");
    if (ps == NULL)
        return -1;

    int rows = 0;
    long pid, ppid, rss;
    while (rows < MAX_PROC_TABLE &&
           fscanf(ps, "%ld %ld %ld", &pid, &ppid, &rss) == 3)
    {
        proc_table[rows].pid = (pid_t)pid;
        proc_table[rows].ppid = (pid_t)ppid;
        proc_table[rows].rss_kb = rss;
        rows++;
    }
    if (pclose(ps) != 0 || rows == 0)
        return -1;

    *count = rows;
    return 0;
}

/**
 * Measures resident memory and process count for a daemon and its descendants.
 *
 * Called around the load phase to turn "the daemon leaks" into an observable
 * number: with a fixed client population, resident memory that keeps climbing
 * is memory the daemon or a session never gave back.
 *
 * @param root  Daemon pid; its whole descendant tree is included.
 * @param out   Receives the sample; untouched on failure.
 * @returns 0 on success, -1 if the process table could not be read.
 */
static int sample_tree(pid_t root, MemSample *out)
{
    // * LEAK instrument. Sessions are separate processes, so a leak can hide
    // * in any of them; the whole descendant tree is measured as one number.
    int rows = 0;
    if (read_proc_table(&rows) != 0)
        return -1;

    static bool in_tree[MAX_PROC_TABLE];
    memset(in_tree, 0, sizeof in_tree);

    /* ps output is not guaranteed to list a parent before its children, so
     * keep sweeping until a pass adds nothing. Session trees are shallow, so
     * this settles in a couple of passes. */
    bool grew = true;
    while (grew)
    {
        grew = false;
        for (int i = 0; i < rows; i++)
        {
            if (in_tree[i])
                continue;
            if (proc_table[i].pid == root)
            {
                in_tree[i] = true;
                grew = true;
                continue;
            }
            for (int j = 0; j < rows; j++)
                if (in_tree[j] && proc_table[j].pid == proc_table[i].ppid)
                {
                    in_tree[i] = true;
                    grew = true;
                    break;
                }
        }
    }

    MemSample sample = {.rss_kb = 0, .procs = 0};
    for (int i = 0; i < rows; i++)
        if (in_tree[i])
        {
            sample.rss_kb += proc_table[i].rss_kb;
            sample.procs++;
        }
    if (sample.procs == 0)
        return -1; /* the daemon is gone: a crash, not a measurement */

    *out = sample;
    return 0;
}

/** Kilobytes of growth tolerated over a baseline. Called by check_memory(). */
static long allowed_growth_kb(long baseline_kb)
{
    long by_pct = baseline_kb * config.max_rss_growth_pct / 100;
    return by_pct > config.max_rss_growth_kb ? by_pct
                                             : config.max_rss_growth_kb;
}

/**
 * Fails the scenario when the daemon tree grew or shrank during the load.
 *
 * Called after the load phase. A drop in process count means a session died
 * without the client noticing; growth beyond the tolerance is memory the
 * daemon or a session never gave back.
 *
 * Growth is measured from @p settled, not from @p baseline, whenever the load
 * ran long enough to produce a settled sample. Sessions page in a few tens of
 * megabytes as play starts, and charging that warm-up to the leak budget would
 * force a tolerance loose enough to hide a slow leak on a long run.
 *
 * @param settled  Sample from a quarter of the way into the load, or a copy of
 *                 @p baseline when the run was too short to take one.
 * @returns 0 when the tree stayed within tolerance, -1 otherwise.
 */
static int check_memory(const char *scenario, const MemSample *baseline,
                        const MemSample *settled, const MemSample *final,
                        const MemSample *peak)
{
    // ! CRASH (silent): a session died without its client seeing a
    // ! disconnect, so only the process count in the tree gives it away.
    if (final->procs < baseline->procs)
    {
        test_output_failure_detailf(
            __FILE__, __LINE__,
            "%s: %d of %d daemon processes died during the load", scenario,
            baseline->procs - final->procs, baseline->procs);
        return -1;
    }

    // ! LEAK: nobody joined or left after warm-up, so a tree that is still
    // ! growing is holding memory it should have released per command.
    // ? Growth, not absolute size: a big steady footprint is fine, a small
    // ? one that never stops climbing is not.
    long growth = final->rss_kb - settled->rss_kb;
    long allowed = allowed_growth_kb(settled->rss_kb);
    if (growth > allowed)
    {
        test_output_failure_detailf(
            __FILE__, __LINE__,
            "%s: daemon tree grew %ld KB after warm-up (%ld -> %ld KB, "
            "allowed %ld KB; warm-up %ld -> %ld KB, peak %ld KB) with a "
            "fixed client population",
            scenario, growth, settled->rss_kb, final->rss_kb, allowed,
            baseline->rss_kb, settled->rss_kb, peak->rss_kb);
        return -1;
    }
    return 0;
}

static int expected_room(ScenarioLayout layout, int client)
{
    if (layout == ONE_ROOM)
        return config.one_room_id;
    if (layout == ONE_PLAYER_PER_ROOM)
        return client + 1;

    int first = 0;
    int rooms =
        (int)(sizeof distributed_room_sizes / sizeof distributed_room_sizes[0]);
    for (int room = 0; room < rooms; room++)
    {
        first += distributed_room_sizes[room];
        if (client < first)
            return room + 1;
    }
    return -1;
}

static int service_once(Client clients[], bool joined[], int games[],
                        ScenarioLayout layout, int timeout_ms)
{
    struct pollfd fds[MAX_SESSIONS];
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        fds[i].fd = client_fd(&clients[i]);
        fds[i].events = POLLIN;
        fds[i].revents = 0;
    }
    if (poll(fds, MAX_SESSIONS, timeout_ms) < 0)
        return -1;
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;
        ClientEvent ev = client_service(&clients[i]);
        // ! CRASH: the session process owning this socket is gone (or refused
        // ! us). Nothing recovers a dead peer, so fail the scenario here.
        if (ev == CLI_EV_DISCONNECT || ev == CLI_EV_REJECT)
            return -1;
        if (ev == CLI_EV_SESSION &&
            clients[i].session.room_id == expected_room(layout, i))
            joined[i] = true;
        if (ev == CLI_EV_GAME)
            games[i]++;
    }
    return 0;
}

static int service_until(Client clients[], bool joined[], int games[],
                         ScenarioLayout layout, int timeout_ms, bool need_games)
{
    long deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline)
    {
        if (service_once(clients, joined, games, layout, 20) != 0)
            return -1;

        bool done = true;
        for (int i = 0; i < MAX_SESSIONS; i++)
            if (!joined[i] || (need_games && games[i] == 0))
                done = false;
        if (done)
            return 0;
    }
    // ! DEADLOCK: the deadline passed with at least one client still waiting.
    // ! A daemon blocked on a room lock keeps every socket open and looks
    // ! healthy, so silence past a deadline is the only symptom there is.
    return -1;
}

static int start_rooms(Client clients[], ScenarioLayout layout)
{
    bool started[UINT8_MAX + 1] = {false};
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        int room = expected_room(layout, i);
        if (!started[room])
        {
            if (client_start(&clients[i]) != 0)
                return -1;
            started[room] = true;
        }
    }
    return 0;
}

static int apply_command_load(Client clients[], bool joined[], int games[],
                              ScenarioLayout layout, pid_t daemon,
                              MemSample *settled, MemSample *peak)
{
    long load_start = now_ms();
    long load_end = load_start + config.load_in_s * 1000;
    long settle_at = load_start + config.load_in_s * 1000 / WARMUP_FRACTION;
    long next_commands = now_ms();
    long next_sample = now_ms() + RSS_SAMPLE_INTERVAL_MS;
    int command = 0;
    bool have_settled = false;

    while (now_ms() < load_end)
    {
        long now = now_ms();
        if (now >= next_sample)
        {
            MemSample sample;
            if (sample_tree(daemon, &sample) != 0)
                return -1;
            if (sample.rss_kb > peak->rss_kb)
                *peak = sample;
            /* First sample past the warm-up window becomes the leak baseline;
             * short runs never reach it and keep the pre-load baseline. */
            if (!have_settled && now >= settle_at)
            {
                *settled = sample;
                have_settled = true;
            }
            next_sample = now + RSS_SAMPLE_INTERVAL_MS;
        }
        if (now >= next_commands)
        {
            for (int i = 0; i < MAX_SESSIONS; i++)
                if (client_move(&clients[i], command & 1) != 0)
                    return -1;
            command++;

            /* Average clickers reach 5-7 clicks per second. A 100 ms
             * interval deliberately tests a faster 10 clicks per second. */
            next_commands = now + config.command_interval_ms;
        }

        long wait_until =
            next_commands < next_sample ? next_commands : next_sample;
        if (wait_until > load_end)
            wait_until = load_end;
        int timeout_ms = (int)(wait_until - now_ms());
        if (timeout_ms < 0)
            timeout_ms = 0;
        if (service_once(clients, joined, games, layout, timeout_ms) != 0)
            return -1;
    }
    return 0;
}

static int run_scenario(const Scenario *scenario)
{
    TestEnv env;
    static Client clients[MAX_SESSIONS];
    bool joined[MAX_SESSIONS] = {false};
    int games[MAX_SESSIONS] = {0};
    const char *stage = "setup";
    int connected = 0;
    int saved_stdout = -1;
    int port = start_daemon(&env);
    if (port < 0)
        goto fail;

    stage = "connect/join";
    if (getenv("TETRISH_TEST_VERBOSE") == NULL &&
        (saved_stdout = silence_stdout()) < 0)
        goto fail_clients;
    for (; connected < MAX_SESSIONS; connected++)
    {
        int room = expected_room(scenario->layout, connected);
        if (room < 1 || room > UINT8_MAX ||
            client_connect(&clients[connected], "127.0.0.1", port,
                           env.ca_path) != 0 ||
            client_guest(&clients[connected]) != 0 ||
            client_join(&clients[connected], (uint8_t)room) != 0)
            goto fail_clients;
    }
    int restore_result = restore_stdout(saved_stdout);
    saved_stdout = -1;
    if (restore_result != 0)
        goto fail_clients;

    stage = "join acknowledgements";
    if (service_until(clients, joined, games, scenario->layout,
                      config.settle_ms, false) != 0)
        goto fail_clients;
    stage = "start rooms";
    if (start_rooms(clients, scenario->layout) != 0)
        goto fail_clients;
    stage = "initial game state";
    if (service_until(clients, joined, games, scenario->layout,
                      config.settle_ms, true) != 0)
        goto fail_clients;

    // * LEAK baseline: every client is connected, joined and playing, so from
    // * here the population is fixed and resident memory should be too. The
    // * load loop refines it with a post-warm-up sample (WARMUP_FRACTION),
    // * because sessions page in tens of megabytes as play starts.
    stage = "baseline memory sample";
    MemSample baseline, settled, peak, final;
    if (sample_tree(env.daemon, &baseline) != 0)
        goto fail_clients;
    settled = baseline;
    peak = baseline;

    memset(games, 0, sizeof games);
    stage = "command load";
    if (apply_command_load(clients, joined, games, scenario->layout, env.daemon,
                           &settled, &peak) != 0)
        goto fail_clients;
    stage = "state broadcast";
    if (service_until(clients, joined, games, scenario->layout,
                      config.settle_ms, true) != 0)
        goto fail_clients;

    stage = "memory growth";
    if (sample_tree(env.daemon, &final) != 0)
        goto fail_clients;
    if (final.rss_kb > peak.rss_kb)
        peak = final;
    if (check_memory(scenario->name, &baseline, &settled, &final, &peak) != 0)
        goto fail_clients;

    for (int i = 0; i < MAX_SESSIONS; i++)
        client_disconnect(&clients[i]);
    stop_daemon(&env);
    clean_env(&env);
    return 0;

fail_clients:
    restore_stdout(saved_stdout);
    for (int i = 0; i < connected; i++)
        client_disconnect(&clients[i]);
    stop_daemon(&env);
    clean_env(&env);
fail:
    test_output_failure_detailf(__FILE__, __LINE__, "%s stage failed: %s",
                                scenario->name, stage);
    return -1;
}

int main(void)
{
    static const Scenario scenarios[] = {
        {"254 players in one room", ONE_ROOM},
        {"254 rooms with one player each", ONE_PLAYER_PER_ROOM},
        {"254 players distributed across 20 rooms", DISTRIBUTED_ROOMS},
    };
    int failed = 0;

    test_output_begin("test_load");
    if (read_config() != 0)
    {
        test_output_fail("valid runtime configuration");
        test_output_summary(1, 1, 0);
        return 1;
    }
    printf("SETTLE_MS=%d LOAD_IN_S=%d COMMAND_INTERVAL_MS=%d ONE_ROOM_ID=%d "
           "MAX_RSS_GROWTH_PCT=%d MAX_RSS_GROWTH_KB=%d\n",
           config.settle_ms, config.load_in_s, config.command_interval_ms,
           config.one_room_id, config.max_rss_growth_pct,
           config.max_rss_growth_kb);
    for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++)
    {
        if (run_scenario(&scenarios[i]) == 0)
            test_output_pass(scenarios[i].name);
        else
        {
            test_output_fail(scenarios[i].name);
            failed++;
        }
    }
    test_output_summary(3, failed, 0);
    return failed == 0 ? 0 : 1;
}
