/**
 * @file bench_latency.c
 * @brief Request latency percentiles, and the throughput where p99 breaks SLO.
 *
 * Drives one real tetrisd with up to MAX_SESSIONS real TLS clients at a fixed
 * OFFERED rate, measures every request's round trip, and sweeps the rate
 * upwards until p99 crosses the SLO. What it prints is p50 / p99 / p99.9 per
 * rate, and one number at the end: the highest offered rate whose p99 still
 * fits the SLO.
 *
 * This is a measurement tool, not a test. It fails only when the run itself
 * breaks (a client disconnects, the daemon will not start); a slow server is
 * a result, not an error.
 */

// * ===== What is measured, and why this request ============================
// * The probe is JOIN sent by a client that is ALREADY in a room, which the
// ? server answers 409. That is the only request/response pair tetriSH has
// ? whose latency is the server's own service time: it crosses the TLS
// ? session, the session process, the socketpair to the admin thread - the
// ? one serialization point every room shares - and all the way back. It
// ? changes no state and broadcasts to nobody, so the rate can be swept
// ? without the load changing what is being loaded.
// *
// ! NOT the command->STATE latency test_saturation.c measures. That one is
// ! quantized by the 20 Hz gravity tick, so its floor is ~25 ms of tick phase
// ! and a percentile below 50 ms means nothing. Both are worth having; this
// ! one is the one with a meaningful p99.
// *
// * OPEN LOOP, and timestamped from when a request was DUE rather than when
// ? it was sent. A closed loop (send, wait, send again) would slow its own
// ? offered rate down whenever the server slowed, and would report a healthy
// ? p99 for a server that had stopped keeping up - coordinated omission. Time
// ? spent waiting for the harness to get around to a request is therefore
// ? part of that request's latency, exactly as it would be for a user.
// * =========================================================================

// * ===== Why the load generator is several processes =======================
// * One process doing TLS for every client IS the bottleneck near the knee.
// ? Measured at 300 clients: at 22,400 req/s the single-process harness was
// ? itself 9.7 ms late on its p99 send against a reported p99 of 24.3 ms.
// ? Two fifths of the tail at the knee was the measuring instrument.
// *
// * So the clients are split across BENCH_WORKERS forked processes, all
// ? driving ONE daemon. Each owns a slice of the clients and the same slice
// ? of the offered rate, and they run each window in lockstep off a shared
// ? CLOCK_MONOTONIC start instant. Per-process poll sets get smaller and the
// ? TLS work spreads over cores, which is exactly the lag that was being
// ? charged to tetrisd.
// *
// * Percentiles are merged by shipping every raw sample back to the parent,
// ? because a percentile of percentiles is not a percentile. The parent sorts
// ? the union and reads p50/p99/p99.9 off that. At 24k req/s for 10 s that is
// ? about a megabyte down a pipe per step, which is nothing.
// *
// * send_lag_p99 stays in the output. It is the honesty check: when it is a
// ! serious fraction of p99, the row describes this program, not the server,
// ! and it is reported HARNESS-BOUND.
// * =========================================================================

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "libtetrisutil/limits.h"
#include "load_harness.h"
#include "tetrisu/client.h"

/* ---- defaults ----------------------------------------------------------- */

#define DEFAULT_CLIENTS 300 /* above the old 254 cap, on purpose */
#define DEFAULT_ROOM_SIZE 8 /* MAX_STANDINGS: the room a player really sits in */
#define DEFAULT_WORKERS 3
#define DEFAULT_DURATION_S 10
#define DEFAULT_WARMUP_S 2
#define DEFAULT_SLO_MS 25
#define DEFAULT_RATE_START 800 /* requests per second, all workers together */
#define DEFAULT_RATE_MAX 60000
#define DEFAULT_RATE_STEPS 12  /* ceiling on how many rates are tried      */
#define DEFAULT_REFINE_STEPS 4 /* bisections between last pass and first fail */
#define DEFAULT_CSV_DIR "var/bench"

/** Growth per sweep step, as a percentage of the previous rate. */
#define DEFAULT_RATE_GROWTH_PCT 100

/** Most load-generator processes one run may fork. */
#define MAX_WORKERS 16

/** Requests one client may have outstanding before the harness stops adding. */
#define MAX_INFLIGHT 256

/** Samples one worker keeps per step. Beyond this a step is already decided. */
#define MAX_SAMPLES (1000 * 1000)

/** Requests placed in one catch-up burst before the loop services reads again.
 * Without a cap, a harness that fell behind would spend its whole timeslice
 * sending and never read the answers it is timing. */
#define MAX_BURST 512

