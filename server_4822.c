#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <signal.h>
#include <stdarg.h>

#define PORT 10822
#define NID "NID:5848"

#define MAX_CLIENTS  FD_SETSIZE
#define BUFFER_SIZE 4096
#define USERNAME_SIZE 50
#define ROOM_SIZE 50
#define FILENAME_SIZE 256
#define INPUT_SIZE 8192

#define STORAGE_ROOT "./storage/IT23584822"
#define LOG_FILE "netmsg_IT23584822.log"

typedef struct
{
    int fd;
    int registered;

    char username[USERNAME_SIZE];
    char room[ROOM_SIZE];

    /*
     * Command input buffer.
     *
     * TCP does not guarantee one recv() == one command.
     */
    char input_buffer[INPUT_SIZE];
    size_t input_length;

    /*
     * Incoming file state.
     */
    int receiving_file;
    long file_size;
    long file_received;

    char file_name[FILENAME_SIZE];
    char file_path[1024];

    FILE *file_fp;

    /*
     * Target information for current file.
     */
    char file_target[USERNAME_SIZE];

} Client;

static Client clients[MAX_CLIENTS];

static volatile sig_atomic_t server_running = 1;

static void handle_sigint(int sig)
{
    (void)sig;
    server_running = 0;
}


/* ========================================================= */
/* Utility functions                                          */
/* ========================================================= */

/*
 * Write one line to the log file.
 * Format: [YYYY-MM-DD HH:MM:SS] [LEVEL] text
 */
static void log_write(const char *level,
                      const char *text)
{
    FILE *fp = fopen(LOG_FILE, "a");

    if (fp == NULL)
    {
        return;
    }

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    if (tm_info != NULL)
    {
        char timebuf[64];

        strftime(timebuf,
                 sizeof(timebuf),
                 "%Y-%m-%d %H:%M:%S",
                 tm_info);

        fprintf(fp,
                "[%s] [%s] %s\n",
                timebuf,
                level,
                text);
    }
    else
    {
        fprintf(fp, "[%s] %s\n", level, text);
    }

    fclose(fp);
}


static void log_event(const char *text)
{
    log_write("INFO", text);
}


static void log_error(const char *text)
{
    log_write("ERROR", text);
}


/* printf-style INFO logging */
static void log_info(const char *fmt, ...)
{
    char buf[BUFFER_SIZE];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    log_write("INFO", buf);
}


static int send_all(int fd,
                    const void *data,
                    size_t length)
{
    const char *ptr = data;
    size_t sent = 0;

    while (sent < length)
    {
        ssize_t n = send(fd,
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


static int send_line(int fd,
                     const char *line)
{
    /*
     * Every error reply is also written to the log,
     * so malformed commands, unknown users and unknown
     * rooms are all recorded in one place.
     */
    if (strncmp(line, "ERR ", 4) == 0)
    {
        char text[BUFFER_SIZE];
        const char *who = "unregistered";
        int i;

        for (i = 0; i < MAX_CLIENTS; i++)
        {
            if (clients[i].fd == fd &&
                clients[i].registered)
            {
                who = clients[i].username;
                break;
            }
        }

        snprintf(text, sizeof(text), "%s", line);
        text[strcspn(text, "\r\n")] = '\0';

        {
            char logmsg[BUFFER_SIZE + 128];

            snprintf(logmsg, sizeof(logmsg),
                     "Error sent to %s (fd=%d): %s",
                     who, fd, text);

            log_error(logmsg);
        }
    }

    return send_all(fd,
                    line,
                    strlen(line));
}


static void reset_client(Client *client)
{
    client->registered = 0;

    client->username[0] = '\0';
    client->room[0] = '\0';

    client->input_length = 0;

    client->receiving_file = 0;
    client->file_size = 0;
    client->file_received = 0;

    client->file_name[0] = '\0';
    client->file_path[0] = '\0';
    client->file_target[0] = '\0';

    client->file_fp = NULL;
}


static void initialise_clients(void)
{
    int i;

    for (i = 0; i < MAX_CLIENTS; i++)
    {
        clients[i].fd = -1;
        reset_client(&clients[i]);
    }
}


static Client *find_username(const char *username)
{
    int i;

    for (i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].fd != -1 &&
            clients[i].registered &&
            strcmp(clients[i].username,
                   username) == 0)
        {
            return &clients[i];
        }
    }

    return NULL;
}


static int room_exists(const char *room)
{
    int i;

    for (i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].fd != -1 &&
            clients[i].registered &&
            strcmp(clients[i].room,
                   room) == 0)
        {
            return 1;
        }
    }

    return 0;
}


