/* Eric OKALA
 * Reclamation des ressources du pool multikernel par l'hote : moteur
 * commun (politiques, victimes, historique, moniteur)
*/

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/kexec.h>
#include <linux/suspend.h>
#include <linux/kernfs.h>
#include <linux/timekeeping.h>
#include <linux/multikernel.h>
#include "internal.h"

/*
 * Host reclaim of the multikernel pool. A resource (the flexible memory
 * pool, later the CPUs) handles the requests written by the host and
 * tells how much is missing; this engine does the rest, the same way for
 * every resource:
 *  - a policy picks the instances to stop (victims) until what they hold
 *    covers what is missing,
 *  - the victims are stopped and deleted at once (INIT, no graceful
 *    halt), as kerf kill --force, unload and delete would do,
 *  - each deletion is recorded for kerf (flex_reclaimed),
 *  - a monitor, a delayed work, handles the requests of every resource
 *    and then runs again every flex_reclaim_interval_ms.
 */

/*
 * Victim selection policies. Each one applies its own rule and nothing
 * else: it does not try to stop fewer instances than its rule says.
 * @cand lists the instances holding the resource, oldest first; a policy
 * may reorder the array and marks its victims until they cover @missing.
 * The engine checks the result, a policy that falls short gets its
 * request refused and nothing is stopped.
 */
/* Ties go to the oldest instance, whatever the order: sort() is not stable */
static int mk_reclaim_cmp_order(const struct mk_reclaim_candidate *x,
				const struct mk_reclaim_candidate *y)
{
	return x->order < y->order ? -1 : x->order > y->order;
}

static int mk_reclaim_cmp_amount_asc(const void *a, const void *b)
{
	const struct mk_reclaim_candidate *x = a, *y = b;

	if (x->amount != y->amount)
		return x->amount < y->amount ? -1 : 1;
	return mk_reclaim_cmp_order(x, y);
}

static int mk_reclaim_cmp_amount_desc(const void *a, const void *b)
{
	const struct mk_reclaim_candidate *x = a, *y = b;

	if (x->amount != y->amount)
		return x->amount > y->amount ? -1 : 1;
	return mk_reclaim_cmp_order(x, y);
}

/* Mark victims from @cand[0] upwards (or downwards) until @missing is covered */
static void mk_reclaim_take(struct mk_reclaim_candidate *cand, int nr,
			    unsigned long missing, bool backwards)
{
	unsigned long got = 0;
	int i;

	for (i = 0; i < nr && got < missing; i++) {
		struct mk_reclaim_candidate *c = &cand[backwards ? nr - 1 - i : i];

		c->victim = true;
		got += c->amount;
	}
}

/* Smallest instances first, adding them up */
static void mk_reclaim_policy_smallest_fit(struct mk_reclaim_candidate *cand, int nr,
					   unsigned long missing)
{
	sort(cand, nr, sizeof(*cand), mk_reclaim_cmp_amount_asc, NULL);
	mk_reclaim_take(cand, nr, missing, false);
}

/* Largest instances first */
static void mk_reclaim_policy_largest(struct mk_reclaim_candidate *cand, int nr,
				      unsigned long missing)
{
	sort(cand, nr, sizeof(*cand), mk_reclaim_cmp_amount_desc, NULL);
	mk_reclaim_take(cand, nr, missing, false);
}

/* Most recently created instances first */
static void mk_reclaim_policy_newest(struct mk_reclaim_candidate *cand, int nr,
				     unsigned long missing)
{
	mk_reclaim_take(cand, nr, missing, true);
}

/* Oldest instances first */
static void mk_reclaim_policy_oldest(struct mk_reclaim_candidate *cand, int nr,
				     unsigned long missing)
{
	mk_reclaim_take(cand, nr, missing, false);
}

/* The first one is the default */
static const struct mk_reclaim_policy mk_reclaim_policies[] = {
	{ "smallest-fit", mk_reclaim_policy_smallest_fit },
	{ "largest",      mk_reclaim_policy_largest },
	{ "newest",       mk_reclaim_policy_newest },
	{ "oldest",       mk_reclaim_policy_oldest },
};

