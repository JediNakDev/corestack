#include "shell.h"

/** Frees the tokens collected so far and empties cmd[], so a refused line
    reads back exactly as a blank one. Called by read_command() on refusal. */
static void discard_command(char **cmd, int count)
{
    for (int i = 0; i < count; i++)
        free(cmd[i]);
    cmd[0] = NULL;
}

/** Consumes the rest of an over-long line and returns how many characters it
    dropped. Called by read_command() when the line buffer filled up. */
static int drain_line()
{
    int discarded = 0;
    for (;;)
    {
        int c = fgetc(stdin);
        if (c == '\n' || c == EOF)
            return discarded;
        discarded++;
    }
}

shell_status_t read_command(char **cmd)
{
    char line[MAX_LINE];

    cmd[0] = NULL;

    // fgets stops at the newline, at EOF, or one short of the buffer, so the
    // buffer can never be overrun no matter what is typed.
    if (fgets(line, sizeof(line), stdin) == NULL)
        return SHELL_EOF;

    // No newline in the buffer means either the line filled it exactly or the
    // line is longer than the shell accepts. Draining tells the two apart: a
    // line that fits leaves nothing behind.
    if (strchr(line, '\n') == NULL && drain_line() > 0)
    {
        fprintf(stderr, "Command is too long (limit %d characters), ignored\n",
                MAX_LINE - 1);
        return SHELL_SUCCESS;
    }

    int count = 0;
    for (char *token = strtok(line, SHELL_DELIMS); token != NULL;
         token = strtok(NULL, SHELL_DELIMS))
    {
        // execvp needs the argument vector to end in a NULL pointer, so slot 63
        // is reserved for that terminator.
        if (count >= MAX_ARGS - 1)
        {
            fprintf(stderr, "Too many arguments (limit %d), command ignored\n",
                    MAX_ARGS - 1);
            discard_command(cmd, count);
            return SHELL_SUCCESS;
        }

        cmd[count] = strdup(token);
        if (cmd[count] == NULL)
        {
            perror("strdup");
            discard_command(cmd, count);
            return SHELL_SUCCESS;
        }
        count++;
    }

    // Null-terminate the cmd array to mark the end of arguments
    cmd[count] = NULL;
    return SHELL_SUCCESS;
}

extern char **environ;

const char *builtin_commands[] = {
    "cd",   // Changes the current directory of the shell to the specified path.
            // If no path is given, it defaults to the user's home directory.
    "help", //  List all builtin commands in the shell
    "exit", // Exits the shell
    "usage",  // Provides a brief usage guide for the shell and its built-in
              // command
    "env",    // Lists all the environment variables currently set in the shell
    "setenv", // Sets or modifies an environment variable for this shell session
    "unsetenv" // Removes an environment variable from the shell
};

/*** This is array of functions, with argument char ***/
shell_status_t (*builtin_command_func[])(char **) = {
    &shell_cd,     // builtin_command_func[0]: cd
    &shell_help,   // builtin_command_func[1]: help
    &shell_exit,   // builtin_command_func[2]: exit
    &shell_usage,  // builtin_command_func[3]: usage
    &list_env,     // builtin_command_func[4]: env
    &set_env_var,  // builtin_command_func[5]: setenv
    &unset_env_var // builtin_command_func[6]: unsetenv
};

// Helper function to figure out how many builtin commands are supported
int num_builtin_functions()
{
    return sizeof(builtin_commands) / sizeof(char *);
}

shell_status_t shell_cd(char **args)
{
    const char *target = args[1];
    if (target == NULL)
    {
        target = getenv("HOME");
        if (target == NULL)
        {
            fprintf(stderr, "cd: HOME not set\n");
            return SHELL_SUCCESS;
        }
    }
    if (chdir(target) != 0)
        perror("cd");
    return SHELL_SUCCESS;
}

shell_status_t shell_help(char **args)
{
    (void)args;
    printf("Tetrish - supported builtin commands:\n");
    for (int i = 0; i < num_builtin_functions(); i++)
        printf("  %s\n", builtin_commands[i]);
    printf("Type 'usage <command>' for a brief description of a command.\n");
    return SHELL_SUCCESS;
}

shell_status_t shell_exit(char **args)
{
    (void)args;
    return SHELL_EXIT;
}

shell_status_t shell_usage(char **args)
{
    // Descriptions aligned by index with builtin_commands[]
    static const char *descriptions[] = {
        "cd [dir]        : change the current directory (defaults to $HOME)",
        "help            : list all builtin commands",
        "exit            : exit the shell",
        "usage [cmd]     : show usage of a builtin command, or of all of them",
        "env             : list all environment variables",
        "setenv KEY=VALUE: set or modify an environment variable",
        "unsetenv KEY    : remove an environment variable"};

    if (args[1] == NULL)
    {
        printf("Command not given. Type usage <command>.\n");
        return SHELL_SUCCESS;
    }

    for (int i = 0; i < num_builtin_functions(); i++)
    {
        if (strcmp(args[1], builtin_commands[i]) == 0)
        {
            printf("  %s\n", descriptions[i]);
            return SHELL_SUCCESS;
        }
    }

    printf("The command '%s' is not part of Tetrish's builtin commands.\n",
           args[1]);
    return SHELL_SUCCESS;
}

shell_status_t list_env(char **args)
{
    (void)args;
    char **env = environ;
    while (*env)
    {
        printf("%s\n", *env);
        env++;
    }
    return SHELL_SUCCESS;
}

shell_status_t set_env_var(char **args)
{
    if (args[1] == NULL)
    {
        fprintf(stderr, "setenv: usage: setenv KEY=VALUE\n");
        return SHELL_SUCCESS;
    }

    char *equals = strchr(args[1], '=');
    if (equals == NULL || equals == args[1])
    {
        fprintf(stderr, "setenv: usage: setenv KEY=VALUE\n");
        return SHELL_SUCCESS;
    }

    *equals = '\0'; // KEY=VALUE to KEY\0VALUE
    char *key = args[1];
    char *value = equals + 1;

    if (setenv(key, value, 1) != 0)
        perror("setenv");

    *equals = '='; // Restore the token
    return SHELL_SUCCESS;
}

shell_status_t unset_env_var(char **args)
{
    if (args[1] == NULL)
    {
        fprintf(stderr, "unsetenv: usage: unsetenv KEY\n");
        return SHELL_SUCCESS;
    }
    if (unsetenv(args[1]) != 0)
        perror("unsetenv");
    return SHELL_SUCCESS;
}

// Function to display the shell prompt
void type_prompt()
{
    fflush(stdout); // Flush the output buffer
    printf("$$ ");  // Print the shell prompt
}
