#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define PORT 10822
#define BUFFER_SIZE 1024
#define MAX_CLIENTS 10
#define USERNAME_SIZE 50

#define NID "NID:5848"

typedef struct
{
    int fd;
    int registered;
    char username[USERNAME_SIZE];

    char input_buffer[BUFFER_SIZE];
    int input_length;

} Client;

/* -------------------------------------------------- */
/* Send a complete response                            */
/* -------------------------------------------------- */

void send_response(int fd, const char *message)
{
    send(fd, message, strlen(message), 0);
}


/* -------------------------------------------------- */
/* Remove a client                                    */
/* -------------------------------------------------- */

void remove_client(Client clients[], int index, fd_set *master_set)
{
    if (clients[index].fd != -1)
    {
        printf("Client disconnected: fd=%d", clients[index].fd);

        if (clients[index].registered)
        {
            printf(" username=%s", clients[index].username);
        }

        printf("\n");

        close(clients[index].fd);

        FD_CLR(clients[index].fd, master_set);

        clients[index].fd = -1;
        clients[index].registered = 0;
        clients[index].username[0] = '\0';
        clients[index].input_length = 0;
        clients[index].input_buffer[0] = '\0';
    }
}


/* -------------------------------------------------- */
/* Check whether username already exists              */
/* -------------------------------------------------- */

int username_exists(Client clients[],
                    int current_index,
                    const char *username)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (i == current_index)
        {
            continue;
        }

        if (clients[i].fd != -1 &&
            clients[i].registered &&
            strcmp(clients[i].username, username) == 0)
        {
            return 1;
        }
    }

    return 0;
}


/* -------------------------------------------------- */
/* Process one complete command                       */
/* -------------------------------------------------- */

int process_command(Client clients[],
                    int index,
                    const char *command,
                    fd_set *master_set)
{
    Client *client = &clients[index];

    char response[BUFFER_SIZE];

    printf("Client fd=%d sent: %s\n",
           client->fd,
           command);


    /* ================================================= */
    /* REGISTER                                          */
    /* ================================================= */

    if (strncmp(command, "REGISTER ", 9) == 0)
    {
        char username[USERNAME_SIZE];

        memset(username, 0, sizeof(username));

        sscanf(command + 9, "%49s", username);

        if (strlen(username) == 0)
        {
            snprintf(response,
                     sizeof(response),
                     "ERR 004 INVALID_USERNAME %s\n",
                     NID);

            send_response(client->fd, response);

            return 0;
        }


        /* Already registered */

        if (client->registered)
        {
            snprintf(response,
                     sizeof(response),
                     "ERR 003 ALREADY_REGISTERED %s\n",
                     NID);

            send_response(client->fd, response);

            return 0;
        }


        /* Check duplicate username */

        if (username_exists(clients,
                             index,
                             username))
        {
            snprintf(response,
                     sizeof(response),
                     "ERR 001 USERNAME_TAKEN %s\n",
                     NID);

            send_response(client->fd, response);

            return 0;
        }


        /* Register the user */

        strcpy(client->username, username);

        client->registered = 1;

        snprintf(response,
                 sizeof(response),
                 "OK REGISTERED %s %s\n",
                 username,
                 NID);

        send_response(client->fd, response);

        printf("User registered: %s\n",
               username);

        return 0;
    }


    /* ================================================= */
    /* LIST                                               */
    /* ================================================= */

    else if (strcmp(command, "LIST") == 0)
    {
        if (!client->registered)
        {
            snprintf(response,
                     sizeof(response),
                     "ERR 002 NOT_REGISTERED %s\n",
                     NID);

            send_response(client->fd, response);

            return 0;
        }


        strcpy(response, "OK USERS ");

        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (clients[i].fd != -1 &&
                clients[i].registered)
            {
                strcat(response,
                       clients[i].username);

                strcat(response, ",");
            }
        }


        /* Remove final comma */

        size_t length = strlen(response);

        if (length > strlen("OK USERS "))
        {
            response[length - 1] = '\0';
        }

        strcat(response, " ");
        strcat(response, NID);
        strcat(response, "\n");

        send_response(client->fd, response);

        return 0;
    }


    /* ================================================= */
    /* QUIT                                               */
    /* ================================================= */

    else if (strcmp(command, "QUIT") == 0)
    {
        snprintf(response,
                 sizeof(response),
                 "OK BYE %s\n",
                 NID);

        send_response(client->fd, response);

        remove_client(clients,
                      index,
                      master_set);

        return 1;
    }


    /* ================================================= */
    /* Any other command                                  */
    /* ================================================= */

    else
    {
        snprintf(response,
                 sizeof(response),
                 "ERR 006 UNKNOWN_COMMAND %s\n",
                 NID);

        send_response(client->fd, response);

        return 0;
    }
}


