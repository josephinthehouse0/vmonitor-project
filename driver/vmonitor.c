// SPDX-License-Identifier: GPL-2.0
/*
 * vmonitor.c - Gun 3: poll() destegi (wait queue)
 *
 * Gun 2'ye ek olarak:
 *   - wait_queue_head_t: kuyruga yeni veri girince bekleyen process'leri uyandirir
 *   - .poll: epoll/poll() sistem cagrisinin sordugu "veri var mi?" sorusuna cevap verir
 *
 * Bu sayede userspace (Gun 4'te yazilacak monitor_server), /dev/vmonitor'i
 * busy-wait ile surekli read() denemek yerine epoll ile bekleyebilecek.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/ioctl.h>
#include <linux/poll.h>
#include <linux/wait.h>

#include "vmonitor_uapi.h"

#define DRIVER_NAME    "vmonitor"
#define QUEUE_CAPACITY 64

#define DEFAULT_PERIOD_MS    1000
#define DEFAULT_THRESHOLD_MC 35000

MODULE_LICENSE("GPL");
MODULE_AUTHOR("staj");
MODULE_DESCRIPTION("Virtual temperature monitor character device - Day 2");

/* Sahte sicaklik uretim orüntüsü (m°C).
 * Spesifikasyon: 10500 -> 21000 -> ... -> 40000, sonra tekrar 20000'den
 * baslayip 40000'e kadar cikar, tekrar 20000'e doner (dongu). */
static const s32 temp_pattern[] = {
    10500, 21000, 31500, 40000,
    20000, 27500, 35000, 40000,
};
#define TEMP_PATTERN_LEN ARRAY_SIZE(temp_pattern)

/* ---- Dairesel kuyruk ---- */
struct vmonitor_queue {
    struct vmonitor_sample items[QUEUE_CAPACITY];
    unsigned int head;
    unsigned int tail;
    unsigned int count;
};

/* ---- Aygit durumu ---- */
struct vmonitor_dev {
    struct cdev cdev;
    struct vmonitor_queue queue;
    spinlock_t lock;           /* queue + sayaclar + ayarlar icin tek kilit */
    wait_queue_head_t sample_wq; /* kuyruga veri girdiginde uyandirilir */

    atomic_t opened;

    struct timer_list timer;
    bool running;
    u32 period_ms;
    s32 threshold_mC;
    unsigned int temp_idx;

    __u64 seq_counter;

    /* Sayaçlar (GET_STATUS icin) */
    u64 produced_total;
    u64 enqueued_total;
    u64 dropped_total;
    u64 read_total;

    __u64 last_seq;
    __s32 last_value_mC;
    __u32 last_alarm;
};

static struct vmonitor_dev *vdev;
static dev_t vmonitor_devno;
static struct class *vmonitor_class;
static struct device *vmonitor_device;

/* ---- Kuyruk yardimci fonksiyonlari (CAGIRAN spinlock'u tutmus olmali) ---- */

static bool queue_is_full(struct vmonitor_queue *q)
{
    return q->count == QUEUE_CAPACITY;
}

static bool queue_is_empty(struct vmonitor_queue *q)
{
    return q->count == 0;
}

static bool queue_push(struct vmonitor_queue *q, const struct vmonitor_sample *s)
{
    if (queue_is_full(q))
        return false;
    q->items[q->tail] = *s;
    q->tail = (q->tail + 1) % QUEUE_CAPACITY;
    q->count++;
    return true;
}

static bool queue_pop(struct vmonitor_queue *q, struct vmonitor_sample *out)
{
    if (queue_is_empty(q))
        return false;
    *out = q->items[q->head];
    q->head = (q->head + 1) % QUEUE_CAPACITY;
    q->count--;
    return true;
}

/* ---- Timer callback: her period_ms'de bir yeni ornek uretir ---- */

static void vmonitor_timer_cb(struct timer_list *t)
{
    struct vmonitor_dev *dev = timer_container_of(dev, t, timer);
    struct vmonitor_sample sample;
    unsigned long flags;
    s32 value;

    spin_lock_irqsave(&dev->lock, flags);

    value = temp_pattern[dev->temp_idx];
    dev->temp_idx = (dev->temp_idx + 1) % TEMP_PATTERN_LEN;

    sample.timestamp_ns = ktime_get_ns();
    sample.value_mC = value;
    sample.alarm = (value >= dev->threshold_mC) ? 1 : 0;
    sample.seq = ++dev->seq_counter;

    dev->produced_total++;
    if (queue_push(&dev->queue, &sample)) {
        dev->enqueued_total++;
    } else {
        dev->dropped_total++;
    }

    dev->last_seq = sample.seq;
    dev->last_value_mC = sample.value_mC;
    dev->last_alarm = sample.alarm;

    /* Timer bir-kerelik (one-shot) oldugu icin hala 'running' isek
     * kendimizi yeniden kuruyoruz -> periyodik davranisi boyle elde ediyoruz. */
    if (dev->running)
        mod_timer(&dev->timer, jiffies + msecs_to_jiffies(dev->period_ms));

    spin_unlock_irqrestore(&dev->lock, flags);

    /* Kilit disinda uyandiriyoruz: wake_up_interruptible kilit altinda da
     * cagrilabilir (uyumaz), ama kritik bolgeyi kisa tutmak icin disarida
     * birakmak daha temiz bir aliskanliktir. Queue durumu zaten guncellendi. */
    wake_up_interruptible(&dev->sample_wq);
}

