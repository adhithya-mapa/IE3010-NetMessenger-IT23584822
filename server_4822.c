/*
 * NetMessenger server  --  IE3010 Network Programming Assignment 2026
 * Registration number : IT23584822
 * Port                : 6000 + 4822 = 10822
 * NID tag             : NID:5848   (digits 3-6 of 23584822)
 * Log file            : netmsg_IT23584822.log
 * Storage path        : ./storage/IT23584822/<sender_username>/<filename>
 *
 * Concurrency model   : one POSIX thread per client (detached), a single
 *                       global mutex (state_lock) protecting users/rooms, and
 *                       one send mutex per client so that two threads never
 *                       interleave bytes on the same socket.
 *
 * Lock order (always): state_lock  ->  client.send_lock  ->  log_lock
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---------- personalisation (derived from IT23584822) ---------- */
#define REG_NO      "IT23584822"
#define LAST4       4822
#define PORT        (6000 + LAST4)              /* 10822 */
#define NID_TAG     "NID:5848"
#define LOG_FILE    "netmsg_" REG_NO ".log"
#define STORAGE_DIR "./storage/" REG_NO

/* ---------- limits ---------- */
#define MAX_CLIENTS   64
#define MAX_ROOMS     64
#define MAX_NAME      32
#define MAX_FNAME     200
#define LINE_BUF      4096
#define OUT_BUF       (LINE_BUF + 512)
#define MAX_FILE_SIZE (10UL * 1024 * 1024)      /* 10 MB */
#define DRAIN_LIMIT   (1UL << 30)               /* 1 GB: above this we drop the connection */
#define SEND_TIMEOUT_S 5

typedef struct {
    int  fd;
    int  active;
    int  registered;
    char name[MAX_NAME + 1];
    char ip[INET_ADDRSTRLEN];
    int  port;
    unsigned char in_room[MAX_ROOMS];
    pthread_mutex_t send_lock;
    char   rbuf[LINE_BUF];       /* receive buffer (framing) */
    size_t rlen;
    int    discard;              /* discarding an over-long line */
} Client;

typedef struct {
    int  active;
    char name[MAX_NAME + 1];
} Room;

static Client clients[MAX_CLIENTS];
static Room   rooms[MAX_ROOMS];
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_lock   = PTHREAD_MUTEX_INITIALIZER;
static FILE *logfp = NULL;
static volatile sig_atomic_t g_stop = 0;
static int listen_fd = -1;

/* ======================= logging ======================= */
static void log_event(const char *fmt, ...)
{
    char ts[32], msg[2048];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&log_lock);
    if (logfp) { fprintf(logfp, "[%s] %s\n", ts, msg); fflush(logfp); }
    printf("[%s] %s\n", ts, msg);
    fflush(stdout);
    pthread_mutex_unlock(&log_lock);
}

/* ======================= low level I/O ======================= */
static int send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* Send raw bytes to a client, serialised by that client's send lock. */
static int send_raw(Client *c, const char *hdr, size_t hlen, const char *data, size_t dlen)
{
    int rc = 0;
    pthread_mutex_lock(&c->send_lock);
    if (hlen && send_all(c->fd, hdr, hlen) < 0) rc = -1;
    if (!rc && dlen && send_all(c->fd, data, dlen) < 0) rc = -1;
    pthread_mutex_unlock(&c->send_lock);
    if (rc) shutdown(c->fd, SHUT_RDWR);   /* wake its thread so it cleans up */
    return rc;
}

/* Forward a "MSG ..." line (no NID tag on forwarded lines). */
static void send_msg(Client *c, const char *fmt, ...)
{
    char buf[OUT_BUF];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n > sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    send_raw(c, buf, (size_t)n, NULL, 0);
}

/* Reply to the requesting client: every OK/ERR line ends with " NID:5848". */
static void reply(Client *c, const char *fmt, ...)
{
    char buf[OUT_BUF];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 16, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n > sizeof buf - 16) n = (int)sizeof buf - 16;
    n += snprintf(buf + n, sizeof buf - (size_t)n, " %s\n", NID_TAG);
    send_raw(c, buf, (size_t)n, NULL, 0);
}

