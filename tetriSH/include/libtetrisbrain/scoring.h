#ifndef TETRISBRAIN_SCORING_H
#define TETRISBRAIN_SCORING_H

/*
 * scoring.h - score / lines / level updates after a line clear.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "libtetrisbrain/model.h"

/* Apply `cleared` (0..4) lines: bump lines and add points (more for multi-line
 * clears). Level is left constant for now (constant falling speed). */
void scoring_add(GameState *g, int cleared);

#endif /* TETRISBRAIN_SCORING_H */