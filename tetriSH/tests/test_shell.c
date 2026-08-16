/**
 * @file test_shell.c
 * @brief Fuzzes the tetrish REPL with input a hostile user could type.
 *
 * Drives the real ./tetrish binary over a pipe, the way a person at the prompt
 * or a script piping commands in would. Every case asserts the shell survives:
 * no signal, no non-zero exit, no line smuggled in behind a refused one.
 *
 * Run from the repo root: make bin/test_shell && ./bin/test_shell
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include "test_output.h"
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../core/src/tetrish/shell.h"

#define SHELL_BIN "./tetrish"
#define OUT_CAP 65536

static int tests_run;
static int tests_failed;

#define CHECK(cond, msg)                                                       \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            test_output_failure_detail(msg, __FILE__, __LINE__);               \
            return -1;                                                         \
        }                                                                      \
    } while (0)

static void run(const char *name, int (*fn)(void))
{
    tests_run++;
    if (fn() != 0)
    {
        tests_failed++;
        test_output_fail(name);
    }
    else
        test_output_pass(name);
}

/**
 * Feeds input to a fresh shell and collects its merged stdout and stderr.
 *
 * Called by every case, most of them through feed_shell(). The child runs in a
 * scratch directory, so it sees only the .tetrishrc this function plants
 * there. `rc` NULL means no file at all, which is what most cases want: the
 * shell starts no daemons and the REPL is exercised alone.
 *
 * @param rc     Contents of the scratch .tetrishrc, or NULL for no file.
 * @param input  Written to the shell's stdin, which is then closed.
 * @param out    Filled with the shell's output, NUL-terminated.
 * @param cap    Size of out; output beyond it is discarded.
 * @returns The raw wait() status, or -1 if the harness itself failed.
 */
