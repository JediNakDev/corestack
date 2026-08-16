/**
 * ServerWithoutSecurity.c
 * -----------------------
 * Plain FTP server — no encryption, no authentication.
 * Receives files from the client using the length-prefixed wire protocol.
 *
 * Usage: ./ServerWithoutSecurity [PORT] [ADDRESS]
 *
 * Received files are saved to recv_files/ with a "recv_" prefix.
 */

#include "libs/common.h"

/* Global socket for SIGINT cleanup */
static int server_fd = -1;

void sigint_handler(int sig)
{
    (void)sig;
    printf("\nSIGINT or CTRL-C detected. Exiting gracefully\n");
    if (server_fd >= 0)
        close(server_fd);
    exit(0);
}

int main(int argc, char *argv[])
{
    signal(SIGINT, sigint_handler);

    int port = (argc > 1) ? atoi(argv[1]) : 4321;
    const char *address = (argc > 2) ? argv[2] : "localhost";

    /* Create listening socket */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (strcmp(address, "localhost") == 0)
        serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else if (strcmp(address, "0.0.0.0") == 0)
        serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else
        inet_pton(AF_INET, address, &serv_addr.sin_addr);

    if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("bind");
        return 1;
    }
    listen(server_fd, 1);
    printf("Server listening on %s:%d\n", address, port);

    /* Accept one client */
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0)
    {
        perror("accept");
        return 1;
    }
    printf("Client connected.\n");

    char filename[4096] = {0};

    while (1)
    {
        /* Read 8-byte message type */
        unsigned char *type_buf = read_bytes(client_fd, INT_BYTES);
        if (!type_buf)
            break;
        uint64_t msg_type = bytes_to_int(type_buf);
        free(type_buf);

        switch (msg_type)
        {
        case MSG_FILENAME:
        {
            /* If the packet is for transferring the filename */
            printf("Receiving file...\n");
            unsigned char *len_buf = read_bytes(client_fd, INT_BYTES);
            uint64_t fn_len = bytes_to_int(len_buf);
            free(len_buf);

            unsigned char *fn_buf = read_bytes(client_fd, fn_len);
            memset(filename, 0, sizeof(filename));
            memcpy(filename, fn_buf, fn_len);
            free(fn_buf);
            break;
        }
        case MSG_FILE_DATA:
        {
            /* If the packet is for transferring a chunk of the file */
            double start_time = get_time();

            unsigned char *len_buf = read_bytes(client_fd, INT_BYTES);
            uint64_t file_len = bytes_to_int(len_buf);
            free(len_buf);

            unsigned char *file_data = read_bytes(client_fd, file_len);

            /* Extract basename and prepend "recv_" */
            const char *base = strrchr(filename, '/');
            base = base ? base + 1 : filename;

            char outpath[4096];
            snprintf(outpath, sizeof(outpath), "recv_files/recv_%s", base);

            /* Write the file with 'recv_' prefix */
            FILE *fp = fopen(outpath, "wb");
            if (fp)
            {
                fwrite(file_data, 1, file_len, fp);
                fclose(fp);
            }
            free(file_data);

            printf("Finished receiving file in %.3fs!\n", get_time() - start_time);
            break;
        }
        case MSG_CLOSE:
            /* Close the connection */
            printf("Closing connection...\n");
            goto done;

        default:
            fprintf(stderr, "Unknown message type: %lu\n", (unsigned long)msg_type);
            goto done;
        }
    }

done:
    close(client_fd);
    close(server_fd);
    return 0;
}
