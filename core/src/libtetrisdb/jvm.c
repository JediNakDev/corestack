/**
 * @file jvm.c
 * @brief The two things every runner needs before it can start.
 */

#include "libtetrisdb/jvm.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Runs "java -version" */
static int java_runs(const char *java)
{
    pid_t pid = fork();
    if (pid < 0)
        return 0;

    if (pid == 0)
    {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0)
        {
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
        }
        execlp(java, java, "-version", (char *)NULL);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int db_jvm_check(const char *java, const char *jar)
{
    if (jar == NULL || jar[0] == '\0' || access(jar, R_OK) != 0)
    {
        fprintf(stderr,
                "tetrisdb: cannot read %s - build it with `ant dist` in "
                "db/\n",
                jar != NULL && jar[0] != '\0' ? jar : "(no jar configured)");
        return -1;
    }

    if (java == NULL || java[0] == '\0' || !java_runs(java))
    {
        fprintf(stderr,
                "tetrisdb: %s does not run - install a JDK or set the "
                "java path\n",
                java != NULL && java[0] != '\0' ? java
                                                : "(no java configured)");
        return -1;
    }
    return 0;
}
