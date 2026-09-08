/* monitor_server.c - Stage 2: real protocol commands + /dev/vmonitor +
 *                     WATCH / EVT SAMPLE live broadcast
 *
 * Protocol (line-based ASCII, one command per line):
 *   <id> GET                 -> RSP <id> OK running=... period_ms=... ...
 *   <id> START                -> RSP <id> OK
 *   <id> STOP                 -> RSP <id> OK
 *   <id> PERIOD <ms>           -> RSP <id> OK
 *   <id> THRESHOLD <mC>         -> RSP <id> OK
 *   <id> INJECT <mC>            -> RSP <id> OK
 *   <id> WATCH <0|1>            -> RSP <id> OK  (then EVT SAMPLE ... follows
 *                                    for every new sample, until WATCH 0)
 * Unknown command / bad argument -> RSP <id> ERR <reason>
 *
 * Live stream to WATCH-ing clients, once per new sample read from the
 * driver's queue:
 *   EVT SAMPLE <seq> <timestamp_ns> <value_mC> <alarm>
 *
 * Still single-threaded epoll, no thread/fork. UDP telemetry is a later
 * task, not implemented here.
 *
 * Build: gcc -Wall -Wextra -O2 -I../include -o monitor_server monitor_server.c
 * Run:   sudo ./monitor_server [port]      (root needed: /dev/vmonitor +
 *                                            sysfs writes require it)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "vmonitor_uapi.h"

#define DEFAULT_PORT       5000
#define MAX_EVENTS         32
#define RX_BUFFER_SIZE     4096
#define MAX_LINE_LEN       1024

#define DEV_PATH           "/dev/vmonitor"
#define SYSFS_PERIOD_MS    "/sys/class/vmonitor/vmonitor/period_ms"
#define SYSFS_THRESHOLD_MC "/sys/class/vmonitor/vmonitor/threshold_mC"

/* Sentinel values distinguishing which fd an epoll event belongs to.
 * NULL = the listening socket, TAG_DEVICE = /dev/vmonitor, anything
 * else = a pointer to a struct client. */
#define TAG_DEVICE ((void *)1)

#define TX_BUFFER_SIZE     (64 * 1024)  /* per-client outbound buffer cap */

struct client {
    int fd;
    char rx_buf[RX_BUFFER_SIZE];
    size_t rx_len;
    char tx_buf[TX_BUFFER_SIZE];
    size_t tx_len;          /* bytes queued, not yet sent */
    int tx_epollout_armed;  /* whether we've told epoll to watch for writability */
    int pending_disconnect; /* set when this client must be dropped (slow/error) */
    int watching;          /* WATCH 1 was requested */
    struct client *next;   /* intrusive singly linked list, for broadcast */
};

static volatile sig_atomic_t g_stop = 0;
static struct client *g_clients_head = NULL;
static int g_dev_fd = -1;
static int g_epfd = -1;

static void handle_sigint(int sig) { (void)sig; g_stop = 1; }

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---- Client list management ---- */

static struct client *client_new(int fd)
{
    struct client *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->fd = fd;
    c->next = g_clients_head;
    g_clients_head = c;
    return c;
}

static void client_remove(struct client *target)
{
    struct client **pp = &g_clients_head;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;
            close(target->fd);
            free(target);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* Tells epoll whether we currently care about EPOLLOUT (writability) for
 * this client. Only armed while tx_buf has pending bytes -- otherwise
 * epoll would keep firing "the socket is writable" forever for no
 * reason, wasting CPU. */
static void update_epollout_interest(struct client *c)
{
    int want_out = (c->tx_len > 0);
    if (want_out == c->tx_epollout_armed)
        return; /* no change needed */

    struct epoll_event ev;
    ev.events = EPOLLIN | (want_out ? EPOLLOUT : 0);
    ev.data.ptr = c;
    if (epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev) == 0)
        c->tx_epollout_armed = want_out;
}

/* Appends data to a client's tx_buf. If it would overflow the 64 KiB cap,
 * this client is too slow to keep up with what we're sending it -- mark
 * it for disconnection (the "drop the slow client" policy) instead of
 * growing memory unboundedly. */
