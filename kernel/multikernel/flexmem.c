/* Eric OKALA 
 * Implementation du pool memoire multikernel kernel flexible
*/

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/types.h>

static phys_addr_t mk_flex_pool_size;

static int __init mkkernel_flex_pool_setup(char *str)
{
	phys_addr_t size;

	if (!str || !*str)
		return -EINVAL;

	size = memparse(str, NULL);

	if (!size) {
		pr_err("EO_multikernel: invalid mkkernel_flex_pool=%s\n", str);
		return -EINVAL;
	}

	mk_flex_pool_size = size;

	pr_info("EO_multikernel: flexible memory pool configured: 0x%llx bytes\n",
		(unsigned long long)mk_flex_pool_size);

	return 0;
}

early_param("mkkernel_flex_pool", mkkernel_flex_pool_setup);