/*
 * Framing: returns line length (>=0), -1 on EOF/error, -2 if the line was
 * too long (it has been fully discarded). Handles partial lines and several
 * lines per recv() because leftover bytes stay in c->rbuf.
 */
static int read_line(Client *c, char *out, size_t outsz)
{
    for (;;) {
        char *nl = memchr(c->rbuf, '\n', c->rlen);
        if (nl) {
            size_t n = (size_t)(nl - c->rbuf);
            int overflow = c->discard || n >= outsz;
            if (!overflow) {
                memcpy(out, c->rbuf, n);
                out[n] = '\0';
                if (n && out[n - 1] == '\r') out[--n] = '\0';
            }
            size_t used = (size_t)(nl - c->rbuf) + 1;   /* bytes consumed incl. '\n' */
            memmove(c->rbuf, c->rbuf + used, c->rlen - used);
            c->rlen -= used;
            c->discard = 0;
            return overflow ? -2 : (int)strlen(out);
        }
        if (c->rlen == sizeof c->rbuf) { c->rlen = 0; c->discard = 1; }
        ssize_t r = recv(c->fd, c->rbuf + c->rlen, sizeof c->rbuf - c->rlen, 0);
        if (r == 0) return -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        c->rlen += (size_t)r;
    }
}

/* Read exactly n raw bytes (first from the framing buffer, then the socket). */
static int read_bytes(Client *c, char *dst, size_t n)
{
    size_t got = 0;
    if (c->rlen) {
        size_t take = c->rlen < n ? c->rlen : n;
        if (dst) memcpy(dst, c->rbuf, take);
        memmove(c->rbuf, c->rbuf + take, c->rlen - take);
        c->rlen -= take;
        got = take;
    }
    while (got < n) {
        char tmp[8192];                       /* used when discarding (dst == NULL) */
        size_t want = n - got;
        char *where;
        if (dst) where = dst + got;
        else { where = tmp; if (want > sizeof tmp) want = sizeof tmp; }
        ssize_t r = recv(c->fd, where, want, 0);
        if (r == 0) return -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        got += (size_t)r;
    }
    return 0;
}

/* ======================= parsing helpers ======================= */
static char *skip_sp(char *p) { while (*p == ' ') p++; return p; }

/* Returns next space-delimited token and advances *pp past it (NULL if none). */
static char *next_token(char **pp)
{
    char *p = skip_sp(*pp);
    if (!*p) { *pp = p; return NULL; }
    char *tok = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = '\0';
    *pp = p;
    return tok;
}

static int valid_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > MAX_NAME) return 0;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '-')) return 0;
    return 1;
}

static int valid_filename(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > MAX_FNAME) return 0;
    if (!strcmp(s, ".") || !strcmp(s, "..")) return 0;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '/' || ch == '\\' || ch < 32 || ch == 127) return 0;
    }
    return 1;
}

/* ---- state lookups: caller must hold state_lock ---- */
static Client *find_user(const char *name)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active && clients[i].registered && !strcasecmp(clients[i].name, name))
            return &clients[i];
    return NULL;
}

static int find_room(const char *name)
{
    for (int i = 0; i < MAX_ROOMS; i++)
        if (rooms[i].active && !strcmp(rooms[i].name, name)) return i;
    return -1;
}

static int room_members(int r)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active && clients[i].in_room[r]) n++;
    return n;
}

/* ======================= command handlers ======================= */
static void cmd_register(Client *c, char *args)
{
    char *name = next_token(&args);
    if (c->registered) { reply(c, "ERR 011 ALREADY_REGISTERED"); return; }
    if (!name || *skip_sp(args)) { reply(c, "ERR 012 BAD_SYNTAX"); return; }
    if (!valid_name(name)) { reply(c, "ERR 006 INVALID_USERNAME"); return; }

    pthread_mutex_lock(&state_lock);
    if (find_user(name)) {
        pthread_mutex_unlock(&state_lock);
        reply(c, "ERR 001 USERNAME_TAKEN");
        log_event("REGISTER rejected (username taken) name=%s from %s:%d", name, c->ip, c->port);
        return;
    }
    snprintf(c->name, sizeof c->name, "%s", name);
    c->registered = 1;
    pthread_mutex_unlock(&state_lock);

    reply(c, "OK REGISTERED %s", name);
    log_event("REGISTER user=%s from %s:%d", name, c->ip, c->port);

    pthread_mutex_lock(&state_lock);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active && clients[i].registered && &clients[i] != c)
            send_msg(&clients[i], "MSG SYS USER_JOINED %s", name);
    pthread_mutex_unlock(&state_lock);
}

