/*
 * NetMessenger client  --  IE3010 Network Programming Assignment 2026
 * Registration number : IT23584822
 * Port                : 6000 + 4822 = 10822
 * Source file         : client_4822.c
 *
 * Usage:  ./client_4822 <username> [server_ip]
 *
 * Single-threaded, uses select() to watch stdin and the socket together.
 * You type raw protocol commands, e.g.:
 *     BCAST hello everyone
 *     PMSG bob hi bob
 *     JOIN lobby
 *     RMSG lobby hello room
 *     SENDFILE bob ./report.pdf        (client builds the real SENDFILE line:
 *                                       SENDFILE bob report.pdf <size> + raw bytes)
 *     QUIT
 * Files received from the server are saved in ./received/<sender>_<filename>.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LAST4 4822
#define PORT  (6000 + LAST4)               /* 10822 */
#define BUF   8192

static int    sock = -1;
static char   rbuf[BUF];
static size_t rlen = 0;

static int send_all(const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(sock, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        off += (size_t)n;
    }
    return 0;
}

/* Read exactly n bytes: buffered leftovers first, then the socket. */
static int read_exact(char *dst, size_t n)
{
    size_t got = 0;
    if (rlen) {
        size_t take = rlen < n ? rlen : n;
        memcpy(dst, rbuf, take);
        memmove(rbuf, rbuf + take, rlen - take);
        rlen -= take;
        got = take;
    }
    while (got < n) {
        ssize_t r = recv(sock, dst + got, n - got, 0);
        if (r == 0) return -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        got += (size_t)r;
    }
    return 0;
}

static void save_file(const char *sender, const char *fname, size_t size)
{
    char *data = malloc(size ? size : 1);
    if (!data) return;
    if (read_exact(data, size) < 0) { free(data); return; }
    mkdir("received", 0755);
    char path[600];
    snprintf(path, sizeof path, "received/%s_%s", sender, fname);
    FILE *f = fopen(path, "wb");
    if (f) { if (size) fwrite(data, 1, size, f); fclose(f); printf("[file saved: %s (%zu bytes)]\n", path, size); }
    else perror("fopen");
    free(data);
}

/* Handle one complete line from the server. */
static void handle_server_line(char *line)
{
    char a[64], b[64], c[256];
    size_t size;
    if (sscanf(line, "MSG FILE %63s %255s %zu", a, c, &size) == 3 && !strncmp(line, "MSG FILE ", 9)) {
        printf("[incoming file from %s: %s, %zu bytes]\n", a, c, size);
        save_file(a, c, size);
    } else if (!strncmp(line, "MSG RFILE ", 10) &&
               sscanf(line, "MSG RFILE %63s %63s %255s %zu", a, b, c, &size) == 4) {
        printf("[incoming file in room %s from %s: %s, %zu bytes]\n", a, b, c, size);
        save_file(b, c, size);
    } else {
        printf("%s\n", line);
    }
    fflush(stdout);
}

/* Returns 0 when the server closed the connection. */
static int pump_socket(void)
{
    ssize_t r = recv(sock, rbuf + rlen, sizeof rbuf - rlen, 0);
    if (r == 0) return 0;
    if (r < 0) return errno == EINTR ? 1 : 0;
    rlen += (size_t)r;

    for (;;) {
        char *nl = memchr(rbuf, '\n', rlen);
        if (!nl) {
            if (rlen == sizeof rbuf) rlen = 0;      /* absurdly long line: drop */
            break;
        }
        size_t n = (size_t)(nl - rbuf);
        char line[BUF];
        memcpy(line, rbuf, n);
        line[n] = '\0';
        if (n && line[n - 1] == '\r') line[n - 1] = '\0';
        memmove(rbuf, nl + 1, rlen - n - 1);
        rlen -= n + 1;
        handle_server_line(line);                   /* may consume more bytes (files) */
    }
    return 1;
}

/* "SENDFILE <target> <path>" typed by the user -> real protocol + raw bytes. */
static int do_sendfile(char *args)
{
    char target[64], path[512];
    if (sscanf(args, "%63s %511s", target, path) != 2) {
        printf("usage: SENDFILE <user|room> <path>\n");
        return 0;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return 0; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return 0; }

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    char hdr[1024];
    int h = snprintf(hdr, sizeof hdr, "SENDFILE %s %s %ld\n", target, base, size);
    if (send_all(hdr, (size_t)h) < 0) { fclose(f); return -1; }

    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        if (send_all(chunk, n) < 0) { fclose(f); return -1; }
    fclose(f);
    printf("[sent %ld bytes of %s]\n", size, base);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <username> [server_ip]\n", argv[0]); return 1; }
    const char *user = argv[1];
    const char *ip = argc > 2 ? argv[2] : "127.0.0.1";

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) { fprintf(stderr, "bad IP address\n"); return 1; }
    if (connect(sock, (struct sockaddr *)&sa, sizeof sa) < 0) { perror("connect"); return 1; }
    printf("Connected to %s:%d as %s. Type protocol commands (QUIT to exit).\n", ip, PORT, user);

    char reg[128];
    int n = snprintf(reg, sizeof reg, "REGISTER %s\n", user);
    if (send_all(reg, (size_t)n) < 0) { perror("send"); return 1; }

    int stdin_open = 1;
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(sock, &rd);
        if (stdin_open) FD_SET(STDIN_FILENO, &rd);
        if (select(sock + 1, &rd, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (FD_ISSET(sock, &rd)) {
            if (!pump_socket()) { printf("[server closed the connection]\n"); break; }
        }
        if (stdin_open && FD_ISSET(STDIN_FILENO, &rd)) {
            char line[4096];
            if (!fgets(line, sizeof line, stdin)) { stdin_open = 0; shutdown(sock, SHUT_WR); continue; }
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
            if (!len) continue;
            if (!strncasecmp(line, "SENDFILE ", 9)) {
                if (do_sendfile(line + 9) < 0) { printf("[send failed]\n"); break; }
                continue;
            }
            line[len++] = '\n';
            if (send_all(line, len) < 0) { perror("send"); break; }
        }
    }
    close(sock);
    return 0;
}
