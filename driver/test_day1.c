/* test_day1.c - Gun 1 duman testi (smoke test)
 *
 * Derleme:   gcc -I../include -o test_day1 test_day1.c
 * Kullanim:  sudo ./test_day1
 *
 * Test ettikleri:
 *   1) /dev/vmonitor acilabiliyor mu
 *   2) Ikinci open EBUSY donuyor mu
 *   3) write() ile ornek basip read() ile geri okumak mumkun mu
 *   4) Kuyruk bossa read() 0 donuyor mu
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include "vmonitor_uapi.h"

#define DEV_PATH "/dev/vmonitor"

static void check(int cond, const char *msg)
{
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
}

int main(void)
{
    int fd1, fd2;
    struct vmonitor_sample out;
    struct vmonitor_sample in = {
        .seq = 0,               /* surucu bunu ezecek */
        .timestamp_ns = 0,      /* surucu Gun 1'de doldurmuyor, sadece elle test */
        .value_mC = 25500,
        .alarm = 0,
    };
    ssize_t n;

    fd1 = open(DEV_PATH, O_RDWR);
    check(fd1 >= 0, "Ilk open basarili");
    if (fd1 < 0) {
        perror("open");
        return 1;
    }

    fd2 = open(DEV_PATH, O_RDWR);
    check(fd2 < 0 && errno == EBUSY, "Ikinci open EBUSY donuyor");
    if (fd2 >= 0)
        close(fd2);

    n = write(fd1, &in, sizeof(in));
    check(n == sizeof(in), "write() 24 byte yazdi");

    n = read(fd1, &out, sizeof(out));
    check(n == sizeof(out), "read() 24 byte okudu");
    check(out.value_mC == 25500, "value_mC dogru geldi");
    check(out.seq == 1, "seq surucu tarafindan 1 olarak atandi");

    n = read(fd1, &out, sizeof(out));
    check(n == 0, "Kuyruk bossa read() 0 donuyor");

    close(fd1);
    printf("\nTum testler tamamlandi.\n");
    return 0;
}
