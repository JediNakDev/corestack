#include "shell.h"

// Function to read a command from the user input
void read_command(char **cmd) {
  // Define a character array to store the command line input
  char line[MAX_LINE];
  // Initialize count to keep track of the number of characters read
  int count = 0, i = 0;
  // Array to hold pointers to the parsed command arguments
  char *array[MAX_ARGS], *command_token;

  // Infinite loop to read characters until a newline or maximum line length is
  // reached
  for (;;) {
    // Read a single character from standard input
    int current_char = fgetc(stdin);
    // Store the character in the line array and increment count
    line[count++] = (char)current_char;
    // If a newline character is encountered, break out of the loop
    if (current_char == '\n')
      break;
    // If the command exceeds the maximum length, print an error and exit
    if (count >= MAX_LINE) {
      printf("Command is too long, unable to process\n");
      exit(1);
    }
  }
  // Null-terminate the command line string
  line[count] = '\0';

  // If only the newline character was entered, return without processing
  if (count == 1)
    return;

  // Use strtok to parse the first token (word) of the command
  command_token = strtok(line, " \n");

  // Continue parsing the line into words and store them in the array
  while (command_token != NULL) {
    array[i++] = strdup(command_token);  // Duplicate the token and store it
    command_token = strtok(NULL, " \n"); // Get the next token
  }

  // Copy the parsed command and its parameters to the cmd array
  for (int j = 0; j < i; j++) {
    cmd[j] = array[j];
  }
  // Null-terminate the cmd array to mark the end of arguments
  cmd[i] = NULL;
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
int (*builtin_command_func[])(char **) = {
    &shell_cd,     // builtin_command_func[0]: cd
    &shell_help,   // builtin_command_func[1]: help
    &shell_exit,   // builtin_command_func[2]: exit
    &shell_usage,  // builtin_command_func[3]: usage
    &list_env,     // builtin_command_func[4]: env
    &set_env_var,  // builtin_command_func[5]: setenv
    &unset_env_var // builtin_command_func[6]: unsetenv
};

// Helper function to figure out how many builtin commands are supported
int num_builtin_functions() {
  return sizeof(builtin_commands) / sizeof(char *);
}

int shell_cd(char **args) {
  const char *target = args[1];
  if (target == NULL) {
    target = getenv("HOME");
    if (target == NULL) {
      fprintf(stderr, "cd: HOME not set\n");
      return SHELL_SUCCESS;
    }
  }
  if (chdir(target) != 0)
    perror("cd");
  return SHELL_SUCCESS;
}

int shell_help(char **args) {
  (void)args;
  printf("Tetrish - supported builtin commands:\n");
  for (int i = 0; i < num_builtin_functions(); i++)
    printf("  %s\n", builtin_commands[i]);
  printf("Type 'usage <command>' for a brief description of a command.\n");
  return SHELL_SUCCESS;
}

int shell_exit(char **args) {
  (void)args;
  return SHELL_EXIT;
}

int shell_usage(char **args) {
  // Descriptions aligned by index with builtin_commands[]
  static const char *descriptions[] = {
      "cd [dir]        : change the current directory (defaults to $HOME)",
      "help            : list all builtin commands",
      "exit            : exit the shell",
      "usage [cmd]     : show usage of a builtin command, or of all of them",
      "env             : list all environment variables",
      "setenv KEY=VALUE: set or modify an environment variable",
      "unsetenv KEY    : remove an environment variable"};

  if (args[1] == NULL) {
    printf("Command not given. Type usage <command>.\n");
    return SHELL_SUCCESS;
  }

  for (int i = 0; i < num_builtin_functions(); i++) {
    if (strcmp(args[1], builtin_commands[i]) == 0) {
      printf("  %s\n", descriptions[i]);
      return SHELL_SUCCESS;
    }
  }

  printf("The command '%s' is not part of Tetrish's builtin commands.\n",
         args[1]);
  return SHELL_SUCCESS;
}

int list_env(char **args) {
  (void)args;
  char **env = environ;
  while (*env) {
    printf("%s\n", *env);
    env++;
  }
  return SHELL_SUCCESS;
}

int set_env_var(char **args) {
  if (args[1] == NULL) {
    fprintf(stderr, "setenv: usage: setenv KEY=VALUE\n");
    return SHELL_SUCCESS;
  }

  char *equals = strchr(args[1], '=');
  if (equals == NULL || equals == args[1]) {
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

int unset_env_var(char **args) {
  if (args[1] == NULL) {
    fprintf(stderr, "unsetenv: usage: unsetenv KEY\n");
    return SHELL_SUCCESS;
  }
  if (unsetenv(args[1]) != 0)
    perror("unsetenv");
  return SHELL_SUCCESS;
}

// Function to display the shell prompt
void type_prompt() {
  fflush(stdout); // Flush the output buffer
  printf("$$ ");  // Print the shell prompt
}
