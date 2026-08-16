#ifndef LIBTETRISUI_TETRISUI_H
#define LIBTETRISUI_TETRISUI_H

/*
 * libtetrisui - modal ncurses widgets (menus, forms, message boxes).
 *
 * Vendored from the BallotBox project's ballottui and renamed. The two
 * codebases already share libtetrissh and libhtttp the same way - by copy, not
 * by submodule - so keep fixes flowing in both directions.
 *
 * SCOPE / WARNING: every widget here is MODAL. It owns the terminal and blocks
 * in wgetch() until the user answers, which is safe only while nothing else
 * needs servicing. tetrisu uses these for the pre-JOIN screens, where the
 * server is provably silent. The wait-for-start screen must NOT use them: a
 * pushed UPD_GAME would sit unread in the socket buffer. See TETRISU_PLAN.md.
 */

/* Bounds the widgets enforce. A caller passing more than this is a
 * programming error and is rejected, rather than the widget reading past the
 * end of the caller's own array. */
#define TETRISUI_MAX_ITEMS 16
#define TETRISUI_MAX_FIELDS 8
#define TETRISUI_FIELD_LEN 128

void tetrisui_init(void);
void tetrisui_shutdown(void);

/* status bar shown at the bottom of every screen */
void tetrisui_set_status(const char *app, const char *actor, const char *state);
void tetrisui_draw_status_bar(const char *hint);

/* Arrow-key menu; returns the selected index, or -1 if the user pressed
 * 'q'/ESC. Also -1 when count <= 0 or count > TETRISUI_MAX_ITEMS. */
int tetrisui_menu(const char *title, const char *items[], int count,
                  const char *hint);

/* single-line text input; returns 0 on ok, -1 if cancelled (ESC) */
int tetrisui_input(const char *title, const char *prompt, char *out,
                   int out_len);

/* multi-field form; labels[count], values[count] pre-filled/edited in place.
 * returns 0 if submitted, -1 if cancelled (ESC) */
int tetrisui_form(const char *title, const char *labels[],
                  char values[][TETRISUI_FIELD_LEN], int count);

/*
 * Extended form: adds what a re-opened, refused form needs and tetrisui_form()
 * does not carry (#50).
 *
 *   mask        bit i set -> field i is drawn as '*' and typed as '*', for a
 *               password field sitting next to an unmasked one. A field, not
 *               a form, property - the alternative is a second parallel bool
 *               array the caller has to keep in step with labels/values.
 *   error       one line under the fields, or NULL for none. A re-opened form
 *               is the only thing on screen, so it is where the refusal
 *               reason belongs.
 *   start_field which field has focus when the form opens - the field the
 *               refusal says is wrong, not always field 0.
 *
 * tetrisui_form() is this with mask 0, error NULL, start_field 0.
 */
int tetrisui_form_ex(const char *title, const char *labels[],
                     char values[][TETRISUI_FIELD_LEN], int count,
                     unsigned mask, const char *error, int start_field);

/* yes/no confirmation; returns 1 for yes, 0 for no (ESC and 'q' mean no) */
int tetrisui_confirm(const char *title, const char *question);

/* simple message box, waits for a keypress */
void tetrisui_message(const char *title, const char *lines[], int line_count);

/* scrollable read-only list view, waits for 'q'/ESC/ENTER */
void tetrisui_list_view(const char *title, const char *lines[], int line_count);

/*
 * Scripted progress animation: every step is reported OK after a fixed delay,
 * so it says nothing about whether the work actually succeeded. Cosmetic only.
 */
void tetrisui_progress(const char *title, const char *steps[], int step_count);

/*
 * Real progress - the caller drives it, so a step can genuinely fail.
 *
 * begin() opens the panel with every step pending; step(i, ok) marks step i
 * OK or FAILED; end() closes it. Needed because the tetrissh handshake really
 * does fail (SESSION_ERR_AUTH on a forged cert), and an animation that always
 * prints OK would lie about it.
 */
void tetrisui_progress_begin(const char *title, const char *steps[],
                             int step_count);
void tetrisui_progress_step(int index, int ok);
void tetrisui_progress_end(void);

#endif
