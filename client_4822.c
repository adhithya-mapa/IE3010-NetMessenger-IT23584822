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

    /* 1. Create client socket */
    client_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (client_fd < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    printf("Client socket created successfully.\n");

    /* 2. Configure server address */
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, "127.0.0.1",
                  &server_addr.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(client_fd);
        exit(EXIT_FAILURE);
    }

    /* 3. Connect to server */
    if (connect(client_fd,
                (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0)
    {
        perror("connect");
        close(client_fd);
        exit(EXIT_FAILURE);
    }

    printf("Connected to server on port %d.\n", PORT);

    /* 4. Send a test message */
    const char *message = "Hello from NetMessenger client!";

    send(client_fd,
         message,
         strlen(message),
         0);

    printf("Message sent to server.\n");

    /* 5. Receive server response */
    memset(buffer, 0, sizeof(buffer));

    int bytes_received = recv(client_fd,
                              buffer,
                              sizeof(buffer) - 1,
                              0);

    if (bytes_received < 0)
    {
        perror("recv");
    }
    else
    {
        buffer[bytes_received] = '\0';

        printf("Server says: %s\n", buffer);
    }

    /* 6. Close connection */
    close(client_fd);

    printf("Client closed.\n");

    return 0;
}
