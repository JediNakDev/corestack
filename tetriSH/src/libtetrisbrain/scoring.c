/*
 * scoring.c - score / lines updates.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "libtetrisbrain/scoring.h"

void scoring_add(GameState *g, int cleared)
{
    /* Points for clearing 0..4 lines at once. A tetris (4) is worth far more
     * than four singles, which is the classic incentive to stack. */
    static const int PTS[5] = {0, 100, 300, 500, 800};

    if (cleared <= 0)
        return;
    if (cleared > 4)
        cleared = 4;

    g->lines += cleared;
    g->score += PTS[cleared];
    /* level is held constant for now -> constant falling speed. */
}
