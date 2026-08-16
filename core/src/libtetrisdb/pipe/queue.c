#include "proc.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Consecutive respawns before giving up on the child for good. */
#define DB_RESPAWN_MAX 3

struct db
{
    db_proc_t proc;
    db_opts_t opts;

    char **items; /* ring of owned strings, queue_cap entries */
    size_t cap, head, tail, count;

    int stopping; /* set by db_stop: drain, then exit the worker */
    int dead;     /* child unusable and out of respawns */

    unsigned long dropped;
    unsigned long errors;

    pthread_mutex_t m;
    pthread_cond_t not_empty;
    pthread_t worker;
    int worker_live;
};

static char *dequeue(db_t *db)
{
    pthread_mutex_lock(&db->m);
    while (db->count == 0 && !db->stopping)
        pthread_cond_wait(&db->not_empty, &db->m);

    char *sql = NULL;
    if (db->count > 0)
    {
        sql = db->items[db->head];
        db->head = (db->head + 1) % db->cap;
        db->count--;
    }
    pthread_mutex_unlock(&db->m);
    return sql; /* NULL only when stopping with an empty queue */
}

static void execute(db_t *db, const char *sql)
{
    char body[DB_BODY_MAX];

    for (int attempt = 0; attempt <= DB_RESPAWN_MAX; attempt++)
    {
        if (db->dead)
        {
            pthread_mutex_lock(&db->m);
            db->dropped++;
            pthread_mutex_unlock(&db->m);
            return;
        }

        int r = db_proc_exec(&db->proc, sql, body, sizeof(body));
        if (r >= 0)
        {
            if (r == 0)
            {
                /* SQL the child rejected. Retrying cannot help */
                pthread_mutex_lock(&db->m);
                db->errors++;
                pthread_mutex_unlock(&db->m);
                fprintf(stderr, "tetrisdb: statement failed: %s\n", body);
            }
            return;
        }

        /* The child is gone. Reap it and try to bring up a replacement. */
        db_proc_kill(&db->proc);
        if (attempt == DB_RESPAWN_MAX ||
            db_proc_spawn(&db->proc, &db->opts) < 0)
        {
            fprintf(stderr, "tetrisdb: giving up on PipeRunner; "
                            "further statements will be dropped\n");
            pthread_mutex_lock(&db->m);
            db->dead = 1;
            db->dropped++; /* the statement that just exhausted every retry */
            pthread_mutex_unlock(&db->m);
            return;
        }
        fprintf(stderr, "tetrisdb: PipeRunner restarted\n");
    }
}

static void *worker_main(void *arg)
{
    db_t *db = arg;
    char *sql;

    while ((sql = dequeue(db)) != NULL)
    {
        execute(db, sql);
        free(sql);
    }
    return NULL;
}

db_t *db_start(const db_opts_t *opts, const char *probe, char *body,
               size_t body_cap)
{
    db_t *db = calloc(1, sizeof(*db));
    if (db == NULL)
    {
        fprintf(stderr, "tetrisdb: out of memory\n");
        return NULL;
    }

    db->opts = *opts;
    db->cap = opts->queue_cap > 0 ? opts->queue_cap : 256;
    db->items = calloc(db->cap, sizeof(*db->items));
    if (db->items == NULL)
    {
        fprintf(stderr, "tetrisdb: out of memory\n");
        free(db);
        return NULL;
    }

    pthread_mutex_init(&db->m, NULL);
    pthread_cond_init(&db->not_empty, NULL);

    if (db_proc_spawn(&db->proc, &db->opts) < 0)
        goto fail;

    if (probe != NULL && db_proc_exec(&db->proc, probe, body, body_cap) < 0)
    {
        fprintf(stderr, "tetrisdb: PipeRunner died during startup probe\n");
        db_proc_kill(&db->proc);
        goto fail;
    }

    if (pthread_create(&db->worker, NULL, worker_main, db) != 0)
    {
        fprintf(stderr, "tetrisdb: cannot start worker thread\n");
        db_proc_close(&db->proc);
        goto fail;
    }
    db->worker_live = 1;
    return db;

fail:
    pthread_cond_destroy(&db->not_empty);
    pthread_mutex_destroy(&db->m);
    free(db->items);
    free(db);
    return NULL;
}

int db_submit(db_t *db, const char *sql)
{
    if (db == NULL)
        return -1;

    char *copy = strdup(sql);
    if (copy == NULL)
    {
        pthread_mutex_lock(&db->m);
        db->dropped++;
        pthread_mutex_unlock(&db->m);
        return -1;
    }

    pthread_mutex_lock(&db->m);
    if (db->count == db->cap || db->stopping || db->dead)
    {
        /* Full, shutting down, or no child left: drop. Never block - see the
         * file comment. */
        db->dropped++;
        pthread_mutex_unlock(&db->m);
        free(copy);
        return -1;
    }
    db->items[db->tail] = copy;
    db->tail = (db->tail + 1) % db->cap;
    db->count++;
    pthread_cond_signal(&db->not_empty);
    pthread_mutex_unlock(&db->m);
    return 0;
}

unsigned long get_db_dropped(db_t *db)
{
    if (db == NULL)
        return 0;
    pthread_mutex_lock(&db->m);
    unsigned long n = db->dropped;
    pthread_mutex_unlock(&db->m);
    return n;
}

unsigned long get_db_errors(db_t *db)
{
    if (db == NULL)
        return 0;
    pthread_mutex_lock(&db->m);
    unsigned long n = db->errors;
    pthread_mutex_unlock(&db->m);
    return n;
}

void db_stop(db_t *db, unsigned long *dropped, unsigned long *errors)
{
    if (db == NULL)
    {
        if (dropped != NULL)
            *dropped = 0;
        if (errors != NULL)
            *errors = 0;
        return;
    }

    /* Ask the worker to finish what is queued and exit. */
    pthread_mutex_lock(&db->m);
    db->stopping = 1;
    pthread_cond_broadcast(&db->not_empty);
    pthread_mutex_unlock(&db->m);

    if (db->worker_live)
        pthread_join(db->worker, NULL);

    db_proc_close(&db->proc);

    while (db->count > 0)
    {
        free(db->items[db->head]);
        db->head = (db->head + 1) % db->cap;
        db->count--;
        db->dropped++;
    }

    if (dropped != NULL)
        *dropped = db->dropped;
    if (errors != NULL)
        *errors = db->errors;

    pthread_cond_destroy(&db->not_empty);
    pthread_mutex_destroy(&db->m);
    free(db->items);
    free(db);
}
