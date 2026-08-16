/*
 * test_ctl_client.c - the JSON the daemon sends, decoded into structs.
 *
 * Headless: no socket, no daemon, no terminal, so unlike the ncurses harnesses
 * this one runs under `make test`. What it pins down is the hand-rolled reader
 * in ctl_client.c, which is substring-based rather than a real parser and is
 * therefore only correct while its assumptions hold.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include <stdio.h>
#include "test_output.h"
#include <string.h>

#include "tetrisctl/ctl_client.h"

static int failures = 0;
static int tests_run = 0;

static void check(const char *what, int ok)
{
    test_output_check(&tests_run, &failures, what, ok);
}

static void test_status(void)
{
    CtlStatus s;
    check("status decodes",
          ctl_decode_status("{\"uptime\":252,\"sessions\":3,\"rooms\":1}",
                            &s) == 0);
    check("status uptime", s.uptime == 252);
    check("status sessions", s.sessions == 3);
    check("status rooms", s.rooms == 1);

    /* An array where an object belongs is a body the verb did not promise. */
    check("status rejects a list", ctl_decode_status("[]", &s) == -1);
}

static void test_rooms(void)
{
    CtlRoom r[8];

    int n = ctl_decode_rooms(
        "[{\"id\":1,\"phase\":\"PLAYING\",\"members\":3,\"owner\":2},"
        "{\"id\":7,\"phase\":\"WAITING\",\"members\":1,\"owner\":-1}]",
        r, 8);
    check("rooms count", n == 2);
    check("rooms id", r[0].id == 1 && r[1].id == 7);
    check("rooms phase", strcmp(r[0].phase, "PLAYING") == 0);
    check("rooms members", r[0].members == 3);
    /* owner < 0 is what the "-" column rendering keys off. */
    check("rooms ownerless", r[1].owner == -1);

    /* Empty must be 0, not 1: json_each finds '[' then walks braces, and an
     * off-by-one here would invent a room out of "[]". */
    check("rooms empty", ctl_decode_rooms("[]", r, 8) == 0);
}

static void test_players(void)
{
    CtlPlayer p[8];

    int n = ctl_decode_players(
        "[{\"room\":1,\"player\":2,\"pid\":8814,\"owner\":true,"
        "\"score\":1200,\"lines\":7,\"name\":\"Player 2\"},"
        "{\"room\":-1,\"player\":3,\"pid\":8815,\"owner\":false,"
        "\"score\":0,\"lines\":0,\"name\":\"\"}]",
        p, 8);
    check("players count", n == 2);
    check("players pid", p[0].pid == 8814);
    check("players owner true", p[0].is_owner);
    check("players owner false", !p[1].is_owner);
    check("players score", p[0].score == 1200);
    check("players lines", p[0].lines == 7);
    check("players name", strcmp(p[0].name, "Player 2") == 0);
    check("players lobby room", p[1].room == -1);
    check("players empty name", p[1].name[0] == '\0');
    check("players empty", ctl_decode_players("[]", p, 8) == 0);
}

/*
 * A name that would forge a field if it reached the reader unescaped.
 *
 * The daemon runs json_escape (control_plane.c) before embedding, so the quote
 * arrives as \" and the literal "score": never appears - find_key cannot be
 * fooled into reading the name's digits as the score. That invariant lives in
 * a different file from the code relying on it; this is the test that ties the
 * two together, and it fails if json_escape is ever removed.
 */
static void test_escaped_name_cannot_forge_a_field(void)
{
    CtlPlayer p[2];
    int n = ctl_decode_players(
        "[{\"room\":1,\"player\":2,\"pid\":8814,\"owner\":false,"
        "\"score\":10,\"lines\":0,"
        "\"name\":\"x\\\",\\\"score\\\":9999\"}]",
        p, 2);
    check("escaped name count", n == 1);
    check("escaped name does not forge score", p[0].score == 10);
}

/* A missing key defaults rather than fails, so a field the daemon has not
 * sent yet does not take the whole row down. */
static void test_missing_keys_default(void)
{
    CtlPlayer p[2];
    int n = ctl_decode_players("[{\"player\":4}]", p, 2);
    check("sparse row decodes", n == 1);
    check("sparse row keeps what is there", p[0].player == 4);
    check("sparse room defaults to lobby", p[0].room == -1);
    check("sparse score defaults to zero", p[0].score == 0);
    check("sparse owner defaults false", !p[0].is_owner);
}

/* A realistic worst-case element must survive json_each's 1024-byte item cap:
 * MAX_PLAYER_NAME is 16, six bytes each escaped, so ~187 bytes. */
static void test_worst_case_element_is_not_truncated(void)
{
    char body[512];
    char name[128] = "";
    for (int i = 0; i < 15; i++)
        strcat(name, "\\u0041");

    snprintf(body, sizeof body,
             "[{\"room\":254,\"player\":254,\"pid\":99999,\"owner\":true,"
             "\"score\":999999,\"lines\":9999,\"name\":\"%s\"}]",
             name);

    CtlPlayer p[2];
    int n = ctl_decode_players(body, p, 2);
    check("worst-case element count", n == 1);
    /* name is the LAST field, so it is what a truncated item loses first. */
    check("worst-case name survives", p[0].name[0] != '\0');
    check("worst-case score intact", p[0].score == 999999);
}

/* A full table must neither overrun the caller's array nor stop early. */
static void test_full_table(void)
{
    static char body[CTL_BODY_MAX];
    static CtlPlayer p[MAX_SESSIONS];
    int off = 0;

    off += snprintf(body + off, sizeof body - (size_t)off, "[");
    for (int i = 0; i < MAX_SESSIONS; i++)
        off += snprintf(body + off, sizeof body - (size_t)off,
                        "%s{\"room\":1,\"player\":%d,\"pid\":%d,"
                        "\"owner\":false,\"score\":0,\"lines\":0,"
                        "\"name\":\"p%d\"}",
                        i ? "," : "", i, 9000 + i, i);
    snprintf(body + off, sizeof body - (size_t)off, "]");

    int n = ctl_decode_players(body, p, MAX_SESSIONS);
    check("full table count", n == MAX_SESSIONS);
    check("full table first", p[0].player == 0);
    check("full table last", p[MAX_SESSIONS - 1].player == MAX_SESSIONS - 1);

    /* A cap smaller than the body must clamp, not write past the end. */
    CtlPlayer small[4];
    check("cap clamps", ctl_decode_players(body, small, 4) == 4);
}

int main(void)
{
    test_output_begin("test_ctl_client");
    test_status();
    test_rooms();
    test_players();
    test_escaped_name_cannot_forge_a_field();
    test_missing_keys_default();
    test_worst_case_element_is_not_truncated();
    test_full_table();

    test_output_summary(tests_run, failures, 0);
    return failures == 0 ? 0 : 1;
}
