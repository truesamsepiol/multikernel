/* Eric OKALA
 * Quarantaine des CPU du pool multikernel
*/

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/bitmap.h>
#include <linux/cpu.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/seq_file.h>
#include <linux/kernfs.h>
#include <linux/multikernel.h>
#include <asm/smp.h>
#include "internal.h"

/*
 * Quarantine of the CPUs of the pool (cpu_quarantine file). A CPU whose
 * test failed must not run the host or a VM again: it is kept offline,
 * out of the pool, and listed until the administrator lifts it.
 *
 * A CPU can be put in quarantine when it is offline: held by an instance
 * (the usual case, the test ran there), free in the pool, or offlined by
 * the administrator. A CPU online for the host is refused (-EBUSY).
 *
 *  - held by an instance: it stays with the instance; when the instance
 *    gives its CPUs back (mk_instance_return_cpus()), this one is not
 *    onlined and does not go back to the pool. Put the CPU in quarantine
 *    before stopping the instance, not after: once the instance is
 *    released the CPU is already online for the host.
 *  - free in the pool: it leaves the pool at once.
 *  - kerf init: a CPU in quarantine is left out of the new pool.
 *  - cpu_reclaim never gives it, kerf create never gets it.
 *
 * Lifting the quarantine gives the CPU to the host (online), as when an
 * instance ends; the next kerf init may take it into the pool again. A
 * CPU still held by an instance only loses its mark.
 *
 * The bitmap is read without lock (test_bit) on the paths that put a CPU
 * back into the pool; it is changed under mk_instance_mutex, then
 * mk_quarantine_mutex, which protects the list.
 */
#define MK_QUARANTINE_REASON_MAX	64

struct mk_quarantined_cpu {
	struct list_head list;
	u32 phys_cpu;			/* APIC ID */
	time64_t when;			/* seconds since the epoch */
	char reason[MK_QUARANTINE_REASON_MAX];
};

static DECLARE_BITMAP(mk_quarantined, NR_CPUS);
static DEFINE_MUTEX(mk_quarantine_mutex);
static LIST_HEAD(mk_quarantine_list);
static struct kernfs_node *mk_quarantine_kn;

bool mk_cpu_quarantined(int phys_cpu)
{
	return phys_cpu >= 0 && phys_cpu < NR_CPUS &&
	       test_bit(phys_cpu, mk_quarantined);
}

/* CPUs of @cpus in quarantine */
unsigned int mk_cpu_quarantined_in(const unsigned long *cpus)
{
	return bitmap_weight_and(cpus, mk_quarantined, NR_CPUS);
}

unsigned int mk_cpu_quarantine_count(void)
{
	return bitmap_weight(mk_quarantined, NR_CPUS);
}

/* The instance holding @phys_cpu, NULL if none (mk_instance_mutex held) */
static struct mk_instance *mk_cpu_holder(int phys_cpu)
{
	struct mk_instance *instance;

	lockdep_assert_held(&mk_instance_mutex);

	list_for_each_entry(instance, &mk_instance_list, list)
		if (instance != root_instance && instance->cpus &&
		    test_bit(phys_cpu, instance->cpus))
			return instance;
	return NULL;
}

static void mk_quarantine_notify(void)
{
	if (mk_quarantine_kn)
		kernfs_notify(mk_quarantine_kn);
}