static void cmd_list(Client *c)
{
    char buf[OUT_BUF - 64];
    size_t off = 0;
    buf[0] = '\0';
    pthread_mutex_lock(&state_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].registered) {
            off += (size_t)snprintf(buf + off, sizeof buf - off, "%s%s", off ? "," : "", clients[i].name);
            if (off >= sizeof buf) break;
        }
    }
    pthread_mutex_unlock(&state_lock);
    reply(c, "OK USERS %s", buf);
}

static void cmd_bcast(Client *c, char *args)
{
    char *msg = skip_sp(args);
    if (!*msg) { reply(c, "ERR 012 BAD_SYNTAX"); return; }
    int delivered = 0;
    pthread_mutex_lock(&state_lock);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active && clients[i].registered && &clients[i] != c) {
            send_msg(&clients[i], "MSG BCAST %s %s", c->name, msg);
            delivered++;
        }
    pthread_mutex_unlock(&state_lock);
    reply(c, "OK SENT");
    log_event("BCAST from=%s recipients=%d bytes=%zu", c->name, delivered, strlen(msg));
}

static void cmd_pmsg(Client *c, char *args)
{
    char *target = next_token(&args);
    char *msg = skip_sp(args);
    if (!target || !*msg) { reply(c, "ERR 012 BAD_SYNTAX"); return; }
    pthread_mutex_lock(&state_lock);
    Client *t = find_user(target);
    if (t) send_msg(t, "MSG PRIV %s %s", c->name, msg);
    pthread_mutex_unlock(&state_lock);
    if (!t) {
        reply(c, "ERR 002 USER_NOT_FOUND");
        log_event("PMSG failed from=%s to=%s (user not found)", c->name, target);
        return;
    }
    reply(c, "OK SENT");
    log_event("PMSG from=%s to=%s bytes=%zu", c->name, target, strlen(msg));
}

static void cmd_join(Client *c, char *args)
{
    char *room = next_token(&args);
    if (!room || *skip_sp(args)) { reply(c, "ERR 012 BAD_SYNTAX"); return; }
    if (!valid_name(room)) { reply(c, "ERR 017 INVALID_ROOM_NAME"); return; }

    pthread_mutex_lock(&state_lock);
    int r = find_room(room);
    int created = 0;
    if (r < 0) {
        for (int i = 0; i < MAX_ROOMS; i++)
            if (!rooms[i].active) {
                rooms[i].active = 1;
                snprintf(rooms[i].name, sizeof rooms[i].name, "%s", room);
                r = i; created = 1;
                break;
            }
    }
    if (r < 0) {
        pthread_mutex_unlock(&state_lock);
        reply(c, "ERR 015 TOO_MANY_ROOMS");
        return;
    }
    c->in_room[r] = 1;
    pthread_mutex_unlock(&state_lock);
    reply(c, "OK JOINED %s", room);
    log_event("JOIN user=%s room=%s%s", c->name, room, created ? " (created)" : "");
}

static void remove_room_if_empty(int r)   /* state_lock held */
{
    if (rooms[r].active && room_members(r) == 0) rooms[r].active = 0;
}

static void cmd_leave(Client *c, char *args)
{
    char *room = next_token(&args);
    if (!room || *skip_sp(args)) { reply(c, "ERR 012 BAD_SYNTAX"); return; }
    pthread_mutex_lock(&state_lock);
    int r = find_room(room);
    int code = 0;                         /* 0 ok, 1 no room, 2 not member */
    if (r < 0) code = 1;
    else if (!c->in_room[r]) code = 2;
    else { c->in_room[r] = 0; remove_room_if_empty(r); }
    pthread_mutex_unlock(&state_lock);
    if (code == 1) { reply(c, "ERR 003 ROOM_NOT_FOUND"); return; }
    if (code == 2) { reply(c, "ERR 007 NOT_IN_ROOM"); return; }
    reply(c, "OK LEFT %s", room);
    log_event("LEAVE user=%s room=%s", c->name, room);
}

