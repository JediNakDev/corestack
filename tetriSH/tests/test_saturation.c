/**
 * @file test_saturation.c
 * @brief Latency and throughput of the whole server as the command rate rises.
 *
 * Fills one room with MAX_SESSIONS clients and replays the same load at four
 * command intervals - 200, 100, 50 and 20 ms per client - recording every
 * command's send-to-next-state latency to CSV. The point is to find where the
 * daemon stops keeping up: at 200 ms it should be idle, at 20 ms every session
 * is fielding 50 commands a second and broadcasting to 253 peers.
 *
 * This suite MEASURES, it does not judge. It fails only when the run itself
 * breaks (a client disconnects, a phase never settles, the CSV cannot be
 * written) - never on a latency number. Percentiles are printed as a
 * convenience; the raw per-command rows are the deliverable.
 */

// * ===== What is measured, and what the numbers mean =======================
// * LATENCY    -> record_send()/resolve(): each command is stamped when sent
// *               and resolved by the next UPD_GAME that reaches that client.
// ? The server pushes UPD_GAME every 50 ms while a room is PLAYING, so a
// ? healthy latency is ~0-50 ms of tick phase, NOT the command's true service
// ? time. What matters is the shape: once latency climbs past the 50 ms
// ? cadence, the daemon is behind, and that is the saturation point.
// * THROUGHPUT -> commands that got a following state, over the load window.
// *               Commands still unresolved at the end are counted separately
// *               rather than silently dropped - a fast run that resolves
// *               nothing is not a fast run.
// * COST       -> the sweep count in the summary is what this client actually
// ?               achieved. If sweeps fall short of the target rate, the test
// ?               harness became the bottleneck, not the daemon - check this
// ?               before reading anything into the latency numbers.
// * =========================================================================

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libtetrisutil/limits.h"
#include "load_harness.h"
#include "test_output.h"
#include "tetrisu/client.h"

#define DEFAULT_SETTLE_MS 5000
#define DEFAULT_LOAD_IN_S 60
#define DEFAULT_ROOM_ID 42
#define DEFAULT_CSV_SAMPLE_EVERY 1

/** Where the CSVs land unless SATURATION_CSV_DIR says otherwise. */
#define DEFAULT_CSV_DIR "var/saturation"

/** Command intervals swept, in milliseconds. Fastest last. */
static const int intervals_ms[] = {200, 100, 50, 20};

/**
 * Milliseconds spent servicing after the load window closes.
 *
 * Long enough for in-flight commands to be answered - without it every phase
 * would end with a burst of commands scored as unresolved purely because the
 * clock ran out.
 */
#define DRAIN_MS 1000

/** Commands one client may have outstanding before the excess is dropped. */
#define MAX_PENDING 256

/** Latency histogram resolution: one bucket per millisecond, plus overflow. */
#define HIST_MS 10000

typedef struct
{
    int settle_ms;
    int load_in_s;
    int room_id;
    int csv_sample_every;
    const char *csv_dir;
} TestConfig;

static TestConfig config = {
    .settle_ms = DEFAULT_SETTLE_MS,
    .load_in_s = DEFAULT_LOAD_IN_S,
    .room_id = DEFAULT_ROOM_ID,
    .csv_sample_every = DEFAULT_CSV_SAMPLE_EVERY,
    .csv_dir = DEFAULT_CSV_DIR,
};

/** Commands one client has sent but not yet seen a state for. A ring. */
typedef struct
{
    long send_ms[MAX_PENDING]; /**< Send time, ms into the phase. */
    long seq[MAX_PENDING];     /**< Per-client command number, for the CSV. */
    int head;                  /**< Oldest outstanding entry. */
    int count;                 /**< Entries in flight. */
    long next_seq;             /**< Commands this client has sent so far. */
} Pending;

/** Everything one interval phase produced. Written to the summary CSV. */
typedef struct
{
    long sweeps;    /**< Command sweeps issued across all clients. */
    long sent;      /**< Commands sent. */
    long completed; /**< Commands a later state resolved. */
    long
        overflowed; /**< Dropped: the client was MAX_PENDING commands behind. */
    long unresolved; /**< Still in flight when the drain ended. */
    long sum_ms;     /**< Latency total, for the mean. */
    long max_ms;
    long hist[HIST_MS + 1]; /**< Last bucket collects everything >= HIST_MS. */
} PhaseStats;

