#include <linux/fs.h>
#include <linux/module.h> 
#include <linux/multikernel.h>

//EO -> 1

static ssize_t eo_write(struct file *file,
                               const char __user *buf,
                               size_t count,
                               loff_t *ppos)
{
	char eo_buf[MK_WRITE_MAX_DATA_SIZE]; 
	size_t to_copy;
	int ret;
    
	if(count == 0)
	    return 0;

	to_copy = min(count, (size_t)(MK_WRITE_MAX_DATA_SIZE));

	if(copy_from_user(eo_buf, buf, to_copy))
	    return -EFAULT;

	eo_buf[to_copy] = '\0';
	
	ret = multikernel_eo_write(eo_buf, to_copy);
	if(ret)
	      return ret;	
	
	return to_copy; 
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