static void tx_buffer_append(struct client *c, const char *data, size_t len)
{
    if (c->tx_len + len > TX_BUFFER_SIZE) {
        fprintf(stderr, "[client fd=%d] tx buffer would overflow (slow client), disconnecting\n",
                c->fd);
        c->pending_disconnect = 1;
        return;
    }
    memcpy(c->tx_buf + c->tx_len, data, len);
    c->tx_len += len;
    update_epollout_interest(c);
}

/* Attempts to send everything currently queued in c->tx_buf. Called both
 * right after queuing new data (to try to flush immediately) and when
 * epoll reports EPOLLOUT (socket became writable again after EAGAIN). */
static void tx_buffer_flush(struct client *c)
{
    while (c->tx_len > 0) {
        ssize_t sent = send(c->fd, c->tx_buf, c->tx_len, MSG_NOSIGNAL);
        if (sent > 0) {
            memmove(c->tx_buf, c->tx_buf + sent, c->tx_len - sent);
            c->tx_len -= (size_t)sent;
            continue;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break; /* socket full for now, wait for the next EPOLLOUT */
        } else {
            if (sent < 0)
                perror("send (flush)");
            c->pending_disconnect = 1;
            return;
        }
    }
    update_epollout_interest(c);
}

/* Send a formatted line to one client. If the socket can take it
 * immediately, it goes out right away with no copy. Otherwise (partial
 * send / EAGAIN) the remainder is queued in tx_buf and retried when
 * epoll reports EPOLLOUT. If tx_buf would overflow (client not draining
 * fast enough), the client is marked for disconnection. */
static void client_send(struct client *c, const char *fmt, ...)
{
    char buf[MAX_LINE_LEN + 64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;

    /* If there's already backlog queued, preserve ordering: append behind
     * it rather than trying to send this message ahead of older ones. */
    if (c->tx_len > 0) {
        tx_buffer_append(c, buf, (size_t)n);
        return;
    }

    ssize_t sent = send(c->fd, buf, (size_t)n, MSG_NOSIGNAL);
    if (sent == n) {
        return; /* common case: it all went out immediately */
    } else if (sent >= 0) {
        /* Partial send: queue the unsent remainder. */
        tx_buffer_append(c, buf + sent, (size_t)(n - sent));
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* Socket buffer is full: queue the whole message. */
        tx_buffer_append(c, buf, (size_t)n);
    } else {
        perror("send");
        c->pending_disconnect = 1;
    }
}

/* Broadcast one EVT SAMPLE line to every client currently watching.
 * Saves 'next' before each iteration step because client_send() may
 * mark (and we may then immediately remove) the current client -- e.g.
 * a slow WATCH-ing client whose tx_buf just overflowed. */
static void broadcast_sample(const struct vmonitor_sample *s)
{
    struct client *c = g_clients_head;
    while (c) {
        struct client *next = c->next;

        if (c->watching) {
            client_send(c, "EVT SAMPLE %llu %llu %d %u\n",
                        (unsigned long long)s->seq,
                        (unsigned long long)s->timestamp_ns,
                        s->value_mC, s->alarm);
            if (c->pending_disconnect) {
                printf("[client fd=%d] disconnected (slow client / send error)\n", c->fd);
                epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, NULL);
                client_remove(c);
            }
        }

        c = next;
    }
}

/* ---- Talking to the driver ---- */

/* Writes a plain-text integer to a sysfs attribute file. Returns 0 on
 * success, -1 on failure (errno set). */
static int sysfs_write_int(const char *path, long value)
{
    char buf[32];
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;

    int n = snprintf(buf, sizeof(buf), "%ld\n", value);
    ssize_t w = write(fd, buf, (size_t)n);
    int saved_errno = errno;
    close(fd);

    if (w != n) {
        errno = saved_errno;
        return -1;
    }
    return 0;
}

