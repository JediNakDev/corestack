#include "system_program.h"
#include "libs/perms.h"

/*
    List the items in the directory
*/
int execute(char **args)
{
    if (args[1] != NULL)
    {
        char *token = strtok(args[1], SHELL_OPT_DELIM);
        // printf("Token is %s\n", token);

        if (token != NULL)
        {
            if (strcmp(token, "r") == 0)
            {
                // call listdirall,
                // execvp still need the ./bin because this was called
                // by a process that was at the .. directory
                if (execvp("./bin/ldr", args) == -1)
                {
                    perror("Failed to execute, command is invalid.");
                }
                return 1;
            }
            else
            {
                printf("Invalid option. Use -r to display all files within the current directory and its subdirectories.\n");
                return EXIT_SUCCESS;
            }
        }
    }

    // print out all the contents of the directory using opendir() function
    DIR *d;
    struct dirent *dir;
    struct stat st;
    char permissions[11];
    d = opendir(".");
    if (d)
    {
        while ((dir = readdir(d)) != NULL)
        {
            // Skip dotfiles
            if (dir->d_name[0] != '.')
            {
                if (stat(dir->d_name, &st) == 0)
                {
                    perms_to_string(st.st_mode, permissions);
                    printf(COLOR_RED "%s " COLOR_GREEN "%s\n" COLOR_RESET, permissions, dir->d_name);
                }
                else
                {
                    perror("stat failed");
                }
            }
        }
        closedir(d);
    }
    else
    {
        printf("Directory doesn't exist. \n");
    }

    return EXIT_SUCCESS;
}

int main(int argc, char **args)
{
    return execute(args);
}