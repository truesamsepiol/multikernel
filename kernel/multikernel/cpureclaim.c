/* Eric OKALA
 * Reclamation des CPU du pool multikernel par l'hote
*/

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/bitmap.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/seq_file.h>
#include <linux/multikernel.h>
#include <asm/smp.h>
#include "internal.h"

/*
 * Host reclaim of the CPUs of the pool (cpu_reclaim file). The host has
 * priority over the pool: a request of @count CPUs takes that many free
 * CPUs of the pool and brings them online for the host, where they stay
 * until the next kerf init. A request larger than the pool gets all of
 * it; only an empty pool is refused. A request of 0 only clears the
 * request: the CPUs given stay with the host.
 *
 * A CPU of the pool is free when its bit is in the pool (root instance)
 * and it is offline; it is held when an instance has it. Instances give
 * their CPUs back to the host when deleted (mk_instance_return_cpus()).
 *
 * Writing a request only records it and wakes the monitor up (reclaim.c).
 * The free CPUs of the pool come first: an instance is only stopped when
 * they are not enough, and then the policy of the request picks the
 * instances (victims) whose CPUs cover what is missing. All the CPUs of
 * a victim go to the host, which may get a few more than asked. The
 * victims are stopped and deleted by the engine (reclaim.c), their CPUs
 * coming back to the host when they are released.
 */
static DEFINE_MUTEX(mk_cpu_reclaim_mutex);
static unsigned int mk_cpu_reclaim_count;	/* last request, in CPUs */
static const struct mk_reclaim_policy *mk_cpu_reclaim_policy;
static bool mk_cpu_reclaim_pending;	/* not handled by the monitor yet */
static int mk_cpu_reclaim_result;	/* 0: granted, else why it was not */
static unsigned int mk_cpu_reclaim_given;	/* free CPUs given for it */
static bool mk_cpu_reclaim_whole_pool;	/* it asked more than the pool */
static struct mk_reclaim_resource mk_cpu_reclaim_res;

struct mk_cpu_usage {
	unsigned int pool;	/* free + held */
	unsigned int instances;	/* held by instances */
	unsigned int free;
};

static bool mk_cpu_offline(int phys_cpu)
{
	int logical_cpu = arch_cpu_from_physical_id(phys_cpu);

	return logical_cpu >= 0 && !cpu_online(logical_cpu);
}

/* mk_instance_mutex held */
static void mk_cpu_usage(struct mk_cpu_usage *u)
{
	struct mk_instance *instance;
	int phys_cpu;

	lockdep_assert_held(&mk_instance_mutex);

	u->free = 0;
	u->instances = 0;
	if (root_instance && root_instance->cpus)
		for_each_set_bit(phys_cpu, root_instance->cpus, NR_CPUS)
			if (mk_cpu_offline(phys_cpu))
				u->free++;

	list_for_each_entry(instance, &mk_instance_list, list)
		if (instance != root_instance && instance->cpus)
			u->instances += bitmap_weight(instance->cpus, NR_CPUS);

	u->pool = u->free + u->instances;
}

/*
 * Take @n free CPUs out of the pool into @taken, lowest APIC IDs first.
 * Under mk_instance_mutex, as kerf create: once their bit is cleared no
 * instance can be given them, they are onlined afterwards.
 */
static unsigned int mk_cpu_take_free(unsigned long *taken, unsigned int n)
{
	unsigned int got = 0;
	int phys_cpu;

	lockdep_assert_held(&mk_instance_mutex);

	for_each_set_bit(phys_cpu, root_instance->cpus, NR_CPUS) {
		if (got == n)
			break;
		if (!mk_cpu_offline(phys_cpu))
			continue;
		clear_bit(phys_cpu, root_instance->cpus);
		set_bit(phys_cpu, taken);
		got++;
	}
	return got;
}

/*
 * Bring the CPUs of @taken online for the host. One that cannot be goes
 * back to the pool, still offline and free. Returns how many were given.
 */
static unsigned int mk_cpu_give_to_host(const unsigned long *taken)
{
	unsigned int given = 0;
	int phys_cpu, logical_cpu, ret;

	for_each_set_bit(phys_cpu, taken, NR_CPUS) {
		logical_cpu = arch_cpu_from_physical_id(phys_cpu);
		ret = add_cpu(logical_cpu);
		if (ret) {
			mutex_lock(&mk_instance_mutex);
			set_bit(phys_cpu, root_instance->cpus);
			mutex_unlock(&mk_instance_mutex);
			pr_err("EO_multikernel: host CPU reclaim: cannot online CPU %d (logical %d): %d, left in the pool\n",
			       phys_cpu, logical_cpu, ret);
			continue;
		}
		pr_info("EO_multikernel: host CPU reclaim: CPU %d (logical %d) given to the host\n",
			phys_cpu, logical_cpu);
		given++;
	}
	return given;
}