static void cmd_rooms(Client *c)
{
    char buf[OUT_BUF - 64];
    size_t off = 0;
    buf[0] = '\0';
    pthread_mutex_lock(&state_lock);
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms[i].active) {
            off += (size_t)snprintf(buf + off, sizeof buf - off, "%s%s", off ? "," : "", rooms[i].name);
            if (off >= sizeof buf) break;
        }
    }
    pthread_mutex_unlock(&state_lock);
    if (off) reply(c, "OK ROOMS %s", buf);
    else     reply(c, "OK ROOMS");
}

static void cmd_rmsg(Client *c, char *args)
{
    char *room = next_token(&args);
    char *msg = skip_sp(args);
    if (!room || !*msg) { reply(c, "ERR 012 BAD_SYNTAX"); return; }
    int code = 0, delivered = 0;
    pthread_mutex_lock(&state_lock);
    int r = find_room(room);
    if (r < 0) code = 1;
    else if (!c->in_room[r]) code = 2;
    else {
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].active && clients[i].in_room[r] && &clients[i] != c) {
                send_msg(&clients[i], "MSG ROOM %s %s %s", room, c->name, msg);
                delivered++;
            }
    }
    pthread_mutex_unlock(&state_lock);
    if (code == 1) { reply(c, "ERR 003 ROOM_NOT_FOUND"); log_event("RMSG failed from=%s room=%s (not found)", c->name, room); return; }
    if (code == 2) { reply(c, "ERR 007 NOT_IN_ROOM"); return; }
    reply(c, "OK SENT");
    log_event("RMSG from=%s room=%s recipients=%d bytes=%zu", c->name, room, delivered, strlen(msg));
}

static int mkdir_p_user(const char *user)
{
    char path[512];
    mkdir("./storage", 0755);
    mkdir(STORAGE_DIR, 0755);
    snprintf(path, sizeof path, "%s/%s", STORAGE_DIR, user);
    if (mkdir(path, 0755) < 0 && errno != EEXIST) return -1;
    return 0;
}

