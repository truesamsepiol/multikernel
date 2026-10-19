#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

static int __init kerf_crash_test_init(void)
{
    pr_info("KERF_CRASH_TEST: init begins\n");
    pr_info("KERF_CRASH_TEST: about to trigger kernel fault\n");

    *(volatile unsigned long *)0 = 0xdeadbeefUL;

    pr_info("KERF_CRASH_TEST: fault instruction returned\n");

    return 0;
}

static void __exit kerf_crash_test_exit(void)
{
    pr_info("KERF_CRASH_TEST: module exit\n");
}

module_init(kerf_crash_test_init);
module_exit(kerf_crash_test_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Controlled kernel fault test for a disposable Kerf instance");