/* -------------------------------------------------- */
/* Main                                                */
/* -------------------------------------------------- */

int main(void)
{
    int server_fd;

    struct sockaddr_in server_addr;

    Client clients[MAX_CLIENTS];


    /* ------------------------------------------------ */
    /* Initialise client table                          */
    /* ------------------------------------------------ */

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        clients[i].fd = -1;
        clients[i].registered = 0;
        clients[i].username[0] = '\0';
        clients[i].input_length = 0;
        clients[i].input_buffer[0] = '\0';
    }


    /* ------------------------------------------------ */
    /* Create socket                                    */
    /* ------------------------------------------------ */

    server_fd = socket(AF_INET,
                       SOCK_STREAM,
                       0);

    if (server_fd < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    printf("Server socket created successfully.\n");


    /* ------------------------------------------------ */
    /* Allow quick restart                              */
    /* ------------------------------------------------ */

    int reuse = 1;

    if (setsockopt(server_fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &reuse,
                   sizeof(reuse)) < 0)
    {
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }


    /* ------------------------------------------------ */
    /* Configure server address                         */
    /* ------------------------------------------------ */

    memset(&server_addr,
           0,
           sizeof(server_addr));

    server_addr.sin_family = AF_INET;

    server_addr.sin_addr.s_addr =
        INADDR_ANY;

    server_addr.sin_port =
        htons(PORT);


    /* ------------------------------------------------ */
    /* Bind                                            */
    /* ------------------------------------------------ */

    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server bound to port %d.\n",
           PORT);


    /* ------------------------------------------------ */
    /* Listen                                           */
    /* ------------------------------------------------ */

    if (listen(server_fd, 10) < 0)
    {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server is listening...\n");
    printf("Maximum clients: %d\n",
           MAX_CLIENTS);


    /* ------------------------------------------------ */
    /* Create select() master set                       */
    /* ------------------------------------------------ */

    fd_set master_set;
    fd_set read_set;

    FD_ZERO(&master_set);

    FD_SET(server_fd,
           &master_set);

    int max_fd = server_fd;


    /* ------------------------------------------------ */
    /* Main server loop                                 */
    /* ------------------------------------------------ */

    while (1)
    {
        read_set = master_set;


        /* Wait until something happens */

        int activity =
            select(max_fd + 1,
                   &read_set,
                   NULL,
                   NULL,
                   NULL);

        if (activity < 0)
        {
            perror("select");
            break;
        }


        /* ============================================ */
        /* Check server socket                          */
        /* ============================================ */

        if (FD_ISSET(server_fd,
                     &read_set))
        {
            struct sockaddr_in client_addr;

            socklen_t client_len =
                sizeof(client_addr);

            int client_fd =
                accept(server_fd,
                       (struct sockaddr *)&client_addr,
                       &client_len);

            if (client_fd < 0)
            {
                perror("accept");
            }
            else
            {
                printf("\nNew client connected from %s\n",
                       inet_ntoa(client_addr.sin_addr));

                int slot = -1;

                for (int i = 0;
                     i < MAX_CLIENTS;
                     i++)
                {
                    if (clients[i].fd == -1)
                    {
                        slot = i;
                        break;
                    }
                }


                /* No free slot */

                if (slot == -1)
                {
                    char response[BUFFER_SIZE];

                    snprintf(response,
                             sizeof(response),
                             "ERR 005 SERVER_FULL %s\n",
                             NID);

                    send_response(client_fd,
                                  response);

                    close(client_fd);

                    printf("Connection rejected: server full.\n");
                }
                else
                {
                    clients[slot].fd =
                        client_fd;

                    clients[slot].registered =
                        0;

                    clients[slot].username[0] =
                        '\0';

                    clients[slot].input_length =
                        0;

                    clients[slot].input_buffer[0] =
                        '\0';

                    FD_SET(client_fd,
                           &master_set);

                    if (client_fd > max_fd)
                    {
                        max_fd = client_fd;
                    }

                    printf("Client assigned slot %d, fd=%d\n",
                           slot,
                           client_fd);
                }
            }
        }


        /* ============================================ */
        /* Check existing clients                       */
        /* ============================================ */

        for (int i = 0;
             i < MAX_CLIENTS;
             i++)
        {
            if (clients[i].fd == -1)
            {
                continue;
            }


            int client_fd =
                clients[i].fd;


            if (!FD_ISSET(client_fd,
                          &read_set))
            {
                continue;
            }


            /* ======================================== */
            /* Receive data                              */
            /* ======================================== */

            char temp_buffer[512];

            int bytes_received =
                recv(client_fd,
                     temp_buffer,
                     sizeof(temp_buffer) - 1,
                     0);


            /* ======================================== */
            /* Client disconnected                      */
            /* ======================================== */

            if (bytes_received <= 0)
            {
                remove_client(clients,
                              i,
                              &master_set);

                continue;
            }


            temp_buffer[bytes_received] =
                '\0';


            /* ======================================== */
            /* Add data to client's buffer              */
            /* ======================================== */

            if (clients[i].input_length +
                    bytes_received
                >= BUFFER_SIZE - 1)
            {
                char response[BUFFER_SIZE];

                snprintf(response,
                         sizeof(response),
                         "ERR 007 MESSAGE_TOO_LONG %s\n",
                         NID);

                send_response(client_fd,
                              response);

                clients[i].input_length = 0;

                clients[i].input_buffer[0] =
                    '\0';

                continue;
            }


            memcpy(clients[i].input_buffer +
                       clients[i].input_length,
                   temp_buffer,
                   bytes_received);

            clients[i].input_length +=
                bytes_received;

            clients[i].input_buffer[
                clients[i].input_length] =
                '\0';


            /* ======================================== */
            /* Process complete lines                   */
            /* ======================================== */

            while (1)
            {
                char *newline =
                    strchr(clients[i].input_buffer,
                           '\n');

                if (newline == NULL)
                {
                    break;
                }


                int line_length =
                    newline -
                    clients[i].input_buffer;


                char command[BUFFER_SIZE];


                memcpy(command,
                       clients[i].input_buffer,
                       line_length);

                command[line_length] =
                    '\0';


                /* Remove CR if present */

                if (line_length > 0 &&
                    command[line_length - 1] ==
                        '\r')
                {
                    command[line_length - 1] =
                        '\0';
                }


                /* Remove processed line */

                int remaining =
                    clients[i].input_length -
                    (line_length + 1);


                memmove(clients[i].input_buffer,
                        newline + 1,
                        remaining);


                clients[i].input_length =
                    remaining;

                clients[i].input_buffer[
                    remaining] =
                    '\0';


                /* Process command */

                int disconnected =
                    process_command(clients,
                                    i,
                                    command,
                                    &master_set);

                if (disconnected)
                {
                    break;
                }
            }
        }
    }


    /* ------------------------------------------------ */
    /* Cleanup                                          */
    /* ------------------------------------------------ */

    for (int i = 0;
         i < MAX_CLIENTS;
         i++)
    {
        if (clients[i].fd != -1)
        {
            close(clients[i].fd);
        }
    }

    close(server_fd);

    return 0;
}
