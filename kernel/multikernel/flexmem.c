/* Eric OKALA 
 * Implementation du pool memoire multikernel kernel flexible
*/

#include <linux/cma.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/types.h>
#include <linux/genalloc.h>

static phys_addr_t mk_flex_pool_size;
static struct cma *mk_flex_cma;



static int __init mkkernel_flex_pool_setup(char *str)
{
	char *cur = str;
	phys_addr_t size;

	if (!str || !*str)
		return -EINVAL;

	size = memparse(cur, &cur);

	if (!size) {
		pr_err("EO_multikernel: invalid mkkernel_flex_pool=%s\n", str);
		return -EINVAL;
	}

	if (*cur != '\0') {
		pr_err("EO_multikernel: invalid mkkernel_flex_pool=%s\n", str);
		return -EINVAL;
	}

	if (!IS_ALIGNED(size, PAGE_SIZE)) {
		pr_err("EO_multikernel: mkkernel_flex_pool size is not page aligned: 0x%llx\n",
		       (unsigned long long)size);
		return -EINVAL;
	}

	mk_flex_pool_size = size;

	pr_info("EO_multikernel: flexible CMA pool requested: 0x%llx bytes\n",
		(unsigned long long)mk_flex_pool_size);

	return 0;
}
early_param("mkkernel_flex_pool", mkkernel_flex_pool_setup);

void __init multikernel_flexmem_reserve(void)
{
	int ret;
	phys_addr_t base;

	if (!mk_flex_pool_size)
		return;

	if (mk_flex_cma) {
		pr_warn("EO_multikernel: flexible CMA pool already initialized\n");
		return;
	}

	pr_info("EO_multikernel: declaring flexible CMA pool: 0x%llx bytes\n",
		(unsigned long long)mk_flex_pool_size);

	ret = cma_declare_contiguous(
		0,
		mk_flex_pool_size,
		0,
		0,
		0,
		false,
		"multikernel-flex",
		&mk_flex_cma);

	if (ret) {
		pr_err("EO_multikernel: failed to declare flexible CMA pool: %d\n",
		       ret);
		mk_flex_cma = NULL;
		return;
	}

	base = cma_get_base(mk_flex_cma);

	pr_info("EO_multikernel: flexible CMA pool created: base=%pa size=0x%llx\n",
		&base, (unsigned long long)cma_get_size(mk_flex_cma));
}


int multikernel_flexmem_acquire(size_t size, phys_addr_t *base)
{
	struct page *pages;
	unsigned long nr_pages;
	unsigned int align_order;

	if (!base)
		return -EINVAL;

	*base = 0;

	if (!mk_flex_cma)
		return -ENODEV;

	if (!size)
		return -EINVAL;

	if (!IS_ALIGNED(size, PAGE_SIZE))
		return -EINVAL;

	nr_pages = size >> PAGE_SHIFT;
	align_order = 0;

	pages = cma_alloc(mk_flex_cma, nr_pages, align_order, false);
	if (!pages) {
		pr_err("EO_multikernel: CMA allocation failed: size=%zu\n",
		       size);
		return -ENOMEM;
	}

	*base = page_to_phys(pages);

	pr_info("EO_multikernel: acquired %zu bytes from flexible CMA: "
		"base=%pa\n", size, base);

	return 0;
}

void multikernel_flexmem_release(phys_addr_t base, size_t size)
{
	struct page *pages;
	unsigned long nr_pages;

	if (!mk_flex_cma || !base || !size)
		return;

	if (!IS_ALIGNED(base, PAGE_SIZE) ||
	    !IS_ALIGNED(size, PAGE_SIZE)) {
		pr_err("EO_multikernel: invalid CMA release: "
		       "base=%pa size=%zu\n", &base, size);
		return;
	}

	nr_pages = size >> PAGE_SHIFT;
	pages = phys_to_page(base);

	if (!cma_release(mk_flex_cma, pages, nr_pages)) {
		pr_err("EO_multikernel: CMA release failed: "
		       "base=%pa size=%zu\n", &base, size);
		return;
	}

	pr_info("EO_multikernel: released %zu bytes to flexible CMA: "
		"base=%pa\n", size, &base);
}



int multikernel_flexmem_create_instance_pool(int instance_id, size_t pool_size, int min_alloc_order,
						void **pool_handle, phys_addr_t *base)
{
	struct gen_pool *pool;
	phys_addr_t phys;
	int ret;

	if (!pool_handle || !base)
		return -EINVAL;

	*pool_handle = NULL;
	*base = 0;

	if (!mk_flex_cma)
		return -ENODEV;

	if (!pool_size)
		return -EINVAL;

	if (!IS_ALIGNED(pool_size, PAGE_SIZE))
		return -EINVAL;

	if (min_alloc_order < PAGE_SHIFT)
		return -EINVAL;

	ret = multikernel_flexmem_acquire(pool_size, &phys);
	if (ret)
		return ret;

	pool = gen_pool_create(min_alloc_order, -1);
	if (!pool) {
		pr_err("EO_multikernel: failed to create gen_pool for instance %d\n", instance_id);
		multikernel_flexmem_release(phys, pool_size);
		return -ENOMEM;
	}

	ret = gen_pool_add(pool, phys, pool_size, -1);
	if (ret) {
		pr_err("EO_multikernel: failed to add CMA region to instance pool %d: %d\n",
		       instance_id, ret);
		gen_pool_destroy(pool);
		multikernel_flexmem_release(phys, pool_size);
		return ret;
	}

	*pool_handle = pool;
	*base = phys;

	pr_info("EO_multikernel: created flexible instance pool %d: base=%pa size=%zu bytes\n",
		instance_id, &phys, pool_size);

	return 0;
}


void multikernel_flexmem_destroy_instance_pool(void *pool_handle, phys_addr_t base, size_t size)
{
	struct gen_pool *pool = pool_handle;

	if (!pool)
		return;

	gen_pool_destroy(pool);

	multikernel_flexmem_release(base, size);

	pr_info("EO_multikernel: destroyed flexible instance pool: base=%pa size=%zu bytes\n",
			&base, size);
}

bool multikernel_flexmem_enabled(void)
{
	return mk_flex_cma != NULL;
}

size_t multikernel_flexmem_size(void)
{
    return mk_flex_pool_size;
}
