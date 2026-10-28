/* Eric OKALA 
 * Implementation du pool memoire multikernel kernel flexible
*/

#include <linux/cma.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/types.h>
#include <linux/genalloc.h>
#include <linux/gfp.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/sched/signal.h>
#include <linux/vmstat.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/multikernel.h>
#include "internal.h"

static phys_addr_t mk_flex_pool_size;
static struct cma *mk_flex_cma;
/* Pages of the pool currently held by instances (cma_alloc'ed) */
static atomic_long_t mk_flex_instance_pages = ATOMIC_LONG_INIT(0);

/*
 * State of the pool. NR_FREE_CMA_PAGES covers every CMA area, the
 * flexible pool being the only one on multikernel hosts.
 */
struct mk_flex_usage {
	unsigned long pool;
	unsigned long instances;
	unsigned long free;
	unsigned long lent;
};

static void multikernel_flexmem_usage(struct mk_flex_usage *u)
{
	u->pool = cma_get_size(mk_flex_cma) >> PAGE_SHIFT;
	u->instances = atomic_long_read(&mk_flex_instance_pages);
	u->free = global_zone_page_state(NR_FREE_CMA_PAGES);
	u->lent = u->pool > u->instances + u->free ?
		  u->pool - u->instances - u->free : 0;
}

static void multikernel_flexmem_policy_name(char *buf, size_t len)
{
	int pct = cma_get_borrow_threshold();

	if (pct < 0)
		strscpy(buf, "default", len);
	else if (!pct)
		strscpy(buf, "never", len);
	else
		snprintf(buf, len, "below %d%%", pct);
}



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

/*
 * Borrow policy of the flexible CMA pool: "default", "never" or a
 * percentage 1..99 (see CMA_BORROW_DEFAULT in <linux/cma.h>).
 * Shared by the boot parameter and the kernfs flex_borrow file.
 */
int multikernel_flexmem_parse_borrow(const char *str, int *pct)
{
	if (!str || !*str || !pct)
		return -EINVAL;

	if (!strcmp(str, "default")) {
		*pct = CMA_BORROW_DEFAULT;
		return 0;
	}

	if (!strcmp(str, "never")) {
		*pct = 0;
		return 0;
	}

	if (kstrtoint(str, 10, pct) || *pct < 1 || *pct > 99)
		return -EINVAL;

	return 0;
}

static void multikernel_flexmem_log_borrow(int pct)
{
	if (pct < 0)
		pr_info("EO_multikernel: CMA borrow policy: default\n");
	else if (!pct)
		pr_info("EO_multikernel: CMA borrow policy: never\n");
	else
		pr_info("EO_multikernel: CMA borrow policy: below %d%% free non-CMA\n",
			pct);
}

/*
 * mkkernel_flex_borrow=default|never|<1-99>
 * Controls when the buddy allocator may lend the flexible CMA pool to
 * movable allocations of the root kernel.
 */
static int __init mkkernel_flex_borrow_setup(char *str)
{
	int pct;

	if (multikernel_flexmem_parse_borrow(str, &pct)) {
		pr_err("EO_multikernel: invalid mkkernel_flex_borrow=%s\n", str);
		return -EINVAL;
	}

	/* Too early to drain per-cpu lists, and nothing has been lent yet */
	cma_set_borrow_threshold(pct);
	multikernel_flexmem_log_borrow(pct);

	return 0;
}
early_param("mkkernel_flex_borrow", mkkernel_flex_borrow_setup);

static DEFINE_MUTEX(mk_flex_borrow_mutex);

/*
 * Runtime policy change. Per-cpu free lists may still hold CMA pages
 * taken while borrowing was allowed, and they are handed out without
 * any ALLOC_CMA check. When the policy becomes stricter, drain them
 * back to the buddy MIGRATE_CMA free lists where the policy applies.
 */
void multikernel_flexmem_set_borrow(int pct)
{
	int old;
	bool stricter;

	mutex_lock(&mk_flex_borrow_mutex);

	old = cma_get_borrow_threshold();
	stricter = pct >= 0 && (old < 0 || pct < old);

	cma_set_borrow_threshold(pct);
	if (stricter)
		drain_all_pages(NULL);

	mutex_unlock(&mk_flex_borrow_mutex);

	multikernel_flexmem_log_borrow(pct);
}

