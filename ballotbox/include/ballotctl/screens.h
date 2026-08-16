#ifndef BALLOTCTL_SCREENS_H
#define BALLOTCTL_SCREENS_H

int screen_login(void); /* returns 1 if logged in, 0 if quit */

void screen_create_election(void);  /* UC-1 */
void screen_open_election(void);    /* UC-1 lifecycle */
void screen_close_election(void);   /* lifecycle */
void screen_publish_results(void);  /* lifecycle */
void screen_view_results(void);     /* UC-5 */
void screen_election_status(void);  /* status list */

#endif
