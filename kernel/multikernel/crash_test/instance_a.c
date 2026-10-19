#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <asm/io.h>

static void *isolation_page;
static phys_addr_t isolation_phys;

static int isolation_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "ISOLATION_A: size=%lu\n", PAGE_SIZE);
	seq_printf(m, "ISOLATION_A: virtual_start=%px\n", isolation_page);
	seq_printf(m, "ISOLATION_A: physical_start=0x%llx\n",
		   (unsigned long long)isolation_phys);
	seq_printf(m, "ISOLATION_A: physical_end=0x%llx\n",
		   (unsigned long long)(isolation_phys + PAGE_SIZE - 1));
	seq_printf(m, "ISOLATION_A: value=0xAA\n");

	return 0;
}

static int isolation_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, isolation_proc_show, NULL);
}

static const struct proc_ops isolation_proc_ops = {
	.proc_open = isolation_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init isolation_test_init(void)
{
	isolation_page = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!isolation_page) {
		pr_err("ISOLATION_A: kmalloc failed\n");
		return -ENOMEM;
	}

	memset(isolation_page, 0xAA, PAGE_SIZE);

	isolation_phys = virt_to_phys(isolation_page);

	proc_create("instance_a", 0444, NULL, &isolation_proc_ops);

	return 0;
}

static void __exit isolation_test_exit(void)
{
	remove_proc_entry("instance_a", NULL);

	kfree(isolation_page);
	isolation_page = NULL;

	pr_info("ISOLATION_A: memory released\n");
}

module_init(isolation_test_init);
module_exit(isolation_test_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kerf memory isolation test - instance A");