const struct mk_reclaim_policy *mk_reclaim_find_policy(const char *name)
{
	int i;

	if (!name)
		return &mk_reclaim_policies[0];

	for (i = 0; i < ARRAY_SIZE(mk_reclaim_policies); i++)
		if (!strcmp(name, mk_reclaim_policies[i].name))
			return &mk_reclaim_policies[i];

	return NULL;
}

/* " [default] other ...", as read in flex_reclaim */
void mk_reclaim_policies_show(struct seq_file *sf)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mk_reclaim_policies); i++)
		seq_printf(sf, i ? " %s" : " [%s]", mk_reclaim_policies[i].name);
}

/*
 * Instances deleted by host reclaims, for kerf: the 16 last ones, read in
 * flex_reclaimed. The file is notified on each deletion (poll()). The
 * lock is taken last and nothing is taken under it.
 */
#define MK_RECLAIM_HISTORY	16
struct mk_reclaimed {
	struct mk_reclaim_candidate c;
	unsigned long shown;		/* amount in the unit of the resource */
	time64_t when;			/* seconds since the epoch */
	const char *policy;
};
static DEFINE_MUTEX(mk_reclaim_history_mutex);
static struct mk_reclaimed mk_reclaim_history[MK_RECLAIM_HISTORY];
static unsigned int mk_reclaim_history_count;	/* ever recorded */
static struct kernfs_node *mk_reclaim_history_kn;

static void mk_reclaim_record(struct mk_reclaim_resource *res,
			      struct mk_reclaim_candidate *c, const char *policy)
{
	struct mk_reclaimed *r;

	mutex_lock(&mk_reclaim_history_mutex);
	r = &mk_reclaim_history[mk_reclaim_history_count++ % MK_RECLAIM_HISTORY];
	r->c = *c;
	r->shown = res->shown(c->amount);
	r->when = ktime_get_real_seconds();
	r->policy = policy;

	if (mk_reclaim_history_kn)
		kernfs_notify(mk_reclaim_history_kn);
	mutex_unlock(&mk_reclaim_history_mutex);
}

/* One line per deleted instance, oldest first: id name MB time policy state */
void mk_reclaim_history_show(struct seq_file *sf)
{
	unsigned int i, first;

	mutex_lock(&mk_reclaim_history_mutex);
	first = mk_reclaim_history_count > MK_RECLAIM_HISTORY ?
		mk_reclaim_history_count - MK_RECLAIM_HISTORY : 0;
	for (i = first; i < mk_reclaim_history_count; i++) {
		struct mk_reclaimed *r = &mk_reclaim_history[i % MK_RECLAIM_HISTORY];

		seq_printf(sf, "%d %s %lu %lld %s %s\n", r->c.id, r->c.name,
			   r->shown, (long long)r->when,
			   r->policy, mk_state_to_string(r->c.state));
	}
	mutex_unlock(&mk_reclaim_history_mutex);
}

void mk_reclaim_set_history_node(struct kernfs_node *kn)
{
	mutex_lock(&mk_reclaim_history_mutex);
	mk_reclaim_history_kn = kn;
	mutex_unlock(&mk_reclaim_history_mutex);
}

/*
 * Snapshot of the instances holding @res, in creation order
 * (mk_instance_list is appended to). Returns their number, or -ENOMEM.
 */
static int mk_reclaim_candidates(struct mk_reclaim_resource *res,
				 struct mk_reclaim_candidate **out)
{
	struct mk_reclaim_candidate *cand;
	struct mk_instance *instance;
	int nr = 0, i = 0;

	mutex_lock(&mk_instance_mutex);
	list_for_each_entry(instance, &mk_instance_list, list)
		if (instance != root_instance && res->amount(instance))
			nr++;

	cand = nr ? kcalloc(nr, sizeof(*cand), GFP_KERNEL) : NULL;
	if (nr && !cand) {
		mutex_unlock(&mk_instance_mutex);
		return -ENOMEM;
	}

	list_for_each_entry(instance, &mk_instance_list, list) {
		unsigned long amount;

		if (instance == root_instance)
			continue;
		amount = res->amount(instance);
		if (!amount)
			continue;
		cand[i].id = instance->id;
		strscpy(cand[i].name, instance->name ?: "", sizeof(cand[i].name));
		cand[i].amount = amount;
		cand[i].state = instance->state;
		cand[i].order = i;
		i++;
	}
	mutex_unlock(&mk_instance_mutex);