/* ---- file_operations ---- */

static int vmonitor_open(struct inode *inode, struct file *filp)
{
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

static ssize_t vmonitor_read(struct file *filp, char __user *buf,
                              size_t count, loff_t *ppos)
{
    struct vmonitor_dev *dev = filp->private_data;
    struct vmonitor_sample sample;
    unsigned long flags;
    bool got;

    if (count < sizeof(sample))
        return -EINVAL;

    spin_lock_irqsave(&dev->lock, flags);
    got = queue_pop(&dev->queue, &sample);
    if (got)
        dev->read_total++;
    spin_unlock_irqrestore(&dev->lock, flags);

    if (!got)
        return 0;

    if (copy_to_user(buf, &sample, sizeof(sample)))
        return -EFAULT;

    return sizeof(sample);
}

static ssize_t vmonitor_write(struct file *filp, const char __user *buf,
                               size_t count, loff_t *ppos)
{
    struct vmonitor_dev *dev = filp->private_data;
    struct vmonitor_sample sample;
    unsigned long flags;
    bool pushed;

    if (count < sizeof(sample))
        return -EINVAL;

    if (copy_from_user(&sample, buf, sizeof(sample)))
        return -EFAULT;

    spin_lock_irqsave(&dev->lock, flags);
    sample.seq = ++dev->seq_counter;
    dev->produced_total++;
    pushed = queue_push(&dev->queue, &sample);
    if (pushed)
        dev->enqueued_total++;
    else
        dev->dropped_total++;
    dev->last_seq = sample.seq;
    dev->last_value_mC = sample.value_mC;
    dev->last_alarm = sample.alarm;
    spin_unlock_irqrestore(&dev->lock, flags);

    if (pushed)
        wake_up_interruptible(&dev->sample_wq);

    return sizeof(sample);
}

static long vmonitor_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct vmonitor_dev *dev = filp->private_data;
    void __user *argp = (void __user *)arg;
    unsigned long flags;
    struct vmonitor_status status;

    switch (cmd) {
    case VMONITOR_IOC_START:
        spin_lock_irqsave(&dev->lock, flags);
        if (!dev->running) {
            dev->running = true;
            mod_timer(&dev->timer, jiffies + msecs_to_jiffies(dev->period_ms));
        }
        spin_unlock_irqrestore(&dev->lock, flags);
        pr_info(DRIVER_NAME ": START\n");
        return 0;

    case VMONITOR_IOC_STOP:
        spin_lock_irqsave(&dev->lock, flags);
        dev->running = false;
        spin_unlock_irqrestore(&dev->lock, flags);
        /* del_timer_sync kilit TUTULMADAN cagrilmali: callback'in
         * tamamlanmasini bekler, kilit tutarken cagirmak deadlock yaratir. */
        timer_delete_sync(&dev->timer);
        pr_info(DRIVER_NAME ": STOP\n");
        return 0;

    case VMONITOR_IOC_GET_STATUS:
        spin_lock_irqsave(&dev->lock, flags);
        status.running        = dev->running;
        status.period_ms      = dev->period_ms;
        status.threshold_mC   = dev->threshold_mC;
        status.produced_total = dev->produced_total;
        status.enqueued_total = dev->enqueued_total;
        status.dropped_total  = dev->dropped_total;
        status.read_total     = dev->read_total;
        status.queued         = dev->queue.count;
        status.last_seq       = dev->last_seq;
        status.last_value_mC  = dev->last_value_mC;
        status.last_alarm     = dev->last_alarm;
        spin_unlock_irqrestore(&dev->lock, flags);

        if (copy_to_user(argp, &status, sizeof(status)))
            return -EFAULT;
        return 0;

    default:
        return -ENOTTY;
    }
}

/*
 * poll(): epoll/poll() sistem cagrisi bu fonksiyonu cagirir.
 * poll_wait(), process'i sample_wq uzerinde bekleyen listeye ekler (henuz
 * uyutmaz) -- eger o sirada kuyrukta zaten veri varsa EPOLLIN hemen donulur,
 * yoksa cagiran taraf (epoll_wait) timer/write birini wake_up_interruptible
 * cagirana kadar bloklanir.
 */
static __poll_t vmonitor_poll(struct file *filp, poll_table *wait)
{
    struct vmonitor_dev *dev = filp->private_data;
    __poll_t mask = 0;
    unsigned long flags;

    poll_wait(filp, &dev->sample_wq, wait);

    spin_lock_irqsave(&dev->lock, flags);
    if (!queue_is_empty(&dev->queue))
        mask |= EPOLLIN | EPOLLRDNORM;
    spin_unlock_irqrestore(&dev->lock, flags);

    return mask;
}

