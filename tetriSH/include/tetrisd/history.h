#ifndef TETRISD_HISTORY_H
#define TETRISD_HISTORY_H

#include <stdbool.h>

#include "libhtttp/htttp.h"
#include "libtetrisutil/historyview.h"
#include "libtetrisutil/limits.h" /* MAX_USER_NAME */
#include "libtetrisutil/sessionstate.h" /* SessionPhase */
#include "libtetrissh/tetrissh.h" /* session_t */

#define HISTORY_DB_TABLE "history"
#define HISTORY_DB_SCHEMA                                                      \
    "id int, player_id int, user_name string, score int, lines int, "          \
    "ts_start int, ts_end int"

#define HISTORY_SEM_NAME "/tetrish_history"

/* Records one finished round. */
void history_db_insert(int player_id, const char *user_name, int score, int lines,
                    long long ts_start, long long ts_end);

typedef struct
{
    char user_name[MAX_USER_NAME];
    int score;
    int lines;
    long long ts_start;
    long long ts_end;
} history_row_t;

/** Suggested capacity for a history_db_read_best_scores() caller's array */
#define HISTORY_BEST_SCORES_MAX 256

void history_db_read_player(const char *user_name, player_history_t *out);

int history_db_read_recent(history_row_t *out, int max);

/* Reads every distinct account's all-time best score. */
int history_db_read_best_scores(history_row_t *out, int max);

/* check if a history request, if yes proceed, otherwise skip */
bool history_offer(const htttp_request_t *req, session_t *sh,
                   const char *user_name, SessionPhase phase);

#endif /* TETRISD_HISTORY_H */