static int feed_shell_rc(const char *rc, const char *input, char *out,
                         size_t cap)
{
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL)
        return -1;

    char scratch[] = "/tmp/tetrish-shell-XXXXXX";
    if (mkdtemp(scratch) == NULL)
        return -1;

    /* Written before the fork, so the child cannot reach fopen() first. */
    char rc_path[PATH_MAX];
    snprintf(rc_path, sizeof(rc_path), "%s/.tetrishrc", scratch);
    if (rc != NULL)
    {
        FILE *f = fopen(rc_path, "w");
        if (f == NULL)
        {
            rmdir(scratch);
            return -1;
        }
        fputs(rc, f);
        fclose(f);
    }

    int to_child[2], from_child[2];
    if (pipe(to_child) != 0 || pipe(from_child) != 0)
    {
        if (rc != NULL)
            unlink(rc_path);
        rmdir(scratch);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0)
    {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        dup2(from_child[1], STDERR_FILENO);
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);

        char bin[PATH_MAX];
        snprintf(bin, sizeof(bin), "%s/%s", cwd, SHELL_BIN);
        if (chdir(scratch) != 0)
            _exit(127);
        execl(bin, bin, (char *)NULL);
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);

    /* SIGPIPE would kill the harness if the shell exits early on its own. */
    signal(SIGPIPE, SIG_IGN);
    size_t len = strlen(input), written = 0;
    while (written < len)
    {
        ssize_t n = write(to_child[1], input + written, len - written);
        if (n <= 0)
            break; /* the shell closed stdin, which the case itself will judge
                    */
        written += (size_t)n;
    }
    close(to_child[1]);

    size_t total = 0;
    for (;;)
    {
        ssize_t n = read(from_child[0], out + total, cap - 1 - total);
        if (n <= 0 || total >= cap - 1)
            break;
        total += (size_t)n;
    }
    out[total] = '\0';
    close(from_child[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        status = -1;
    if (rc != NULL)
        unlink(rc_path);
    rmdir(scratch);
    return status;
}

/** Runs a shell with no .tetrishrc at all. Called by every case that does not
    exercise the rc reader. */
static int feed_shell(const char *input, char *out, size_t cap)
{
    return feed_shell_rc(NULL, input, out, cap);
}

/** Builds `fill` repeated `count` times followed by `tail`. Called by the cases
    that need a line longer than the shell's buffer. Caller frees. */
static char *build_line(char fill, size_t count, const char *tail)
{
    size_t tail_len = strlen(tail);
    char *buf = malloc(count + tail_len + 1);
    if (buf == NULL)
        return NULL;
    memset(buf, fill, count);
    memcpy(buf + count, tail, tail_len + 1);
    return buf;
}

/** Counts non-overlapping occurrences of needle. Called to check that one line
    produced at most one command attempt. */
static int count_occurrences(const char *haystack, const char *needle)
{
    int n = 0;
    for (const char *p = strstr(haystack, needle); p != NULL;
         p = strstr(p + 1, needle))
        n++;
    return n;
}

/** Asserts the shell exited normally with status 0, naming `what` on failure.
    Called by every case on the status from feed_shell(). */
static int survived(int status, const char *what)
{
    if (status == -1)
    {
        test_output_failure_detailf(__FILE__, __LINE__,
                                    "harness could not run the shell (%s)",
                                    what);
        return -1;
    }
    if (WIFSIGNALED(status))
    {
        test_output_failure_detailf(__FILE__, __LINE__,
                                    "%s killed the shell with signal %d", what,
                                    WTERMSIG(status));
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        test_output_failure_detailf(
            __FILE__, __LINE__, "%s made the shell exit with status %d", what,
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return -1;
    }
    return 0;
}

static int test_blank_input(void)
{
    static const struct
    {
        const char *name;
        const char *input;
    } cases[] = {
        {"empty line", "\nexit\n"},
        {"many empty lines", "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\nexit\n"},
        {"only spaces", "        \nexit\n"},
        {"only tabs", "\t\t\t\nexit\n"},
        {"mixed whitespace", " \t \r \t \nexit\n"},
    };

    char out[OUT_CAP];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        int status = feed_shell(cases[i].input, out, sizeof(out));
        if (survived(status, cases[i].name) != 0)
            return -1;
        CHECK(strstr(out, "not found") == NULL,
              "whitespace-only line was executed as a command");
    }
    return 0;
}

/** Lines at and beyond the length limit. The reader used to write the NUL one
    past its line buffer when a line filled it exactly, and to exit(1) on
    anything longer. */
static int test_long_lines(void)
{
    static const size_t lengths[] = {MAX_LINE - 2, MAX_LINE - 1, MAX_LINE,
                                     MAX_LINE + 1, 4096,         64 * 1024};

    char out[OUT_CAP];
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
    {
        char *input = build_line('a', lengths[i], "\nexit\n");
        CHECK(input != NULL, "out of memory building the long line");

        int status = feed_shell(input, out, sizeof(out));
        free(input);

        char what[64];
        snprintf(what, sizeof(what), "a %zu-character line", lengths[i]);
        if (survived(status, what) != 0)
            return -1;

        /* One line is one command attempt. A line that fits is run once and
         * fails to exec once; a line that does not fit is refused outright.
         * Either way its tail must never come back as a second attempt - that
         * is how a rejected line turns into a real command. */
        CHECK(count_occurrences(out, "not found") <= 1,
              "the tail of an over-long line was executed as another command");
    }
    return 0;
}

/** Argument counts at and beyond the limit. The reader used to strdup() every
    token into a fixed MAX_ARGS array without bounding the count, walking off
    the end of both its own frame and main's. */
static int test_many_arguments(void)
{
    static const size_t counts[] = {MAX_ARGS - 2, MAX_ARGS - 1, MAX_ARGS,
                                    MAX_ARGS + 1, 200,          500};

    char out[OUT_CAP];
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
    {
        /* "x " per argument keeps every line inside the length limit. */
        size_t n = counts[i];
        char *input = malloc(n * 2 + 16);
        CHECK(input != NULL, "out of memory building the argument list");
        char *p = input;
        for (size_t j = 0; j < n; j++)
        {
            *p++ = 'x';
            *p++ = ' ';
        }
        memcpy(p, "\nexit\n", 7);

        int status = feed_shell(input, out, sizeof(out));
        free(input);

        char what[64];
        snprintf(what, sizeof(what), "%zu arguments", n);
        if (survived(status, what) != 0)
            return -1;
    }
    return 0;
}

static int test_hostile_tokens(void)
{
    static const struct
    {
        const char *name;
        const char *input;
    } cases[] = {
        {"unmatched double quote", "echo \"hello\nexit\n"},
        {"unmatched single quote", "echo 'hello\nexit\n"},
        {"quote only", "\"\nexit\n"},
        {"ampersand alone", "&\nexit\n"},
        {"ampersand run", "& & &\nexit\n"},
        {"missing binary", "definitely_not_a_binary_xyz\nexit\n"},
        {"missing binary backgrounded",
         "definitely_not_a_binary_xyz &\nexit\n"},
        {"redirection metacharacters", "> | ; < >>\nexit\n"},
        {"high-byte junk", "\x01\x02\x7f\xff\nexit\n"},
        {"builtins without arguments", "setenv\nunsetenv\nusage\ncd\nexit\n"},
        {"setenv without a value", "setenv =\nsetenv KEY\nexit\n"},
        {"cd to a missing directory", "cd /nonexistent/path/xyz\nexit\n"},
    };

    char out[OUT_CAP];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        int status = feed_shell(cases[i].input, out, sizeof(out));
        if (survived(status, cases[i].name) != 0)
            return -1;
    }
    return 0;
}