static int room_member_count(const char *room)
{
    int count = 0;
    int i;

    for (i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].fd != -1 &&
            clients[i].registered &&
            strcmp(clients[i].room,
                   room) == 0)
        {
            count++;
        }
    }

    return count;
}


/* ========================================================= */
/* Broadcast presence                                         */
/* ========================================================= */

static void notify_join(const Client *joined)
{
    char msg[BUFFER_SIZE];

    snprintf(msg,
             sizeof(msg),
             "MSG JOIN %s\n",
             joined->username);

    int i;

    for (i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].fd != -1 &&
            clients[i].registered &&
            &clients[i] != joined)
        {
            send_line(clients[i].fd, msg);
        }
    }
}


static void notify_leave(const char *username)
{
    char msg[BUFFER_SIZE];

    snprintf(msg,
             sizeof(msg),
             "MSG LEAVE %s\n",
             username);

    int i;

    for (i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].fd != -1 &&
            clients[i].registered)
        {
            send_line(clients[i].fd, msg);
        }
    }
}


/* ========================================================= */
/* File target helpers                                        */
/* ========================================================= */

static int target_is_room(const char *target)
{
    return room_exists(target);
}


static int safe_filename(const char *filename)
{
    if (filename == NULL ||
        filename[0] == '\0')
    {
        return 0;
    }

    if (strstr(filename, "..") != NULL)
    {
        return 0;
    }

    if (strchr(filename, '/') != NULL)
    {
        return 0;
    }

    if (strchr(filename, '\\') != NULL)
    {
        return 0;
    }

    return 1;
}


/*
 * Send a file to one receiver.
 *
 * Internal receiver framing:
 *
 * FILE_FROM <sender> <filename> <filesize>\n
 * <exact raw bytes>
 *
 * This header is an implementation detail needed because the
 * assignment specifies the sender->server framing but does not
 * specify a receiver-side file header.
 */
static int send_file_to_client(Client *receiver,
                               const char *sender,
                               const char *filename,
                               const char *path,
                               long filesize)
{
    char header[BUFFER_SIZE];

    int length = snprintf(header,
                          sizeof(header),
                          "FILE_FROM %s %s %ld\n",
                          sender,
                          filename,
                          filesize);

    if (length < 0 ||
        (size_t)length >= sizeof(header))
    {
        return -1;
    }

    if (send_all(receiver->fd,
                 header,
                 (size_t)length) < 0)
    {
        return -1;
    }

    FILE *fp = fopen(path, "rb");

    if (fp == NULL)
    {
        return -1;
    }

    char buffer[BUFFER_SIZE];

    long remaining = filesize;

    while (remaining > 0)
    {
        size_t wanted =
            remaining > BUFFER_SIZE
                ? BUFFER_SIZE
                : (size_t)remaining;

        size_t n = fread(buffer,
                         1,
                         wanted,
                         fp);

        if (n == 0)
        {
            fclose(fp);
            return -1;
        }

        if (send_all(receiver->fd,
                     buffer,
                     n) < 0)
        {
            fclose(fp);
            return -1;
        }

        remaining -= (long)n;
    }

    fclose(fp);

    return 0;
}


