#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/overflow.h>
#include <linux/string.h>

#define ISOLATION_FILE         "/isolation_a"
#define RESULT_BUFFER_SIZE     512

static phys_addr_t isolation_a_start;
static size_t isolation_a_size;

static char result_buffer[RESULT_BUFFER_SIZE];
static struct proc_dir_entry *result_entry;

static int isolation_proc_show(struct seq_file *m, void *v)
{
        seq_puts(m, result_buffer);
        return 0;
}

static int isolation_proc_open(struct inode *inode, struct file *file)
{
        return single_open(file, isolation_proc_show, NULL);
}

static const struct proc_ops isolation_proc_ops = {
        .proc_open    = isolation_proc_open,
        .proc_read    = seq_read,
        .proc_lseek   = seq_lseek,
        .proc_release = single_release,
};

/*
 * Lit :
 *
 * ISOLATION_A: physical_base=0x100000000
 * ISOLATION_A: size=0x1f400000
 */
static int isolation_b_read_metadata(void)
{
        struct file *file;
        char buffer[256];
        loff_t pos = 0;
        ssize_t len;
        unsigned long long base;
        unsigned long long size;
        int ret;

        file = filp_open(ISOLATION_FILE, O_RDONLY, 0);
        if (IS_ERR(file))
                return PTR_ERR(file);

        len = kernel_read(file, buffer, sizeof(buffer) - 1, &pos);
        filp_close(file, NULL);

        if (len < 0)
                return len;

        buffer[len] = '\0';

        ret = sscanf(buffer,
                     "ISOLATION_A: physical_base=0x%llx\n"
                     "ISOLATION_A: size=0x%llx",
                     &base, &size);
        if (ret != 2)
                return -EINVAL;

        /*
         * Vérifier que les valeurs lues entrent sans troncature dans les
         * types employés par le noyau.
         */
        isolation_a_start = (phys_addr_t)base;
        isolation_a_size = (size_t)size;

        if ((unsigned long long)isolation_a_start != base ||
            (unsigned long long)isolation_a_size != size)
                return -ERANGE;

        return 0;
}

static int isolation_b_validate_region(phys_addr_t *physical_end)
{
        if (!isolation_a_start || !isolation_a_size)
                return -EINVAL;

        /*
         * cma_alloc() alloue des pages : base et taille doivent être
         * multiples de PAGE_SIZE.
         */
        if (!PAGE_ALIGNED(isolation_a_start) ||
            !PAGE_ALIGNED(isolation_a_size))
                return -EINVAL;

        /*
         * Évite un dépassement lors du calcul de :
         * end = base + size - 1.
         */
        if (check_add_overflow(isolation_a_start,
                               (phys_addr_t)isolation_a_size - 1,
                               physical_end))
                return -ERANGE;

        return 0;
}

static int isolation_b_write_cma_region(phys_addr_t physical_end)
{
        void *mapped_addr;
        u8 first_byte;
        u8 last_byte;

        /*
         * Mappe exactement la taille publiée :
         *
         * start = 0x100000000
         * size  = 0x1f400000 (500 MiB)
         *
         * MEMREMAP_WB est approprié à une zone CMA de RAM normale :
         * lecture/écriture cacheable Write-Back.
         */
        mapped_addr = memremap(isolation_a_start,
                               isolation_a_size,
                               MEMREMAP_WB);
        if (!mapped_addr)
                return -ENOMEM;

        /*
         * Écriture sur toute la zone CMA.
         *
         * À ne faire que si cette allocation CMA est explicitement détenue
         * ou partagée avec l’instance B, et n’est pas libérée en parallèle.
         */
        memset(mapped_addr, 0xAA, isolation_a_size);

        /*
         * Lit le premier et le dernier octet de la zone pour vérifier que
         * les écritures sont visibles localement.
         */
        first_byte = READ_ONCE(*(u8 *)mapped_addr);
        last_byte = READ_ONCE(*((u8 *)mapped_addr +
                                isolation_a_size - 1));

        /*
         * Si A utilise un drapeau de synchronisation partagé, une barrière
         * doit être exécutée avant de publier ce drapeau.
         */
        smp_wmb();

        scnprintf(result_buffer, sizeof(result_buffer),
                  "ISOLATION_B: init entered\n"
                  "ISOLATION_B: write completed\n"
                  "ISOLATION_B: base=0x%llx\n"
                  "ISOLATION_B: end=0x%llx\n"
                  "ISOLATION_B: size=0x%zx (%zu MiB)\n"
                  "ISOLATION_B: first=0x%02x last=0x%02x\n",
                  (unsigned long long)isolation_a_start,
                  (unsigned long long)physical_end,
                  isolation_a_size,
                  isolation_a_size >> 20,
                  first_byte,
                  last_byte);

        memunmap(mapped_addr);
        return 0;
}

static int __init isolation_test_init(void)
{
        phys_addr_t physical_end;
        int ret;

        /*
         * Étape 1 : créer le fichier de diagnostic avant toute opération
         * susceptible d’échouer ou de provoquer un fault.
         */
        result_entry = proc_create("isolation_b_result", 0444, NULL,
                                   &isolation_proc_ops);
        if (!result_entry)
                return -ENOMEM;

        /*
         * Confirmation immédiate : si le module réussit à atteindre cette
         * ligne, le fichier le montrera.
         */
        scnprintf(result_buffer, sizeof(result_buffer),
                  "ISOLATION_B: init entered\n");

        /*
         * Étape 2 : lire les informations publiées par l’instance A.
         */
        ret = isolation_b_read_metadata();
        if (ret) {
                scnprintf(result_buffer, sizeof(result_buffer),
                          "ISOLATION_B: init entered\n"
                          "ISOLATION_B: metadata read/parse failed: %d\n",
                          ret);
                return 0;
        }

        /*
         * Étape 3 : valider base, taille et calcul de fin.
         */
        ret = isolation_b_validate_region(&physical_end);
        if (ret) {
                scnprintf(result_buffer, sizeof(result_buffer),
                          "ISOLATION_B: init entered\n"
                          "ISOLATION_B: invalid region: %d\n"
                          "ISOLATION_B: base=0x%llx size=0x%zx\n",
                          ret,
                          (unsigned long long)isolation_a_start,
                          isolation_a_size);
                return 0;
        }

        /*
         * Étape 4 : mapper et écrire dans la région CMA.
         */
        ret = isolation_b_write_cma_region(physical_end);
        if (ret) {
                scnprintf(result_buffer, sizeof(result_buffer),
                          "ISOLATION_B: init entered\n"
                          "ISOLATION_B: mapping/write failed: %d\n"
                          "ISOLATION_B: base=0x%llx size=0x%zx\n",
                          ret,
                          (unsigned long long)isolation_a_start,
                          isolation_a_size);
                return 0;
        }

        return 0;
}

static void __exit isolation_test_exit(void)
{
        if (result_entry)
                proc_remove(result_entry);

        result_entry = NULL;
}

module_init(isolation_test_init);
module_exit(isolation_test_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kerf CMA shared-memory write test - instance B");