/* Drains all currently-queued samples from /dev/vmonitor (called when
 * epoll reports the device fd readable) and broadcasts each one to
 * WATCH-ing clients. */
static void drain_device_samples(void)
{
    struct vmonitor_sample s;
    for (;;) {
        ssize_t n = read(g_dev_fd, &s, sizeof(s));
        if (n == (ssize_t)sizeof(s)) {
            broadcast_sample(&s);
            continue; /* there may be more queued */
        } else if (n == 0) {
            break; /* queue is empty, nothing more to drain */
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            perror("read(/dev/vmonitor)");
            break;
        }
    }
}

/* ---- Protocol command dispatch ---- */

/* Splits "<id> <CMD> [ARG]" into up to 3 tokens. Returns the number of
 * tokens found (1-3). Modifies 'line' in place (inserts '\0's). */
static int tokenize(char *line, char *tokens[3])
{
    int count = 0;
    char *p = line;
    while (*p && count < 3) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        tokens[count++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
            *p++ = '\0';
    }
    return count;
}

/* Case-insensitive compare, and tolerant of a stray trailing '/' (the
 * task's own protocol table shows "GET/" as an example). */
static int cmd_is(const char *tok, const char *name)
{
    size_t len = strlen(tok);
    if (len > 0 && tok[len - 1] == '/')
        len--;
    if (len != strlen(name))
        return 0;
    for (size_t i = 0; i < len; i++) {
        if (tolower((unsigned char)tok[i]) != tolower((unsigned char)name[i]))
            return 0;
    }
    return 1;
}

static void handle_command(struct client *c, char *line)
{
    char *tokens[3];
    int ntok = tokenize(line, tokens);

    if (ntok < 2) {
        client_send(c, "RSP ? ERR malformed line (expected: <id> <CMD> [arg])\n");
        return;
    }

    const char *id = tokens[0];
    const char *cmd = tokens[1];
    const char *arg = (ntok >= 3) ? tokens[2] : NULL;

    if (cmd_is(cmd, "GET")) {
        struct vmonitor_status st;
        if (ioctl(g_dev_fd, VMONITOR_IOC_GET_STATUS, &st) < 0) {
            client_send(c, "RSP %s ERR ioctl GET_STATUS failed: %s\n", id, strerror(errno));
            return;
        }
        client_send(c,
            "RSP %s OK running=%u period_ms=%u threshold_mC=%d "
            "produced_total=%llu enqueued_total=%llu dropped_total=%llu "
            "read_total=%llu queued=%u last_seq=%llu last_value_mC=%d last_alarm=%u\n",
            id, st.running, st.period_ms, st.threshold_mC,
            (unsigned long long)st.produced_total,
            (unsigned long long)st.enqueued_total,
            (unsigned long long)st.dropped_total,
            (unsigned long long)st.read_total,
            st.queued,
            (unsigned long long)st.last_seq,
            st.last_value_mC, st.last_alarm);

    } else if (cmd_is(cmd, "START")) {
        if (ioctl(g_dev_fd, VMONITOR_IOC_START, NULL) < 0)
            client_send(c, "RSP %s ERR ioctl START failed: %s\n", id, strerror(errno));
        else
            client_send(c, "RSP %s OK\n", id);

    } else if (cmd_is(cmd, "STOP")) {
        if (ioctl(g_dev_fd, VMONITOR_IOC_STOP, NULL) < 0)
            client_send(c, "RSP %s ERR ioctl STOP failed: %s\n", id, strerror(errno));
        else
            client_send(c, "RSP %s OK\n", id);

    } else if (cmd_is(cmd, "PERIOD")) {
        if (!arg) {
            client_send(c, "RSP %s ERR PERIOD requires an argument (ms)\n", id);
            return;
        }
        char *end;
        long ms = strtol(arg, &end, 10);
        if (*end != '\0' || ms <= 0) {
            client_send(c, "RSP %s ERR invalid period value: %s\n", id, arg);
            return;
        }
        if (sysfs_write_int(SYSFS_PERIOD_MS, ms) < 0)
            client_send(c, "RSP %s ERR could not set period_ms: %s\n", id, strerror(errno));
        else
            client_send(c, "RSP %s OK\n", id);

    } else if (cmd_is(cmd, "THRESHOLD")) {
        if (!arg) {
            client_send(c, "RSP %s ERR THRESHOLD requires an argument (mC)\n", id);
            return;
        }
        char *end;
        long mC = strtol(arg, &end, 10);
        if (*end != '\0') {
            client_send(c, "RSP %s ERR invalid threshold value: %s\n", id, arg);
            return;
        }
        if (sysfs_write_int(SYSFS_THRESHOLD_MC, mC) < 0)
            client_send(c, "RSP %s ERR could not set threshold_mC: %s\n", id, strerror(errno));
        else
            client_send(c, "RSP %s OK\n", id);

    } else if (cmd_is(cmd, "INJECT")) {
        if (!arg) {
            client_send(c, "RSP %s ERR INJECT requires an argument (mC)\n", id);
            return;
        }
        char *end;
        long mC = strtol(arg, &end, 10);
        if (*end != '\0') {
            client_send(c, "RSP %s ERR invalid temperature value: %s\n", id, arg);
            return;
        }

        struct vmonitor_sample s;
        memset(&s, 0, sizeof(s));
        s.value_mC = (__s32)mC;
        s.alarm = 0; /* driver doesn't auto-evaluate threshold on write() */

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        s.timestamp_ns = (__u64)ts.tv_sec * 1000000000ULL + (__u64)ts.tv_nsec;

        ssize_t n = write(g_dev_fd, &s, sizeof(s));
        if (n != (ssize_t)sizeof(s))
            client_send(c, "RSP %s ERR write to device failed: %s\n", id, strerror(errno));
        else
            client_send(c, "RSP %s OK\n", id);

    } else if (cmd_is(cmd, "WATCH")) {
        if (!arg || (strcmp(arg, "0") != 0 && strcmp(arg, "1") != 0)) {
            client_send(c, "RSP %s ERR WATCH requires 0 or 1\n", id);
            return;
        }
        c->watching = (strcmp(arg, "1") == 0);
        client_send(c, "RSP %s OK\n", id);

    } else {
        client_send(c, "RSP %s ERR unknown command: %s\n", id, cmd);
    }
}