static void deliver_file(Client *sender)
{
    /*
     * Sender's file has already been completely received
     * and stored.
     */

    if (target_is_room(sender->file_target))
    {
        int i;

        for (i = 0; i < MAX_CLIENTS; i++)
        {
            if (clients[i].fd != -1 &&
                clients[i].registered &&
                strcmp(clients[i].room,
                       sender->file_target) == 0 &&
                &clients[i] != sender)
            {
                send_file_to_client(
                    &clients[i],
                    sender->username,
                    sender->file_name,
                    sender->file_path,
                    sender->file_size);
            }
        }
    }
    else
    {
        Client *receiver =
            find_username(sender->file_target);

        if (receiver != NULL &&
            receiver != sender)
        {
            send_file_to_client(
                receiver,
                sender->username,
                sender->file_name,
                sender->file_path,
                sender->file_size);
        }
    }
}


/* ========================================================= */
/* Complete incoming file                                     */
/* ========================================================= */

static void finish_file(Client *client)
{
    if (client->file_fp != NULL)
    {
        fclose(client->file_fp);
        client->file_fp = NULL;
    }

    printf("File received from %s: %s (%ld bytes)\n",
           client->username,
           client->file_name,
           client->file_size);

    char logmsg[BUFFER_SIZE];

    snprintf(logmsg,
             sizeof(logmsg),
             "FILE_RECEIVED sender=%s target=%s file=%s size=%ld",
             client->username,
             client->file_target,
             client->file_name,
             client->file_size);

    log_event(logmsg);

    /*
     * Deliver the stored file to target(s).
     */
    deliver_file(client);

    /*
     * Response to sender.
     */
    char response[BUFFER_SIZE];

    snprintf(response,
             sizeof(response),
             "OK FILE_RECEIVED %s %s\n",
             client->file_name,
             NID);

    send_line(client->fd,
              response);

    /*
     * Reset file state.
     */
    client->receiving_file = 0;
    client->file_size = 0;
    client->file_received = 0;

    client->file_name[0] = '\0';
    client->file_path[0] = '\0';
    client->file_target[0] = '\0';
}


/* ========================================================= */
/* Receive file bytes                                         */
/* ========================================================= */

static int receive_file_bytes(Client *client,
                              const char *data,
                              size_t length)
{
    size_t consumed = 0;

    while (consumed < length &&
           client->receiving_file)
    {
        long remaining =
            client->file_size -
            client->file_received;

        if (remaining <= 0)
        {
            finish_file(client);
            break;
        }

        size_t amount =
            remaining < (long)(length - consumed)
                ? (size_t)remaining
                : length - consumed;

        size_t written =
            fwrite(data + consumed,
                   1,
                   amount,
                   client->file_fp);

        if (written != amount)
        {
            fclose(client->file_fp);
            client->file_fp = NULL;

            client->receiving_file = 0;

            send_line(client->fd,
                      "ERR 005 FILE_WRITE_ERROR NID:5848\n");

            return -1;
        }

        client->file_received +=
            (long)written;

        consumed += written;

        if (client->file_received ==
            client->file_size)
        {
            finish_file(client);
        }
    }

    return 0;
}


/* ========================================================= */
/* Process SENDFILE command                                   */
/* ========================================================= */

