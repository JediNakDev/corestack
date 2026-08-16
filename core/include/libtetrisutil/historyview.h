#ifndef LIBTETRISUTIL_HISTORYVIEW_H
#define LIBTETRISUTIL_HISTORYVIEW_H

#include <stdint.h>

/** How many past rounds ride in one player_history_t. */
#define HISTORY_VIEW_ROUNDS 5

typedef enum
{
    HISTORY_VIEW_OK = 0,
    HISTORY_VIEW_EMPTY,
    HISTORY_VIEW_GUEST,
    HISTORY_VIEW_UNAVAILABLE
} history_view_status_t;

/* One finished round. */
typedef struct
{
    int32_t score;
    int32_t lines;
    /* ts_start/ts_end are 32-bit here because SimpleDB dont have datetime. */
    int32_t ts_start;
    int32_t ts_end;
} history_round_t;

/** UPD_HISTORY's body: the asking player's own rounds and lifetime figures. */
typedef struct
{
    history_view_status_t status;
    history_round_t recent[HISTORY_VIEW_ROUNDS]; /**< Newest first. */
    int recent_count; /**< Valid entries in recent[]; 0 unless status == OK. */
    int32_t best_score;
    int32_t best_lines;
    int32_t games_played;
} player_history_t;

static inline long history_survived_secs(long long ts_start, long long ts_end)
{
    long long d = ts_end - ts_start;
    return d < 0 ? 0 : (long)d;
}

#endif /* LIBTETRISUTIL_HISTORYVIEW_H */