static int store_file(const char *user, const char *fname, const char *data, size_t size)
{
    char path[1024];
    if (mkdir_p_user(user) < 0) return -1;
    snprintf(path, sizeof path, "%s/%s/%s", STORAGE_DIR, user, fname);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = (size == 0 || fwrite(data, 1, size, f) == size);
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

/* returns 1 to keep the connection, 0 to drop it */
static int cmd_sendfile(Client *c, char *args)
{
    char *target = next_token(&args);
    char *fname  = next_token(&args);
    char *sztok  = next_token(&args);
    if (!target || !fname || !sztok || *skip_sp(args)) { reply(c, "ERR 012 BAD_SYNTAX"); return 1; }
    for (char *p = sztok; *p; p++)
        if (!isdigit((unsigned char)*p)) { reply(c, "ERR 012 BAD_SYNTAX"); return 1; }
    if (strlen(sztok) > 12) { reply(c, "ERR 004 FILE_TOO_LARGE"); return 0; }
    unsigned long size = strtoul(sztok, NULL, 10);

    /* Too large: tell the client, then discard the raw bytes to stay in sync. */
    if (size > MAX_FILE_SIZE) {
        log_event("SENDFILE rejected from=%s file=%s size=%lu (too large)", c->registered ? c->name : "?", fname, size);
        reply(c, "ERR 004 FILE_TOO_LARGE");
        if (size > DRAIN_LIMIT) return 0;
        return read_bytes(c, NULL, size) == 0;
    }

    char *data = malloc(size ? size : 1);
    if (!data) { reply(c, "ERR 018 SERVER_ERROR"); return read_bytes(c, NULL, size) == 0; }
    if (read_bytes(c, data, size) < 0) { free(data); return 0; }   /* client vanished mid-transfer */

    /* All <filesize> bytes are consumed; now validate and answer. */
    if (!c->registered) { reply(c, "ERR 010 NOT_REGISTERED"); free(data); return 1; }
    if (!valid_filename(fname)) { reply(c, "ERR 005 BAD_FILENAME"); free(data); return 1; }

    pthread_mutex_lock(&state_lock);
    Client *t = find_user(target);
    int r = t ? -1 : find_room(target);
    int code = 0;                                  /* 0 ok, 2 user, 3 room, 7 not member */
    if (!t && r < 0) code = 2;
    else if (!t && !c->in_room[r]) code = 7;
    pthread_mutex_unlock(&state_lock);

    if (code == 2) {
        /* error code depends on whether the sender meant a user or a room; we
           cannot know, so USER_NOT_FOUND is used (spec allows ERR 002/003). */
        reply(c, "ERR 002 USER_NOT_FOUND");
        log_event("SENDFILE failed from=%s target=%s file=%s (target not found)", c->name, target, fname);
        free(data);
        return 1;
    }
    if (code == 7) { reply(c, "ERR 007 NOT_IN_ROOM"); free(data); return 1; }

    if (store_file(c->name, fname, data, size) < 0) {
        reply(c, "ERR 013 STORAGE_ERROR");
        log_event("SENDFILE storage error from=%s file=%s: %s", c->name, fname, strerror(errno));
        free(data);
        return 1;
    }

    char hdr[OUT_BUF];
    int delivered = 0;
    pthread_mutex_lock(&state_lock);
    t = find_user(target);
    if (t) {
        int h = snprintf(hdr, sizeof hdr, "MSG FILE %s %s %lu\n", c->name, fname, size);
        if (send_raw(t, hdr, (size_t)h, data, size) == 0) delivered++;
    } else {
        r = find_room(target);
        if (r >= 0) {
            int h = snprintf(hdr, sizeof hdr, "MSG RFILE %s %s %s %lu\n", target, c->name, fname, size);
            for (int i = 0; i < MAX_CLIENTS; i++)
                if (clients[i].active && clients[i].in_room[r] && &clients[i] != c)
                    if (send_raw(&clients[i], hdr, (size_t)h, data, size) == 0) delivered++;
        }
    }
    pthread_mutex_unlock(&state_lock);

    reply(c, "OK FILE_RECEIVED %s", fname);
    log_event("SENDFILE from=%s target=%s file=%s size=%lu stored=%s/%s/%s delivered_to=%d",
              c->name, target, fname, size, STORAGE_DIR, c->name, fname, delivered);
    free(data);
    return 1;
}

/* returns 0 when the connection should be closed */
static int handle_line(Client *c, char *line)
{
    char *p = line;
    char *cmd = next_token(&p);
    if (!cmd) return 1;

    if (!strcasecmp(cmd, "QUIT")) { reply(c, "OK BYE"); return 0; }
    if (!strcasecmp(cmd, "REGISTER")) { cmd_register(c, p); return 1; }
    if (!strcasecmp(cmd, "SENDFILE")) return cmd_sendfile(c, p);

    int known = !strcasecmp(cmd, "LIST") || !strcasecmp(cmd, "BCAST") || !strcasecmp(cmd, "PMSG") ||
                !strcasecmp(cmd, "JOIN") || !strcasecmp(cmd, "LEAVE") || !strcasecmp(cmd, "ROOMS") ||
                !strcasecmp(cmd, "RMSG");
    if (!known) {
        reply(c, "ERR 009 UNKNOWN_COMMAND");
        log_event("Unknown command from %s:%d: \"%.40s\"", c->ip, c->port, cmd);
        return 1;
    }
    if (!c->registered) { reply(c, "ERR 010 NOT_REGISTERED"); return 1; }

    if (!strcasecmp(cmd, "LIST"))  cmd_list(c);
    else if (!strcasecmp(cmd, "BCAST")) cmd_bcast(c, p);
    else if (!strcasecmp(cmd, "PMSG"))  cmd_pmsg(c, p);
    else if (!strcasecmp(cmd, "JOIN"))  cmd_join(c, p);
    else if (!strcasecmp(cmd, "LEAVE")) cmd_leave(c, p);
    else if (!strcasecmp(cmd, "ROOMS")) cmd_rooms(c);
    else if (!strcasecmp(cmd, "RMSG"))  cmd_rmsg(c, p);
    return 1;
}

/* ======================= per-client thread ======================= */
static void cleanup_client(Client *c, const char *why)
{
    char name[MAX_NAME + 1] = "";
    char ip[INET_ADDRSTRLEN];
    int was_reg, fd, port;

    pthread_mutex_lock(&state_lock);
    /* Copy everything we still need BEFORE releasing the slot: once active==0
       the accept loop may reuse this Client struct for a new connection. */
    fd = c->fd;
    port = c->port;
    snprintf(ip, sizeof ip, "%s", c->ip);
    was_reg = c->registered;
    snprintf(name, sizeof name, "%s", c->name);
    for (int r = 0; r < MAX_ROOMS; r++)
        if (c->in_room[r]) { c->in_room[r] = 0; remove_room_if_empty(r); }
    c->active = 0;                       /* nobody can pick this slot as a recipient now */
    c->registered = 0;
    if (was_reg)
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].active && clients[i].registered)
                send_msg(&clients[i], "MSG SYS USER_LEFT %s", name);
    pthread_mutex_unlock(&state_lock);

    close(fd);
    log_event("DISCONNECT %s:%d user=%s (%s)", ip, port, was_reg ? name : "-", why);
}