static int start_file_transfer(Client *client,
                               const char *command)
{
    char target[USERNAME_SIZE];
    char filename[FILENAME_SIZE];
    long filesize;

    memset(target, 0, sizeof(target));
    memset(filename, 0, sizeof(filename));

    filesize = 0;

    int fields =
        sscanf(command,
               "SENDFILE %49s %255s %ld",
               target,
               filename,
               &filesize);

    if (fields != 3 ||
        filesize < 0 ||
        !safe_filename(filename))
    {
        send_line(client->fd,
                  "ERR 006 INVALID_SENDFILE NID:5848\n");

        return 0;
    }

    /*
     * Determine whether target is a user or room.
     */
    Client *target_client =
        find_username(target);

    if (target_client == NULL &&
        !room_exists(target))
    {
        send_line(client->fd,
                  "ERR 002 USER_NOT_FOUND NID:5848\n");

        return 0;
    }

    /*
     * If it is a room, make sure it actually has members.
     */
    if (target_client == NULL &&
        room_member_count(target) == 0)
    {
        send_line(client->fd,
                  "ERR 003 ROOM_NOT_FOUND NID:5848\n");

        return 0;
    }

    /*
     * Create personalised storage directory.
     */
    mkdir("storage", 0755);

    mkdir(STORAGE_ROOT, 0755);

    char sender_directory[512];

    snprintf(sender_directory,
             sizeof(sender_directory),
             "%s/%s",
             STORAGE_ROOT,
             client->username);

    mkdir(sender_directory, 0755);

    char filepath[1024];

    snprintf(filepath,
             sizeof(filepath),
             "%s/%s",
             sender_directory,
             filename);

    FILE *fp =
        fopen(filepath, "wb");

    if (fp == NULL)
    {
        send_line(client->fd,
                  "ERR 005 FILE_WRITE_ERROR NID:5848\n");

        return 0;
    }

    client->file_fp = fp;

    client->receiving_file = 1;

    client->file_size = filesize;
    client->file_received = 0;

    strncpy(client->file_name,
            filename,
            FILENAME_SIZE - 1);

    client->file_name[FILENAME_SIZE - 1] =
        '\0';

    strncpy(client->file_path,
            filepath,
            sizeof(client->file_path) - 1);

    client->file_path[
        sizeof(client->file_path) - 1] =
        '\0';

    strncpy(client->file_target,
            target,
            USERNAME_SIZE - 1);

    client->file_target[
        USERNAME_SIZE - 1] =
        '\0';

    printf("Starting file transfer:\n");
    printf("  Sender : %s\n", client->username);
    printf("  Target : %s\n", target);
    printf("  File   : %s\n", filename);
    printf("  Size   : %ld bytes\n", filesize);

    return 0;
}


/* ========================================================= */
/* Process one complete text command                          */
/* ========================================================= */