static const char *mk_cpu_reclaim_reason(int err)
{
	switch (err) {
	case 0:
		return "granted";
	case -ENOSPC:
		return "refused: no CPU in the pool";
	case -EBUSY:
		return "refused: not enough CPUs even if every instance is stopped";
	case -ENOMEM:
		return "refused: the policy did not select enough victims";
	case -EAGAIN:
		return "pending, stopping the victims";
	default:
		return "refused";
	}
}

/* CPUs held by the victims chosen for the request */
static unsigned long mk_cpu_victims_cpus(void)
{
	struct mk_reclaim_victims *vs = &mk_cpu_reclaim_res.victims;
	unsigned long cpus = 0;
	int i;

	for (i = 0; i < vs->nr; i++)
		cpus += vs->v[i].c.amount;
	return cpus;
}

/*
 * Called by the monitor with mk_cpu_reclaim_mutex held. The free CPUs are
 * given and the victims chosen once per request: a retry (-EAGAIN, a
 * lock held by kerf) only stops the remaining victims.
 */
static void mk_cpu_reclaim_process(unsigned int count)
{
	struct mk_reclaim_victims *vs = &mk_cpu_reclaim_res.victims;
	struct mk_cpu_usage u = {};
	unsigned long *taken = NULL;
	unsigned int target;
	int ret = 0;

	if (!count) {
		mk_reclaim_victims_reset(&mk_cpu_reclaim_res);
		mk_cpu_reclaim_given = 0;
		mk_cpu_reclaim_whole_pool = false;
		mk_cpu_reclaim_result = 0;
		pr_info("EO_multikernel: host CPU reclaim cleared, the CPUs given stay with the host\n");
		return;
	}

	if (!vs->chosen) {
		taken = bitmap_zalloc(NR_CPUS, GFP_KERNEL);
		if (!taken) {
			ret = -ENOMEM;
			goto out;
		}

		mutex_lock(&mk_instance_mutex);
		mk_cpu_usage(&u);
		mutex_unlock(&mk_instance_mutex);
		if (!u.pool) {
			ret = -ENOSPC;
			goto out;
		}

		/* More than the pool: the host gets all of it */
		target = min(count, u.pool);
		mk_cpu_reclaim_whole_pool = count > u.pool;

		/* The free CPUs first, instances only for what they miss */
		if (u.free < target) {
			pr_info("EO_multikernel: host reclaim of %u CPUs (%s): %u free CPUs in the pool, %u missing, choosing victims\n",
				count, mk_cpu_reclaim_policy->name, u.free,
				target - u.free);
			ret = mk_reclaim_select_victims(&mk_cpu_reclaim_res,
							mk_cpu_reclaim_policy,
							target - u.free);
			if (ret)
				goto out;
		}

		/* kerf create may have taken one meanwhile: fewer are given */
		mutex_lock(&mk_instance_mutex);
		mk_cpu_take_free(taken, min(target, u.free));
		mutex_unlock(&mk_instance_mutex);
		mk_cpu_reclaim_given = mk_cpu_give_to_host(taken);
	}

	/*
	 * Once its victims are deleted the request is granted: their CPUs
	 * come back to the host when they are released, later if a kernfs
	 * file of the instance is still open.
	 */
	if (vs->chosen)
		ret = mk_reclaim_remove_victims(&mk_cpu_reclaim_res,
						mk_cpu_reclaim_policy->name);
out:
	bitmap_free(taken);
	mk_cpu_reclaim_result = ret;
	if (ret == -EAGAIN) {
		mk_cpu_reclaim_pending = true;
		return;
	}

	if (ret)
		pr_warn("EO_multikernel: host reclaim of %u CPUs (%s) %s (pool %u CPUs: %u free, %u held by instances)\n",
			count, mk_cpu_reclaim_policy->name, mk_cpu_reclaim_reason(ret),
			u.pool, u.free, u.instances);
	else if (vs->nr)
		pr_info("EO_multikernel: host reclaim of %u CPUs granted after stopping %d instance(s): %u free + %lu CPUs of the victims given to the host%s\n",
			count, vs->nr, mk_cpu_reclaim_given, mk_cpu_victims_cpus(),
			mk_cpu_reclaim_whole_pool ? ", all the CPUs of the pool" : "");
	else if (mk_cpu_reclaim_whole_pool)
		pr_info("EO_multikernel: host reclaim of %u CPUs granted: only %u CPUs in the pool, all of them given to the host (%u given)\n",
			count, u.pool, mk_cpu_reclaim_given);
	else
		pr_info("EO_multikernel: host reclaim of %u CPUs granted, %u CPUs given to the host\n",
			count, mk_cpu_reclaim_given);
}

static void mk_cpu_reclaim_monitor(void)
{
	mutex_lock(&mk_cpu_reclaim_mutex);
	if (mk_cpu_reclaim_pending) {
		mk_cpu_reclaim_pending = false;
		mk_cpu_reclaim_process(mk_cpu_reclaim_count);
	}
	mutex_unlock(&mk_cpu_reclaim_mutex);
}