	*out = cand;
	return nr;
}

/*
 * @res lacks @missing: let @policy pick the instances to stop. Called
 * with the lock of the resource held.
 */
int mk_reclaim_select_victims(struct mk_reclaim_resource *res,
			      const struct mk_reclaim_policy *policy,
			      unsigned long missing)
{
	struct mk_reclaim_victims *vs = &res->victims;
	struct mk_reclaim_candidate *cand;
	unsigned long total = 0, got = 0;
	int nr, i, ret;

	nr = mk_reclaim_candidates(res, &cand);
	if (nr < 0)
		return nr;

	for (i = 0; i < nr; i++)
		total += cand[i].amount;
	/* Stopping every instance would not be enough: stop none */
	if (total < missing) {
		ret = -EBUSY;
		goto out;
	}

	policy->select(cand, nr, missing);

	for (i = 0; i < nr; i++) {
		if (!cand[i].victim)
			continue;
		got += cand[i].amount;
		/* Each victim must be recorded to be stopped */
		if (vs->nr == MK_RECLAIM_MAX_VICTIMS) {
			got = 0;
			break;
		}
		vs->v[vs->nr].c = cand[i];
		vs->v[vs->nr].done = false;
		vs->nr++;
	}
	if (got < missing) {
		vs->nr = 0;
		ret = -ENOMEM;
		goto out;
	}

	vs->chosen = true;
	ret = 0;
out:
	kfree(cand);
	return ret;
}

/* The instance @c still exists: same id and same name as when chosen */
static struct mk_instance *mk_reclaim_victim_lookup(struct mk_reclaim_candidate *c)
{
	struct mk_instance *instance;

	lockdep_assert_held(&mk_instance_mutex);

	instance = idr_find(&mk_instance_idr, c->id);
	if (instance && strcmp(instance->name ?: "", c->name))
		return NULL;
	return instance;
}

/*
 * Stop and delete one victim, as kerf kill --force, unload and delete:
 * INIT if it runs, then unload its kernel, then delete it. What it holds
 * goes back to the pool when its last reference is dropped.
 */
static int mk_reclaim_remove_victim(struct mk_reclaim_resource *res,
				    struct mk_reclaim_candidate *c,
				    const char *policy)
{
	struct mk_instance *instance;
	enum mk_instance_state state;
	int ret;

	mutex_lock(&mk_instance_mutex);
	instance = mk_reclaim_victim_lookup(c);
	state = instance ? instance->state : MK_STATE_EMPTY;
	mutex_unlock(&mk_instance_mutex);

	/* Deleted meanwhile (kerf delete): what it held is coming back anyway */
	if (!instance) {
		pr_info("EO_multikernel: host reclaim (%s): instance %d (%s) already gone\n",
			policy, c->id, c->name);
		return 0;
	}

	if (state == MK_STATE_ACTIVE) {
		ret = multikernel_force_halt_by_id(c->id);
		if (ret)
			return ret;
	}

	ret = multikernel_kexec_unload_by_id(c->id);
	if (ret)
		return ret;

	mutex_lock(&mk_instance_mutex);
	instance = mk_reclaim_victim_lookup(c);
	ret = instance ? mk_instance_destroy(instance) : 0;
	mutex_unlock(&mk_instance_mutex);
	if (ret)
		return ret;

	mk_reclaim_record(res, c, policy);
	pr_warn("EO_multikernel: host reclaim (%s): instance %d (%s, %lu %s, %s) stopped and deleted\n",
		policy, c->id, c->name, res->shown(c->amount), res->unit,
		mk_state_to_string(c->state));
	return 0;
}

/*
 * Stop the victims not done yet. system_transition_mutex serializes with
 * kerf start and kill, and is only tried, as the kexec lock: the monitor
 * never waits on kerf, it retries at its next period (-EAGAIN).
 */