static Client clients[MAX_SESSIONS];
static Pending pending[MAX_SESSIONS];
static bool joined[MAX_SESSIONS];
static int games[MAX_SESSIONS];

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
    const char *dir = getenv("SATURATION_CSV_DIR");
    if (dir != NULL && dir[0] != '\0')
        config.csv_dir = dir;

    return read_config_int("SETTLE_MS", config.settle_ms, 1, INT_MAX,
                           &config.settle_ms) == 0 &&
                   read_config_int("LOAD_IN_S", config.load_in_s, 1, INT_MAX,
                                   &config.load_in_s) == 0 &&
                   read_config_int("ROOM_ID", config.room_id, 1, UINT8_MAX,
                                   &config.room_id) == 0 &&
                   read_config_int("CSV_SAMPLE_EVERY", config.csv_sample_every,
                                   1, INT_MAX, &config.csv_sample_every) == 0
               ? 0
               : -1;
}

/**
 * Creates a directory and every missing parent of it.
 *
 * Called once before the CSVs are opened, so a fresh checkout with no var/
 * does not fail the run on its last step.
 *
 * @returns 0 if the directory exists afterwards, -1 otherwise.
 */
static int make_dirs(const char *path)
{
    char buf[PATH_MAX];
    if (snprintf(buf, sizeof buf, "%s", path) >= (int)sizeof buf)
        return -1;

    for (char *p = buf + 1; *p != '\0'; p++)
        if (*p == '/')
        {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    return mkdir(buf, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/** Records a command as in flight. Called once per client per sweep. */
static void record_send(Pending *p, PhaseStats *stats, long at_ms)
{
    stats->sent++;
    if (p->count == MAX_PENDING)
    {
        // ! The client is MAX_PENDING states behind: the daemon is not
        // ! answering anywhere near the send rate. Drop the oldest rather
        // ! than the newest, so the samples that survive stay recent.
        p->head = (p->head + 1) % MAX_PENDING;
        p->count--;
        stats->overflowed++;
    }
    int slot = (p->head + p->count) % MAX_PENDING;
    p->send_ms[slot] = at_ms;
    p->seq[slot] = p->next_seq++;
    p->count++;
}

/**
 * Resolves every outstanding command for one client against a state push.
 *
 * Called when client_service() reports CLI_EV_GAME. One state answers all of
 * that client's in-flight commands: "send to next state" is exactly what the
 * metric asks for, and the server coalesces commands into ticks anyway.
 *
 * @param csv  Sample file; every csv_sample_every-th sample is written.
 */
static void resolve(int client, Pending *p, PhaseStats *stats, long at_ms,
                    int interval_ms, FILE *csv)
{
    while (p->count > 0)
    {
        long send_ms = p->send_ms[p->head];
        long seq = p->seq[p->head];
        p->head = (p->head + 1) % MAX_PENDING;
        p->count--;

        long latency = at_ms - send_ms;
        if (latency < 0)
            latency = 0;

        stats->completed++;
        stats->sum_ms += latency;
        if (latency > stats->max_ms)
            stats->max_ms = latency;
        stats->hist[latency < HIST_MS ? latency : HIST_MS]++;

        if (stats->completed % config.csv_sample_every == 0)
            fprintf(csv, "%d,%d,%ld,%ld,%ld,%ld\n", interval_ms, client, seq,
                    send_ms, at_ms, latency);
    }
}

/**
 * Polls every client once and folds in whatever arrived.
 *
 * @param at_ms  Phase-relative now, stamped onto anything resolved here.
 * @returns 0 normally, -1 if a client disconnected or was refused.
 */
static int service_once(PhaseStats *stats, long at_ms, int interval_ms,
                        FILE *csv, int timeout_ms)
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
        // ! A dead or refused peer invalidates the phase: the remaining
        // ! clients would be measured against a server missing a session.
        if (ev == CLI_EV_DISCONNECT || ev == CLI_EV_REJECT)
            return -1;
        if (ev == CLI_EV_SESSION &&
            clients[i].session.room_id == config.room_id)
            joined[i] = true;
        if (ev == CLI_EV_GAME)
        {
            games[i]++;
            if (stats != NULL)
                resolve(i, &pending[i], stats, at_ms, interval_ms, csv);
        }
    }
    return 0;
}

/** Waits for every client to join, and optionally to see a first state. */
static int service_until(int timeout_ms, bool need_games)
{
    long deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline)
    {
        if (service_once(NULL, 0, 0, NULL, 20) != 0)
            return -1;

        bool done = true;
        for (int i = 0; i < MAX_SESSIONS; i++)
            if (!joined[i] || (need_games && games[i] == 0))
                done = false;
        if (done)
            return 0;
    }
    return -1;
}

