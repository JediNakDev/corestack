#include "libtetrisui/tetrisui.h"

#include <ncurses.h>
#include <string.h>

static char g_app[32] = "";
static char g_actor[64] = "(not logged in)";
static char g_state[32] = "";

void tetrisui_init(void)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    /*
     * Start colour HERE, not lazily in the first screen that happens to want
     * it.
     *
     * start_color() redefines colour pair 0 - the pair everything unstyled is
     * drawn in - from "whatever the terminal is already using" to an explicit
     * white-on-black. Calling it partway through a session therefore repaints
     * every character already on screen: the game screen used to arrive with
     * the whole terminal flashing to hard white-on-black, because render.c was
     * the first thing in the program to ask for colour.
     *
     * use_default_colors() then hands pair 0 back to the terminal's own
     * foreground and background, so only the cells that explicitly ask for a
     * colour get one and the lobby keeps looking like the rest of the terminal.
     */
    if (has_colors())
    {
        start_color();
        use_default_colors();
    }
}

void tetrisui_shutdown(void)
{
    endwin();
}

void tetrisui_set_status(const char *app, const char *actor, const char *state)
{
    if (app)
        strncpy(g_app, app, sizeof(g_app) - 1);
    if (actor)
        strncpy(g_actor, actor, sizeof(g_actor) - 1);
    if (state)
        strncpy(g_state, state, sizeof(g_state) - 1);
}

void tetrisui_draw_status_bar(const char *hint)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    char buf[256];
    snprintf(buf, sizeof(buf), " %s | %s | %s | %s", g_app, g_actor,
             g_state[0] ? g_state : "-", hint ? hint : "");
    move(rows - 1, 0);
    clrtoeol();
    attron(A_REVERSE);
    mvhline(rows - 1, 0, ' ', cols);
    mvprintw(rows - 1, 0, "%.*s", cols, buf);
    attroff(A_REVERSE);
}

static WINDOW *frame_win(const char *title, int height, int width, int starty,
                         int startx)
{
    /* wipe remnants of any previously drawn (possibly larger) window */
    clear();
    refresh();
    WINDOW *win = newwin(height, width, starty, startx);
    /* newwin returns NULL if the geometry does not fit the terminal (or on an
     * allocation failure). Every caller dereferences the result, so returning
     * NULL unchecked turns a too-small terminal into a segfault. */
    if (!win)
        return NULL;
    box(win, 0, 0);
    if (title)
    {
        mvwprintw(win, 0, 2, " %s ", title);
    }
    return win;
}

int tetrisui_menu(const char *title, const char *items[], int count,
                  const char *hint)
{
    /* count == 0 would make the wrap-around arithmetic below compute
     * (sel - 1 + 0) % 0 -> SIGFPE. count > TETRISUI_MAX_ITEMS means the
     * caller's array is smaller than it claims, so refuse rather than read past
     * it. */
    if (count <= 0 || count > TETRISUI_MAX_ITEMS)
        return -1;

    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int height = count + 4;
    int width = cols - 10 > 40 ? cols - 10 : 40;
    int starty = (rows - height) / 2;
    int startx = (cols - width) / 2;
    WINDOW *win = frame_win(title, height, width, starty, startx);
    if (!win)
        return -1;
    keypad(win, TRUE);

    int sel = 0;
    int ch;
    do
    {
        for (int i = 0; i < count; i++)
        {
            if (i == sel)
                wattron(win, A_REVERSE);
            mvwprintw(win, i + 2, 2, "%-*s", width - 4, items[i]);
            if (i == sel)
                wattroff(win, A_REVERSE);
        }
        tetrisui_draw_status_bar(hint ? hint
                                      : "Up/Down move  Enter select  q back");
        wrefresh(win);
        refresh();
        ch = wgetch(win);
        if (ch == KEY_UP)
            sel = (sel - 1 + count) % count;
        else if (ch == KEY_DOWN)
            sel = (sel + 1) % count;
    } while (ch != '\n' && ch != 'q' && ch != 27);

    delwin(win);
    if (ch == 'q' || ch == 27)
        return -1;
    return sel;
}