static const struct file_operations vmonitor_fops = {
    .owner          = THIS_MODULE,
    .open           = vmonitor_open,
    .release        = vmonitor_release,
    .read           = vmonitor_read,
    .write          = vmonitor_write,
    .unlocked_ioctl = vmonitor_ioctl,
    .poll           = vmonitor_poll,
};

/* ---- sysfs: period_ms ---- */

static ssize_t period_ms_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct vmonitor_dev *vd = dev_get_drvdata(dev);
    unsigned long flags;
    u32 val;

    spin_lock_irqsave(&vd->lock, flags);
    val = vd->period_ms;
    spin_unlock_irqrestore(&vd->lock, flags);

    return sysfs_emit(buf, "%u\n", val);
}

static ssize_t period_ms_store(struct device *dev, struct device_attribute *attr,
                                const char *buf, size_t count)
{
    struct vmonitor_dev *vd = dev_get_drvdata(dev);
    unsigned long flags;
    unsigned int val;

    if (kstrtouint(buf, 10, &val))
        return -EINVAL;
    if (val == 0)
        return -EINVAL; /* 0 ms periyot anlamsiz */

    spin_lock_irqsave(&vd->lock, flags);
    vd->period_ms = val;
    if (vd->running)
        mod_timer(&vd->timer, jiffies + msecs_to_jiffies(val));
    spin_unlock_irqrestore(&vd->lock, flags);

    return count;
}
static DEVICE_ATTR_RW(period_ms);

/* ---- sysfs: threshold_mC ---- */

static ssize_t threshold_mC_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct vmonitor_dev *vd = dev_get_drvdata(dev);
    unsigned long flags;
    s32 val;

    spin_lock_irqsave(&vd->lock, flags);
    val = vd->threshold_mC;
    spin_unlock_irqrestore(&vd->lock, flags);

    return sysfs_emit(buf, "%d\n", val);
}

static ssize_t threshold_mC_store(struct device *dev, struct device_attribute *attr,
                                   const char *buf, size_t count)
{
    struct vmonitor_dev *vd = dev_get_drvdata(dev);
    unsigned long flags;
    int val;

    if (kstrtoint(buf, 10, &val))
        return -EINVAL;

    spin_lock_irqsave(&vd->lock, flags);
    vd->threshold_mC = val;
    spin_unlock_irqrestore(&vd->lock, flags);

    return count;
}
static DEVICE_ATTR_RW(threshold_mC);

/* ---- Modul init/exit ---- */

static int __init vmonitor_init(void)
{
    int ret;

    vdev = kzalloc(sizeof(*vdev), GFP_KERNEL);
    if (!vdev)
        return -ENOMEM;

    atomic_set(&vdev->opened, 0);
    spin_lock_init(&vdev->lock);
    init_waitqueue_head(&vdev->sample_wq);
    vdev->period_ms    = DEFAULT_PERIOD_MS;
    vdev->threshold_mC = DEFAULT_THRESHOLD_MC;
    vdev->running      = false;
    timer_setup(&vdev->timer, vmonitor_timer_cb, 0);

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

    vmonitor_class = class_create(DRIVER_NAME);
    if (IS_ERR(vmonitor_class)) {
        ret = PTR_ERR(vmonitor_class);
        pr_err(DRIVER_NAME ": class_create basarisiz\n");
        goto err_cdev_del;
    }

    vmonitor_device = device_create(vmonitor_class, NULL, vmonitor_devno, NULL, DRIVER_NAME);
    if (IS_ERR(vmonitor_device)) {
        ret = PTR_ERR(vmonitor_device);
        pr_err(DRIVER_NAME ": device_create basarisiz\n");
        goto err_class_destroy;
    }
    dev_set_drvdata(vmonitor_device, vdev);

    ret = device_create_file(vmonitor_device, &dev_attr_period_ms);
    if (ret) {
        pr_err(DRIVER_NAME ": period_ms sysfs dosyasi olusturulamadi\n");
        goto err_device_destroy;
    }

    ret = device_create_file(vmonitor_device, &dev_attr_threshold_mC);
    if (ret) {
        pr_err(DRIVER_NAME ": threshold_mC sysfs dosyasi olusturulamadi\n");
        goto err_remove_period_attr;
    }

    pr_info(DRIVER_NAME ": yuklendi, major=%d minor=%d, period_ms=%u threshold_mC=%d\n",
            MAJOR(vmonitor_devno), MINOR(vmonitor_devno),
            vdev->period_ms, vdev->threshold_mC);
    return 0;

err_remove_period_attr:
    device_remove_file(vmonitor_device, &dev_attr_period_ms);
err_device_destroy:
    device_destroy(vmonitor_class, vmonitor_devno);
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
    timer_delete_sync(&vdev->timer);
    device_remove_file(vmonitor_device, &dev_attr_threshold_mC);
    device_remove_file(vmonitor_device, &dev_attr_period_ms);
    device_destroy(vmonitor_class, vmonitor_devno);
    class_destroy(vmonitor_class);
    cdev_del(&vdev->cdev);
    unregister_chrdev_region(vmonitor_devno, 1);
    kfree(vdev);
    pr_info(DRIVER_NAME ": kaldirildi\n");
}

module_init(vmonitor_init);
module_exit(vmonitor_exit);