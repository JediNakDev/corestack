/**
 * ClientWithoutSecurity.c -> now will be modified to ClientWithSecurity.c
 * -----------------------
 * Plain FTP client — no encryption, no authentication.
 * Sends files to the server using the length-prefixed wire protocol.
 *
 * Usage: ./ClientWithoutSecurity [PORT] [ADDRESS]
 *
 * Wire protocol (all lengths are 8-byte big-endian):
 *   [MSG_FILENAME=0][filename_len][filename_bytes]
 */

#include "libs/common.h"

int main(int argc, char *argv[])
{
    int port = (argc > 1) ? atoi(argv[1]) : 4321;
    const char *server_address = (argc > 2) ? argv[2] : "localhost";

    double start_time = get_time();

    printf("Establishing connection to server...\n");

    /* Create TCP socket and connect to server */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket");
        return 1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    struct hostent *he = gethostbyname(server_address);
    if (!he)
    {
        fprintf(stderr, "Cannot resolve host: %s\n", server_address);
        return 1;
    }
    memcpy(&serv_addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("connect");
        return 1;
    }
    printf("Connected\n");

    // authentication
    unsigned char auth_message[32];
    generate_session_key(auth_message);
    size_t msg_len = sizeof(auth_message);
    send_int(sockfd, MSG_AUTH);
    send_int(sockfd, msg_len);
    send_all(sockfd, (unsigned char *)auth_message, msg_len);    
    // receiving M1-2: signed_message
    unsigned char *signed_message_len_buf = read_bytes(sockfd, INT_BYTES);
    uint64_t signed_message_len = bytes_to_int(signed_message_len_buf);
    unsigned char *signed_message = read_bytes(sockfd, signed_message_len);
    // receiving M3-4: server_cert
    unsigned char *server_cert_len_buf = read_bytes(sockfd, INT_BYTES);
    uint64_t server_cert_len = bytes_to_int(server_cert_len_buf);
    unsigned char *server_cert_buf = read_bytes(sockfd, server_cert_len);

    // verifying server cert first, then signed message
    X509 *server_cert = load_cert_bytes(server_cert_buf, server_cert_len);
    if(!verify_server_cert(server_cert, "auth/cacsertificate.crt")){
        fprintf(stderr, "server cert not verifed");
        close(sockfd);
        return 1;
    }
    printf("Verified Server Cert\n");
    
    if(!verify_message_pss(server_cert, signed_message, signed_message_len, auth_message, msg_len)){
        fprintf(stderr, "signed message not verified");
        close(sockfd);
        return 1;
    }
    printf("Verified Signed Message\n");

    free(server_cert_len_buf);
    free(signed_message_len_buf);
    free(signed_message);
    free(server_cert_buf);
    X509_free(server_cert);
    /* Interactive file sending loop */
    while (1)
    {
        char filename[4096];
        printf("Enter a filename to send (enter -1 to exit):");
        if (!fgets(filename, sizeof(filename), stdin))
            break;

        /* Strip trailing newline */
        filename[strcspn(filename, "\n")] = '\0';

        /* Validate filename */
        while (strcmp(filename, "-1") != 0)
        {
            struct stat st;
            if (stat(filename, &st) == 0 && S_ISREG(st.st_mode))
                break;
            printf("Invalid filename. Please try again:");
            if (!fgets(filename, sizeof(filename), stdin))
                goto done;
            filename[strcspn(filename, "\n")] = '\0';
        }

        if (strcmp(filename, "-1") == 0)
        {
            send_int(sockfd, MSG_CLOSE);
            break;
        }

        /* Send the filename: [0][len][bytes] */
        size_t fn_len = strlen(filename);
        send_int(sockfd, MSG_FILENAME);
        send_int(sockfd, fn_len);
        send_all(sockfd, (unsigned char *)filename, fn_len);

        /* Read the entire file into memory */
        FILE *fp = fopen(filename, "rb");
        if (!fp)
        {
            perror("fopen");
            continue;
        }
        fseek(fp, 0, SEEK_END);
        long file_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        unsigned char *file_data = malloc(file_size);
        fread(file_data, 1, file_size, fp);
        fclose(fp);

        /* Send the file data: [1][len][bytes] */
        send_int(sockfd, MSG_FILE_DATA);
        send_int(sockfd, (uint64_t)file_size);
        send_all(sockfd, file_data, (uint64_t)file_size);
        free(file_data);
    }

done:
    /* Send close message */
    send_int(sockfd, MSG_CLOSE);
    printf("Closing connection...\n");
    close(sockfd);

    double end_time = get_time();
    printf("Program took %.3fs to run.\n", end_time - start_time);
    return 0;
}