/** How long in-flight requests get to be answered after the load window. */
#define DRAIN_MS 2000

/** Unanswered fraction above which a rate is unsustainable whatever its p99. */
#define MAX_UNANSWERED_PCT 1.0

/**
 * Notice given to the workers before a window opens.
 *
 * They have to agree on the instant the schedule starts, or the first second
 * of every step is one worker's requests arriving before another's. Long
 * enough to cover the pipe write and a scheduling hiccup on a loaded machine.
 */
#define STEP_LEAD_MS 300

/**
 * Share of p99 the harness's own send lag may be before the row is suspect.
 *
 * A quarter, not a half. The 300-client run that prompted the multi-process
 * rewrite sat at two fifths and was NOT flagged by the old rule, which is
 * precisely the case the flag exists to catch.
 */
#define HARNESS_LAG_SHARE 0.25

/** Lag below this is never worth flagging, whatever share of p99 it is. */
#define HARNESS_LAG_FLOOR_US 2000

/* ---- configuration ------------------------------------------------------ */

typedef struct
{
    int clients;
    int room_size;
    int workers;
    int duration_s;
    int warmup_s;
    int slo_ms;
    int rate_start;
    int rate_max;
    int rate_steps;
    int rate_growth_pct;
    int refine_steps;
    int settle_ms;
    const char *csv_dir;
} BenchConfig;

static BenchConfig cfg = {
    .clients = DEFAULT_CLIENTS,
    .room_size = DEFAULT_ROOM_SIZE,
    .workers = DEFAULT_WORKERS,
    .duration_s = DEFAULT_DURATION_S,
    .warmup_s = DEFAULT_WARMUP_S,
    .slo_ms = DEFAULT_SLO_MS,
    .rate_start = DEFAULT_RATE_START,
    .rate_max = DEFAULT_RATE_MAX,
    .rate_steps = DEFAULT_RATE_STEPS,
    .rate_growth_pct = DEFAULT_RATE_GROWTH_PCT,
    .refine_steps = DEFAULT_REFINE_STEPS,
    .settle_ms = 30000,
    .csv_dir = DEFAULT_CSV_DIR,
};

/* ---- parent <-> worker protocol ----------------------------------------- */

/** One window, handed to every worker so they all open it at the same instant.
 */
typedef struct
{
    bool stop;             /**< Nothing else is valid; exit. */
    double rate;           /**< THIS worker's share of the offered rate. */
    long long start_ns;    /**< When the schedule begins. */
    long long measure_ns;  /**< When recording begins (warmup ends). */
    long long end_ns;      /**< When sending stops. */
} StepCmd;

/** What one worker produced, ahead of its raw samples. */
typedef struct
{
    bool ok;          /**< false: this worker's step broke. */
    /*
     * The server hung up on one of this worker's clients.
     *
     * Its own failure mode, not lumped in with "the step broke", because it
     * is a RESULT: tetrisd drops a session whose socketpair it cannot write
     * (room.c - "a peer that has let its buffer fill is not slow, it is
     * gone"), so a rate that triggers it is a rate the server cannot serve.
     * Worth saying out loud rather than reporting as a broken benchmark.
     */
    bool session_lost;
    long long sent;
    long long completed;
    long long blocked;
    long long unanswered;
    long long duration_ms;
    long n_samples;        /**< uint32 latencies then uint32 lags follow. */
} StepReply;

/** Everything one rate step produced, merged across workers. */
typedef struct
{
    double offered_rps;
    double achieved_rps;
    long long sent;
    long long completed;   /**< Answered, and counted (post-warmup). */
    long long unanswered;  /**< Still in flight when the drain ended. */
    long long blocked;     /**< Not sent: the client was MAX_INFLIGHT behind. */
    long long duration_ms; /**< Measured window, warmup excluded. */
    long p50_us, p90_us, p99_us, p999_us, max_us;
    long lag_p99_us; /**< Harness lag: how late sends actually went out. */
    bool harness_bound;
    bool meets_slo;
} StepStats;

/* ---- per-client bookkeeping --------------------------------------------- */

/** One client's outstanding probes, oldest first.
 *
 * A FIFO is exact here, not an approximation: one connection carries one
 * request stream and the session process answers it in order, so the n-th
 * response is the answer to the n-th request. Nothing has to be correlated. */
typedef struct
{
    long long due_ns[MAX_INFLIGHT];  /**< When the request was SCHEDULED. */
    long long sent_ns[MAX_INFLIGHT]; /**< When it actually went out.      */
    int head;
    int count;
} Inflight;

/* ---- worker-side state (each forked process has its own) ---------------- */

