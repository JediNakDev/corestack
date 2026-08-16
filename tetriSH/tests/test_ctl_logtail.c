/*
 * test_ctl_logtail.c - the file-reading half of the console's log pane.
 *
 * Headless: no socket, no daemon, no terminal, so unlike the ncurses
 * harnesses this one runs under `make test`, exactly like
 * tests/test_ctl_client.c (this file's prior art, per its own comment).
 * Every case owns one scratch file under TEST_DIR and drives it with plain
 * write()/rename() calls - logtail.c's whole job is turning that into a
 * bounded, ordered ring of lines.
 */

#include <fcntl.h>
#include <stdio.h>
#include "test_output.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tetrisctl/ctl_logtail.h"

#define TEST_DIR "var/logtail_test"

static int tests_run = 0, failures = 0;

static void check(const char *what, int ok)
{
    test_output_check(&tests_run, &failures, what, ok);
}

static void write_all(const char *path, const char *data)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    ssize_t _long = write(fd, data, strlen(data));
    (void)_long;
    close(fd);
}

static void append_all(const char *path, const char *data)
{
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0)
        return;
    ssize_t _long = write(fd, data, strlen(data));
    (void)_long;
    close(fd);
}

static void test_appended_lines_in_order(void)
{
    const char *path = TEST_DIR "/append.log";
    unlink(path);
    write_all(path, "line one\nline two\n");

    logtail_t *t = logtail_init_at(path);
    check("open succeeds", t != NULL);
    logtail_poll(t);

    const char *lines[16];
    int n = logtail_lines(t, lines, 16);
    check("two complete lines are read", n == 2);
    check("first line is first", n >= 1 && strcmp(lines[0], "line one") == 0);
    check("second line is second", n >= 2 && strcmp(lines[1], "line two") == 0);

    logtail_close(t);
}

/* Proves the poll is incremental, not a full re-read: re-reading the whole
 * file from byte 0 on every poll would push "first" into the ring twice,
 * making this three lines rather than two. */
static void test_second_poll_only_new_bytes(void)
{
    const char *path = TEST_DIR "/incremental.log";
    unlink(path);
    write_all(path, "first\n");

    logtail_t *t = logtail_init_at(path);
    logtail_poll(t);
    const char *lines[16];
    check("first poll sees one line", logtail_lines(t, lines, 16) == 1);

    append_all(path, "second\n");
    logtail_poll(t);
    int n = logtail_lines(t, lines, 16);
    check("second poll sees two lines total, not a re-read of the first",
          n == 2);
    check("the appended line is exactly the new one",
          n >= 2 && strcmp(lines[1], "second") == 0);

    logtail_close(t);
}

static void test_ring_evicts_oldest(void)
{
    const char *path = TEST_DIR "/ring.log";
    unlink(path);

    int total = LOGTAIL_RING_LINES + 5;
    char *buf = malloc((size_t)total * 8 + 64);
    size_t off = 0;
    for (int i = 0; i < total; i++)
        off += (size_t)snprintf(buf + off, (size_t)total * 8 + 64 - off,
                                "l%d\n", i);
    write_all(path, buf);
    free(buf);

    logtail_t *t = logtail_init_at(path);
    logtail_poll(t);

    const char *lines[LOGTAIL_RING_LINES + 10];
    int n = logtail_lines(t, lines, LOGTAIL_RING_LINES + 10);
    check("the ring caps at LOGTAIL_RING_LINES", n == LOGTAIL_RING_LINES);
    check("the oldest lines were evicted, not the newest",
          strcmp(lines[0], "l5") == 0);

    char want_last[16];
    snprintf(want_last, sizeof want_last, "l%d", total - 1);
    check("the newest line survives",
          n > 0 && strcmp(lines[n - 1], want_last) == 0);

    logtail_close(t);
}

/* Truncation: same path, same inode, smaller size - the heuristic
 * logtail.c documents as covering both this and rotation. The ring keeps
 * what it already showed (a live tail does not erase scrollback); only the
 * read position resets to the file's new start. */
static void test_truncated_file_read_from_start(void)
{
    const char *path = TEST_DIR "/trunc.log";
    unlink(path);
    write_all(path, "aaaa\nbbbb\ncccc\n");

    logtail_t *t = logtail_init_at(path);
    logtail_poll(t);
    const char *lines[16];
    check("before truncation: three lines seen",
          logtail_lines(t, lines, 16) == 3);

    write_all(path, "zz\n"); /* O_TRUNC: same file, now much shorter */
    logtail_poll(t);
    int n = logtail_lines(t, lines, 16);
    check("after truncation: the new content is read from the start, not "
          "lost or skipped",
          n == 4 && strcmp(lines[3], "zz") == 0);

    logtail_close(t);
}

/* Rotation: the path is replaced by a different inode. Only reopening by
 * path (not seeking within the already-open fd) can follow it. */
static void test_rotated_file_is_reopened(void)
{
    const char *path = TEST_DIR "/rotate.log";
    const char *rotated = TEST_DIR "/rotate.log.1";
    unlink(path);
    unlink(rotated);
    write_all(path, "old one\nold two\nold three\nold four\n");

    logtail_t *t = logtail_init_at(path);
    logtail_poll(t);
    const char *lines[16];
    check("before rotation: four lines seen", logtail_lines(t, lines, 16) == 4);

    check("fixture: rotate the file aside", rename(path, rotated) == 0);
    write_all(path, "new one\n");
    logtail_poll(t);
    int n = logtail_lines(t, lines, 16);
    check("after rotation: the new file's line is picked up",
          n == 5 && strcmp(lines[4], "new one") == 0);

    logtail_close(t);
    unlink(rotated);
}

static void test_partial_line_waits_for_its_newline(void)
{
    const char *path = TEST_DIR "/partial.log";
    unlink(path);
    write_all(path, "whole\nunfin"); /* no trailing newline */

    logtail_t *t = logtail_init_at(path);
    logtail_poll(t);
    const char *lines[16];
    int n = logtail_lines(t, lines, 16);
    check("only the complete line is emitted",
          n == 1 && strcmp(lines[0], "whole") == 0);

    append_all(path, "ished\n"); /* completes "unfinished" across two writes */
    logtail_poll(t);
    n = logtail_lines(t, lines, 16);
    check("the completed line arrives whole, not split across two polls",
          n == 2 && strcmp(lines[1], "unfinished") == 0);

    logtail_close(t);
}

static void test_missing_file_reports_and_recovers(void)
{
    const char *path = TEST_DIR "/missing.log";
    unlink(path);

    logtail_t *t = logtail_init_at(path);
    check("missing is reported once polled",
          (logtail_poll(t), is_logtail_missing(t)));

    write_all(path, "arrived\n");
    logtail_poll(t);
    check("no longer missing once the file exists", !is_logtail_missing(t));

    const char *lines[16];
    int n = logtail_lines(t, lines, 16);
    check("the first line is picked up once the file appears",
          n == 1 && strcmp(lines[0], "arrived") == 0);

    logtail_close(t);
}

int main(void)
{
    test_output_begin("test_logtail");

    (void)mkdir("var", 0700);
    (void)mkdir(TEST_DIR, 0700);

    test_appended_lines_in_order();
    test_second_poll_only_new_bytes();
    test_ring_evicts_oldest();
    test_truncated_file_read_from_start();
    test_rotated_file_is_reopened();
    test_partial_line_waits_for_its_newline();
    test_missing_file_reports_and_recovers();

    test_output_summary(tests_run, failures, 0);
    return failures == 0 ? 0 : 1;
}
