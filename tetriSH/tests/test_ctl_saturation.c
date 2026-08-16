/* End-to-end proof that TCP listener load cannot block the local ctl plane. */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include "test_output.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FLOOD_CONNECTIONS 64

static int wait_pid(pid_t pid, int timeout_ms, int *status)
{
    for (int waited = 0; waited < timeout_ms; waited += 20)
    {
        pid_t rc = waitpid(pid, status, WNOHANG);
        if (rc == pid)
            return 0;
        if (rc < 0)
            return -1;
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 20000000L};
        nanosleep(&ts, NULL);
    }
    return -1;
}

static int reserve_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                               .sin_port = 0};
    socklen_t len = sizeof addr;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &len) != 0)
    {
        close(fd);
        return -1;
    }
    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

static int wait_for_path(const char *path, int timeout_ms)
{
    struct stat st;
    for (int waited = 0; waited < timeout_ms; waited += 20)
    {
        if (lstat(path, &st) == 0 && S_ISSOCK(st.st_mode))
            return 0;
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 20000000L};
        nanosleep(&ts, NULL);
    }
    return -1;
}

int main(void)
{
    test_output_begin("test_ctl_saturation");
    char root[PATH_MAX];
    char tmp[] = "/tmp/tetrish-ctl-saturation-XXXXXX";
    if (getcwd(root, sizeof root) == NULL || mkdtemp(tmp) == NULL)
    {
        test_output_fail("control-plane saturation setup");
        test_output_summary(1, 1, 0);
        return 1;
    }

    int port = reserve_port();
    char rc_path[PATH_MAX], ctl_path[PATH_MAX], daemon_path[PATH_MAX];
    char log_path[PATH_MAX];
    snprintf(rc_path, sizeof rc_path, "%s/.tetrishrc", tmp);
    snprintf(ctl_path, sizeof ctl_path, "%s/tetrisd.ctl", tmp);
    snprintf(daemon_path, sizeof daemon_path, "%s/bin/tetrisd", root);
    snprintf(log_path, sizeof log_path, "%s/tetrisd.log", tmp);

    FILE *rc = fopen(rc_path, "w");
    if (port < 0 || rc == NULL)
    {
        test_output_fail("control-plane saturation setup");
        test_output_summary(1, 1, 0);
        return 1;
    }
    /* All six directives rc_config() demands, or tetrisd refuses to start and
     * that reads as "the daemon died under the flood". No handshake here. */
    fprintf(rc,
            "listen_port = %d\nctl_ipc = %s\nlog_ipc = %s/no-log.sock\n"
            "cert_path = %s/auth/server_signed.crt\n"
            "key_path = %s/auth/private_key.pem\n"
            "ca_path = %s/auth/cacsertificate.crt\n"
            "log_path = %s\n",
            port, ctl_path, tmp, root, root, root, log_path);
    fclose(rc);

    pid_t daemon = fork();
    if (daemon == 0)
    {
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd >= 0)
        {
            dup2(nullfd, STDOUT_FILENO);
            dup2(nullfd, STDERR_FILENO);
        }
        setenv("TETRISH_ROOT", tmp, 1);
        execl(daemon_path, "tetrisd", (char *)NULL);
        _exit(127);
    }
    if (daemon < 0 || wait_for_path(ctl_path, 3000) != 0)
        goto fail;

    int flood[FLOOD_CONNECTIONS];
    int opened = 0;
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                               .sin_port = htons((unsigned short)port)};
    for (int i = 0; i < FLOOD_CONNECTIONS; i++)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            break;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0 ||
            errno == EINPROGRESS)
        {
            flood[opened++] = fd;
        }
        else
        {
            close(fd);
        }
    }
    if (opened < 16)
        goto fail;

    char ctl_bin[PATH_MAX];
    snprintf(ctl_bin, sizeof ctl_bin, "%s/bin/tetrisctl", root);
    pid_t ctl = fork();
    if (ctl == 0)
    {
        execl(ctl_bin, "tetrisctl", "--socket", ctl_path, "shutdown",
              (char *)NULL);
        _exit(127);
    }

    int ctl_status = 0, daemon_status = 0;
    int ok = ctl > 0 && wait_pid(ctl, 3000, &ctl_status) == 0 &&
             WIFEXITED(ctl_status) && WEXITSTATUS(ctl_status) == 0 &&
             wait_pid(daemon, 5000, &daemon_status) == 0 &&
             WIFEXITED(daemon_status) && WEXITSTATUS(daemon_status) == 0;

    for (int i = 0; i < opened; i++)
        close(flood[i]);
    unlink(ctl_path);
    unlink(rc_path);
    unlink(log_path);
    rmdir(tmp);
    if (ok)
        test_output_pass("control shutdown under flooded TCP connections");
    else
        test_output_fail("control shutdown under flooded TCP connections");
    test_output_summary(1, ok ? 0 : 1, 0);
    return ok ? 0 : 1;

fail:
    if (daemon > 0)
    {
        kill(daemon, SIGTERM);
        waitpid(daemon, NULL, 0);
    }
    unlink(ctl_path);
    unlink(rc_path);
    unlink(log_path);
    rmdir(tmp);
    test_output_fail("control-plane saturation setup");
    test_output_summary(1, 1, 0);
    return 1;
}