int multikernel_cpu_quarantine(u32 phys_cpu, const char *reason)
{
	struct mk_quarantined_cpu *q;
	struct mk_instance *holder;
	int logical_cpu, ret = 0;

	if (phys_cpu >= NR_CPUS)
		return -EINVAL;
	logical_cpu = arch_cpu_from_physical_id(phys_cpu);
	if (logical_cpu < 0)
		return -ENODEV;

	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (!q)
		return -ENOMEM;
	q->phys_cpu = phys_cpu;
	q->when = ktime_get_real_seconds();
	strscpy(q->reason, reason && *reason ? reason : "-", sizeof(q->reason));

	mutex_lock(&mk_instance_mutex);
	if (test_bit(phys_cpu, mk_quarantined)) {
		ret = -EEXIST;
		goto unlock;
	}

	holder = mk_cpu_holder(phys_cpu);
	if (!holder && cpu_online(logical_cpu)) {
		pr_warn("EO_multikernel: CPU %u (logical %d) is online for the host, not put in quarantine: offline it first\n",
			phys_cpu, logical_cpu);
		ret = -EBUSY;
		goto unlock;
	}

	/* Free in the pool, or offlined by the administrator */
	if (!holder && root_instance && root_instance->cpus)
		clear_bit(phys_cpu, root_instance->cpus);

	set_bit(phys_cpu, mk_quarantined);
	mutex_lock(&mk_quarantine_mutex);
	list_add_tail(&q->list, &mk_quarantine_list);
	mutex_unlock(&mk_quarantine_mutex);

	if (holder)
		pr_warn("EO_multikernel: CPU %u (logical %d) put in quarantine (%s): held by instance %d (%s), it will stay offline when the instance ends\n",
			phys_cpu, logical_cpu, q->reason, holder->id, holder->name);
	else
		pr_warn("EO_multikernel: CPU %u (logical %d) put in quarantine (%s): offline, out of the pool\n",
			phys_cpu, logical_cpu, q->reason);
	q = NULL;
unlock:
	mutex_unlock(&mk_instance_mutex);
	kfree(q);
	if (!ret)
		mk_quarantine_notify();
	return ret;
}

int multikernel_cpu_unquarantine(u32 phys_cpu)
{
	struct mk_quarantined_cpu *q, *found = NULL;
	struct mk_instance *holder;
	int logical_cpu, ret;

	if (phys_cpu >= NR_CPUS)
		return -EINVAL;
	logical_cpu = arch_cpu_from_physical_id(phys_cpu);
	if (logical_cpu < 0)
		return -ENODEV;

	mutex_lock(&mk_instance_mutex);
	if (!test_bit(phys_cpu, mk_quarantined)) {
		mutex_unlock(&mk_instance_mutex);
		return -ENOENT;
	}
	holder = mk_cpu_holder(phys_cpu);
	clear_bit(phys_cpu, mk_quarantined);
	mutex_lock(&mk_quarantine_mutex);
	list_for_each_entry(q, &mk_quarantine_list, list)
		if (q->phys_cpu == phys_cpu) {
			found = q;
			list_del(&q->list);
			break;
		}
	mutex_unlock(&mk_quarantine_mutex);
	mutex_unlock(&mk_instance_mutex);
	kfree(found);
	mk_quarantine_notify();

	if (holder) {
		pr_info("EO_multikernel: quarantine of CPU %u (logical %d) lifted, still held by instance %d (%s)\n",
			phys_cpu, logical_cpu, holder->id, holder->name);
		return 0;
	}

	/* Given to the host, as the CPUs of an instance that ends */
	ret = add_cpu(logical_cpu);
	if (ret)
		pr_err("EO_multikernel: quarantine of CPU %u (logical %d) lifted, cannot online it: %d\n",
		       phys_cpu, logical_cpu, ret);
	else
		pr_info("EO_multikernel: quarantine of CPU %u (logical %d) lifted, CPU given to the host\n",
			phys_cpu, logical_cpu);
	return 0;
}

/*
 * One line per CPU in quarantine, oldest first:
 *   apic_id logical_cpu time where reason
 * where: "instance:<id>:<name>" while an instance holds it, else "offline".
 */
void multikernel_cpu_quarantine_show(struct seq_file *sf)
{
	struct mk_quarantined_cpu *q;
	struct mk_instance *holder;

	mutex_lock(&mk_instance_mutex);
	mutex_lock(&mk_quarantine_mutex);
	list_for_each_entry(q, &mk_quarantine_list, list) {
		holder = mk_cpu_holder(q->phys_cpu);
		if (holder)
			seq_printf(sf, "%u %d %lld instance:%d:%s %s\n", q->phys_cpu,
				   arch_cpu_from_physical_id(q->phys_cpu),
				   (long long)q->when, holder->id, holder->name,
				   q->reason);
		else
			seq_printf(sf, "%u %d %lld offline %s\n", q->phys_cpu,
				   arch_cpu_from_physical_id(q->phys_cpu),
				   (long long)q->when, q->reason);
	}
	mutex_unlock(&mk_quarantine_mutex);
	mutex_unlock(&mk_instance_mutex);
}

void mk_quarantine_set_node(struct kernfs_node *kn)
{
	mk_quarantine_kn = kn;
}
