/**
 * @file screen_auth.c
 * @brief The auth gate: mode choice, credential form, result (#50).
 *
 * screen_auth(c) offers LOGIN / REGISTER / GUEST before entering the lobby.
 * Cancelling the mode choice quits the program because the session is gated
 * until one of the three succeeds (#50).
 *
 * The reply wait is poll-shaped, never a blocking session_recv(), so the
 * screen stays responsive to a server that never answers. Disconnect is
 * learned ONLY from client_service()'s CLI_EV_DISCONNECT, never inferred from
 * a raw POLLHUP/POLLERR in revents - that shortcut (screen_wait_start.c)
 * would silently lose the bit #61's reconnect offer depends on.
 */

#include <ctype.h>
#include <errno.h>
#include <ncurses.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>

#include "libtetrisui/tetrisui.h"
#include "libtetrisutil/name.h"
#include "tetrisu/screens.h"

#define AUTH_POLL_MS                                                           \
    250 /* redraw/idle cadence of the "checking..." wait -                     \
         * see WAIT_POLL_MS in screens.c */

static void say(const char *title, const char *line)
{
    const char *lines[] = {line};
    tetrisui_message(title, lines, 1);
}

/* The rule is libtetrisutil/name.h's, shared with the server's credential
 * reader, so a name this accepts is a name that side accepts. It used to be
 * spelled here with isalnum(), which is locale-sensitive and could let through
 * a name the server then refused. */
static bool valid_username(const char *s)
{
    return user_name_ok(s, strlen(s));
}

/* 8-128 bytes, enforced at REGISTER only (#47, docs/libtetrisauth.md): a
 * LOGIN with a short password is somebody's existing account, and rejecting
 * it locally before the wire has ever gotten to hear the server's answer
 * would make raising the minimum later un-log-in-able for them. */
static bool valid_password(const char *s, bool registering)
{
    size_t len = strlen(s);
    if (len < 1)
        return false;
    if (registering && (len < 8 || len > 128))
        return false;
    return true;
}

/*
 * 401 and 404 (#47 overturned the earlier "one message for both" design):
 * distinguishing them in copy is the entire benefit that decision bought.
 * From the second counted failure onward the line also reports the count
 * this client has actually seen and names the knob rather than its value -
 * tetrisu holds no copy of auth_max_attempts (#57 section 3).
 */
static void warn_line(const Client *c, int status, char *buf, size_t buflen)
{
    const char *base = (status == 401) ? "Incorrect password."
                                       : "No account with that username.";
    if (c->auth_failures >= 2)
        snprintf(buf, buflen,
                 "%s Attempt failed (%d failed so far on this connection). "
                 "The server closes the connection after auth_max_attempts "
                 "failed attempts on one connection.",
                 base, c->auth_failures);
    else
        snprintf(buf, buflen, "%s", base);
}

/*
 * Poll for the reply to whatever LOGIN/REGISTER/GUEST just set auth_pending,
 * showing a real progress panel (the round trip can take ~2s worst case -
 * #44's database deadline, #50's "checking..." finding). Returns
 * CLI_EV_AUTH_OK, CLI_EV_REJECT or CLI_EV_DISCONNECT - the only three things
 * client_service() can still be returning once fold_auth_reply() has run.
 */
static ClientEvent wait_auth_reply(Client *c)
{
    const char *steps[] = {"Waiting for the account service"};
    tetrisui_progress_begin("Checking", steps, 1);

    for (;;)
    {
        struct pollfd pfd;
        pfd.fd = client_fd(c);
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, AUTH_POLL_MS);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            tetrisui_progress_step(0, 0);
            tetrisui_progress_end();
            return CLI_EV_DISCONNECT;
        }

        /* Any event on the fd - readable, hung up or errored - is a reason
         * to ask client_service() what happened; we never read revents
         * beyond that (see the file comment). */
        if (pfd.revents & (POLLIN | POLLHUP | POLLERR))
        {
            ClientEvent ev = client_service(c);
            if (ev == CLI_EV_AUTH_OK || ev == CLI_EV_REJECT ||
                ev == CLI_EV_DISCONNECT)
            {
                tetrisui_progress_step(0, ev != CLI_EV_DISCONNECT);
                tetrisui_progress_end();
                return ev;
            }
            /* CLI_EV_NONE and the room events are not possible pre-JOIN,
             * but nothing here assumes that - just keep waiting. */
        }
    }
}

static ClientEvent send_guest_and_wait(Client *c)
{
    if (client_guest(c) != 0)
        return CLI_EV_DISCONNECT;
    return wait_auth_reply(c);
}

/* GUEST from the mode-choice screen (not the automatic 500 downgrade, which
 * has its own call site below - the messaging differs). */