/** Latency at a percentile, read off the histogram. 0 when nothing completed.
 */
static long percentile_ms(const PhaseStats *stats, double fraction)
{
    if (stats->completed == 0)
        return 0;

    long target = (long)(fraction * (double)stats->completed);
    if (target < 1)
        target = 1;

    long seen = 0;
    for (int ms = 0; ms <= HIST_MS; ms++)
    {
        seen += stats->hist[ms];
        if (seen >= target)
            return ms;
    }
    return stats->max_ms;
}

/**
 * Runs the load at one command interval against a fresh daemon.
 *
 * Every phase gets its own daemon and its own 254 clients, so a slow phase
 * cannot leave state behind that colours the next one.
 *
 * @param samples  Open sample CSV; one row per recorded command.
 * @param stats    Zeroed on entry, filled with this phase's totals.
 * @returns 0 on a clean run, -1 if the phase broke before it finished.
 */
static int run_phase(int interval_ms, FILE *samples, PhaseStats *stats,
                     long *duration_ms)
{
    TestEnv env;
    const char *stage = "setup";
    int connected = 0;
    int saved_stdout = -1;

    memset(stats, 0, sizeof *stats);
    memset(pending, 0, sizeof pending);
    memset(joined, 0, sizeof joined);
    memset(games, 0, sizeof games);

    int port = start_daemon(&env);
    if (port < 0)
        goto fail;

    stage = "connect/join";
    if (getenv("TETRISH_TEST_VERBOSE") == NULL &&
        (saved_stdout = silence_stdout()) < 0)
        goto fail_clients;
    for (; connected < MAX_SESSIONS; connected++)
        if (client_connect(&clients[connected], "127.0.0.1", port,
                           env.ca_path) != 0 ||
            client_guest(&clients[connected]) != 0 ||
            client_join(&clients[connected], (uint8_t)config.room_id) != 0)
            goto fail_clients;
    int restore_result = restore_stdout(saved_stdout);
    saved_stdout = -1;
    if (restore_result != 0)
        goto fail_clients;

    stage = "join acknowledgements";
    if (service_until(config.settle_ms, false) != 0)
        goto fail_clients;
    stage = "start room";
    if (client_start(&clients[0]) != 0)
        goto fail_clients;
    stage = "initial game state";
    if (service_until(config.settle_ms, true) != 0)
        goto fail_clients;

    /* Measurement starts here: everyone is playing, so the only thing that
     * changes from phase to phase is how fast the commands arrive. */
    stage = "command load";
    long phase_start = now_ms();
    long load_end = phase_start + (long)config.load_in_s * 1000;
    long next_sweep = phase_start;

    while (now_ms() < load_end)
    {
        long now = now_ms();
        if (now >= next_sweep)
        {
            long at_ms = now - phase_start;
            for (int i = 0; i < MAX_SESSIONS; i++)
            {
                if (client_move(&clients[i], (int)(stats->sweeps & 1)) != 0)
                    goto fail_clients;
                record_send(&pending[i], stats, at_ms);
            }
            stats->sweeps++;

            /* Fixed cadence, but never a backlog: if a sweep overran its own
             * interval the next one goes out immediately and the shortfall
             * shows up as a sweep count below the target rate. */
            next_sweep += interval_ms;
            if (next_sweep < now)
                next_sweep = now;
        }

        long wait_until = next_sweep < load_end ? next_sweep : load_end;
        int timeout_ms = (int)(wait_until - now_ms());
        if (timeout_ms < 0)
            timeout_ms = 0;
        if (service_once(stats, now_ms() - phase_start, interval_ms, samples,
                         timeout_ms) != 0)
            goto fail_clients;
    }
    *duration_ms = now_ms() - phase_start;

    /* Drain: answer what is already in flight before scoring the rest as
     * unresolved. No new commands go out here. */
    stage = "drain";
    long drain_end = now_ms() + DRAIN_MS;
    while (now_ms() < drain_end)
        if (service_once(stats, now_ms() - phase_start, interval_ms, samples,
                         20) != 0)
            goto fail_clients;

    for (int i = 0; i < MAX_SESSIONS; i++)
        stats->unresolved += pending[i].count;

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
    test_output_failure_detailf(__FILE__, __LINE__,
                                "%d ms interval: %s stage failed", interval_ms,
                                stage);
    return -1;
}

