/* monitor_cli.c - Command-line client for monitor_server
 *
 * Three modes:
 *   1) Single command:  monitor_cli <get|start|stop|period|threshold|inject> [arg]
 *      Connects, sends "<id> <CMD> [arg]", prints the RSP line, exits.
 *   2) Live watch:       monitor_cli watch
 *      Connects, sends WATCH 1, then prints every EVT SAMPLE line as it
 *      arrives until interrupted with Ctrl+C (sends WATCH 0 on the way out).
 *   3) UDP listen:       monitor_cli udp
 *      Binds to the UDP telemetry port and prints every STAT packet
 *      received, until interrupted with Ctrl+C.
 *
 * Build: gcc -Wall -Wextra -O2 -o monitor_cli monitor_cli.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_HOST      "127.0.0.1"
#define SERVER_TCP_PORT  5000
#define UDP_LISTEN_PORT  5001
#define LINE_BUF_SIZE    4096

static volatile sig_atomic_t g_stop = 0;
static void handle_sigint(int sig) { (void)sig; g_stop = 1; }

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s get\n"
        "  %s start\n"
        "  %s stop\n"
        "  %s period <ms>\n"
        "  %s threshold <mC>\n"
        "  %s inject <mC>\n"
        "  %s watch          (live EVT SAMPLE stream, Ctrl+C to stop)\n"
        "  %s udp            (listen for UDP STAT telemetry, Ctrl+C to stop)\n",
        prog, prog, prog, prog, prog, prog, prog, prog);
}

static int connect_to_server(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_TCP_PORT);
    if (inet_pton(AF_INET, SERVER_HOST, &addr.sin_addr) != 1) {
        fprintf(stderr, "inet_pton failed\n");
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Could not connect to %s:%d: %s\n",
                SERVER_HOST, SERVER_TCP_PORT, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* Reads and prints complete lines from fd as they arrive, handling the
 * same "a line may be split across reads" problem the server itself
 * has to handle -- a line-buffered client is just as necessary as a
 * line-buffered server. Blocks until at least one full line is read, or
 * returns 0 if the connection closed, -1 on error. */
static int read_and_print_lines(int fd, char *buf, size_t *buf_len, int max_lines)
{
    int printed = 0;
    for (;;) {
        char *nl = memchr(buf, '\n', *buf_len);
        while (nl) {
            size_t line_len = (size_t)(nl - buf);
            printf("%.*s\n", (int)line_len, buf);
            size_t consumed = line_len + 1;
            memmove(buf, buf + consumed, *buf_len - consumed);
            *buf_len -= consumed;
            printed++;
            if (max_lines > 0 && printed >= max_lines)
                return 1;
            nl = memchr(buf, '\n', *buf_len);
        }

        if (max_lines > 0 && printed > 0)
            return 1; /* got at least one full line for single-command mode */

        ssize_t r = recv(fd, buf + *buf_len, LINE_BUF_SIZE - *buf_len, 0);
        if (r > 0) {
            *buf_len += (size_t)r;
            continue;
        } else if (r == 0) {
            return 0; /* server closed the connection */
        } else {
            if (errno == EINTR)
                continue;
            perror("recv");
            return -1;
        }
    }
}

static int run_single_command(const char *cmd, const char *arg)
{
    int fd = connect_to_server();
    if (fd < 0)
        return 1;

    char out[256];
    int n;
    if (arg)
        n = snprintf(out, sizeof(out), "1 %s %s\n", cmd, arg);
    else
        n = snprintf(out, sizeof(out), "1 %s\n", cmd);

    if (send(fd, out, (size_t)n, 0) != n) {
        perror("send");
        close(fd);
        return 1;
    }

    char buf[LINE_BUF_SIZE];
    size_t buf_len = 0;
    int rc = read_and_print_lines(fd, buf, &buf_len, 1);
    close(fd);
    return (rc == 1) ? 0 : 1;
}

static int run_watch_mode(void)
{
    int fd = connect_to_server();
    if (fd < 0)
        return 1;

    const char *watch_on = "1 WATCH 1\n";
    if (send(fd, watch_on, strlen(watch_on), 0) < 0) {
        perror("send");
        close(fd);
        return 1;
    }

    /* A 1-second receive timeout means a blocked recv() always returns
     * (with EAGAIN) at least once a second even if no data arrives,
     * giving us a chance to notice g_stop and exit promptly on Ctrl+C
     * instead of potentially sitting inside recv() indefinitely. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    signal(SIGINT, handle_sigint);
    printf("Watching for live samples (Ctrl+C to stop)...\n");

    char buf[LINE_BUF_SIZE];
    size_t buf_len = 0;
    int closed_by_peer = 0;

    while (!g_stop) {
        /* Print any complete lines already sitting in the buffer first. */
        char *nl;
        while ((nl = memchr(buf, '\n', buf_len)) != NULL) {
            size_t line_len = (size_t)(nl - buf);
            printf("%.*s\n", (int)line_len, buf);
            fflush(stdout);
            size_t consumed = line_len + 1;
            memmove(buf, buf + consumed, buf_len - consumed);
            buf_len -= consumed;
        }

        ssize_t r = recv(fd, buf + buf_len, sizeof(buf) - buf_len, 0);
        if (r > 0) {
            buf_len += (size_t)r;
        } else if (r == 0) {
            closed_by_peer = 1;
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue; /* just the 1s timeout (or a signal) -- loop back to check g_stop */
            perror("recv");
            break;
        }
    }

    if (closed_by_peer)
        printf("Server closed the connection.\n");

    /* Best-effort: tell the server we're done watching before closing. */
    const char *watch_off = "2 WATCH 0\n";
    send(fd, watch_off, strlen(watch_off), 0);
    close(fd);
    printf("\nStopped watching.\n");
    return 0;
}

static int run_udp_listen_mode(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(UDP_LISTEN_PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return 1;
    }

    signal(SIGINT, handle_sigint);
    printf("Listening for UDP telemetry on port %d (Ctrl+C to stop)...\n", UDP_LISTEN_PORT);

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[1024];
    while (!g_stop) {
        ssize_t r = recvfrom(fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (r > 0) {
            buf[r] = '\0';
            fputs(buf, stdout);
            if (buf[r - 1] != '\n')
                fputc('\n', stdout);
            fflush(stdout);
        } else if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue; /* timeout or signal -- loop back to check g_stop */
            perror("recvfrom");
            break;
        }
    }

    close(fd);
    printf("\nStopped listening.\n");
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *mode = argv[1];

    if (strcmp(mode, "watch") == 0) {
        return run_watch_mode();
    } else if (strcmp(mode, "udp") == 0) {
        return run_udp_listen_mode();
    } else if (strcmp(mode, "get") == 0) {
        return run_single_command("GET", NULL);
    } else if (strcmp(mode, "start") == 0) {
        return run_single_command("START", NULL);
    } else if (strcmp(mode, "stop") == 0) {
        return run_single_command("STOP", NULL);
    } else if (strcmp(mode, "period") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        return run_single_command("PERIOD", argv[2]);
    } else if (strcmp(mode, "threshold") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        return run_single_command("THRESHOLD", argv[2]);
    } else if (strcmp(mode, "inject") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        return run_single_command("INJECT", argv[2]);
    } else {
        print_usage(argv[0]);
        return 1;
    }
}