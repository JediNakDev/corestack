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

#define SHELL_EXIT 0
#define SHELL_SUCCESS 1
#define SHELL_INVALID_CMD -1

void type_prompt();
void read_command(char **cmd);

extern const char *builtin_commands[];
extern int (*builtin_command_func[])(char **);

int shell_cd(char **args);
int shell_help(char **args);
int shell_exit(char **args);
int shell_usage(char **args);
int list_env(char **args);
int set_env_var(char **args);
int unset_env_var(char **args);

// Helper: number of builtin commands supported by the shell
int num_builtin_functions();

#endif