/* An instance holds its CPUs */
static unsigned long mk_cpu_reclaim_amount(struct mk_instance *instance)
{
	return instance->cpus ? bitmap_weight(instance->cpus, NR_CPUS) : 0;
}

static unsigned long mk_cpu_reclaim_shown(unsigned long cpus)
{
	return cpus;
}

/* Its victims are protected by mk_cpu_reclaim_mutex */
static struct mk_reclaim_resource mk_cpu_reclaim_res = {
	.name = "cpu",
	.unit = "CPUs",
	.amount = mk_cpu_reclaim_amount,
	.shown = mk_cpu_reclaim_shown,
	.process = mk_cpu_reclaim_monitor,
};

int multikernel_cpu_reclaim(unsigned int count, const char *policy_name)
{
	const struct mk_reclaim_policy *policy;
	struct mk_cpu_usage u;

	policy = mk_reclaim_find_policy(policy_name);
	if (!policy)
		return -EINVAL;

	/* Known at once, the monitor checks it again */
	mutex_lock(&mk_instance_mutex);
	mk_cpu_usage(&u);
	mutex_unlock(&mk_instance_mutex);

	mutex_lock(&mk_cpu_reclaim_mutex);
	mk_cpu_reclaim_count = count;
	mk_cpu_reclaim_policy = policy;
	mk_cpu_reclaim_given = 0;
	mk_cpu_reclaim_whole_pool = false;
	mk_reclaim_victims_reset(&mk_cpu_reclaim_res);
	if (count && !u.pool) {
		mk_cpu_reclaim_pending = false;
		mk_cpu_reclaim_result = -ENOSPC;
		mutex_unlock(&mk_cpu_reclaim_mutex);
		pr_warn("EO_multikernel: host reclaim of %u CPUs %s\n",
			count, mk_cpu_reclaim_reason(-ENOSPC));
		return -ENOSPC;
	}
	mk_cpu_reclaim_pending = true;
	mk_cpu_reclaim_result = 0;
	mutex_unlock(&mk_cpu_reclaim_mutex);

	mk_reclaim_wake();
	return 0;
}

void multikernel_cpu_reclaim_show(struct seq_file *sf)
{
	struct mk_cpu_usage u;

	mutex_lock(&mk_cpu_reclaim_mutex);
	mutex_lock(&mk_instance_mutex);
	mk_cpu_usage(&u);
	mutex_unlock(&mk_instance_mutex);

	if (mk_cpu_reclaim_pending && mk_cpu_reclaim_result != -EAGAIN)
		seq_printf(sf, "last request: %u CPUs (%s), pending\n",
			   mk_cpu_reclaim_count, mk_cpu_reclaim_policy->name);
	else if (mk_cpu_reclaim_count && !mk_cpu_reclaim_result &&
		 mk_cpu_reclaim_res.victims.nr)
		seq_printf(sf, "last request: %u CPUs (%s), granted: %u free + %d victims, %lu CPUs given to the host%s\n",
			   mk_cpu_reclaim_count, mk_cpu_reclaim_policy->name,
			   mk_cpu_reclaim_given, mk_cpu_reclaim_res.victims.nr,
			   mk_cpu_reclaim_given + mk_cpu_victims_cpus(),
			   mk_cpu_reclaim_whole_pool ? ", all the CPUs of the pool" : "");
	else if (mk_cpu_reclaim_count && !mk_cpu_reclaim_result)
		seq_printf(sf, "last request: %u CPUs (%s), granted: %u CPUs given to the host%s\n",
			   mk_cpu_reclaim_count, mk_cpu_reclaim_policy->name,
			   mk_cpu_reclaim_given,
			   mk_cpu_reclaim_whole_pool ? ", all the CPUs of the pool" : "");
	else if (mk_cpu_reclaim_count)
		seq_printf(sf, "last request: %u CPUs (%s), %s\n",
			   mk_cpu_reclaim_count, mk_cpu_reclaim_policy->name,
			   mk_cpu_reclaim_reason(mk_cpu_reclaim_result));
	else
		seq_puts(sf, "last request: none\n");
	mk_reclaim_victims_show(&mk_cpu_reclaim_res, sf);
	seq_printf(sf, "pool: %u CPUs, instances %u, free %u\n",
		   u.pool, u.instances, u.free);
	seq_printf(sf, "monitor period: %u ms\n", mk_reclaim_interval());
	seq_puts(sf, "policies:");
	mk_reclaim_policies_show(sf);
	seq_putc(sf, '\n');
	mutex_unlock(&mk_cpu_reclaim_mutex);
}

static int __init mk_cpu_reclaim_init(void)
{
	mk_reclaim_register(&mk_cpu_reclaim_res);
	return 0;
}
late_initcall(mk_cpu_reclaim_init);
