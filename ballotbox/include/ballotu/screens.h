#ifndef BALLOTU_SCREENS_H
#define BALLOTU_SCREENS_H

/* returns CERT_VALID/EXPIRED/... via mock_check_voter_cert; loops until a
 * name is entered or the user cancels. Returns 1 if logged in, 0 if quit. */
int screen_login(void);

void screen_join_election(void);   /* UC-2 */
void screen_cast_vote(void);       /* UC-3 (routes to update per alt flow) */
void screen_update_vote(void);     /* UC-4 (routes to cast per alt flow) */
void screen_view_results(void);    /* UC-5 */
void screen_check_vote(void);      /* UC-6 */

#endif
