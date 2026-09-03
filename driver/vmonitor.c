// SPDX-License-Identifier: GPL-2.0
/*
 * vmonitor.c - Gun 1: Karakter cihazi + dairesel kuyruk
 *
 * Bu asamada:
 *   - /dev/vmonitor karakter cihazi olusturulur
 *   - Tekli open kisitlamasi (EBUSY) uygulanir
 *   - write() ile struct vmonitor_sample elle kuyruga eklenir
 *   - read() ile kuyruktan en eski eleman okunur
 *
 * NOT: Spinlock Gun 2'de timer eklenince gerekli olacak (write yolu ile
 * timer yolu ayni kuyruga erisecek). Simdilik tek-thread test yeterli.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>
#include <linux/slab.h>

#include "vmonitor_uapi.h"

#define DRIVER_NAME   "vmonitor"
#define QUEUE_CAPACITY 64

MODULE_LICENSE("GPL");
MODULE_AUTHOR("staj");
MODULE_DESCRIPTION("Virtual temperature monitor character device - Day 1");

/* ---- Dairesel kuyruk ---- */
struct vmonitor_queue {
    struct vmonitor_sample items[QUEUE_CAPACITY];
    unsigned int head;   /* okunacak sonraki index */
    unsigned int tail;   /* yazilacak sonraki index */
    unsigned int count;  /* mevcut eleman sayisi */
};

/* ---- Aygit durumu ---- */
struct vmonitor_dev {
    struct cdev cdev;
    struct vmonitor_queue queue;

    atomic_t opened;      /* 0 = kapali, 1 = acik -> tekli erisim kilidi */

    __u64 seq_counter;    /* bir sonraki uretilecek/atanacak seq */

    /* Sayaçlar (Gun 2'de GET_STATUS icin kullanilacak) */
    u64 produced_total;
    u64 enqueued_total;
    u64 dropped_total;
    u64 read_total;
};

static struct vmonitor_dev *vdev;
static dev_t vmonitor_devno;
static struct class *vmonitor_class;

/* ---- Kuyruk yardimci fonksiyonlari ---- */

static bool queue_is_full(struct vmonitor_queue *q)
{
    return q->count == QUEUE_CAPACITY;
}

static bool queue_is_empty(struct vmonitor_queue *q)
{
    return q->count == 0;
}

/*
 * Yeni bir sample'i kuyruga ekler.
 * Kuyruk doluysa: DROP-NEWEST politikasi -> yeni gelen veri dusurulur,
 * eskisi korunur. dropped_total arttirilir, false doner.
 */
static bool queue_push(struct vmonitor_queue *q, const struct vmonitor_sample *s)
{
    if (queue_is_full(q))
        return false;

    q->items[q->tail] = *s;
    q->tail = (q->tail + 1) % QUEUE_CAPACITY;
    q->count++;
    return true;
}

/*
 * Kuyruktan en eski elemani cikartir.
 * Basarili ise true, kuyruk bossa false doner.
 */
static bool queue_pop(struct vmonitor_queue *q, struct vmonitor_sample *out)
{
    if (queue_is_empty(q))
        return false;

    *out = q->items[q->head];
    q->head = (q->head + 1) % QUEUE_CAPACITY;
    q->count--;
    return true;
}

/* ---- file_operations ---- */

static int vmonitor_open(struct inode *inode, struct file *filp)
{
    /* atomic_cmpxchg(&opened, 0, 1): opened 0 ise 1 yap ve eski degeri (0) don.
     * Eski deger 0 degilse (yani zaten 1 ise) baskasi acik demektir -> EBUSY. */
    if (atomic_cmpxchg(&vdev->opened, 0, 1) != 0) {
        pr_info(DRIVER_NAME ": open reddedildi, cihaz zaten acik (EBUSY)\n");
        return -EBUSY;
    }

    filp->private_data = vdev;
    pr_info(DRIVER_NAME ": acildi\n");
    return 0;
}

static int vmonitor_release(struct inode *inode, struct file *filp)
{
    atomic_set(&vdev->opened, 0);
    pr_info(DRIVER_NAME ": kapatildi\n");
    return 0;
}

/*
 * read(): Kuyruktan bir adet 24-byte'lik struct vmonitor_sample okur.
 * - count < sizeof(struct vmonitor_sample) ise -EINVAL
 * - Kuyruk bossa 0 doner (Gun 1'de bloklamiyoruz; Gun 3'te poll() ile
 *   birlikte non-blocking/blocking secenegi netlesecek)
 */