/* Ctrl-D, or a script whose last line has no trailing newline. The reader used
 * to mistake EOF for a character and fill its buffer with (char)-1 until it
 * declared the line too long and exited non-zero. */
static int test_end_of_input(void)
{
    static const struct
    {
        const char *name;
        const char *input;
    } cases[] = {
        {"immediate EOF", ""},
        {"EOF after a command", "echo hi\n"},
        {"EOF mid-line", "echo hi"},
        {"EOF after whitespace", "   "},
        {"EOF after a blank line", "\n"},
    };

    char out[OUT_CAP];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        int status = feed_shell(cases[i].input, out, sizeof(out));
        if (survived(status, cases[i].name) != 0)
            return -1;
    }

    /* A last line without its newline is still a command. */
    int status = feed_shell("echo tail-command", out, sizeof(out));
    if (survived(status, "EOF mid-line") != 0)
        return -1;
    CHECK(strstr(out, "tail-command") != NULL,
          "the final unterminated line was dropped instead of run");
    return 0;
}

/* A refused line must not leave its remainder in the input stream, and the
 * shell must keep taking commands afterwards. */
static int test_recovery_after_refusal(void)
{
    char out[OUT_CAP];

    char *input = build_line('a', 4096, "\necho still-alive\nexit\n");
    CHECK(input != NULL, "out of memory building the long line");
    int status = feed_shell(input, out, sizeof(out));
    free(input);
    if (survived(status, "a long line followed by a command") != 0)
        return -1;
    CHECK(strstr(out, "still-alive") != NULL,
          "the shell stopped accepting commands after an over-long line");

    status = feed_shell("cd /nonexistent/xyz\necho still-alive\nexit\n", out,
                        sizeof(out));
    if (survived(status, "a failed builtin followed by a command") != 0)
        return -1;
    CHECK(strstr(out, "still-alive") != NULL,
          "the shell stopped accepting commands after a failed cd");
    return 0;
}

/* .tetrishrc is shared: most of its lines are directives belonging to other
 * readers, which each ask rc_get() for their own keys by name. The shell's
 * only job is to leave them alone - and to keep telling a directive apart from
 * a command that happens to contain '='. */
static int test_rc_directives(void)
{
    char out[OUT_CAP];

    /* Every directive in the file, from every namespace, and one key no reader
     * asks for. None is the shell's business, and none may be run as a command:
     * "Command log_path not found" once per line is what this prevents. */
    int status = feed_shell_rc("log_level = debug\n"
                               "db_timeout = 2000\n"
                               "auth_max_attempts = 5\n"
                               "ctl_ipc = var/run/tetrisd.ctl\n"
                               "listen_port = 5555\n"
                               "cert_path = auth/server_signed.crt\n"
                               "key_path = auth/private_key.pem\n"
                               "ca_path = auth/cacsertificate.crt\n"
                               "nonsense_key = 1\n"
                               "# a comment\n",
                               "exit\n", out, sizeof(out));
    if (survived(status, "directives in .tetrishrc") != 0)
        return -1;
    CHECK(strstr(out, "not found") == NULL,
          "a directive was executed as a command");

    /* A command containing '=' is still a command. This is the line the
     * directive check must not swallow: it is how the rc sets a variable. */
    status = feed_shell_rc("setenv RC_MARKER=present\n", "env\nexit\n", out,
                           sizeof(out));
    if (survived(status, "setenv from the rc file") != 0)
        return -1;
    CHECK(strstr(out, "RC_MARKER=present") != NULL,
          "setenv in .tetrishrc was mistaken for a directive");
    return 0;
}

int main(void)
{
    test_output_begin("test_shell");
    run("blank and whitespace-only lines", test_blank_input);
    run("lines at and beyond the length limit", test_long_lines);
    run("argument counts at and beyond the limit", test_many_arguments);
    run("hostile tokens and missing binaries", test_hostile_tokens);
    run("end of input", test_end_of_input);
    run("recovery after a refused line", test_recovery_after_refusal);
    run("directives in .tetrishrc", test_rc_directives);
    test_output_summary(tests_run, tests_failed, 0);
    return tests_failed == 0 ? 0 : 1;
}