static int process_command(Client *client,
                            const char *command,
                            fd_set *master_set)
{
    (void)master_set;   /* not used in this function */

    char response[BUFFER_SIZE];

    printf("fd=%d command=%s\n",
           client->fd,
           command);


    /* ----------------------------------------------------- */
    /* REGISTER                                               */
    /* ----------------------------------------------------- */

    if (strncmp(command,
                "REGISTER ",
                9) == 0)
    {
        char username[USERNAME_SIZE];

        memset(username, 0, sizeof(username));

        if (sscanf(command + 9,
                   "%49s",
                   username) != 1)
        {
            send_line(client->fd,
                      "ERR 004 INVALID_USERNAME NID:5848\n");

            return 0;
        }

        if (client->registered)
        {
            send_line(client->fd,
                      "ERR 003 ALREADY_REGISTERED NID:5848\n");

            return 0;
        }

        if (find_username(username) != NULL)
        {
            send_line(client->fd,
                      "ERR 001 USERNAME_TAKEN NID:5848\n");

            return 0;
        }

        strcpy(client->username,
               username);

        client->registered = 1;

        snprintf(response,
                 sizeof(response),
                 "OK REGISTERED %s %s\n",
                 username,
                 NID);

        send_line(client->fd,
                  response);

        char logmsg[BUFFER_SIZE];

        snprintf(logmsg,
                 sizeof(logmsg),
                 "User registered: %s",
                 username);

        log_event(logmsg);

        notify_join(client);

        return 0;
    }


    /* ----------------------------------------------------- */
    /* All commands after this require registration          */
    /* ----------------------------------------------------- */

    if (!client->registered)
    {
        send_line(client->fd,
                  "ERR 002 NOT_REGISTERED NID:5848\n");

        return 0;
    }


    /* ----------------------------------------------------- */
    /* LIST                                                    */
    /* ----------------------------------------------------- */

    if (strcmp(command,
               "LIST") == 0)
    {
        char users[BUFFER_SIZE];

        strcpy(users,
               "OK USERS ");

        int first = 1;
        int i;

        for (i = 0; i < MAX_CLIENTS; i++)
        {
            if (clients[i].fd != -1 &&
                clients[i].registered)
            {
                if (!first)
                {
                    strncat(users,
                            ",",
                            sizeof(users) -
                            strlen(users) - 1);
                }

                strncat(users,
                        clients[i].username,
                        sizeof(users) -
                        strlen(users) - 1);

                first = 0;
            }
        }

        strncat(users,
                " ",
                sizeof(users) -
                strlen(users) - 1);

        strncat(users,
                NID,
                sizeof(users) -
                strlen(users) - 1);

        strncat(users,
                "\n",
                sizeof(users) -
                strlen(users) - 1);

        send_line(client->fd,
                  users);

        return 0;
    }


    /* ----------------------------------------------------- */
    /* BCAST                                                   */
    /* ----------------------------------------------------- */

    if (strncmp(command,
                "BCAST ",
                6) == 0)
    {
        char message[BUFFER_SIZE];

        strncpy(message,
                command + 6,
                sizeof(message) - 1);

        message[sizeof(message) - 1] = '\0';

        snprintf(response,
                 sizeof(response),
                 "OK SENT %s\n",
                 NID);

        send_line(client->fd,
                  response);

        log_info("BCAST from %s", client->username);

        char msg[BUFFER_SIZE + 256];

        snprintf(msg,
                 sizeof(msg),
                 "MSG BCAST %s %s\n",
                 client->username,
                 message);

        int i;

        for (i = 0; i < MAX_CLIENTS; i++)
        {
            if (clients[i].fd != -1 &&
                clients[i].registered &&
                &clients[i] != client)
            {
                send_line(clients[i].fd,
                          msg);
            }
        }

        return 0;
    }


    /* ----------------------------------------------------- */
    /* PMSG                                                    */
    /* ----------------------------------------------------- */

    if (strncmp(command,
                "PMSG ",
                5) == 0)
    {
        char target[USERNAME_SIZE];
        char message[BUFFER_SIZE];

        memset(target, 0, sizeof(target));
        memset(message, 0, sizeof(message));

        if (sscanf(command,
                   "PMSG %49s %1023[^\n]",
                   target,
                   message) != 2)
        {
            send_line(client->fd,
                      "ERR 006 INVALID_PMSG NID:5848\n");

            return 0;
        }

        Client *receiver =
            find_username(target);

        if (receiver == NULL)
        {
            send_line(client->fd,
                      "ERR 002 USER_NOT_FOUND NID:5848\n");

            return 0;
        }

        snprintf(response,
                 sizeof(response),
                 "OK SENT %s\n",
                 NID);

        send_line(client->fd,
                  response);

        log_info("PMSG from %s to %s", client->username, receiver->username);

        char msg[BUFFER_SIZE + 256];

        snprintf(msg,
                 sizeof(msg),
                 "MSG PRIV %s %s\n",
                 client->username,
                 message);

        send_line(receiver->fd,
                  msg);

        return 0;
    }


    /* ----------------------------------------------------- */
    /* JOIN                                                    */
    /* ----------------------------------------------------- */

    if (strncmp(command,
                "JOIN ",
                5) == 0)
    {
        char room[ROOM_SIZE];

        memset(room, 0, sizeof(room));

        if (sscanf(command + 5,
                   "%49s",
                   room) != 1)
        {
            send_line(client->fd,
                      "ERR 006 INVALID_JOIN NID:5848\n");

            return 0;
        }

        strncpy(client->room,
                room,
                ROOM_SIZE - 1);

        client->room[ROOM_SIZE - 1] = '\0';

        snprintf(response,
                 sizeof(response),
                 "OK JOINED %s %s\n",
                 room,
                 NID);

        send_line(client->fd,
                  response);

        log_info("User %s joined %s", client->username, room);

        return 0;
    }


    /* ----------------------------------------------------- */
    /* LEAVE                                                   */
    /* ----------------------------------------------------- */

    if (strcmp(command, "LEAVE") == 0 ||
        strncmp(command, "LEAVE ", 6) == 0)
    {
        char room[ROOM_SIZE];

        if (sscanf(command,
                   "LEAVE %49s",
                   room) != 1)
        {
            send_line(client->fd,
                      "ERR 003 ROOM_NOT_FOUND NID:5848\n");

            return 0;
        }

        if (strcmp(client->room,
                   room) != 0)
        {
            send_line(client->fd,
                      "ERR 003 ROOM_NOT_FOUND NID:5848\n");

            return 0;
        }

        client->room[0] = '\0';

        snprintf(response,
                 sizeof(response),
                 "OK LEFT %s %s\n",
                 room,
                 NID);

        send_line(client->fd,
                  response);

        log_info("User %s left %s", client->username, room);

        return 0;
    }


    /* ----------------------------------------------------- */
    /* ROOMS                                                   */
    /* ----------------------------------------------------- */

    if (strcmp(command,
               "ROOMS") == 0)
    {
        char rooms[BUFFER_SIZE];

        strcpy(rooms,
               "OK ROOMS ");

        int first = 1;
        int i;
        int j;

        for (i = 0; i < MAX_CLIENTS; i++)
        {
            if (clients[i].fd == -1 ||
                !clients[i].registered ||
                clients[i].room[0] == '\0')
            {
                continue;
            }

            int duplicate = 0;

            for (j = 0; j < i; j++)
            {
                if (clients[j].fd != -1 &&
                    clients[j].registered &&
                    strcmp(clients[j].room,
                           clients[i].room) == 0)
                {
                    duplicate = 1;
                    break;
                }
            }

            if (!duplicate)
            {
                if (!first)
                {
                    strncat(rooms,
                            ",",
                            sizeof(rooms) -
                            strlen(rooms) - 1);
                }

                strncat(rooms,
                        clients[i].room,
                        sizeof(rooms) -
                        strlen(rooms) - 1);

                first = 0;
            }
        }

        strncat(rooms,
                " ",
                sizeof(rooms) -
                strlen(rooms) - 1);

        strncat(rooms,
                NID,
                sizeof(rooms) -
                strlen(rooms) - 1);

        strncat(rooms,
                "\n",
                sizeof(rooms) -
                strlen(rooms) - 1);

        send_line(client->fd,
                  rooms);

        return 0;
    }


    /* ----------------------------------------------------- */
    /* RMSG                                                     */
    /* ----------------------------------------------------- */

    if (strncmp(command,
                "RMSG ",
                5) == 0)
    {
        char room[ROOM_SIZE];
        char message[BUFFER_SIZE];

        memset(room, 0, sizeof(room));
        memset(message, 0, sizeof(message));

        if (sscanf(command,
                   "RMSG %49s %1023[^\n]",
                   room,
                   message) != 2)
        {
            send_line(client->fd,
                      "ERR 006 INVALID_RMSG NID:5848\n");

            return 0;
        }

        if (!room_exists(room))
        {
            send_line(client->fd,
                      "ERR 003 ROOM_NOT_FOUND NID:5848\n");

            return 0;
        }

        if (strcmp(client->room,
                   room) != 0)
        {
            send_line(client->fd,
                      "ERR 003 ROOM_NOT_FOUND NID:5848\n");

            return 0;
        }

        snprintf(response,
                 sizeof(response),
                 "OK SENT %s\n",
                 NID);

        send_line(client->fd,
                  response);

        log_info("RMSG from %s to %s", client->username, room);

        char msg[BUFFER_SIZE + 256];

        snprintf(msg,
                 sizeof(msg),
                 "MSG ROOM %s %s %s\n",
                 room,
                 client->username,
                 message);

        int i;

        for (i = 0; i < MAX_CLIENTS; i++)
        {
            if (clients[i].fd != -1 &&
                clients[i].registered &&
                strcmp(clients[i].room,
                       room) == 0 &&
                &clients[i] != client)
            {
                send_line(clients[i].fd,
                          msg);
            }
        }

        return 0;
    }


    /* ----------------------------------------------------- */
    /* SENDFILE                                                 */
    /* ----------------------------------------------------- */

    if (strncmp(command,
                "SENDFILE ",
                9) == 0)
    {
        return start_file_transfer(client,
                                   command);
    }


    /* ----------------------------------------------------- */
    /* QUIT                                                    */
    /* ----------------------------------------------------- */

    if (strcmp(command,
               "QUIT") == 0)
    {
        send_line(client->fd,
                  "OK BYE NID:5848\n");

        log_info("User %s sent QUIT", client->username);

        return 1;
    }


    /* ----------------------------------------------------- */
    /* Unknown command                                         */
    /* ----------------------------------------------------- */

    send_line(client->fd,
              "ERR 006 UNKNOWN_COMMAND NID:5848\n");

    return 0;
}