static ssize_t vmonitor_read(struct file *filp, char __user *buf,
                              size_t count, loff_t *ppos)
{
    struct vmonitor_dev *dev = filp->private_data;
    struct vmonitor_sample sample;

    if (count < sizeof(sample))
        return -EINVAL;

    if (!queue_pop(&dev->queue, &sample))
        return 0; /* veri yok */

    if (copy_to_user(buf, &sample, sizeof(sample)))
        return -EFAULT;

    dev->read_total++;
    return sizeof(sample);
}

/*
 * write(): Kullanicidan gelen 24 byte'lik struct'i kuyruga ekler.
 * Bu, Gun 2'deki timer'dan once kuyruk mantigini elle test etmek icindir
 * (ornegin: bir userspace test programindan INJECT benzeri veri basmak).
 *
 * seq alani surucu tarafindan uretilir (kullanicinin gonderdigi seq yok
 * sayilir), boylece sira numarasi tutarli kalir.
 */
static ssize_t vmonitor_write(struct file *filp, const char __user *buf,
                               size_t count, loff_t *ppos)
{
    struct vmonitor_dev *dev = filp->private_data;
    struct vmonitor_sample sample;

    if (count < sizeof(sample))
        return -EINVAL;

    if (copy_from_user(&sample, buf, sizeof(sample)))
        return -EFAULT;

    sample.seq = ++dev->seq_counter;
    dev->produced_total++;

    if (queue_push(&dev->queue, &sample))
        dev->enqueued_total++;
    else
        dev->dropped_total++;

    return sizeof(sample);
}

static const struct file_operations vmonitor_fops = {
    .owner   = THIS_MODULE,
    .open    = vmonitor_open,
    .release = vmonitor_release,
    .read    = vmonitor_read,
    .write   = vmonitor_write,
    /* .unlocked_ioctl ve .poll Gun 2/3'te eklenecek */
};

/* ---- Modul init/exit ---- */

static int __init vmonitor_init(void)
{
    int ret;
    struct device *dev_ret;

    vdev = kzalloc(sizeof(*vdev), GFP_KERNEL);
    if (!vdev)
        return -ENOMEM;

    atomic_set(&vdev->opened, 0);

    ret = alloc_chrdev_region(&vmonitor_devno, 0, 1, DRIVER_NAME);
    if (ret < 0) {
        pr_err(DRIVER_NAME ": alloc_chrdev_region basarisiz\n");
        goto err_free_dev;
    }

    cdev_init(&vdev->cdev, &vmonitor_fops);
    vdev->cdev.owner = THIS_MODULE;

    ret = cdev_add(&vdev->cdev, vmonitor_devno, 1);
    if (ret < 0) {
        pr_err(DRIVER_NAME ": cdev_add basarisiz\n");
        goto err_unregister;
    }

    /* /dev/vmonitor otomatik olusturulsun diye class + device_create
     * NOT: Kernel 6.4+ surumlerinde class_create() tek parametre alir
     * (THIS_MODULE parametresi kaldirildi). 7.0.0 icin bu haliyle dogru. */
    vmonitor_class = class_create(DRIVER_NAME);
    if (IS_ERR(vmonitor_class)) {
        ret = PTR_ERR(vmonitor_class);
        pr_err(DRIVER_NAME ": class_create basarisiz\n");
        goto err_cdev_del;
    }

    dev_ret = device_create(vmonitor_class, NULL, vmonitor_devno, NULL, DRIVER_NAME);
    if (IS_ERR(dev_ret)) {
        ret = PTR_ERR(dev_ret);
        pr_err(DRIVER_NAME ": device_create basarisiz\n");
        goto err_class_destroy;
    }

    pr_info(DRIVER_NAME ": yuklendi, major=%d minor=%d\n",
            MAJOR(vmonitor_devno), MINOR(vmonitor_devno));
    return 0;

err_class_destroy:
    class_destroy(vmonitor_class);
err_cdev_del:
    cdev_del(&vdev->cdev);
err_unregister:
    unregister_chrdev_region(vmonitor_devno, 1);
err_free_dev:
    kfree(vdev);
    return ret;
}

static void __exit vmonitor_exit(void)
{
    device_destroy(vmonitor_class, vmonitor_devno);
    class_destroy(vmonitor_class);
    cdev_del(&vdev->cdev);
    unregister_chrdev_region(vmonitor_devno, 1);
    kfree(vdev);
    pr_info(DRIVER_NAME ": kaldirildi\n");
}

module_init(vmonitor_init);
module_exit(vmonitor_exit);