int tetrisui_input(const char *title, const char *prompt, char *out,
                   int out_len)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int width = cols - 10 > 50 ? cols - 10 : 50;
    int height = 6;
    int starty = (rows - height) / 2;
    int startx = (cols - width) / 2;
    WINDOW *win = frame_win(title, height, width, starty, startx);
    if (!win)
        return -1;
    keypad(win, TRUE);
    mvwprintw(win, 2, 2, "%s", prompt);

    curs_set(1);
    char buf[TETRISUI_FIELD_LEN];
    memset(buf, 0, sizeof(buf));
    int max =
        out_len - 1 < (int)sizeof(buf) - 1 ? out_len - 1 : (int)sizeof(buf) - 1;
    int cancelled = 0;
    for (;;)
    {
        mvwprintw(win, 3, 2, "%-*s", width - 4, buf);
        tetrisui_draw_status_bar("Enter submit  Esc cancel");
        wmove(win, 3, 2 + (int)strlen(buf));
        wrefresh(win);
        int ch = wgetch(win);
        if (ch == 27)
        {
            cancelled = 1;
            break;
        }
        /* keypad(TRUE) can deliver KEY_ENTER, and a raw terminal may send '\r';
         * accepting only '\n' makes Enter appear dead on those. */
        if (ch == '\n' || ch == KEY_ENTER || ch == '\r')
            break;
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8)
        {
            int len = (int)strlen(buf);
            if (len > 0)
                buf[len - 1] = '\0';
            continue;
        }
        if (ch >= 32 && ch < 127 && (int)strlen(buf) < max)
        {
            int len = (int)strlen(buf);
            buf[len] = (char)ch;
            buf[len + 1] = '\0';
        }
    }
    curs_set(0);

    delwin(win);
    if (cancelled)
        return -1;
    strncpy(out, buf, out_len - 1);
    out[out_len - 1] = '\0';
    return 0;
}

int tetrisui_form(const char *title, const char *labels[],
                  char values[][TETRISUI_FIELD_LEN], int count)
{
    return tetrisui_form_ex(title, labels, values, count, 0, NULL, 0);
}

/* Draw values[i] as '*' repeated for its length when mask bit i is set. Same
 * width and padding as the unmasked print, so the field box never resizes
 * when the mask flips. */
static void draw_field_value(WINDOW *win, int y, int width, const char *value,
                             int masked)
{
    if (!masked)
    {
        mvwprintw(win, y, 2, "%-*s", width - 4, value);
        return;
    }
    char stars[TETRISUI_FIELD_LEN];
    int len = (int)strlen(value);
    if (len > (int)sizeof(stars) - 1)
        len = (int)sizeof(stars) - 1;
    memset(stars, '*', (size_t)len);
    stars[len] = '\0';
    mvwprintw(win, y, 2, "%-*s", width - 4, stars);
}