/* ---- Per-client rx buffering (unchanged from stage 1, dispatch swapped) ---- */

static int process_rx_buffer(struct client *c)
{
    for (;;) {
        char *nl = memchr(c->rx_buf, '\n', c->rx_len);
        if (!nl)
            break;

        size_t line_len = (size_t)(nl - c->rx_buf);
        size_t effective_len = line_len;
        if (effective_len > 0 && c->rx_buf[effective_len - 1] == '\r')
            effective_len--;

        char line[MAX_LINE_LEN + 1];
        size_t copy_len = effective_len < MAX_LINE_LEN ? effective_len : MAX_LINE_LEN;
        memcpy(line, c->rx_buf, copy_len);
        line[copy_len] = '\0';

        handle_command(c, line);

        size_t consumed = line_len + 1;
        memmove(c->rx_buf, c->rx_buf + consumed, c->rx_len - consumed);
        c->rx_len -= consumed;
    }

    if (c->rx_len == RX_BUFFER_SIZE) {
        fprintf(stderr, "[client fd=%d] line too long, disconnecting\n", c->fd);
        return -1; /* signal caller to disconnect this client */
    }
    return 0;
}

int main(int argc, char *argv[])
{
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    int listen_fd, epfd;
    struct sockaddr_in server_addr;
    struct epoll_event ev, events[MAX_EVENTS];

    /* When stdout is redirected to a file (not a TTY), glibc switches to
     * fully-buffered mode by default, so printf() output can sit in a
     * buffer for a long time before actually reaching the file. Force
     * line buffering so log messages show up promptly either way. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    signal(SIGINT, handle_sigint);
    signal(SIGPIPE, SIG_IGN);

    /* Open the driver ONCE. The single-open lock in the driver guarantees
     * we are the sole owner -- if this fails with EBUSY, some other
     * process (e.g. a leftover test program) still has it open. */
    g_dev_fd = open(DEV_PATH, O_RDWR | O_NONBLOCK);
    if (g_dev_fd < 0) {
        fprintf(stderr, "Could not open %s: %s\n", DEV_PATH, strerror(errno));
        fprintf(stderr, "Is the vmonitor module loaded? Is another process holding it open?\n");
        return 1;
    }

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind"); close(listen_fd); return 1;
    }
    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen"); close(listen_fd); return 1;
    }
    if (set_nonblocking(listen_fd) < 0) {
        perror("set_nonblocking(listen_fd)"); close(listen_fd); return 1;
    }

    epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); close(listen_fd); return 1; }
    g_epfd = epfd;

    ev.events = EPOLLIN;
    ev.data.ptr = NULL; /* listening socket */
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    ev.events = EPOLLIN;
    ev.data.ptr = TAG_DEVICE;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, g_dev_fd, &ev) < 0) {
        perror("epoll_ctl(dev_fd)");
        close(listen_fd); close(g_dev_fd); close(epfd);
        return 1;
    }

    printf("monitor_server (stage 2) listening on port %d, /dev/vmonitor open (fd=%d)\n",
           port, g_dev_fd);
    printf("Test with: nc 127.0.0.1 %d\n", port);

    while (!g_stop) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == NULL) {
                /* Listening socket: drain the accept backlog */
                for (;;) {
                    struct sockaddr_in caddr;
                    socklen_t alen = sizeof(caddr);
                    int cfd = accept(listen_fd, (struct sockaddr *)&caddr, &alen);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept"); break;
                    }
                    if (set_nonblocking(cfd) < 0) { close(cfd); continue; }

                    struct client *c = client_new(cfd);
                    if (!c) { close(cfd); continue; }

                    struct epoll_event cev;
                    cev.events = EPOLLIN;
                    cev.data.ptr = c;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev) < 0) {
                        client_remove(c);
                        continue;
                    }
                    printf("[client fd=%d] connected from %s:%d\n",
                           cfd, inet_ntoa(caddr.sin_addr), ntohs(caddr.sin_port));
                }
                continue;
            }

            if (events[i].data.ptr == TAG_DEVICE) {
                /* /dev/vmonitor has new samples queued -> drain + broadcast */
                drain_device_samples();
                continue;
            }

            /* Otherwise: event on a client socket */
            struct client *c = events[i].data.ptr;

            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                printf("[client fd=%d] disconnected (HUP/ERR)\n", c->fd);
                epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                client_remove(c);
                continue;
            }

            if (events[i].events & EPOLLOUT) {
                /* Socket became writable again -- try to drain any
                 * backlog queued from a previous partial/EAGAIN send. */
                tx_buffer_flush(c);
                if (c->pending_disconnect) {
                    printf("[client fd=%d] disconnected (slow client / send error)\n", c->fd);
                    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                    client_remove(c);
                    continue;
                }
            }

            if (events[i].events & EPOLLIN) {
                int disconnected = 0;
                for (;;) {
                    size_t space = RX_BUFFER_SIZE - c->rx_len;
                    if (space == 0) break;

                    ssize_t r = recv(c->fd, c->rx_buf + c->rx_len, space, 0);
                    if (r > 0) {
                        c->rx_len += (size_t)r;
                        continue;
                    } else if (r == 0) {
                        printf("[client fd=%d] closed connection\n", c->fd);
                        disconnected = 1;
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("recv");
                        disconnected = 1;
                        break;
                    }
                }

                if (!disconnected) {
                    if (process_rx_buffer(c) < 0)
                        disconnected = 1;
                    else if (c->pending_disconnect)
                        disconnected = 1; /* client_send() during command handling overflowed tx_buf */
                }

                if (disconnected) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                    client_remove(c);
                }
            }
        }
    }

    printf("\nShutting down.\n");
    close(listen_fd);
    close(g_dev_fd);
    close(epfd);
    return 0;
}