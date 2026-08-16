#include "libs/rc_parser.h"
#include "shell.h"

static int run_builtin(char **cmd) {
  for (int i = 0; i < num_builtin_functions(); i++) {
    if (strcmp(cmd[0], builtin_commands[i]) == 0)
      return (*builtin_command_func[i])(cmd);
  }
  return SHELL_INVALID_CMD;
}

static void run_system_program(char **cmd) {
  pid_t pid = fork();

  if (pid < 0) {
    fprintf(stderr, "Failed to fork process\n");
  } else if (pid == 0) {
    execvp(cmd[0], cmd);

    fprintf(stderr, "Command %s not found\n", cmd[0]);
    _exit(1);
  } else {
    int status;
    waitpid(pid, &status, WUNTRACED);

    if (WIFEXITED(status)) {
      int child_exit_status = WEXITSTATUS(status);
      (void)child_exit_status;
    }
  }
}

static void execute_command_line(char *line) {
  char *args[MAX_ARGS];
  int n = 0;

  char *token = strtok(line, " \t\r\n");
  while (token != NULL && n < MAX_ARGS - 1) {
    args[n++] = token;
    token = strtok(NULL, " \t\r\n");
  }
  args[n] = NULL;

  // If no arguments are provided, return
  if (n == 0)
    return;

  if (run_builtin(args) == SHELL_INVALID_CMD)
    run_system_program(args);
}

static void process_rc_file(const char *shell_dir) {
  char rc_path[PATH_MAX];
  snprintf(rc_path, sizeof(rc_path), "%s/.tetrishrc", shell_dir);

  FILE *rc = fopen(rc_path, "r");
  if (rc == NULL)
    return;

  char line[MAX_LINE];
  while (fgets(line, sizeof(line), rc) != NULL) {
    line[strcspn(line, "\r\n")] = '\0';

    const char *value = NULL;
    rc_line_type_t type = classify_rc_line(line, &value);

    if (type == RC_LINE_EMPTY) {
      continue;
    } else if (type == RC_LINE_PATH) {
      if (setenv("PATH", value, 1) != 0)
        perror("setenv PATH");
    } else {
      execute_command_line((char *)value);
    }
  }

  fclose(rc);
}

int main(void) {
  char *cmd[MAX_ARGS];

  char shell_dir[PATH_MAX];
  if (getcwd(shell_dir, sizeof(shell_dir)) == NULL) {
    printf("Failed to get current working directory.\n");
    return 1;
  }

  process_rc_file(shell_dir);

  for (;;) {
    cmd[0] = NULL;

    type_prompt();
    read_command(cmd);

    if (cmd[0] == NULL)
      continue;

    int builtin_result = run_builtin(cmd);
    if (builtin_result == SHELL_INVALID_CMD)
      run_system_program(cmd);

    for (int i = 0; cmd[i] != NULL; i++)
      free(cmd[i]);

    if (builtin_result == SHELL_EXIT)
      break;
  }

  return 0;
}
