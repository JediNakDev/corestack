#ifndef SHELL_H
#define SHELL_H

#include <limits.h> // For PATH_MAX
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_LINE 1024
#define MAX_ARGS 64
#define BIN_PATH "./bin/"

/** Token separators; shared by the REPL reader and the .tetrishrc reader so a
    tab-indented line parses the same at the prompt as it does at startup. */
#define SHELL_DELIMS " \t\r\n"

/** What the shell should do next with the line it just read or ran. */
typedef enum
{
    SHELL_EOF = -2,         /**< stdin is exhausted; stop reading. */
    SHELL_INVALID_CMD = -1, /**< Not a builtin; hand the line to execvp(). */
    SHELL_EXIT = 0,         /**< The `exit` builtin ran; leave the REPL. */
    SHELL_SUCCESS = 1       /**< Handled; carry on with the next line. */
} shell_status_t;

void type_prompt();

/**
 * Reads one line from stdin and splits it into command tokens.
 *
 * Called once per iteration of the REPL. A line the shell refuses - too long,
 * too many arguments - is reported on stderr and discarded, which reads back
 * as a blank line rather than as an error.
 *
 * @param cmd  Filled with NULL-terminated tokens; needs room for MAX_ARGS
 *             pointers. Caller owns each token and must free() it.
 * @returns SHELL_SUCCESS, or SHELL_EOF when stdin is exhausted (Ctrl-D or a
 *          closed pipe), in which case cmd[0] is NULL.
 */
shell_status_t read_command(char **cmd);

extern const char *builtin_commands[];
extern shell_status_t (*builtin_command_func[])(char **);

shell_status_t shell_cd(char **args);
shell_status_t shell_help(char **args);
shell_status_t shell_exit(char **args);
shell_status_t shell_usage(char **args);
shell_status_t list_env(char **args);
shell_status_t set_env_var(char **args);
shell_status_t unset_env_var(char **args);

// Helper: number of builtin commands supported by the shell
int num_builtin_functions();

#endif