static ScreenResult guest_flow(Client *c)
{
    ClientEvent ev = send_guest_and_wait(c);
    if (ev == CLI_EV_AUTH_OK)
    {
        tetrisui_set_status(NULL, "(guest)", NULL);
        return SCR_AUTH_OK;
    }
    if (ev == CLI_EV_DISCONNECT)
        return SCR_DISCONNECTED;

    /* A guest opens no database connection at all (#52 Invariant A), so a
     * refusal here is unreachable from the mode-choice screen by construction.
     * Draw it rather than asserting, so a client/server desync cannot crash an
     * ncurses program. */
    say("Could not continue as guest",
        "Unexpected server state. Try again, or quit and reconnect.");
    return SCR_BACK;
}

/*
 * LOGIN or REGISTER, looping on a refusal until it succeeds, disconnects, or
 * the user cancels the form. registering picks the method.
 */
static ScreenResult credential_flow(Client *c, bool registering)
{
    char values[2][TETRISUI_FIELD_LEN];
    memset(values, 0, sizeof values);
    const char *labels[2] = {"Username", "Password"};
    char err_buf[224];
    const char *error = NULL;
    int start_field = 0;
    unsigned mask = 1u << 1; /* password only (#50) */

    for (;;)
    {
        const char *title =
            registering ? "tetrisu - register" : "tetrisu - log in";
        if (tetrisui_form_ex(title, labels, values, 2, mask, error,
                             start_field) != 0)
            return SCR_BACK; /* cancelled: caller re-shows the mode choice */

        error = NULL;

        if (!valid_username(values[0]))
        {
            snprintf(
                err_buf, sizeof err_buf,
                "Username: 1-15 characters, letters, digits, _ or - only.");
            error = err_buf;
            start_field = 0;
            continue;
        }
        if (!valid_password(values[1], registering))
        {
            snprintf(err_buf, sizeof err_buf,
                     registering ? "Password: 8-128 characters."
                                 : "Enter your password.");
            error = err_buf;
            start_field = 1;
            continue;
        }

        int rc = registering ? client_register(c, values[0], values[1])
                             : client_login(c, values[0], values[1]);
        /* The password never survives a round trip either way - only the
         * username persists across a failed attempt (#50). */
        memset(values[1], 0, sizeof values[1]);
        if (rc != 0)
        {
            snprintf(err_buf, sizeof err_buf, "Could not reach the server.");
            error = err_buf;
            continue;
        }

        ClientEvent ev = wait_auth_reply(c);
        if (ev == CLI_EV_DISCONNECT)
            return SCR_DISCONNECTED;
        if (ev == CLI_EV_AUTH_OK)
        {
            tetrisui_set_status(NULL, values[0], NULL);
            return SCR_AUTH_OK;
        }

        int status = c->last_reject;
        switch (status)
        {
        case 500:
            say("Account service unreachable",
                "Could not reach the account service - continuing as a guest.");
            {
                ClientEvent gev = send_guest_and_wait(c);
                if (gev == CLI_EV_DISCONNECT)
                    return SCR_DISCONNECTED;
                if (gev == CLI_EV_AUTH_OK)
                {
                    tetrisui_set_status(NULL, "(guest)", NULL);
                    return SCR_AUTH_OK;
                }
            }
            /* GUEST refused too (unreachable by construction - #52
             * Invariant A - but not asserted on, per #57 section 2): give up
             * on this attempt rather than loop forever. */
            return SCR_BACK;

        case 400:
            /* This client validated the allowlist above, so a 400 here is a
             * bug in this client, not a mistake the user can fix - nothing
             * more specific to tell them (libtetrisutil/authbudget.h). */
            snprintf(err_buf, sizeof err_buf,
                     "The server rejected that request. Try again.");
            error = err_buf;
            start_field = 0;
            continue;

        case 401:
            warn_line(c, status, err_buf, sizeof err_buf);
            error = err_buf;
            start_field = 1; /* the password is what to retype */
            continue;

        case 404:
            warn_line(c, status, err_buf, sizeof err_buf);
            error = err_buf;
            start_field = 0; /* the username is what to retype */
            continue;

        case 409:
            if (registering)
            {
                snprintf(err_buf, sizeof err_buf,
                         "That username is taken. Pick another.");
                error = err_buf;
                start_field = 0;
                continue;
            }
            /* LOGIN, first auth: auth_retry_handler() runs before any state
             * exists to conflict with, so this status is unreachable except as
             * a client/server desync (#57 section 2). */
            snprintf(err_buf, sizeof err_buf,
                     "Unexpected server state. Try again, or quit and "
                     "reconnect.");
            error = err_buf;
            continue;

        default:
            snprintf(err_buf, sizeof err_buf,
                     "The server refused that request (%d).", status);
            error = err_buf;
            continue;
        }
    }
}

ScreenResult screen_auth(Client *c)
{
    for (;;)
    {
        const char *items[] = {"Log in", "Register", "Play as guest"};
        int sel = tetrisui_menu("tetrisu - welcome", items, 3,
                                "Up/Down move  Enter select  q quit");
        if (sel < 0)
            return SCR_QUIT;

        ScreenResult r =
            (sel == 2) ? guest_flow(c) : credential_flow(c, sel == 1);

        if (r == SCR_BACK)
            continue; /* mode choice again */
        return r;     /* SCR_AUTH_OK or SCR_DISCONNECTED */
    }
}
