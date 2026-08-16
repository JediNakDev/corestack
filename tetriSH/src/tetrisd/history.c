#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libhtttp/htttp.h"
#include "libtetrisdb/schema.h"
#include "libtetrisdb/socket/conf.h"
#include "libtetrisdb/socket/db.h"
#include "libtetrissh/tetrissh.h"
#include "libtetrisutil/limits.h"
#include "libtetrisutil/logmsg.h"
#include "libtetrisutil/rc.h"
#include "tetrisd/history.h"

/** buffer size for body read from db */
#define BODY_MAX 4096
#define WIDE_BODY_MAX 16384

#define SEM_WAIT_MS 1000
#define SEM_POLL_MIN_US 1000
#define SEM_POLL_SPAN_US 4000

#define TXN_ATTEMPTS 3
#define TXN_BACKOFF_US 5000

/** A quoted username: two quotes, a NUL, and room for the doubling
 *db_quote() applies to any inner quote. */
#define QUOTED_MAX (MAX_USER_NAME * 2 + 4)

static unsigned poll_delay_us(void)
{
    return SEM_POLL_MIN_US + (unsigned)getpid() % SEM_POLL_SPAN_US;
}

static db_socket_t *conn_open(void)
{
    db_socket_opts_t opts;

    db_socket_opts_load(&opts);
    (void)rc_get("db_ipc", DB_DEFAULT_IPC, opts.sock, sizeof opts.sock);
    (void)rc_get_int("db_timeout", DB_DEFAULT_TIMEOUT_MS, DB_TIMEOUT_MIN_MS,
                     DB_TIMEOUT_MAX_MS, &opts.timeout_ms);

    return db_socket_open(&opts);
}

static sem_t *sem_acquire(void)
{
    sem_t *sem = sem_open(HISTORY_SEM_NAME, O_CREAT, 0600, 1);
    if (sem == SEM_FAILED)
    {
        (void)log_send(LOG_DEBUG,
                       "history: sem_open(" HISTORY_SEM_NAME ") failed: %s",
                       strerror(errno));
        return NULL;
    }

    for (int waited_us = 0; waited_us < SEM_WAIT_MS * 1000;)
    {
        if (sem_trywait(sem) == 0)
            return sem;

        unsigned nap = poll_delay_us();
        usleep(nap);
        waited_us += (int)nap;
    }

    (void)log_send(LOG_DEBUG,
                   "history: no turn at " HISTORY_SEM_NAME " within %d ms",
                   SEM_WAIT_MS);
    sem_close(sem);
    return NULL;
}

static void sem_release(sem_t *sem)
{
    if (sem == NULL)
        return;
    sem_post(sem);
    sem_close(sem);
}

/* Allocates an id and inserts the row, as one transaction on conn. */
static db_status_t history_insert_txn(db_socket_t *conn, const char *quoted,
                                      int player_id, int score, int lines,
                                      long long ts_start, long long ts_end)
{
    char body[BODY_MAX];
    char sql[512];
    db_status_t st;

    st = db_socket_exec(conn, "set transaction read write;", NULL, 0);
    if (st != DB_OK)
        return st;

    st = db_socket_exec(conn, "select max(id) from " HISTORY_DB_TABLE ";", body,
                        sizeof body);
    if (st != DB_OK)
        return st;

    long long id = db_next_id(body);
    if (id < 0)
    {
        (void)log_send(LOG_DEBUG,
                       "history: could not read max(id) from the reply");
        return DB_ERROR;
    }

    snprintf(sql, sizeof sql,
             "insert into " HISTORY_DB_TABLE
             " values (%lld, %d, %s, %d, %d, %lld, %lld);",
             id, player_id, quoted, score, lines, ts_start, ts_end);

    st = db_socket_exec(conn, sql, NULL, 0);
    if (st != DB_OK)
        return st;

    return db_socket_exec(conn, "commit;", NULL, 0);
}

void history_db_insert(int player_id, const char *user_name, int score,
                       int lines, long long ts_start, long long ts_end)
{
    char quoted[QUOTED_MAX];
    db_quote(quoted, sizeof quoted, user_name);

    sem_t *sem = sem_acquire();
    if (sem == NULL)
    {
        (void)log_send(LOG_WARN,
                       "operation=history_db_insert phase=complete status=%d",
                       DB_TIMEOUT);
        return;
    }

    db_status_t st = DB_IO;
    db_socket_t *conn = conn_open();

    if (conn == NULL)
    {
        (void)log_send(LOG_DEBUG, "history: the database is unreachable");
    }
    else
    {
        for (int attempt = 0; attempt < TXN_ATTEMPTS; attempt++)
        {
            st = history_insert_txn(conn, quoted, player_id, score, lines,
                                    ts_start, ts_end);
            if (st != DB_RETRY)
                break;
            usleep(TXN_BACKOFF_US);
        }
        db_socket_close(conn);
    }

    sem_release(sem);

    (void)log_send(st == DB_OK ? LOG_INFO : LOG_WARN,
                   "operation=history_db_insert phase=complete status=%d", st);
}

