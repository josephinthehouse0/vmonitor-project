/* test_day3_poll.c - poll() smoke test
 *
 * Iki senaryo test edilir:
 *   1) Kuyruk BOSKEN poll() zaman asimina ugramali (veri yok, timeout doner)
 *   2) INJECT (write) sonrasi poll() HEMEN POLLIN donmeli (veri var)
 *
 * Derleme:  gcc -I../include -o test_day3_poll test_day3_poll.c
 * Kullanim: sudo ./test_day3_poll
 */
/* _POSIX_C_SOURCE, glibc'nin CLOCK_MONOTONIC gibi POSIX.1b zamanlama
 * sembollerini gostermesi icin herhangi bir include'dan ONCE tanimlanmali. */
#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "vmonitor_uapi.h"

#define DEV_PATH "/dev/vmonitor"

static void check(int cond, const char *msg)
{
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
}

static long elapsed_ms(struct timespec *start, struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000 +
           (end->tv_nsec - start->tv_nsec) / 1000000;
}

int main(void)
{
    int fd;
    struct pollfd pfd;
    int ret;
    struct timespec t0, t1;

    fd = open(DEV_PATH, O_RDWR);
    check(fd >= 0, "open basarili");
    if (fd < 0) { perror("open"); return 1; }

    /* TANI: poll()'a guvenmeden once kuyrugun GERCEKTEN bos olup olmadigini
     * dogrudan read() ile kontrol edelim. */
    {
        struct vmonitor_sample probe;
        ssize_t n = read(fd, &probe, sizeof(probe));
        printf("  TANI (poll'dan once dogrudan read): n=%zd", n);
        if (n > 0)
            printf(" -> seq=%llu value_mC=%d alarm=%u (kuyrukta BEKLEYEN veri VARMIS!)\n",
                   (unsigned long long)probe.seq, probe.value_mC, probe.alarm);
        else if (n == 0)
            printf(" -> kuyruk gercekten BOS\n");
        else
            printf(" -> hata: errno=%d (%s)\n", errno, strerror(errno));
    }

    /* Senaryo 1: kuyruk bos, timer da kapali (START edilmedi) ->
     * poll() 500ms icinde veri gelmedigi icin timeout (0) donmeli. */
    pfd.fd = fd;
    pfd.events = POLLIN;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    ret = poll(&pfd, 1, 500 /* ms timeout */);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    check(ret == 0, "bos kuyrukta poll() timeout ile donuyor (veri yok)");
    printf("  gecen sure: %ld ms (yaklasik 500 ms olmali)\n", elapsed_ms(&t0, &t1));
    if (ret != 0)
        printf("  TANI: ret=%d errno=%d (%s) revents=0x%x\n",
               ret, errno, strerror(errno), pfd.revents);

    /* Senaryo 2: write() ile manuel bir ornek enjekte edelim, poll()
     * bu sefer HEMEN (timer beklemeden) POLLIN donmeli. */
    {
        struct vmonitor_sample in;
        memset(&in, 0, sizeof(in));
        in.value_mC = 30000;

        ssize_t n = write(fd, &in, sizeof(in));
        check(n == (ssize_t)sizeof(in), "write() ile ornek enjekte edildi");
    }

    clock_gettime(CLOCK_MONOTONIC, &t0);
    ret = poll(&pfd, 1, 2000 /* ms timeout, ama hemen donmeli */);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    check(ret == 1, "veri geldikten sonra poll() hemen doner (ret=1)");
    check((pfd.revents & POLLIN) != 0, "POLLIN biti set edilmis");
    printf("  gecen sure: %ld ms (500ms'den cok daha az olmali)\n", elapsed_ms(&t0, &t1));

    /* Veriyi okuyup temizleyelim */
    {
        struct vmonitor_sample out;
        ssize_t n = read(fd, &out, sizeof(out));
        check(n == (ssize_t)sizeof(out), "enjekte edilen ornek read() ile okundu");
    }

    close(fd);
    printf("\nTum testler tamamlandi.\n");
    return 0;
}