/* ========================================================= */
/* Remove client                                               */
/* ========================================================= */

static void remove_client(int index,
                          fd_set *master_set)
{
    Client *client = &clients[index];

    if (client->fd == -1)
    {
        return;
    }

    char username[USERNAME_SIZE];

    strncpy(username,
            client->username,
            sizeof(username) - 1);

    username[sizeof(username) - 1] = '\0';

    if (client->file_fp != NULL)
    {
        fclose(client->file_fp);
        client->file_fp = NULL;
    }

    if (client->registered)
    {
        notify_leave(username);

        char logmsg[BUFFER_SIZE];

        snprintf(logmsg,
                 sizeof(logmsg),
                 "User disconnected: %s",
                 username);

        log_event(logmsg);
    }
    else
    {
        log_event("Unregistered client disconnected");
    }

    FD_CLR(client->fd,
           master_set);

    close(client->fd);

    client->fd = -1;

    reset_client(client);

    printf("Client disconnected: %s\n",
           username);
}


/* ========================================================= */
/* Process data from one client                                */
/* ========================================================= */

static int process_received_data(int index,
                                 const char *data,
                                 size_t length,
                                 fd_set *master_set)
{
    Client *client = &clients[index];

    size_t offset = 0;


    while (offset < length)
    {
        /*
         * If receiving raw file bytes, consume exactly
         * the number of bytes specified by SENDFILE.
         */
        if (client->receiving_file)
        {
            size_t remaining_input =
                length - offset;

            long remaining_file =
                client->file_size -
                client->file_received;

            size_t amount =
                remaining_file <
                (long)remaining_input
                    ? (size_t)remaining_file
                    : remaining_input;

            if (receive_file_bytes(
                    client,
                    data + offset,
                    amount) < 0)
            {
                return 0;
            }

            offset += amount;

            continue;
        }


        /*
         * Otherwise this is command data.
         *
         * Append one byte at a time so partial TCP lines
         * are handled correctly.
         */
        if (client->input_length >=
            sizeof(client->input_buffer) - 1)
        {
            client->input_length = 0;

            send_line(client->fd,
                      "ERR 006 COMMAND_TOO_LONG NID:5848\n");

            continue;
        }

        client->input_buffer[
            client->input_length++] =
            data[offset++];

        client->input_buffer[
            client->input_length] = '\0';


        /*
         * Search for newline.
         */
        char *newline =
            memchr(client->input_buffer,
                   '\n',
                   client->input_length);

        if (newline == NULL)
        {
            continue;
        }


        /*
         * Calculate command length.
         */
        size_t command_length =
            (size_t)(newline -
                     client->input_buffer);

        if (command_length > 0 &&
            client->input_buffer[
                command_length - 1] == '\r')
        {
            command_length--;
        }


        char command[INPUT_SIZE];

        memcpy(command,
               client->input_buffer,
               command_length);

        command[command_length] = '\0';


        /*
         * Remove processed command from buffer.
         */
        size_t remaining =
            client->input_length -
            (size_t)((newline -
                      client->input_buffer) + 1);

        memmove(client->input_buffer,
                newline + 1,
                remaining);

        client->input_length = remaining;

        client->input_buffer[
            remaining] = '\0';


        int should_close =
            process_command(client,
                            command,
                            master_set);

        if (should_close)
        {
            send_line(client->fd,
                      "");

            return 1;
        }
    }

    return 0;
}


