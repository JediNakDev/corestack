#ifndef TETRISCTL_LOGTAIL_H
#define TETRISCTL_LOGTAIL_H

#include <stdbool.h>

/** The maximum number of log lines kept in the in-memory circular buffer. */
#define LOGTAIL_RING_LINES 200
#define LOGTAIL_LINE_LEN 256

typedef struct logtail logtail_t;

logtail_t *logtail_init(void);
logtail_t *logtail_init_at(const char *path);

void logtail_poll(logtail_t *t);

bool is_logtail_missing(const logtail_t *t);

/* The current tail, oldest kept line first. */
int logtail_lines(const logtail_t *t, const char *out[], int max);

void logtail_close(logtail_t *t);

#endif /* TETRISCTL_LOGTAIL_H */