static Client *clients;    /**< This worker's slice, my_count entries. */
static Inflight *inflight;
static struct pollfd *pfds;
static int my_base;  /**< Index of this worker's first client, globally. */
static int my_count; /**< How many clients this worker owns. */

static uint32_t *lat_us; /**< Latency samples for the current window. */
static uint32_t *lag_us; /**< Send-lag samples, same indices.         */
static long n_samples;

/* ---- parent-side state -------------------------------------------------- */

typedef struct
{
    pid_t pid;
    int cmd_fd; /**< Parent writes StepCmd here. */
    int res_fd; /**< Parent reads StepReply and samples here. */
    int clients;
} Worker;

static Worker workers[MAX_WORKERS];
static int n_workers;

/* Merged sample space, parent only. Sized for every worker's full quota. */
static uint32_t *all_lat;
static uint32_t *all_lag;

/* ---- small helpers ------------------------------------------------------ */

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/** read()/write() loops. A pipe splits a big buffer; a short count is normal. */
static int read_all(int fd, void *buf, size_t len)
{
    unsigned char *p = buf;
    while (len > 0)
    {
        ssize_t n = read(fd, p, len);
        if (n > 0)
        {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1; /* EOF counts: the peer died mid-message */
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    while (len > 0)
    {
        ssize_t n = write(fd, p, len);
        if (n > 0)
        {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int read_env_int(const char *name, int current, int min, int max,
                        int *out)
{
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0')
    {
        *out = current;
        return 0;
    }

    char *end;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < min ||
        parsed > max)
    {
        fprintf(stderr, "bench: %s must be an integer from %d to %d\n", name,
                min, max);
        return -1;
    }
    *out = (int)parsed;
    return 0;
}

static int read_config(void)
{
    const char *dir = getenv("BENCH_CSV_DIR");
    if (dir != NULL && dir[0] != '\0')
        cfg.csv_dir = dir;

    if (read_env_int("BENCH_CLIENTS", cfg.clients, 1, MAX_SESSIONS,
                     &cfg.clients) != 0 ||
        read_env_int("BENCH_ROOM_SIZE", cfg.room_size, 1, MAX_ROOM_MEMBERS,
                     &cfg.room_size) != 0 ||
        read_env_int("BENCH_WORKERS", cfg.workers, 1, MAX_WORKERS,
                     &cfg.workers) != 0 ||
        read_env_int("BENCH_DURATION_S", cfg.duration_s, 1, 3600,
                     &cfg.duration_s) != 0 ||
        read_env_int("BENCH_WARMUP_S", cfg.warmup_s, 0, 3600, &cfg.warmup_s) !=
            0 ||
        read_env_int("BENCH_SLO_MS", cfg.slo_ms, 1, 60000, &cfg.slo_ms) != 0 ||
        read_env_int("BENCH_RATE_START", cfg.rate_start, 1, 10000000,
                     &cfg.rate_start) != 0 ||
        read_env_int("BENCH_RATE_MAX", cfg.rate_max, 1, 10000000,
                     &cfg.rate_max) != 0 ||
        read_env_int("BENCH_RATE_STEPS", cfg.rate_steps, 1, 64,
                     &cfg.rate_steps) != 0 ||
        read_env_int("BENCH_RATE_GROWTH_PCT", cfg.rate_growth_pct, 1, 1000,
                     &cfg.rate_growth_pct) != 0 ||
        read_env_int("BENCH_REFINE_STEPS", cfg.refine_steps, 0, 16,
                     &cfg.refine_steps) != 0 ||
        read_env_int("BENCH_SETTLE_MS", cfg.settle_ms, 1000, 600000,
                     &cfg.settle_ms) != 0)
        return -1;

    if (cfg.rate_max < cfg.rate_start)
    {
        fprintf(stderr, "bench: BENCH_RATE_MAX is below BENCH_RATE_START\n");
        return -1;
    }
    if (cfg.workers > cfg.clients)
        cfg.workers = cfg.clients; /* a worker with no clients sends nothing */
    return 0;
}

/** Creates a directory and every missing parent of it. */
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

/** The room a client sits in, by GLOBAL index. Rooms count from 1; 0 = new. */
static int room_of(int global_index)
{
    return global_index / cfg.room_size + 1;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/** Exact percentile off the sorted sample array. 0 when nothing was measured. */
static long percentile_us(const uint32_t *sorted, long n, double fraction)
{
    if (n <= 0)
        return 0;

    long idx = (long)(fraction * (double)n);
    if (idx >= n)
        idx = n - 1;
    if (idx < 0)
        idx = 0;
    return (long)sorted[idx];
}

/* ---- the probe (worker side) -------------------------------------------- */

/**
 * Sends one probe on this worker's client `i` and records when it was due.
 *
 * @returns 0 if it went out, 1 if this client is already MAX_INFLIGHT behind
 *          (the caller counts that as offered-but-not-placed), -1 on a send
 *          failure, which ends the window.
 */
static int probe_send(int i, long long due_ns)
{
    Inflight *q = &inflight[i];
    if (q->count == MAX_INFLIGHT)
        return 1;

    /* JOIN naming the room this client is already in: answered 409 by the
     * admin thread, and it leaves the room table exactly as it found it. */
    if (client_join(&clients[i], room_of(my_base + i)) != 0)
        return -1;

    int slot = (q->head + q->count) % MAX_INFLIGHT;
    q->due_ns[slot] = due_ns;
    q->sent_ns[slot] = now_ns();
    q->count++;
    return 0;
}

/**
 * Resolves the oldest outstanding probe on client `i` against a reply.
 *
 * @param record  false during warmup: the reply still clears the queue, it is
 *                just not measured.
 */
static void probe_resolve(int i, long long at_ns, bool record, StepReply *rep)
{
    Inflight *q = &inflight[i];
    if (q->count == 0)
        return; /* a reply we never asked for; nothing to time */

    long long due = q->due_ns[q->head];
    long long sent = q->sent_ns[q->head];
    q->head = (q->head + 1) % MAX_INFLIGHT;
    q->count--;

    if (!record)
        return;

    long long latency_ns = at_ns - due;
    if (latency_ns < 0)
        latency_ns = 0;

    rep->completed++;
    if (n_samples < MAX_SAMPLES)
    {
        lat_us[n_samples] = (uint32_t)(latency_ns / 1000);
        lag_us[n_samples] = (uint32_t)((sent - due) / 1000);
        n_samples++;
    }
}

/**
 * Polls this worker's clients once and folds in whatever arrived.
 *
 * @returns 0 normally, -1 if a client died - which invalidates the window,
 *          since the rest would be measured against a smaller server.
 */
/** Set by service_once when a client's session went away under it. */
static bool g_session_lost;

static int service_once(int timeout_ms, bool record, StepReply *rep)
{
    for (int i = 0; i < my_count; i++)
    {
        pfds[i].fd = client_fd(&clients[i]);
        pfds[i].events = POLLIN;
        pfds[i].revents = 0;
    }
    if (poll(pfds, (nfds_t)my_count, timeout_ms) < 0)
        return errno == EINTR ? 0 : -1;

    for (int i = 0; i < my_count; i++)
    {
        if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;

        ClientEvent ev = client_service(&clients[i]);
        if (ev == CLI_EV_DISCONNECT)
        {
            g_session_lost = true;
            return -1;
        }
        if (ev == CLI_EV_REJECT)
            probe_resolve(i, now_ns(), record, rep);
        /* Anything else - a stray UPD_SESSION from the join phase - is not an
         * answer to a probe and is deliberately not timed. */
    }
    return 0;
}

/**
 * Runs one window at this worker's share of the rate.
 *
 * Warmup and measurement are one continuous run: the rate never pauses, only
 * the recording starts later, so the server is never handed a cold start it
 * would not see in the middle of a real load.
 */
static void run_window(const StepCmd *cmd, StepReply *rep)
{
    memset(rep, 0, sizeof *rep);
    memset(inflight, 0, sizeof *inflight * (size_t)my_count);
    n_samples = 0;
    rep->ok = true;
    g_session_lost = false;

    long long interval_ns = (long long)(1e9 / cmd->rate);
    if (interval_ns < 1)
        interval_ns = 1;
    long long next_due = cmd->start_ns;
    int rr = 0;

    /* Wait out the lead time the parent left, so every worker opens the
     * window on the same instant rather than on its own fork order. */
    while (now_ns() < cmd->start_ns)
        if (service_once(1, false, rep) != 0)
        {
            rep->ok = false;
            rep->session_lost = g_session_lost;
            return;
        }

    while (true)
    {
        long long now = now_ns();
        if (now >= cmd->end_ns)
            break;

        /* (1) everything that has come due, up to a burst cap. */
        int burst = 0;
        while (next_due <= now && next_due < cmd->end_ns && burst < MAX_BURST)
        {
            bool record = next_due >= cmd->measure_ns;
            int placed = -1;

            /* Round-robin, skipping clients that are already behind. One full
             * lap with nowhere to put it means every client is saturated. */
            for (int tried = 0; tried < my_count && placed != 0; tried++)
            {
                int i = rr++ % my_count;
                placed = probe_send(i, next_due);
                if (placed < 0)
                {
                    rep->ok = false;
                    rep->session_lost = g_session_lost;
                    return;
                }
            }

            if (record)
            {
                if (placed == 0)
                    rep->sent++;
                else
                    rep->blocked++;
            }
            next_due += interval_ns;
            burst++;
        }

        /* (2) answers, until the next request is due. */
        long long wait_until =
            next_due < cmd->end_ns ? next_due : cmd->end_ns;
        long long wait_ns = wait_until - now_ns();
        int timeout_ms = wait_ns > 0 ? (int)(wait_ns / 1000000) : 0;
        if (service_once(timeout_ms, now_ns() >= cmd->measure_ns, rep) != 0)
        {
            rep->ok = false;
            rep->session_lost = g_session_lost;
            return;
        }
    }
    rep->duration_ms = (now_ns() - cmd->measure_ns) / 1000000;

    /* Drain: answer what is already in flight before scoring the rest as
     * unanswered. No new requests go out here. */
    long long drain_end = now_ns() + (long long)DRAIN_MS * 1000000LL;
    while (now_ns() < drain_end)
    {
        if (service_once(20, true, rep) != 0)
        {
            rep->ok = false;
            rep->session_lost = g_session_lost;
            return;
        }

        long long left = 0;
        for (int i = 0; i < my_count; i++)
            left += inflight[i].count;
        if (left == 0)
            break;
    }
    for (int i = 0; i < my_count; i++)
        rep->unanswered += inflight[i].count;
    rep->n_samples = n_samples;
}

/* ---- worker lifecycle --------------------------------------------------- */

/** Waits until every client this worker owns is in the room it asked for. */
static int wait_for_joins(void)
{
    long deadline = now_ms() + cfg.settle_ms;
    StepReply scratch;
    memset(&scratch, 0, sizeof scratch);

    while (now_ms() < deadline)
    {
        if (service_once(20, false, &scratch) != 0)
            return -1;

        bool done = true;
        for (int i = 0; i < my_count; i++)
            if (!clients[i].have_session ||
                clients[i].session.room_id != room_of(my_base + i))
                done = false;
        if (done)
            return 0;
    }
    return -1;
}

/**
 * A load-generator process: connect a slice of the clients, then serve windows
 * until the parent closes the command pipe.
 *
 * Never returns. Exits 0 on a clean shutdown, 1 if it could not get started -
 * and says so on the result pipe first, so the parent stops rather than
 * waiting out a settle timeout for a worker that is already gone.
 */
static void worker_main(int cmd_fd, int res_fd, int port, const char *ca_path)
{
    StepReply rep;
    char ready = 0;

    clients = calloc((size_t)my_count, sizeof *clients);
    inflight = calloc((size_t)my_count, sizeof *inflight);
    pfds = calloc((size_t)my_count, sizeof *pfds);
    lat_us = malloc(sizeof *lat_us * MAX_SAMPLES);
    lag_us = malloc(sizeof *lag_us * MAX_SAMPLES);

    if (clients == NULL || inflight == NULL || pfds == NULL ||
        lat_us == NULL || lag_us == NULL)
        goto fail;

    for (int i = 0; i < my_count; i++)
        if (client_connect(&clients[i], "127.0.0.1", port, ca_path) != 0 ||
            client_guest(&clients[i]) != 0 ||
            client_join(&clients[i], room_of(my_base + i)) != 0)
            goto fail;

    if (wait_for_joins() != 0)
        goto fail;

    ready = 1;
    if (write_all(res_fd, &ready, 1) != 0)
        _exit(1);

    for (;;)
    {
        StepCmd cmd;
        if (read_all(cmd_fd, &cmd, sizeof cmd) != 0 || cmd.stop)
            break; /* parent closed the pipe, or said so outright */

        run_window(&cmd, &rep);
        if (write_all(res_fd, &rep, sizeof rep) != 0)
            break;
        if (rep.ok && rep.n_samples > 0 &&
            (write_all(res_fd, lat_us,
                       sizeof *lat_us * (size_t)rep.n_samples) != 0 ||
             write_all(res_fd, lag_us,
                       sizeof *lag_us * (size_t)rep.n_samples) != 0))
            break;
    }

    for (int i = 0; i < my_count; i++)
        client_disconnect(&clients[i]);
    _exit(0);

fail:
    (void)write_all(res_fd, &ready, 1); /* ready == 0: "I did not come up" */
    _exit(1);
}

/**
 * Forks the load generators and waits for all of them to have their clients
 * in rooms.
 *
 * @returns 0 when every worker is ready, -1 otherwise (callers stop the run;
 *          the daemon and any live workers are cleaned up by the exit path).
 */
static int start_workers(int port, const char *ca_path)
{
    int per = cfg.clients / cfg.workers;
    int extra = cfg.clients % cfg.workers;
    int base = 0;

    for (int w = 0; w < cfg.workers; w++)
    {
        int count = per + (w < extra ? 1 : 0);
        int cmd_pipe[2], res_pipe[2];

        if (pipe(cmd_pipe) != 0 || pipe(res_pipe) != 0)
            return -1;

        pid_t pid = fork();
        if (pid < 0)
            return -1;

        if (pid == 0)
        {
            /* Child: keep only its own ends, and only its own slice. */
            close(cmd_pipe[1]);
            close(res_pipe[0]);
            for (int p = 0; p < n_workers; p++)
            {
                close(workers[p].cmd_fd);
                close(workers[p].res_fd);
            }
            my_base = base;
            my_count = count;
            if (getenv("BENCH_VERBOSE") == NULL)
            {
                /* The client library narrates every handshake, and hundreds of
                 * those from several processes at once bury the report. */
                int nullfd = open("/dev/null", O_WRONLY);
                if (nullfd >= 0)
                {
                    dup2(nullfd, STDOUT_FILENO);
                    close(nullfd);
                }
            }
            worker_main(cmd_pipe[0], res_pipe[1], port, ca_path);
        }

        close(cmd_pipe[0]);
        close(res_pipe[1]);
        workers[n_workers].pid = pid;
        workers[n_workers].cmd_fd = cmd_pipe[1];
        workers[n_workers].res_fd = res_pipe[0];
        workers[n_workers].clients = count;
        n_workers++;
        base += count;
    }

    for (int w = 0; w < n_workers; w++)
    {
        char ready = 0;
        if (read_all(workers[w].res_fd, &ready, 1) != 0 || ready != 1)
        {
            fprintf(stderr,
                    "bench: worker %d (%d clients) never got its clients into "
                    "rooms - is the session table or an fd limit smaller than "
                    "%d?\n",
                    w, workers[w].clients, cfg.clients);
            return -1;
        }
    }
    return 0;
}

static void stop_workers(void)
{
    for (int w = 0; w < n_workers; w++)
        close(workers[w].cmd_fd); /* EOF: the worker disconnects and exits */
    for (int w = 0; w < n_workers; w++)
    {
        close(workers[w].res_fd);
        waitpid(workers[w].pid, NULL, 0);
    }
    n_workers = 0;
}

/* ---- one rate step (parent side) ---------------------------------------- */

/**
 * Runs one offered rate across every worker and merges what comes back.
 *
 * @returns 0 on a clean step, STEP_SESSION_LOST if the server hung up on a
 *          client (a verdict about the rate, not a broken run), -1 if the
 *          step broke for any other reason.
 */
#define STEP_SESSION_LOST 1

static int run_step(double rate, StepStats *st)
{
    StepCmd cmd;
    long merged = 0;
    bool session_lost = false;

    memset(st, 0, sizeof *st);
    st->offered_rps = rate;

    cmd.stop = false;
    cmd.rate = rate / (double)n_workers;
    cmd.start_ns = now_ns() + (long long)STEP_LEAD_MS * 1000000LL;
    cmd.measure_ns = cmd.start_ns + (long long)cfg.warmup_s * 1000000000LL;
    cmd.end_ns = cmd.measure_ns + (long long)cfg.duration_s * 1000000000LL;

    for (int w = 0; w < n_workers; w++)
        if (write_all(workers[w].cmd_fd, &cmd, sizeof cmd) != 0)
            return -1;

    for (int w = 0; w < n_workers; w++)
    {
        StepReply rep;
        if (read_all(workers[w].res_fd, &rep, sizeof rep) != 0)
            return -1;
        if (!rep.ok)
        {
            /* Drain the rest of the workers' replies before returning, or the
             * next step would read this step's leftovers. */
            if (rep.session_lost)
                session_lost = true;
            else
                return -1;
            continue;
        }

        st->sent += rep.sent;
        st->completed += rep.completed;
        st->blocked += rep.blocked;
        st->unanswered += rep.unanswered;
        if (rep.duration_ms > st->duration_ms)
            st->duration_ms = rep.duration_ms;

        if (rep.n_samples > 0)
        {
            if (read_all(workers[w].res_fd, all_lat + merged,
                         sizeof *all_lat * (size_t)rep.n_samples) != 0 ||
                read_all(workers[w].res_fd, all_lag + merged,
                         sizeof *all_lag * (size_t)rep.n_samples) != 0)
                return -1;
            merged += rep.n_samples;
        }
    }

    if (session_lost)
        return STEP_SESSION_LOST;

    /* Percentiles off the union of every worker's samples, sorted. Exact
     * rather than bucketed, and merged rather than averaged: the mean of
     * three p99s is not the p99 of anything. */
    qsort(all_lat, (size_t)merged, sizeof *all_lat, cmp_u32);
    st->p50_us = percentile_us(all_lat, merged, 0.50);
    st->p90_us = percentile_us(all_lat, merged, 0.90);
    st->p99_us = percentile_us(all_lat, merged, 0.99);
    st->p999_us = percentile_us(all_lat, merged, 0.999);
    st->max_us = merged > 0 ? (long)all_lat[merged - 1] : 0;

    qsort(all_lag, (size_t)merged, sizeof *all_lag, cmp_u32);
    st->lag_p99_us = percentile_us(all_lag, merged, 0.99);

    double seconds = st->duration_ms > 0 ? (double)st->duration_ms / 1000.0 : 1;
    st->achieved_rps = (double)st->completed / seconds;

    st->harness_bound =
        st->lag_p99_us > (long)(HARNESS_LAG_SHARE * (double)st->p99_us) &&
        st->lag_p99_us > HARNESS_LAG_FLOOR_US;

    long long offered = st->sent + st->blocked;
    double unanswered_pct =
        offered > 0 ? 100.0 * (double)(st->unanswered + st->blocked) /
                          (double)offered
                    : 0.0;
    st->meets_slo = st->p99_us <= (long)cfg.slo_ms * 1000 &&
                    unanswered_pct <= MAX_UNANSWERED_PCT;
    return 0;
}

/* ---- reporting ---------------------------------------------------------- */

static void report_step(FILE *csv, const StepStats *s, const char *kind)
{
    fprintf(csv,
            "%s,%d,%d,%d,%.0f,%.1f,%lld,%lld,%lld,%lld,%ld,%ld,%ld,%ld,%ld,"
            "%ld,%d,%d\n",
            kind, cfg.clients, cfg.room_size, n_workers, s->offered_rps,
            s->achieved_rps, s->sent, s->completed, s->blocked, s->unanswered,
            s->p50_us, s->p90_us, s->p99_us, s->p999_us, s->max_us,
            s->lag_p99_us, s->harness_bound ? 1 : 0, s->meets_slo ? 1 : 0);
    fflush(csv);

    printf("  %-6s offered=%-8.0f achieved=%-8.1f  p50=%-8.3f p99=%-8.3f "
           "p99.9=%-8.3f max=%-8.3f  lag99=%-7.3f blocked=%-6lld unans=%-5lld  "
           "%s%s\n",
           kind, s->offered_rps, s->achieved_rps, s->p50_us / 1000.0,
           s->p99_us / 1000.0, s->p999_us / 1000.0, s->max_us / 1000.0,
           s->lag_p99_us / 1000.0, s->blocked, s->unanswered,
           s->meets_slo ? "OK " : "SLO BREACH",
           s->harness_bound ? "  [HARNESS-BOUND]" : "");
    fflush(stdout);
}

int main(void)
{
    char csv_path[PATH_MAX];
    TestEnv env;
    FILE *csv = NULL;
    int rc = 1;

    /* A client whose session process has already gone takes its next write
     * with it otherwise: the default SIGPIPE kills the benchmark mid-step,
     * and the step it was in reports nothing at all. */
    signal(SIGPIPE, SIG_IGN);

    if (read_config() != 0)
        return 1;

    all_lat = malloc(sizeof *all_lat * MAX_SAMPLES * (size_t)cfg.workers);
    all_lag = malloc(sizeof *all_lag * MAX_SAMPLES * (size_t)cfg.workers);
    if (all_lat == NULL || all_lag == NULL)
    {
        fprintf(stderr, "bench: out of memory for merged samples\n");
        goto done;
    }

    if (make_dirs(cfg.csv_dir) != 0 ||
        snprintf(csv_path, sizeof csv_path, "%s/latency_steps.csv",
                 cfg.csv_dir) >= (int)sizeof csv_path)
    {
        fprintf(stderr, "bench: cannot use CSV directory %s\n", cfg.csv_dir);
        goto done;
    }

    csv = fopen(csv_path, "w");
    if (csv == NULL)
    {
        perror("bench: csv");
        goto done;
    }
    fprintf(csv, "kind,clients,room_size,workers,offered_rps,achieved_rps,"
                 "sent,completed,blocked,unanswered,p50_us,p90_us,p99_us,"
                 "p999_us,max_us,send_lag_p99_us,harness_bound,meets_slo\n");

    printf("tetriSH request-latency benchmark\n");
    printf("  clients=%d (server cap %d)  room_size=%d  rooms=%d  workers=%d\n",
           cfg.clients, MAX_SESSIONS, cfg.room_size,
           (cfg.clients + cfg.room_size - 1) / cfg.room_size, cfg.workers);
    printf("  probe=JOIN-while-joined (409)  window=%ds  warmup=%ds  "
           "SLO: p99 <= %d ms\n",
           cfg.duration_s, cfg.warmup_s, cfg.slo_ms);
    printf("  csv -> %s\n\n", csv_path);
    fflush(stdout);

    int port = start_daemon(&env);
    if (port < 0)
    {
        fprintf(stderr, "bench: could not start tetrisd\n");
        goto done;
    }

    printf("connecting %d clients across %d load generators ...\n",
           cfg.clients, cfg.workers);
    fflush(stdout);
    if (start_workers(port, env.ca_path) != 0)
        goto stop;
    printf("all %d clients in rooms; sweeping offered rate\n\n", cfg.clients);
    fflush(stdout);

    /* Ascending sweep: stop at the first rate whose p99 breaks the SLO, then
     * bisect between it and the last rate that held. The knee is where a
     * capacity number actually is - reporting only the coarse pass/fail pair
     * would leave it anywhere inside a doubling. */
    double last_ok = 0, first_bad = 0;
    /*
     * The rate at which tetrisd started hanging up, if it did.
     *
     * Once it has, the run stops: those clients are gone for good, so every
     * later rate would be measured against a smaller server than the earlier
     * ones. Stopping and saying so beats quietly reporting a knee found with
     * a different number of clients than the rows above it.
     */
    double dropped_at = 0;
    StepStats st;

    for (int step = 0; step < cfg.rate_steps; step++)
    {
        double rate = (double)cfg.rate_start;
        for (int k = 0; k < step; k++)
            rate *= 1.0 + (double)cfg.rate_growth_pct / 100.0;
        if (rate > cfg.rate_max)
            break;

        int step_rc = run_step(rate, &st);
        if (step_rc == STEP_SESSION_LOST)
        {
            dropped_at = rate;
            first_bad = rate;
            printf("  sweep  offered=%-8.0f the server dropped a session; "
                   "stopping here\n",
                   rate);
            break;
        }
        if (step_rc != 0)
        {
            fprintf(stderr, "bench: step at %.0f rps broke\n", rate);
            goto stop;
        }
        report_step(csv, &st, "sweep");

        if (st.meets_slo)
            last_ok = rate;
        else
        {
            first_bad = rate;
            break;
        }
    }

    if (first_bad > 0 && last_ok > 0)
    {
        printf("\nrefining between %.0f and %.0f rps\n", last_ok, first_bad);
        fflush(stdout);
        for (int i = 0; i < cfg.refine_steps; i++)
        {
            double mid = (last_ok + first_bad) / 2.0;
            if (mid - last_ok < 1.0)
                break;
            int step_rc = run_step(mid, &st);
            if (step_rc == STEP_SESSION_LOST)
            {
                dropped_at = mid;
                first_bad = mid;
                printf("  refine offered=%-8.0f the server dropped a session; "
                       "stopping here\n",
                       mid);
                break;
            }
            if (step_rc != 0)
            {
                fprintf(stderr, "bench: refine step at %.0f rps broke\n", mid);
                goto stop;
            }
            report_step(csv, &st, "refine");
            if (st.meets_slo)
                last_ok = mid;
            else
                first_bad = mid;
        }
    }

    printf("\n=== result ==============================================\n");
    printf("  clients            : %d across %d load generators\n", cfg.clients,
           n_workers);
    printf("  SLO                : p99 <= %d ms\n", cfg.slo_ms);
    if (last_ok > 0)
        printf("  max throughput     : %.0f req/s within SLO\n", last_ok);
    else
        printf("  max throughput     : none - the SLO was already missed at "
               "%d req/s\n",
               cfg.rate_start);
    if (first_bad > 0)
        printf("  p99 breaks at      : %.0f req/s\n", first_bad);
    else
        printf("  p99 breaks at      : not reached below %d req/s\n",
               cfg.rate_max);
    if (dropped_at > 0)
        printf("  sessions dropped at: %.0f req/s (the server hung up rather "
               "than queueing)\n",
               dropped_at);
    printf("  per-rate p50/p99/p99.9 : %s\n", csv_path);
    printf("=========================================================\n");
    rc = 0;

stop:
    stop_workers();
    stop_daemon(&env);
    clean_env(&env);
done:
    if (csv != NULL)
        fclose(csv);
    free(all_lat);
    free(all_lag);
    return rc;
}