/** Appends one phase to the summary CSV and prints the same numbers. */
static void report_phase(FILE *summary, int interval_ms, const PhaseStats *s,
                         long duration_ms)
{
    double seconds = duration_ms > 0 ? (double)duration_ms / 1000.0 : 1.0;
    double throughput = (double)s->completed / seconds;
    double mean =
        s->completed > 0 ? (double)s->sum_ms / (double)s->completed : 0.0;
    long p50 = percentile_ms(s, 0.50);
    long p95 = percentile_ms(s, 0.95);
    long p99 = percentile_ms(s, 0.99);

    fprintf(summary,
            "%d,%d,%ld,%ld,%ld,%ld,%ld,%ld,%.2f,%.2f,%ld,%ld,%ld,%ld\n",
            interval_ms, MAX_SESSIONS, duration_ms, s->sweeps, s->sent,
            s->completed, s->overflowed, s->unresolved, throughput, mean, p50,
            p95, p99, s->max_ms);

    printf("      %3d ms  sweeps=%-6ld sent=%-9ld completed=%-9ld "
           "%.0f/s  p50=%ld p95=%ld p99=%ld max=%ld ms\n",
           interval_ms, s->sweeps, s->sent, s->completed, throughput, p50, p95,
           p99, s->max_ms);
    fflush(stdout);
}

int main(void)
{
    char samples_path[PATH_MAX], summary_path[PATH_MAX];
    int failed = 0;
    int phases = (int)(sizeof intervals_ms / sizeof intervals_ms[0]);

    test_output_begin("test_saturation");
    if (read_config() != 0)
    {
        test_output_fail("valid runtime configuration");
        test_output_summary(1, 1, 0);
        return 1;
    }

    if (make_dirs(config.csv_dir) != 0 ||
        snprintf(samples_path, sizeof samples_path, "%s/saturation_samples.csv",
                 config.csv_dir) >= (int)sizeof samples_path ||
        snprintf(summary_path, sizeof summary_path, "%s/saturation_summary.csv",
                 config.csv_dir) >= (int)sizeof summary_path)
    {
        test_output_fail("writable CSV directory");
        test_output_summary(1, 1, 0);
        return 1;
    }

    FILE *samples = fopen(samples_path, "w");
    FILE *summary = fopen(summary_path, "w");
    if (samples == NULL || summary == NULL)
    {
        if (samples != NULL)
            fclose(samples);
        if (summary != NULL)
            fclose(summary);
        test_output_fail("open CSV files");
        test_output_summary(1, 1, 0);
        return 1;
    }
    fprintf(samples, "interval_ms,client,seq,send_ms,recv_ms,latency_ms\n");
    fprintf(summary, "interval_ms,clients,duration_ms,sweeps,commands_sent,"
                     "commands_completed,commands_overflowed,"
                     "commands_unresolved,completed_per_s,mean_ms,p50_ms,"
                     "p95_ms,p99_ms,max_ms\n");

    printf("SETTLE_MS=%d LOAD_IN_S=%d ROOM_ID=%d CSV_SAMPLE_EVERY=%d\n",
           config.settle_ms, config.load_in_s, config.room_id,
           config.csv_sample_every);
    printf("samples -> %s\nsummary -> %s\n", samples_path, summary_path);
    fflush(stdout);

    for (int i = 0; i < phases; i++)
    {
        static PhaseStats stats; /* 40 KB of histogram: too big for the stack */
        char name[64];
        long duration_ms = 0;
        snprintf(name, sizeof name, "%d clients at one command per %d ms",
                 MAX_SESSIONS, intervals_ms[i]);

        if (run_phase(intervals_ms[i], samples, &stats, &duration_ms) == 0)
        {
            test_output_pass(name);
            report_phase(summary, intervals_ms[i], &stats, duration_ms);
        }
        else
        {
            test_output_fail(name);
            failed++;
        }
        fflush(samples);
        fflush(summary);
    }

    fclose(samples);
    fclose(summary);
    test_output_summary(phases, failed, 0);
    return failed == 0 ? 0 : 1;
}
