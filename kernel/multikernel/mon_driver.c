#include <linux/fs.h>
#include <linux/module.h>

static ssize_t eo_write(struct file *file,
                               const char __user *buf,
                               size_t count,
                               loff_t *ppos)
{
    char kbuf[64];

    if (count > sizeof(kbuf))
        count = sizeof(kbuf);

    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;

    pr_info("mychardev: got %zu bytes: %*phN\n", count, (int)count, kbuf);

    return count; 
}

static const struct file_operations eo_fops = {
    .owner = THIS_MODULE,
    .write = eo_write,
};


static int major;

static int __init my_init(void)
{
    major = register_chrdev(0, "eo_driver", &eo_fops);
    if (major < 0)
        return major;
    pr_info("EO -> eo_driver: registered with major %d\n", major);
    return 0;
}

static void __exit my_exit(void)
{
    unregister_chrdev(major, "eo_driver");
}

module_init(my_init);
module_exit(my_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("EO -> Linux driver");
MODULE_AUTHOR("Eric OKALA");

/*
// Ensuite côté user :

mknod /dev/eo_driver c <major> 0
echo "test" > /dev/eo_driver   # -> appellera eo_write
*/