int mk_reclaim_remove_victims(struct mk_reclaim_resource *res, const char *policy)
{
	struct mk_reclaim_victims *vs = &res->victims;
	int i, ret = 0;

	if (!mutex_trylock(&system_transition_mutex))
		return -EAGAIN;

	for (i = 0; i < vs->nr; i++) {
		if (vs->v[i].done)
			continue;
		ret = mk_reclaim_remove_victim(res, &vs->v[i].c, policy);
		if (ret)
			break;
		vs->v[i].done = true;
	}

	mutex_unlock(&system_transition_mutex);

	if (ret) {
		pr_warn_ratelimited("EO_multikernel: host reclaim (%s): cannot stop instance %d (%s) yet: %d, retrying\n",
				    policy, vs->v[i].c.id, vs->v[i].c.name, ret);
		return -EAGAIN;
	}
	return 0;
}

void mk_reclaim_victims_show(struct mk_reclaim_resource *res, struct seq_file *sf)
{
	struct mk_reclaim_victims *vs = &res->victims;
	int i;

	for (i = 0; i < vs->nr; i++)
		seq_printf(sf, "  victim: instance %d (%s, %lu %s, %s), %s\n",
			   vs->v[i].c.id, vs->v[i].c.name,
			   res->shown(vs->v[i].c.amount), res->unit,
			   mk_state_to_string(vs->v[i].c.state),
			   vs->v[i].done ? "stopped" : "to stop");
}

/*
 * The monitor. Writing a request only records it in its resource and
 * wakes the monitor up; each run calls every resource, which handles its
 * pending request if any.
 */
#define MK_RECLAIM_INTERVAL_MS		100
#define MK_RECLAIM_INTERVAL_MIN_MS	10
#define MK_RECLAIM_INTERVAL_MAX_MS	60000

static unsigned int mk_reclaim_interval_ms = MK_RECLAIM_INTERVAL_MS;
/* Resources are only registered at boot, before the monitor starts */
static LIST_HEAD(mk_reclaim_resources);
static bool mk_reclaim_started;

static void mk_reclaim_monitor(struct work_struct *work);
static DECLARE_DELAYED_WORK(mk_reclaim_work, mk_reclaim_monitor);

static void mk_reclaim_monitor(struct work_struct *work)
{
	struct mk_reclaim_resource *res;

	list_for_each_entry(res, &mk_reclaim_resources, list)
		res->process();

	/* A request written meanwhile has already queued an immediate run */
	queue_delayed_work(system_unbound_wq, &mk_reclaim_work,
			   msecs_to_jiffies(READ_ONCE(mk_reclaim_interval_ms)));
}

/* Wake the monitor up now rather than at its next period */
void mk_reclaim_wake(void)
{
	mod_delayed_work(system_unbound_wq, &mk_reclaim_work, 0);
}

unsigned int mk_reclaim_interval(void)
{
	return READ_ONCE(mk_reclaim_interval_ms);
}

int mk_reclaim_set_interval(unsigned int ms)
{
	if (ms < MK_RECLAIM_INTERVAL_MIN_MS || ms > MK_RECLAIM_INTERVAL_MAX_MS)
		return -EINVAL;

	WRITE_ONCE(mk_reclaim_interval_ms, ms);
	/* The new period applies from now, not after the current one */
	if (READ_ONCE(mk_reclaim_started))
		mod_delayed_work(system_unbound_wq, &mk_reclaim_work,
				 msecs_to_jiffies(ms));

	pr_info("EO_multikernel: host reclaim monitor period set to %u ms\n", ms);
	return 0;
}

/* Each resource registers from a late_initcall */
void __init mk_reclaim_register(struct mk_reclaim_resource *res)
{
	list_add_tail(&res->list, &mk_reclaim_resources);
}

/*
 * Workqueues are not up when the resources are set up. The monitor
 * starts after every late_initcall: the list no longer changes.
 */
static int __init mk_reclaim_start(void)
{
	if (list_empty(&mk_reclaim_resources))
		return 0;

	WRITE_ONCE(mk_reclaim_started, true);
	queue_delayed_work(system_unbound_wq, &mk_reclaim_work,
			   msecs_to_jiffies(READ_ONCE(mk_reclaim_interval_ms)));
	pr_info("EO_multikernel: host reclaim monitor started, period %u ms\n",
		READ_ONCE(mk_reclaim_interval_ms));
	return 0;
}
late_initcall_sync(mk_reclaim_start);