/* --- reading the table back ----------------------------------------------- */

static int field_to_ll(const char *p, size_t len, long long *out)
{
    char text[32];
    if (len == 0 || len >= sizeof text)
        return -1;
    memcpy(text, p, len);
    text[len] = '\0';
    char *end;
    *out = strtoll(text, &end, 10);
    return *end == '\0' ? 0 : -1;
}

static void field_to_str(const char *p, size_t len, char *dst, size_t cap)
{
    if (len >= cap)
        len = cap - 1;
    memcpy(dst, p, len);
    dst[len] = '\0';
}

/** Runs sql, which must be an aggregate expected to return exactly one row
 * with one field (max(...) or count(...)  */
static long long read_scalar(db_socket_t *conn, const char *sql)
{
    char body[BODY_MAX];
    if (db_socket_exec(conn, sql, body, sizeof body) != DB_OK)
        return -1;
    if (db_row_count(body) != 1)
        return -1;

    const char *f[1];
    size_t len[1];
    if (db_row_fields(body, 0, f, len, 1) != 1)
        return -1;

    long long v;
    return field_to_ll(f[0], len[0], &v) == 0 ? v : -1;
}

void history_db_read_player(const char *user_name, player_history_t *out)
{
    memset(out, 0, sizeof *out);
    out->status = HISTORY_VIEW_UNAVAILABLE;

    char quoted[QUOTED_MAX];
    db_quote(quoted, sizeof quoted, user_name);

    db_socket_t *conn = conn_open();
    if (conn == NULL)
    {
        (void)log_send(LOG_DEBUG,
                       "history: player history query: database unreachable");
        return;
    }

    char sql[256];
    char body[BODY_MAX];
    snprintf(sql, sizeof sql,
             "select score, lines, ts_start, ts_end from " HISTORY_DB_TABLE
             " where user_name = %s order by id desc;",
             quoted);

    if (db_socket_exec(conn, sql, body, sizeof body) != DB_OK)
    {
        db_socket_close(conn);
        (void)log_send(LOG_DEBUG, "history: player history query failed");
        return;
    }

    int rows = db_row_count(body);
    if (rows < 0)
    {
        db_socket_close(conn);
        (void)log_send(LOG_DEBUG,
                       "history: player history reply carried no table");
        return;
    }
    if (rows == 0)
    {
        db_socket_close(conn);
        out->status = HISTORY_VIEW_EMPTY;
        return;
    }

    int n = rows < HISTORY_VIEW_ROUNDS ? rows : HISTORY_VIEW_ROUNDS;
    for (int i = 0; i < n; i++)
    {
        const char *f[4];
        size_t len[4];
        long long score, lines, ts_start, ts_end;

        if (db_row_fields(body, i, f, len, 4) != 4 ||
            field_to_ll(f[0], len[0], &score) != 0 ||
            field_to_ll(f[1], len[1], &lines) != 0 ||
            field_to_ll(f[2], len[2], &ts_start) != 0 ||
            field_to_ll(f[3], len[3], &ts_end) != 0)
        {
            db_socket_close(conn);
            (void)log_send(LOG_DEBUG,
                           "history: a history row has an unreadable field");
            return;
        }
        out->recent[i].score = (int32_t)score;
        out->recent[i].lines = (int32_t)lines;
        out->recent[i].ts_start = (int32_t)ts_start;
        out->recent[i].ts_end = (int32_t)ts_end;
    }
    out->recent_count = n;

    snprintf(sql, sizeof sql,
             "select max(score) from " HISTORY_DB_TABLE
             " where user_name = %s;",
             quoted);
    long long best_score = read_scalar(conn, sql);

    snprintf(sql, sizeof sql,
             "select max(lines) from " HISTORY_DB_TABLE
             " where user_name = %s;",
             quoted);
    long long best_lines = read_scalar(conn, sql);

    snprintf(sql, sizeof sql,
             "select count(id) from " HISTORY_DB_TABLE " where user_name = %s;",
             quoted);
    long long games_played = read_scalar(conn, sql);

    db_socket_close(conn);

    if (best_score < 0 || best_lines < 0 || games_played < 0)
    {
        (void)log_send(LOG_DEBUG,
                       "history: a player history aggregate query failed");
        return; /* out->status stays HISTORY_VIEW_UNAVAILABLE */
    }

    out->best_score = (int32_t)best_score;
    out->best_lines = (int32_t)best_lines;
    out->games_played = (int32_t)games_played;
    out->status = HISTORY_VIEW_OK;
}

