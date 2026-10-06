#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#define PORT 10822
#define BUFFER_SIZE 4096
#define USERNAME_SIZE 50
#define FILENAME_SIZE 256

#define NID "5848"

typedef struct
{
    int receiving_file;

    FILE *file_fp;

    long file_size;
    long file_received;

    char sender[USERNAME_SIZE];
    char filename[FILENAME_SIZE];

} ReceiveFileState;


/* ========================================================= */
/* send_all                                                    */
/* ========================================================= */

static int send_all(int fd,
                    const void *data,
                    size_t length)
{
    const char *ptr = data;

    size_t sent = 0;

    while (sent < length)
    {
        ssize_t n =
            send(fd,
                 ptr + sent,
                 length - sent,
                 0);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return -1;
        }

        if (n == 0)
        {
            return -1;
        }

        sent += (size_t)n;
    }

    return 0;
}


/* ========================================================= */
/* Receive exact number of bytes                               */
/* ========================================================= */

static int receive_exact(int fd,
                         void *buffer,
                         size_t length)
{
    char *ptr = buffer;

    size_t received = 0;

    while (received < length)
    {
        ssize_t n =
            recv(fd,
                 ptr + received,
                 length - received,
                 0);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return -1;
        }

        if (n == 0)
        {
            return -1;
        }

        received += (size_t)n;
    }

    return 0;
}


/* ========================================================= */
/* Receive incoming file                                      */
/* ========================================================= */

static int receive_file(int fd,
                        const char *header,
                        const char *username)
{
    char sender[USERNAME_SIZE];

    char filename[FILENAME_SIZE];

    long filesize;


    memset(sender, 0, sizeof(sender));

    memset(filename, 0, sizeof(filename));

    filesize = 0;


    if (sscanf(header,
               "FILE_FROM %49s %255s %ld",
               sender,
               filename,
               &filesize) != 3)
    {
        printf("Invalid file header received.\n");

        return -1;
    }


    if (filesize < 0)
    {
        printf("Invalid file size.\n");

        return -1;
    }


    /*
     * Save received files locally.
     *
     * The server's required storage copy is separate.
     */
    mkdir("received",
          0755);


    char filepath[512];

    snprintf(filepath,
             sizeof(filepath),
             "received/%s",
             filename);


    FILE *fp =
        fopen(filepath,
              "wb");

    if (fp == NULL)
    {
        perror("fopen");

        return -1;
    }


    printf("\n========================================\n");
    printf("Incoming file\n");
    printf("From : %s\n", sender);
    printf("File : %s\n", filename);
    printf("Size : %ld bytes\n",
           filesize);
    printf("========================================\n");


    char buffer[BUFFER_SIZE];

    long remaining =
        filesize;

    long total =
        0;


    while (remaining > 0)
    {
        size_t wanted =
            remaining > BUFFER_SIZE
                ? BUFFER_SIZE
                : (size_t)remaining;


        ssize_t n =
            recv(fd,
                 buffer,
                 wanted,
                 0);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            perror("recv");

            fclose(fp);

            return -1;
        }


        if (n == 0)
        {
            printf("Server disconnected during file transfer.\n");

            fclose(fp);

            return -1;
        }


        fwrite(buffer,
               1,
               (size_t)n,
               fp);


        total += n;

        remaining -= n;


        printf("\rReceived: %ld / %ld bytes",
               total,
               filesize);

        fflush(stdout);
    }


    fclose(fp);


    printf("\nFile received successfully.\n");
    printf("Saved as: %s\n",
           filepath);


    /*
     * username is used to avoid compiler warnings
     * if the implementation is later extended.
     */
    (void)username;

    return 0;
}


/* ========================================================= */
/* Main                                                        */
/* ========================================================= */

