/* test_day2.c - Gun 2 duman testi: timer + ioctl
 *
 * Derleme:  gcc -I../include -o test_day2 test_day2.c
 * Kullanim: sudo ./test_day2
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include "vmonitor_uapi.h"

#define DEV_PATH "/dev/vmonitor"

static void check(int cond, const char *msg)
{
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
}

static void print_status(const struct vmonitor_status *s)
{
    printf("  running=%u period_ms=%u threshold_mC=%d\n",
           s->running, s->period_ms, s->threshold_mC);
    printf("  produced=%llu enqueued=%llu dropped=%llu read=%llu queued=%u\n",
           (unsigned long long)s->produced_total,
           (unsigned long long)s->enqueued_total,
           (unsigned long long)s->dropped_total,
           (unsigned long long)s->read_total,
           s->queued);
    printf("  last_seq=%llu last_value_mC=%d last_alarm=%u\n",
           (unsigned long long)s->last_seq, s->last_value_mC, s->last_alarm);
}

int main(void)
{
    int fd;
    struct vmonitor_status st;

    fd = open(DEV_PATH, O_RDWR);
    check(fd >= 0, "open basarili");
    if (fd < 0) { perror("open"); return 1; }

    /* Periyodu hizli test icin 100ms yapalim (henuz START etmedik) */
    if (ioctl(fd, VMONITOR_IOC_GET_STATUS, &st) == 0) {
        printf("Baslangic durumu:\n");
        print_status(&st);
    }

    check(ioctl(fd, VMONITOR_IOC_START, NULL) == 0, "START basarili");

    /* period_ms=1000 varsayilaniyla ~1 saniyede 1 ornek uretilir. Timer'in
     * tam period sinirinda yarisa girmemek icin 2.5 saniye bekliyoruz, boylece
     * en az 2 ornek uretilmis olacagini garanti ediyoruz. */
    usleep(2500 * 1000);

    check(ioctl(fd, VMONITOR_IOC_GET_STATUS, &st) == 0, "GET_STATUS basarili");
    printf("2.5 saniye sonra durum:\n");
    print_status(&st);
    check(st.running == 1, "running=1 dogru");
    check(st.produced_total >= 2, "en az 2 ornek uretilmis");

    /* Kuyruktan bir ornek okuyalim */
    {
        struct vmonitor_sample sample;
        ssize_t n = read(fd, &sample, sizeof(sample));
        check(n == sizeof(sample), "read() ile timer'in urettigi ornek okundu");
        printf("  okunan ornek: seq=%llu value_mC=%d alarm=%u\n",
               (unsigned long long)sample.seq, sample.value_mC, sample.alarm);
    }

    check(ioctl(fd, VMONITOR_IOC_STOP, NULL) == 0, "STOP basarili");

    if (ioctl(fd, VMONITOR_IOC_GET_STATUS, &st) == 0) {
        check(st.running == 0, "STOP sonrasi running=0");
    }

    close(fd);
    printf("\nTum testler tamamlandi.\n");
    return 0;
}