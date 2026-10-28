// SPDX-License-Identifier: GPL-2.0-only
/*
 * Example of a host reclaim policy provided by a module.
 *
 * "ready-first" spares the instances that run a test: it stops first the
 * instances that are not running (ready or loaded), then the running
 * ones, each group from the smallest to the largest, until what they
 * hold covers what is missing. It works the same for the memory
 * (flex_reclaim) and the CPUs (cpu_reclaim):
 *
 *   insmod reclaim_policy_ready_first.ko
 *   echo "2 ready-first" > /sys/fs/multikernel/cpu_reclaim
 *
 * The module cannot be unloaded while the last request of a resource
 * names its policy: write another request (or 0) first.
 */

#include <linux/module.h>
#include <linux/sort.h>
#include <linux/multikernel.h>

static int rf_cmp(const void *a, const void *b)
{
	const struct mk_reclaim_candidate *x = a, *y = b;
	bool xrun = x->state == MK_STATE_ACTIVE;
	bool yrun = y->state == MK_STATE_ACTIVE;

	if (xrun != yrun)
		return xrun ? 1 : -1;
	if (x->amount != y->amount)
		return x->amount < y->amount ? -1 : 1;
	/* sort() is not stable: the oldest first among equals */
	return x->order < y->order ? -1 : x->order > y->order;
}

static void rf_select(struct mk_reclaim_candidate *cand, int nr,
		      unsigned long missing, const char *resource)
{
	unsigned long got = 0;
	int i;

	sort(cand, nr, sizeof(*cand), rf_cmp, NULL);
	for (i = 0; i < nr && got < missing; i++) {
		cand[i].victim = true;
		got += cand[i].amount;
	}
	pr_info("ready-first: %s, %lu missing, %d candidate(s)\n",
		resource, missing, nr);
}

static struct mk_reclaim_policy rf_policy = {
	.name = "ready-first",
	.select = rf_select,
	.owner = THIS_MODULE,
};

static int __init rf_init(void)
{
	return mk_reclaim_register_policy(&rf_policy);
}

static void __exit rf_exit(void)
{
	mk_reclaim_unregister_policy(&rf_policy);
}

module_init(rf_init);
module_exit(rf_exit);
MODULE_DESCRIPTION("Multikernel host reclaim policy sparing running instances");
MODULE_LICENSE("GPL");
