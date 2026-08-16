#ifndef TETRISCTL_CTL_TUI_H
#define TETRISCTL_CTL_TUI_H

/*
 * ctl_tui.h - the live admin console.
 *
 * Entered by a bare `tetrisctl` on a tty. Adds no capability the argv CLI does
 * not have; everything here is ctl_client and ctl_lifecycle with a face on it.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

/* Runs until the user quits. Returns a process exit code. */
int ctl_tui_run(const char *sock);

#endif /* TETRISCTL_CTL_TUI_H */
