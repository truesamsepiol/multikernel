#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <asm/io.h>

#define RESULT_FILE "/home/samsepiol/multikernel/resultats_tests/instance_a"

static void *isolation_page;
static phys_addr_t isolation_phys;

static int __init isolation_test_init(void)
{
    struct file *file;
    char buffer[256];
    loff_t pos = 0;
    int len;
    ssize_t ret;

    /*
     * Allouer une page de mémoire sur l'hôte.
     */
    isolation_page = kmalloc(PAGE_SIZE, GFP_KERNEL);

    if (!isolation_page) {
        pr_err("ISOLATION_HOST: kmalloc failed\n");
        return -ENOMEM;
    }

    /*
     * Initialiser la page avec 0xAA.
     */
    memset(isolation_page, 0xAA, PAGE_SIZE);

    /*
     * Récupérer son adresse physique.
     */
    isolation_phys = virt_to_phys(isolation_page);

    /*
     * Préparer le contenu du fichier.
     */
    len = scnprintf(buffer,
                    sizeof(buffer),
                    "ISOLATION_HOST: size=%lu\n"
                    "ISOLATION_HOST: physical_start=0x%llx\n"
                    "ISOLATION_HOST: physical_end=0x%llx\n"
                    "ISOLATION_HOST: value=0xAA\n",
                    PAGE_SIZE,
                    (unsigned long long)isolation_phys,
                    (unsigned long long)(isolation_phys + PAGE_SIZE - 1));

    /*
     * Ouvrir le fichier de résultats de l'hôte.
     */
    file = filp_open(RESULT_FILE,
                     O_WRONLY | O_CREAT | O_TRUNC,
                     0644);

    if (IS_ERR(file)) {
        pr_err("ISOLATION_HOST: cannot open result file\n");

        kfree(isolation_page);
        isolation_page = NULL;

        return PTR_ERR(file);
    }

    /*
     * Écrire les informations.
     */
    ret = kernel_write(file, buffer, len, &pos);

    filp_close(file, NULL);

    if (ret < 0) {
        pr_err("ISOLATION_HOST: kernel_write failed: %zd\n", ret);

        kfree(isolation_page);
        isolation_page = NULL;

        return ret;
    }

    return 0;
}

static void __exit isolation_test_exit(void)
{
    /*
     * La page reste réservée pendant tout le test.
     */
    kfree(isolation_page);
    isolation_page = NULL;

    pr_info("ISOLATION_HOST: memory released\n");
}

module_init(isolation_test_init);
module_exit(isolation_test_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kerf memory isolation test - host memory");