int main(void)
{
    int client_fd;

    struct sockaddr_in server_addr;

    char username[USERNAME_SIZE];


    /* ----------------------------------------------------- */
    /* Socket                                                  */
    /* ----------------------------------------------------- */

    client_fd =
        socket(AF_INET,
               SOCK_STREAM,
               0);

    if (client_fd < 0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }


    /* ----------------------------------------------------- */
    /* Server address                                          */
    /* ----------------------------------------------------- */

    memset(&server_addr,
           0,
           sizeof(server_addr));

    server_addr.sin_family =
        AF_INET;

    server_addr.sin_port =
        htons(PORT);


    if (inet_pton(AF_INET,
                  "127.0.0.1",
                  &server_addr.sin_addr) <= 0)
    {
        perror("inet_pton");

        close(client_fd);

        return EXIT_FAILURE;
    }


    /* ----------------------------------------------------- */
    /* Connect                                                  */
    /* ----------------------------------------------------- */

    if (connect(client_fd,
                (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0)
    {
        perror("connect");

        close(client_fd);

        return EXIT_FAILURE;
    }


    printf("Connected to NetMessenger server.\n");
    printf("Port: %d\n",
           PORT);


    /* ----------------------------------------------------- */
    /* Register                                                */
    /* ----------------------------------------------------- */

    printf("\nEnter username: ");

    if (fgets(username,
              sizeof(username),
              stdin) == NULL)
    {
        close(client_fd);

        return EXIT_FAILURE;
    }


    username[
        strcspn(username,
                "\r\n")] = '\0';


    char register_command[BUFFER_SIZE];

    snprintf(register_command,
             sizeof(register_command),
             "REGISTER %s\n",
             username);


    if (send_all(client_fd,
                 register_command,
                 strlen(register_command)) < 0)
    {
        perror("send");

        close(client_fd);

        return EXIT_FAILURE;
    }


    /*
     * Receive registration response.
     */
    char response[BUFFER_SIZE];

    size_t response_length = 0;


    while (response_length <
           sizeof(response) - 1)
    {
        char c;

        ssize_t n =
            recv(client_fd,
                 &c,
                 1,
                 0);

        if (n <= 0)
        {
            printf("Server disconnected.\n");

            close(client_fd);

            return EXIT_FAILURE;
        }

        response[
            response_length++] = c;

        if (c == '\n')
        {
            break;
        }
    }


    response[response_length] =
        '\0';


    printf("Server: %s",
           response);


    /* ----------------------------------------------------- */
    /* Main loop                                               */
    /* ----------------------------------------------------- */

    while (1)
    {
        fd_set read_set;

        FD_ZERO(&read_set);

        FD_SET(STDIN_FILENO,
               &read_set);

        FD_SET(client_fd,
               &read_set);


        int max_fd =
            client_fd >
            STDIN_FILENO
                ? client_fd
                : STDIN_FILENO;


        int result =
            select(max_fd + 1,
                   &read_set,
                   NULL,
                   NULL,
                   NULL);


        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            perror("select");

            break;
        }


        /* ================================================= */
        /* Data from server                                   */
        /* ================================================= */

        if (FD_ISSET(client_fd,
                     &read_set))
        {
            char line[BUFFER_SIZE];

            size_t length = 0;

            int is_file = 0;


            /*
             * Read one line first.
             *
             * This handles normal messages and the
             * FILE_FROM header.
             */
            while (length <
                   sizeof(line) - 1)
            {
                char c;

                ssize_t n =
                    recv(client_fd,
                         &c,
                         1,
                         0);

                if (n < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }

                    perror("recv");

                    close(client_fd);

                    return EXIT_FAILURE;
                }

                if (n == 0)
                {
                    printf("\nServer disconnected.\n");

                    close(client_fd);

                    return 0;
                }

                line[length++] = c;

                if (c == '\n')
                {
                    break;
                }
            }


            line[length] = '\0';


            /*
             * File transfer header.
             */
            if (strncmp(line,
                        "FILE_FROM ",
                        10) == 0)
            {
                is_file = 1;
            }


            if (is_file)
            {
                receive_file(client_fd,
                             line,
                             username);

                continue;
            }


            /*
             * Normal server message.
             */
            printf("\nServer: %s",
                   line);


            if (strcmp(line,
                       "OK BYE NID:5848\n") == 0)
            {
                break;
            }


            printf("Enter command: ");
            fflush(stdout);
        }


        /* ================================================= */
        /* Keyboard input                                    */
        /* ================================================= */

        if (FD_ISSET(STDIN_FILENO,
                     &read_set))
        {
            char command[BUFFER_SIZE];


            printf("\n");
            printf("Commands:\n");
            printf("  LIST\n");
            printf("  BCAST <message>\n");
            printf("  PMSG <user> <message>\n");
            printf("  JOIN <room>\n");
            printf("  LEAVE <room>\n");
            printf("  ROOMS\n");
            printf("  RMSG <room> <message>\n");
            printf("  SENDFILE <user/room> <filename>\n");
            printf("  QUIT\n");
            printf("\nEnter command: ");


            fflush(stdout);


            if (fgets(command,
                      sizeof(command),
                      stdin) == NULL)
            {
                break;
            }


            command[
                strcspn(command,
                        "\r\n")] = '\0';


            if (strlen(command) == 0)
            {
                continue;
            }


            /*
             * Special handling for SENDFILE.
             *
             * User enters:
             *
             * SENDFILE kamal test.txt
             *
             * Client reads file size itself and sends:
             *
             * SENDFILE kamal test.txt <filesize>\n
             *
             * immediately followed by raw bytes.
             */
            if (strncmp(command,
                        "SENDFILE ",
                        9) == 0)
            {
                char target[USERNAME_SIZE];

                char filename[FILENAME_SIZE];


                if (sscanf(command,
                           "SENDFILE %49s %255s",
                           target,
                           filename) != 2)
                {
                    printf("Usage: SENDFILE <target> <filename>\n");

                    continue;
                }


                FILE *fp =
                    fopen(filename,
                          "rb");

                if (fp == NULL)
                {
                    perror("fopen");

                    continue;
                }


                /*
                 * Determine file size.
                 */
                if (fseek(fp,
                          0,
                          SEEK_END) != 0)
                {
                    perror("fseek");

                    fclose(fp);

                    continue;
                }


                long filesize =
                    ftell(fp);


                if (filesize < 0)
                {
                    perror("ftell");

                    fclose(fp);

                    continue;
                }


                rewind(fp);


                char header[BUFFER_SIZE];

                snprintf(header,
                         sizeof(header),
                         "SENDFILE %s %s %ld\n",
                         target,
                         filename,
                         filesize);


                /*
                 * Send command first.
                 */
                if (send_all(client_fd,
                             header,
                             strlen(header)) < 0)
                {
                    perror("send");

                    fclose(fp);

                    break;
                }


                /*
                 * Immediately send EXACTLY filesize
                 * raw bytes.
                 */
                char file_buffer[BUFFER_SIZE];

                long remaining =
                    filesize;


                while (remaining > 0)
                {
                    size_t wanted =
                        remaining > BUFFER_SIZE
                            ? BUFFER_SIZE
                            : (size_t)remaining;


                    size_t n =
                        fread(file_buffer,
                              1,
                              wanted,
                              fp);


                    if (n == 0)
                    {
                        break;
                    }


                    if (send_all(client_fd,
                                 file_buffer,
                                 n) < 0)
                    {
                        perror("send");

                        fclose(fp);

                        close(client_fd);

                        return EXIT_FAILURE;
                    }


                    remaining -=
                        (long)n;
                }


                fclose(fp);


                printf("File sent: %s (%ld bytes)\n",
                       filename,
                       filesize);


                continue;
            }


            /*
             * Normal command.
             */
            char command_line[BUFFER_SIZE];

            snprintf(command_line,
                     sizeof(command_line),
                     "%s\n",
                     command);


            if (send_all(client_fd,
                         command_line,
                         strlen(command_line)) < 0)
            {
                perror("send");

                break;
            }
        }
    }


    close(client_fd);

    printf("Client closed.\n");

    return 0;
}