int tetrisui_form_ex(const char *title, const char *labels[],
                     char values[][TETRISUI_FIELD_LEN], int count,
                     unsigned mask, const char *error, int start_field)
{
    if (count <= 0 || count > TETRISUI_MAX_FIELDS)
        return -1; /* see tetrisui_menu */
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int width = cols - 6 > 60 ? cols - 6 : 60;
    /* One extra row for the error line, when there is one - blank otherwise
     * rather than shrinking the form when a re-open clears it. */
    int height = count * 2 + 4 + (error ? 1 : 0);
    int starty = (rows - height) / 2;
    int startx = (cols - width) / 2;
    WINDOW *win = frame_win(title, height, width, starty, startx);
    if (!win)
        return -1;
    keypad(win, TRUE);
    /* Not height - 1: that row IS the bottom border, and writing there erases
     * it. The same hint is on the status bar anyway. */

    int field = (start_field >= 0 && start_field < count) ? start_field : 0;
    curs_set(1);
    int cancelled = 0;
    for (;;)
    {
        for (int i = 0; i < count; i++)
        {
            int y = i * 2 + 2;
            int masked = (mask >> i) & 1u;
            if (i == field)
                wattron(win, A_BOLD | A_UNDERLINE);
            mvwprintw(win, y, 2, "%s:", labels[i]);
            if (i == field)
                wattroff(win, A_BOLD | A_UNDERLINE);
            if (i == field)
                wattron(win, A_REVERSE);
            draw_field_value(win, y + 1, width, values[i], masked);
            if (i == field)
                wattroff(win, A_REVERSE);
        }
        if (error)
        {
            wattron(win, A_BOLD);
            mvwprintw(win, count * 2 + 2, 2, "%.*s", width - 4, error);
            wattroff(win, A_BOLD);
        }
        tetrisui_draw_status_bar(
            "Tab/Down next field  Enter submit  Esc cancel");
        wmove(win, field * 2 + 3, 2 + (int)strlen(values[field]));
        wrefresh(win);
        refresh();
        int ch = wgetch(win);
        if (ch == 27)
        {
            cancelled = 1;
            break;
        }
        if (ch == '\n' || ch == KEY_ENTER || ch == '\r')
            break;
        if (ch == '\t' || ch == KEY_DOWN)
        {
            field = (field + 1) % count;
            continue;
        }
        if (ch == KEY_UP)
        {
            field = (field - 1 + count) % count;
            continue;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8)
        {
            int len = (int)strlen(values[field]);
            if (len > 0)
                values[field][len - 1] = '\0';
            continue;
        }
        if (ch >= 32 && ch < 127)
        {
            int len = (int)strlen(values[field]);
            if (len < TETRISUI_FIELD_LEN - 1)
            {
                values[field][len] = (char)ch;
                values[field][len + 1] = '\0';
            }
        }
    }
    curs_set(0);
    delwin(win);
    return cancelled ? -1 : 0;
}

int tetrisui_confirm(const char *title, const char *question)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int width =
        (int)strlen(question) + 10 > 50 ? (int)strlen(question) + 10 : 50;
    if (width > cols - 4)
        width = cols - 4;
    int height = 6;
    int starty = (rows - height) / 2;
    int startx = (cols - width) / 2;
    WINDOW *win = frame_win(title, height, width, starty, startx);
    if (!win)
        return 0;
    keypad(win, TRUE);
    mvwprintw(win, 2, 2, "%s", question);

    int sel = 0; /* 0 = yes, 1 = no */
    int ch;
    do
    {
        const char *yes = "[ Yes ]";
        const char *no = "[ No ]";
        wattron(win, sel == 0 ? A_REVERSE : A_NORMAL);
        mvwprintw(win, 4, 2, "%s", yes);
        wattroff(win, sel == 0 ? A_REVERSE : A_NORMAL);
        wattron(win, sel == 1 ? A_REVERSE : A_NORMAL);
        mvwprintw(win, 4, 12, "%s", no);
        wattroff(win, sel == 1 ? A_REVERSE : A_NORMAL);
        tetrisui_draw_status_bar("Left/Right choose  Enter confirm");
        wrefresh(win);
        refresh();
        ch = wgetch(win);
        if (ch == KEY_LEFT || ch == KEY_RIGHT)
            sel = 1 - sel;
        /* ESC and 'q' mean "no". Without this the loop only exits on Enter, so
         * a confirmation could not be backed out of - inconsistent with every
         * other widget here, all of which treat ESC as cancel. */
        if (ch == 27 || ch == 'q')
        {
            delwin(win);
            return 0;
        }
    } while (ch != '\n' && ch != KEY_ENTER && ch != '\r');

    delwin(win);
    return sel == 0 ? 1 : 0;
}

void tetrisui_message(const char *title, const char *lines[], int line_count)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int width = cols - 10 > 50 ? cols - 10 : 50;
    int height = line_count + 4;
    int starty = (rows - height) / 2;
    int startx = (cols - width) / 2;
    WINDOW *win = frame_win(title, height, width, starty, startx);
    if (!win)
        return;
    for (int i = 0; i < line_count; i++)
    {
        mvwprintw(win, i + 2, 2, "%.*s", width - 4, lines[i]);
    }
    tetrisui_draw_status_bar("Press any key to continue");
    wrefresh(win);
    refresh();
    wgetch(win);
    delwin(win);
}