/* ========================================================= */
/* Main                                                        */
/* ========================================================= */

int main(void)
{
    int server_fd;

    struct sockaddr_in server_addr;

    fd_set master_set;
    fd_set read_set;

    int max_fd;


    initialise_clients();


    server_fd =
        socket(AF_INET,
               SOCK_STREAM,
               0);

    if (server_fd < 0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }


    int reuse = 1;

    if (setsockopt(server_fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &reuse,
                   sizeof(reuse)) < 0)
    {
        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }


    memset(&server_addr,
           0,
           sizeof(server_addr));

    server_addr.sin_family =
        AF_INET;

    server_addr.sin_addr.s_addr =
        INADDR_ANY;

    server_addr.sin_port =
        htons(PORT);


    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }


    if (listen(server_fd,
               10) < 0)
    {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }


    printf("========================================\n");
    printf("NetMessenger Server\n");
    printf("Registration : IT23584822\n");
    printf("Port         : %d\n", PORT);
    printf("NID          : 5848\n");
    printf("========================================\n");


    /*
     * A client that disconnects while we send() to it must
     * not kill the server with SIGPIPE.
     */
    signal(SIGPIPE, SIG_IGN);

    /*
     * Ctrl+C stops the server cleanly (and logs it).
     * No SA_RESTART, so select() returns EINTR.
     */
    {
        struct sigaction sa;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = handle_sigint;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    {
        FILE *log_file = fopen(LOG_FILE, "a");

        if (log_file == NULL)
        {
            perror("fopen");
        }
        else
        {
            fclose(log_file);
        }
    }

    log_info("Server started on port %d", PORT);


    FD_ZERO(&master_set);

    FD_SET(server_fd,
           &master_set);

    max_fd = server_fd;


    while (server_running)
    {
        read_set = master_set;


        if (select(max_fd + 1,
                   &read_set,
                   NULL,
                   NULL,
                   NULL) < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            perror("select");
            break;
        }


        /*
         * New connection.
         */
        if (FD_ISSET(server_fd,
                     &read_set))
        {
            struct sockaddr_in client_addr;

            socklen_t client_len =
                sizeof(client_addr);

            int new_fd =
                accept(server_fd,
                       (struct sockaddr *)&client_addr,
                       &client_len);

            if (new_fd < 0)
            {
                perror("accept");
            }
            else
            {
                int i;
                int stored = 0;

                for (i = 0;
                     i < MAX_CLIENTS;
                     i++)
                {
                    if (clients[i].fd == -1)
                    {
                        clients[i].fd =
                            new_fd;

                        reset_client(
                            &clients[i]);

                        /*
                         * reset_client doesn't reset fd.
                         */
                        clients[i].fd =
                            new_fd;

                        FD_SET(new_fd,
                               &master_set);

                        if (new_fd > max_fd)
                        {
                            max_fd = new_fd;
                        }

                        stored = 1;

                        printf("New client connected fd=%d\n",
                               new_fd);

                        log_event("Client connected");

                        break;
                    }
                }

                if (!stored)
                {
                    send_line(new_fd,
                              "ERR 006 SERVER_FULL NID:5848\n");

                    close(new_fd);
                }
            }
        }


        /*
         * Existing clients.
         */
        int i;

        for (i = 0;
             i < MAX_CLIENTS;
             i++)
        {
            if (clients[i].fd == -1)
            {
                continue;
            }

            if (!FD_ISSET(clients[i].fd,
                          &read_set))
            {
                continue;
            }


            char buffer[BUFFER_SIZE];

            int fd =
                clients[i].fd;

            ssize_t n =
                recv(fd,
                     buffer,
                     sizeof(buffer),
                     0);

            if (n == 0)
            {
                remove_client(i,
                              &master_set);

                continue;
            }

            if (n < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }

                remove_client(i,
                              &master_set);

                continue;
            }


            int close_client =
                process_received_data(
                    i,
                    buffer,
                    (size_t)n,
                    &master_set);

            if (close_client)
            {
                remove_client(i,
                              &master_set);
            }
        }
    }


    /*
     * Shutdown.
     */
    int i;

    for (i = 0;
         i < MAX_CLIENTS;
         i++)
    {
        if (clients[i].fd != -1)
        {
            remove_client(i,
                          &master_set);
        }
    }

    close(server_fd);

    log_event("Server stopped");

    return 0;
}


