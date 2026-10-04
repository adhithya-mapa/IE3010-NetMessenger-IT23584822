#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 10822
#define BUFFER_SIZE 1024
#define MAX_USERS 10
#define USERNAME_SIZE 50

#define NID "NID:5848"

int main(void)
{
    int server_fd;
    int client_fd;

    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;

    socklen_t client_len = sizeof(client_addr);

    char buffer[BUFFER_SIZE];

    /* Store registered usernames */
    char usernames[MAX_USERS][USERNAME_SIZE];

    int user_count = 0;

    /* -------------------------------------------------- */
    /* 1. Create server socket                            */
    /* -------------------------------------------------- */

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    printf("Server socket created successfully.\n");

    /* -------------------------------------------------- */
    /* 2. Configure server address                        */
    /* -------------------------------------------------- */

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    /* -------------------------------------------------- */
    /* 3. Bind server to port 10822                       */
    /* -------------------------------------------------- */

    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server bound to port %d.\n", PORT);

    /* -------------------------------------------------- */
    /* 4. Listen for clients                              */
    /* -------------------------------------------------- */

    if (listen(server_fd, 5) < 0)
    {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server is listening...\n");

    /* -------------------------------------------------- */
    /* 5. Accept one client                               */
    /* -------------------------------------------------- */

    client_fd = accept(server_fd,
                       (struct sockaddr *)&client_addr,
                       &client_len);

    if (client_fd < 0)
    {
        perror("accept");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Client connected from %s\n",
           inet_ntoa(client_addr.sin_addr));

    int registered = 0;
    char current_username[USERNAME_SIZE];

    /* -------------------------------------------------- */
    /* 6. Communication loop                              */
    /* -------------------------------------------------- */

    while (1)
    {
        memset(buffer, 0, sizeof(buffer));

        int bytes_received = recv(client_fd,
                                  buffer,
                                  sizeof(buffer) - 1,
                                  0);

        if (bytes_received <= 0)
        {
            printf("Client disconnected.\n");
            break;
        }

        buffer[bytes_received] = '\0';

        /* Remove newline if present */
        buffer[strcspn(buffer, "\r\n")] = '\0';

        printf("Received: %s\n", buffer);

        /* ================================================== */
        /* REGISTER command                                   */
        /* ================================================== */

        if (strncmp(buffer, "REGISTER ", 9) == 0)
        {
            if (registered)
            {
                char response[BUFFER_SIZE];

                snprintf(response,
                         sizeof(response),
                         "ERR 003 ALREADY_REGISTERED %s",
                         NID);

                send(client_fd,
                     response,
                     strlen(response),
                     0);

                continue;
            }

            char username[USERNAME_SIZE];

            memset(username, 0, sizeof(username));

            sscanf(buffer + 9, "%49s", username);

            /* Check empty username */
            if (strlen(username) == 0)
            {
                char response[BUFFER_SIZE];

                snprintf(response,
                         sizeof(response),
                         "ERR 004 INVALID_USERNAME %s",
                         NID);

                send(client_fd,
                     response,
                     strlen(response),
                     0);

                continue;
            }

            /* Check duplicate username */
            int duplicate = 0;

            for (int i = 0; i < user_count; i++)
            {
                if (strcmp(usernames[i], username) == 0)
                {
                    duplicate = 1;
                    break;
                }
            }

            if (duplicate)
            {
                char response[BUFFER_SIZE];

                snprintf(response,
                         sizeof(response),
                         "ERR 001 USERNAME_TAKEN %s",
                         NID);

                send(client_fd,
                     response,
                     strlen(response),
                     0);

                continue;
            }

            /* Check maximum number of users */
            if (user_count >= MAX_USERS)
            {
                char response[BUFFER_SIZE];

                snprintf(response,
                         sizeof(response),
                         "ERR 005 SERVER_FULL %s",
                         NID);

                send(client_fd,
                     response,
                     strlen(response),
                     0);

                continue;
            }

            /* Store username */
            strcpy(usernames[user_count], username);

            user_count++;

            strcpy(current_username, username);

            registered = 1;

            printf("User registered: %s\n", username);

            /* Send successful registration response */
            char response[BUFFER_SIZE];

            snprintf(response,
                     sizeof(response),
                     "OK REGISTERED %s %s",
                     username,
                     NID);

            send(client_fd,
                 response,
                 strlen(response),
                 0);
        }

        /* ================================================== */
        /* LIST command                                       */
        /* ================================================== */

        else if (strcmp(buffer, "LIST") == 0)
        {
            if (!registered)
            {
                char response[BUFFER_SIZE];

                snprintf(response,
                         sizeof(response),
                         "ERR 002 NOT_REGISTERED %s",
                         NID);

                send(client_fd,
                     response,
                     strlen(response),
                     0);

                continue;
            }

            char response[BUFFER_SIZE];

            memset(response, 0, sizeof(response));

            strcpy(response, "OK USERS ");

            for (int i = 0; i < user_count; i++)
            {
                strcat(response, usernames[i]);

                if (i < user_count - 1)
                {
                    strcat(response, ",");
                }
            }

            strcat(response, " ");
            strcat(response, NID);

            send(client_fd,
                 response,
                 strlen(response),
                 0);
        }

        /* ================================================== */
        /* QUIT command                                       */
        /* ================================================== */

        else if (strcmp(buffer, "QUIT") == 0)
        {
            char response[BUFFER_SIZE];

            snprintf(response,
                     sizeof(response),
                     "OK BYE %s",
                     NID);

            send(client_fd,
                 response,
                 strlen(response),
                 0);

            printf("Client requested disconnect.\n");

            break;
        }

        /* ================================================== */
        /* Unknown command                                    */
        /* ================================================== */

        else
        {
            char response[BUFFER_SIZE];

            snprintf(response,
                     sizeof(response),
                     "ERR 006 UNKNOWN_COMMAND %s",
                     NID);

            send(client_fd,
                 response,
                 strlen(response),
                 0);
        }
    }

    /* -------------------------------------------------- */
    /* Close client and server                            */
    /* -------------------------------------------------- */

    close(client_fd);
    close(server_fd);

    printf("Server closed.\n");

    return 0;
}