/*
 * Host reclaim of the flexible pool (flex_reclaim file). The host has
 * priority over the whole pool: a request of @size bytes says the host
 * needs that much memory, and once granted every free page of the pool
 * may be lent to the host, whatever the borrow policy. Instance creation
 * is unchanged, cma_alloc() migrates the lent pages away. A request of 0
 * ends the host priority and the borrow policy applies again.
 *
 * Writing a request only records it and wakes the monitor up (reclaim.c),
 * which handles it and then runs again every flex_reclaim_interval_ms.
 * A new request replaces one still pending: only the latest need counts.
 *
 * When the pool has not enough free pages, the policy of the request
 * picks the instances to stop (victims) until their memory covers what
 * is missing, and the engine stops and deletes them (reclaim.c).
 */
static size_t mk_flex_reclaim_size;	/* last request, in bytes */
static const struct mk_reclaim_policy *mk_flex_reclaim_policy;
static bool mk_flex_reclaim_pending;	/* not handled by the monitor yet */
static int mk_flex_reclaim_result;	/* 0: granted, else why it was not */

/* An instance holds the pages of its flexible pool */
static unsigned long multikernel_flexmem_reclaim_amount(struct mk_instance *instance)
{
	return instance->flexmem_size >> PAGE_SHIFT;
}

static unsigned long multikernel_flexmem_reclaim_shown(unsigned long pages)
{
	return pages >> (20 - PAGE_SHIFT);
}

static void multikernel_flexmem_reclaim_monitor(void);

/* Its victims are protected by mk_flex_borrow_mutex */
static struct mk_reclaim_resource mk_flex_reclaim_res = {
	.unit = "MB",
	.amount = multikernel_flexmem_reclaim_amount,
	.shown = multikernel_flexmem_reclaim_shown,
	.process = multikernel_flexmem_reclaim_monitor,
};

static const char *multikernel_flexmem_reclaim_reason(int err)
{
	switch (err) {
	case 0:
		return "granted";
	case -ENOSPC:
		return "refused: larger than the pool";
	case -EBUSY:
		return "refused: not enough memory even if every instance is stopped";
	case -ENOMEM:
		return "refused: the policy did not select enough victims";
	case -EAGAIN:
		return "pending, stopping the victims";
	default:
		return "refused";
	}
}

/* Called by the monitor with mk_flex_borrow_mutex held */
static void multikernel_flexmem_reclaim_process(size_t size)
{
	struct mk_reclaim_victims *vs = &mk_flex_reclaim_res.victims;
	struct mk_flex_usage u;
	unsigned long nr_pages = DIV_ROUND_UP(size, PAGE_SIZE);
	int ret = 0;

	if (!size) {
		bool was_on = cma_get_host_priority();

		cma_set_host_priority(false);
		/* Same as a stricter policy: CMA pages left on per-cpu lists */
		if (was_on && cma_get_borrow_threshold() >= 0)
			drain_all_pages(NULL);
		mk_reclaim_victims_reset(&mk_flex_reclaim_res);
		mk_flex_reclaim_result = 0;
		pr_info("EO_multikernel: host priority over the flexible CMA pool ended\n");
		return;
	}

	multikernel_flexmem_usage(&u);
	/* Victims are chosen once: a retry only stops the remaining ones */
	if (!vs->chosen && nr_pages > u.free) {
		/* Free pages may sit on per-cpu lists, uncounted */
		drain_all_pages(NULL);
		multikernel_flexmem_usage(&u);
		if (nr_pages > u.free) {
			pr_info("EO_multikernel: host reclaim of %zu MB (%s): free %lu MB in the pool, %lu MB missing, choosing victims\n",
				size >> 20, mk_flex_reclaim_policy->name,
				u.free >> (20 - PAGE_SHIFT),
				(nr_pages - u.free) >> (20 - PAGE_SHIFT));
			ret = mk_reclaim_select_victims(&mk_flex_reclaim_res,
							mk_flex_reclaim_policy,
							nr_pages - u.free);
		}
	}

	/*
	 * Once its victims are deleted the request is granted, without
	 * waiting for their memory: an instance still referenced (an open
	 * kernfs file) frees it later, and counting the pool again could
	 * make the policy stop more instances than needed.
	 */
	if (!ret && vs->chosen)
		ret = mk_reclaim_remove_victims(&mk_flex_reclaim_res,
						mk_flex_reclaim_policy->name);

	mk_flex_reclaim_result = ret;
	if (ret == -EAGAIN) {
		mk_flex_reclaim_pending = true;
		return;
	}
	if (!ret)
		cma_set_host_priority(true);

	if (ret)
		pr_warn("EO_multikernel: host reclaim of %zu MB (%s) %s (free %lu MB in the pool)\n",
			size >> 20, mk_flex_reclaim_policy->name,
			multikernel_flexmem_reclaim_reason(ret),
			u.free >> (20 - PAGE_SHIFT));
	else if (vs->nr)
		pr_info("EO_multikernel: host reclaim of %zu MB granted after stopping %d instance(s), the pool is lendable to the host\n",
			size >> 20, vs->nr);
	else
		pr_info("EO_multikernel: host reclaim of %zu MB granted, %lu MB free in the pool now lendable to the host\n",
			size >> 20, u.free >> (20 - PAGE_SHIFT));
}

