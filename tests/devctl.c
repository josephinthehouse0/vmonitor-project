/* devctl.c - Minimal direct /dev/vmonitor inspector, no server involved.
 *
 * Used specifically to demonstrate the queue-full/drop scenario: with
 * the server not running (or crashed) and the driver's timer still
 * running (STOP was never called), nobody drains the queue and it
 * genuinely fills up and starts dropping samples.
 *
 * Modes:
 *   devctl status   -> ioctl(GET_STATUS), print, exit (no side effects)
 *   devctl stop     -> ioctl(STOP), then print status, exit
 *
 * Build: gcc -Wall -Wextra -O2 -I../include -o devctl devctl.c
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "vmonitor_uapi.h"

#define DEV_PATH "/dev/vmonitor"

static void print_status(const struct vmonitor_status *s)
{
    printf("running=%u period_ms=%u threshold_mC=%d\n",
           s->running, s->period_ms, s->threshold_mC);
    printf("produced_total=%llu enqueued_total=%llu dropped_total=%llu read_total=%llu queued=%u\n",
           (unsigned long long)s->produced_total,
           (unsigned long long)s->enqueued_total,
           (unsigned long long)s->dropped_total,
           (unsigned long long)s->read_total,
           s->queued);
    printf("last_seq=%llu last_value_mC=%d last_alarm=%u\n",
           (unsigned long long)s->last_seq, s->last_value_mC, s->last_alarm);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <status|stop>\n", argv[0]);
        return 1;
    }

    int fd = open(DEV_PATH, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (strcmp(argv[1], "stop") == 0) {
        if (ioctl(fd, VMONITOR_IOC_STOP, NULL) < 0) {
            perror("ioctl(STOP)");
            close(fd);
            return 1;
        }
        printf("Timer stopped.\n");
    } else if (strcmp(argv[1], "status") != 0) {
        fprintf(stderr, "Unknown mode: %s\n", argv[1]);
        close(fd);
        return 1;
    }

    struct vmonitor_status st;
    if (ioctl(fd, VMONITOR_IOC_GET_STATUS, &st) < 0) {
        perror("ioctl(GET_STATUS)");
        close(fd);
        return 1;
    }
    print_status(&st);

    close(fd);
    return 0;
}