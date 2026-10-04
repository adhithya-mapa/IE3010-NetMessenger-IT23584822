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

int main(void)
{
    int client_fd;

    struct sockaddr_in server_addr;

    char buffer[BUFFER_SIZE];

    /* -------------------------------------------------- */
    /* 1. Create client socket                            */
    /* -------------------------------------------------- */

    client_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (client_fd < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    printf("Client socket created successfully.\n");

    /* -------------------------------------------------- */
    /* 2. Configure server address                        */
    /* -------------------------------------------------- */

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET,
                  "127.0.0.1",
                  &server_addr.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(client_fd);
        exit(EXIT_FAILURE);
    }

    /* -------------------------------------------------- */
    /* 3. Connect to server                               */
    /* -------------------------------------------------- */

    if (connect(client_fd,
                (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0)
    {
        perror("connect");
        close(client_fd);
        exit(EXIT_FAILURE);
    }

    printf("Connected to server on port %d.\n", PORT);

    /* -------------------------------------------------- */
    /* 4. Get username                                    */
    /* -------------------------------------------------- */

    char username[50];

    printf("\nEnter username: ");

    if (fgets(username,
              sizeof(username),
              stdin) == NULL)
    {
        close(client_fd);
        return 1;
    }

    username[strcspn(username, "\r\n")] = '\0';

    /* -------------------------------------------------- */
    /* 5. Send REGISTER command                           */
    /* -------------------------------------------------- */

    snprintf(buffer,
             sizeof(buffer),
             "REGISTER %s",
             username);

    send(client_fd,
         buffer,
         strlen(buffer),
         0);

    printf("Sent: %s\n", buffer);

    /* -------------------------------------------------- */
    /* 6. Receive registration response                   */
    /* -------------------------------------------------- */

    memset(buffer, 0, sizeof(buffer));

    int bytes_received = recv(client_fd,
                              buffer,
                              sizeof(buffer) - 1,
                              0);

    if (bytes_received <= 0)
    {
        printf("Server disconnected.\n");
        close(client_fd);
        return 1;
    }

    buffer[bytes_received] = '\0';

    printf("Server: %s\n", buffer);

    /* -------------------------------------------------- */
    /* 7. Command loop                                    */
    /* -------------------------------------------------- */

    while (1)
    {
        printf("\nEnter command (LIST / QUIT): ");

        memset(buffer, 0, sizeof(buffer));

        if (fgets(buffer,
                  sizeof(buffer),
                  stdin) == NULL)
        {
            break;
        }

        buffer[strcspn(buffer, "\r\n")] = '\0';

        /* Don't send empty commands */
        if (strlen(buffer) == 0)
        {
            continue;
        }

        /* Send command */
        send(client_fd,
             buffer,
             strlen(buffer),
             0);

        printf("Sent: %s\n", buffer);

        /* Receive response */
        memset(buffer, 0, sizeof(buffer));

        bytes_received = recv(client_fd,
                              buffer,
                              sizeof(buffer) - 1,
                              0);

        if (bytes_received <= 0)
        {
            printf("Server disconnected.\n");
            break;
        }

        buffer[bytes_received] = '\0';

        printf("Server: %s\n", buffer);

        /* Stop if QUIT was sent */
        if (strcmp(buffer, "OK BYE NID:5848") == 0)
        {
            break;
        }
    }

    /* -------------------------------------------------- */
    /* 8. Close client socket                             */
    /* -------------------------------------------------- */

    close(client_fd);

    printf("Client closed.\n");

    return 0;
}