static void multikernel_flexmem_reclaim_monitor(void)
{
	mutex_lock(&mk_flex_borrow_mutex);
	if (mk_flex_reclaim_pending) {
		mk_flex_reclaim_pending = false;
		multikernel_flexmem_reclaim_process(mk_flex_reclaim_size);
	}
	mutex_unlock(&mk_flex_borrow_mutex);
}

int multikernel_flexmem_reclaim(size_t size, const char *policy_name)
{
	const struct mk_reclaim_policy *policy;

	if (!mk_flex_cma)
		return -ENODEV;

	policy = mk_reclaim_find_policy(policy_name);
	if (!policy)
		return -EINVAL;

	/* Known at once: even with every instance stopped it cannot fit */
	if (DIV_ROUND_UP(size, PAGE_SIZE) > cma_get_size(mk_flex_cma) >> PAGE_SHIFT) {
		mutex_lock(&mk_flex_borrow_mutex);
		mk_flex_reclaim_size = size;
		mk_flex_reclaim_policy = policy;
		mk_flex_reclaim_pending = false;
		mk_flex_reclaim_result = -ENOSPC;
		mk_reclaim_victims_reset(&mk_flex_reclaim_res);
		mutex_unlock(&mk_flex_borrow_mutex);
		pr_warn("EO_multikernel: host reclaim of %zu MB %s\n",
			size >> 20, multikernel_flexmem_reclaim_reason(-ENOSPC));
		return -ENOSPC;
	}

	mutex_lock(&mk_flex_borrow_mutex);
	mk_flex_reclaim_size = size;
	mk_flex_reclaim_policy = policy;
	mk_flex_reclaim_pending = true;
	mk_flex_reclaim_result = 0;
	mk_reclaim_victims_reset(&mk_flex_reclaim_res);
	mutex_unlock(&mk_flex_borrow_mutex);

	mk_reclaim_wake();
	return 0;
}

void multikernel_flexmem_reclaim_show(struct seq_file *sf)
{
	struct mk_flex_usage u;
	char policy[16];

	if (!mk_flex_cma) {
		seq_puts(sf, "no flexible pool\n");
		return;
	}

	mutex_lock(&mk_flex_borrow_mutex);
	multikernel_flexmem_usage(&u);
	multikernel_flexmem_policy_name(policy, sizeof(policy));

	seq_printf(sf, "host priority: %s\n",
		   cma_get_host_priority() ? "on" : "off");
	if (mk_flex_reclaim_pending && mk_flex_reclaim_result != -EAGAIN)
		seq_printf(sf, "last request: %zu MB (%s), pending\n",
			   mk_flex_reclaim_size >> 20, mk_flex_reclaim_policy->name);
	else if (mk_flex_reclaim_size)
		seq_printf(sf, "last request: %zu MB (%s), %s\n",
			   mk_flex_reclaim_size >> 20, mk_flex_reclaim_policy->name,
			   multikernel_flexmem_reclaim_reason(mk_flex_reclaim_result));
	else
		seq_puts(sf, "last request: none\n");
	mk_reclaim_victims_show(&mk_flex_reclaim_res, sf);
	seq_printf(sf, "pool: %lu MB, instances %lu MB, free %lu MB, lent to the host %lu MB\n",
		   u.pool >> (20 - PAGE_SHIFT), u.instances >> (20 - PAGE_SHIFT),
		   u.free >> (20 - PAGE_SHIFT), u.lent >> (20 - PAGE_SHIFT));
	seq_printf(sf, "borrow policy: %s\n", policy);
	seq_printf(sf, "monitor period: %u ms\n", mk_reclaim_interval());
	seq_puts(sf, "policies:");
	mk_reclaim_policies_show(sf);
	seq_putc(sf, '\n');
	mutex_unlock(&mk_flex_borrow_mutex);
}