void tetrisui_list_view(const char *title, const char *lines[], int line_count)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int height = rows - 6 > 6 ? rows - 6 : 6;
    int width = cols - 10 > 50 ? cols - 10 : 50;
    int starty = (rows - height) / 2;
    int startx = (cols - width) / 2;
    WINDOW *win = frame_win(title, height, width, starty, startx);
    if (!win)
        return;
    keypad(win, TRUE);

    int top = 0;
    int visible = height - 3;
    int ch;
    do
    {
        for (int row = 0; row < visible; row++)
        {
            mvwprintw(win, row + 2, 2, "%-*s", width - 4, "");
            int idx = top + row;
            if (idx < line_count)
                mvwprintw(win, row + 2, 2, "%.*s", width - 4, lines[idx]);
        }
        tetrisui_draw_status_bar("Up/Down scroll  q/Enter back");
        wrefresh(win);
        refresh();
        ch = wgetch(win);
        if (ch == KEY_DOWN && top + visible < line_count)
            top++;
        else if (ch == KEY_UP && top > 0)
            top--;
    } while (ch != 'q' && ch != 27 && ch != '\n');

    delwin(win);
}

void tetrisui_progress(const char *title, const char *steps[], int step_count)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int width = cols - 10 > 50 ? cols - 10 : 50;
    int height = step_count + 4;
    int starty = (rows - height) / 2;
    int startx = (cols - width) / 2;
    WINDOW *win = frame_win(title, height, width, starty, startx);
    if (!win)
        return;
    for (int i = 0; i < step_count; i++)
    {
        mvwprintw(win, i + 2, 2, "%s ...", steps[i]);
        wrefresh(win);
        napms(220);
        mvwprintw(win, i + 2, 2, "%s ... OK", steps[i]);
        wrefresh(win);
    }
    napms(300);
    delwin(win);
}

/* --- Real progress -------------------------------------------------------
 *
 * tetrisui_progress above is scripted: it prints OK for every step no matter
 * what happened. That is fine for a mock, and wrong for anything that can fail.
 * The tetrissh handshake can fail (SESSION_ERR_AUTH on a forged or expired
 * cert), so tetrisu drives this version instead and reports what actually
 * occurred.
 *
 * The panel is kept in statics rather than returned as a handle: only one
 * progress panel is ever on screen, and threading a handle through the connect
 * path bought nothing.
 */

static WINDOW *g_prog_win;
static const char **g_prog_steps;
static int g_prog_count;

void tetrisui_progress_begin(const char *title, const char *steps[],
                             int step_count)
{
    if (step_count <= 0)
        return;
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int width = cols - 10 > 50 ? cols - 10 : 50;
    int height = step_count + 4;
    int starty = (rows - height) / 2;
    int startx = (cols - width) / 2;

    g_prog_win = frame_win(title, height, width, starty, startx);
    if (!g_prog_win)
        return;
    g_prog_steps = steps;
    g_prog_count = step_count;

    for (int i = 0; i < step_count; i++)
    {
        mvwprintw(g_prog_win, i + 2, 2, "%-*.*s ...", width - 10, width - 10,
                  steps[i]);
    }
    tetrisui_draw_status_bar("working...");
    wrefresh(g_prog_win);
    refresh();
}

void tetrisui_progress_step(int index, int ok)
{
    if (!g_prog_win || index < 0 || index >= g_prog_count)
        return;
    int rows, cols;
    (void)rows;
    getmaxyx(g_prog_win, rows, cols);
    /* Repaint the whole line: "OK" is shorter than "FAILED", so overwriting in
     * place would leave the tail of a previous word behind. */
    mvwprintw(g_prog_win, index + 2, 2, "%-*.*s ... %s", cols - 14, cols - 14,
              g_prog_steps[index], ok ? "OK" : "FAILED");
    wrefresh(g_prog_win);
    refresh();
}

void tetrisui_progress_end(void)
{
    if (!g_prog_win)
        return;
    napms(200);
    delwin(g_prog_win);
    g_prog_win = NULL;
    g_prog_steps = NULL;
    g_prog_count = 0;
}