int history_db_read_recent(history_row_t *out, int max)
{
    db_socket_t *conn = conn_open();
    if (conn == NULL)
    {
        (void)log_send(LOG_DEBUG,
                       "history: recent-rounds query: database unreachable");
        return -1;
    }

    char body[WIDE_BODY_MAX];
    db_status_t st =
        db_socket_exec(conn,
                       "select user_name, score, lines, ts_start, ts_end "
                       "from " HISTORY_DB_TABLE " order by id desc;",
                       body, sizeof body);
    db_socket_close(conn);
    if (st != DB_OK)
    {
        (void)log_send(LOG_DEBUG, "history: recent-rounds query failed");
        return -1;
    }

    int rows = db_row_count(body);
    if (rows < 0)
    {
        (void)log_send(LOG_DEBUG,
                       "history: recent-rounds reply carried no table");
        return -1;
    }

    int n = rows < max ? rows : max;
    for (int i = 0; i < n; i++)
    {
        const char *f[5];
        size_t len[5];
        long long score, lines, ts_start, ts_end;

        if (db_row_fields(body, i, f, len, 5) != 5 ||
            field_to_ll(f[1], len[1], &score) != 0 ||
            field_to_ll(f[2], len[2], &lines) != 0 ||
            field_to_ll(f[3], len[3], &ts_start) != 0 ||
            field_to_ll(f[4], len[4], &ts_end) != 0)
        {
            (void)log_send(LOG_DEBUG,
                           "history: a recent-rounds row has an unreadable "
                           "field");
            return -1;
        }
        field_to_str(f[0], len[0], out[i].user_name, sizeof out[i].user_name);
        out[i].score = (int)score;
        out[i].lines = (int)lines;
        out[i].ts_start = ts_start;
        out[i].ts_end = ts_end;
    }
    return n;
}

int history_db_read_best_scores(history_row_t *out, int max)
{
    db_socket_t *conn = conn_open();
    if (conn == NULL)
    {
        (void)log_send(LOG_DEBUG,
                       "history: best-scores query: database unreachable");
        return -1;
    }

    char body[WIDE_BODY_MAX];
    db_status_t st =
        db_socket_exec(conn,
                       "select user_name, max(score) from " HISTORY_DB_TABLE
                       " group by user_name;",
                       body, sizeof body);
    db_socket_close(conn);
    if (st != DB_OK)
    {
        (void)log_send(LOG_DEBUG, "history: best-scores query failed");
        return -1;
    }

    int rows = db_row_count(body);
    if (rows < 0)
    {
        (void)log_send(LOG_DEBUG,
                       "history: best-scores reply carried no table");
        return -1;
    }

    int n = rows < max ? rows : max;
    for (int i = 0; i < n; i++)
    {
        const char *f[2];
        size_t len[2];
        long long score;

        if (db_row_fields(body, i, f, len, 2) != 2 ||
            field_to_ll(f[1], len[1], &score) != 0)
        {
            (void)log_send(
                LOG_DEBUG,
                "history: a best-scores row has an unreadable field");
            return -1;
        }
        field_to_str(f[0], len[0], out[i].user_name, sizeof out[i].user_name);
        out[i].score = (int)score;
        out[i].lines = 0;
        out[i].ts_start = 0;
        out[i].ts_end = 0;
    }
    return n;
}

/* --- HISTORY dispatch arm ------------------------------------------------- */

/** Serializes ph as UPD_HISTORY /player/history and sends it. Called once by
 * history_offer(), with the result of a read (or the guest/unavailable
 * status, if none was attempted). Returns 0, or -1 on a serialize or send
 * failure. */
static int push_history(session_t *sh, const player_history_t *ph)
{
    htttp_request_t req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "UPD_HISTORY");
    snprintf(req.path, sizeof req.path, "/player/history");
    (void)htttp_header_set(req.headers, &req.n_headers, "Content-Type",
                           "application/tetris-state");
    req.body = (const uint8_t *)ph;
    req.body_len = (uint32_t)sizeof *ph;

    uint8_t out[SESSION_MAX_FRAME];
    uint32_t out_len = sizeof out;
    if (htttp_serialize_request(&req, out, &out_len) != HTTTP_OK)
        return -1;
    return session_send(sh, out, out_len) == SESSION_OK ? 0 : -1;
}

/** Sends a bare status response - used for the 409 refusal only. */
static int push_status(session_t *sh, int status)
{
    htttp_response_t res;
    memset(&res, 0, sizeof res);
    res.status = status;

    uint8_t out[SESSION_MAX_FRAME];
    uint32_t out_len = sizeof out;
    if (htttp_serialize_response(&res, out, &out_len) != HTTTP_OK)
        return -1;
    return session_send(sh, out, out_len) == SESSION_OK ? 0 : -1;
}

bool history_offer(const htttp_request_t *req, session_t *sh,
                   const char *user_name, SessionPhase phase)
{
    if (strcmp(req->method, "HISTORY") != 0)
        return false;

    if (phase == SESSION_PLAYING)
    {
        int rc = push_status(sh, 409);
        (void)log_send(LOG_WARN,
                       "operation=history_offer phase=complete status=409 "
                       "rc=%d",
                       rc);
        return true;
    }

    player_history_t ph;
    memset(&ph, 0, sizeof ph);

    if (user_name[0] == '\0')
        ph.status = HISTORY_VIEW_GUEST;
    else
        history_db_read_player(user_name, &ph);

    int rc = push_history(sh, &ph);
    (void)log_send(rc == 0 ? LOG_INFO : LOG_ERROR,
                   "operation=history_offer phase=complete view_status=%d "
                   "rc=%d",
                   ph.status, rc);
    return true;
}