/* The pool is declared before the monitor can start: register here */
static int __init multikernel_flexmem_monitor_init(void)
{
	if (mk_flex_cma)
		mk_reclaim_register(&mk_flex_reclaim_res);
	return 0;
}
late_initcall(multikernel_flexmem_monitor_init);

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


/*
 * Explain why the pool could not provide a contiguous range. The pool
 * holds three kinds of pages: those of the instances, free ones, and
 * those lent to the host (CMA borrowing). Only lent pages can be pinned;
 * free space split by instances or lent pages means fragmentation.
 * NR_FREE_CMA_PAGES covers every CMA area, the flexible pool being the
 * only one on multikernel hosts.
 */
static void multikernel_flexmem_report_failure(size_t size)
{
	struct mk_flex_usage u;
	unsigned long pool, instances, free, lent;
	char policy[16];

	multikernel_flexmem_usage(&u);
	pool = u.pool;
	instances = u.instances;
	free = u.free;
	lent = u.lent;
	multikernel_flexmem_policy_name(policy, sizeof(policy));

	pr_err("EO_multikernel: cannot reserve %zu MB for an instance in the flexible CMA pool\n",
	       size >> 20);
	pr_err("EO_multikernel: pool %lu MB: instances %lu MB, free %lu MB, lent to the host %lu MB (borrow policy: %s)\n",
	       pool >> (20 - PAGE_SHIFT), instances >> (20 - PAGE_SHIFT),
	       free >> (20 - PAGE_SHIFT), lent >> (20 - PAGE_SHIFT), policy);

	if (instances + (size >> PAGE_SHIFT) > pool)
		pr_err("EO_multikernel: not enough room left in the pool for this instance\n");
	/* Below a pageblock, lent is counter drift or per-cpu lists, not a cause */
	else if (lent >= pageblock_nr_pages)
		pr_err("EO_multikernel: pages lent to the host could not be migrated (pinned or in use); flex_borrow=never keeps the pool free for instances\n");
	else
		pr_err("EO_multikernel: no contiguous free range of that size, the pool is fragmented\n");
}

/*
 * A short-term pin (O_DIRECT I/O, a page under writeback) makes a range
 * unmigratable for a few milliseconds only: retry a couple of times after
 * a pause before giving up. Long-term pins never sit in CMA, gup moves
 * those pages out when they are pinned.
 */
#define MK_FLEX_ACQUIRE_ATTEMPTS	3
#define MK_FLEX_ACQUIRE_DELAY_MS	100

int multikernel_flexmem_acquire(size_t size, phys_addr_t *base)
{
	struct page *pages = NULL;
	unsigned long nr_pages;
	unsigned int align_order;
	int attempt;

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
	/*
	 * Start on a pageblock boundary. When a candidate range holds a page
	 * that cannot be migrated (e.g. pinned), cma_alloc() retries at the
	 * next aligned start: with order 0 that is one page further, i.e. up
	 * to one full-size migration attempt per page of the request.
	 */
	align_order = pageblock_order;

	for (attempt = 1; attempt <= MK_FLEX_ACQUIRE_ATTEMPTS; attempt++) {
		/* Only the last attempt lets CMA report its own failure */
		pages = cma_alloc(mk_flex_cma, nr_pages, align_order,
				  attempt < MK_FLEX_ACQUIRE_ATTEMPTS);
		if (pages || attempt == MK_FLEX_ACQUIRE_ATTEMPTS)
			break;

		/* kerf was interrupted (Ctrl-C): do not keep it waiting */
		if (fatal_signal_pending(current))
			break;

		pr_info("EO_multikernel: CMA allocation of %zu bytes failed (attempt %d/%d), retrying in %d ms\n",
			size, attempt, MK_FLEX_ACQUIRE_ATTEMPTS,
			MK_FLEX_ACQUIRE_DELAY_MS);
		msleep(MK_FLEX_ACQUIRE_DELAY_MS);
	}

	if (!pages) {
		if (fatal_signal_pending(current)) {
			pr_info("EO_multikernel: CMA allocation of %zu bytes interrupted\n",
				size);
			return -EINTR;
		}
		multikernel_flexmem_report_failure(size);
		return -ENOMEM;
	}

	atomic_long_add(nr_pages, &mk_flex_instance_pages);
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

	atomic_long_sub(nr_pages, &mk_flex_instance_pages);

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

/* Actual pool size: CMA rounds the requested size up to a pageblock */
size_t multikernel_flexmem_size(void)
{
	return mk_flex_cma ? cma_get_size(mk_flex_cma) : 0;
}