static void *client_thread(void *arg)
{
    Client *c = arg;
    char line[LINE_BUF];
    const char *why = "client closed connection";

    for (;;) {
        int n = read_line(c, line, sizeof line);
        if (n == -1) { why = "connection lost / client closed"; break; }
        if (n == -2) { reply(c, "ERR 016 LINE_TOO_LONG"); log_event("Over-long line from %s:%d discarded", c->ip, c->port); continue; }
        if (n == 0) continue;
        if (!handle_line(c, line)) { why = "QUIT or protocol close"; break; }
    }
    cleanup_client(c, why);
    return NULL;
}

/* ======================= main ======================= */
static void on_signal(int sig) { (void)sig; g_stop = 1; }

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;               /* no SA_RESTART: accept() must return EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    logfp = fopen(LOG_FILE, "a");
    if (!logfp) perror("warning: cannot open log file");

    mkdir("./storage", 0755);
    mkdir(STORAGE_DIR, 0755);

    for (int i = 0; i < MAX_CLIENTS; i++) pthread_mutex_init(&clients[i].send_lock, NULL);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(listen_fd, 16) < 0) { perror("listen"); return 1; }

    log_event("SERVER START reg=%s port=%d nid=%s log=%s storage=%s", REG_NO, PORT, NID_TAG, LOG_FILE, STORAGE_DIR);

    while (!g_stop) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof ca;
        int fd = accept(listen_fd, (struct sockaddr *)&ca, &cl);
        if (fd < 0) { if (errno == EINTR) continue; perror("accept"); continue; }

        struct timeval tv = { SEND_TIMEOUT_S, 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof yes);

        pthread_mutex_lock(&state_lock);
        Client *c = NULL;
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (!clients[i].active) { c = &clients[i]; break; }
        if (c) {
            c->fd = fd; c->registered = 0; c->name[0] = '\0';
            c->rlen = 0; c->discard = 0;
            memset(c->in_room, 0, sizeof c->in_room);
            inet_ntop(AF_INET, &ca.sin_addr, c->ip, sizeof c->ip);
            c->port = ntohs(ca.sin_port);
            c->active = 1;
        }
        pthread_mutex_unlock(&state_lock);

        if (!c) {
            const char *msg = "ERR 014 SERVER_FULL " NID_TAG "\n";
            send_all(fd, msg, strlen(msg));
            close(fd);
            log_event("Connection refused: server full");
            continue;
        }
        log_event("CONNECT %s:%d", c->ip, c->port);

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tid, &attr, client_thread, c) != 0) {
            log_event("pthread_create failed");
            pthread_mutex_lock(&state_lock);
            c->active = 0;
            pthread_mutex_unlock(&state_lock);
            close(fd);
        }
        pthread_attr_destroy(&attr);
    }

    log_event("SERVER STOP");
    close(listen_fd);
    if (logfp) fclose(logfp);
    return 0;
}
