extern struct resource multikernel_res;
extern struct mutex mk_instance_mutex;
extern struct mutex mk_host_dtb_mutex;
extern struct idr mk_instance_idr;
extern struct list_head mk_instance_list;
extern struct mk_instance *root_instance;

/* kernfs.c */
extern struct kernfs_node *mk_root_kn;
extern struct kernfs_node *mk_instances_kn;
int mk_create_instance_from_dtb(const char *name, int id, const void *fdt,
				      int resources_node, size_t dtb_size);
struct mk_instance *mk_instance_find_by_name(const char *name);
int mk_instance_destroy(struct mk_instance *instance);

/* dts.c */
int mk_dt_parse_resources(const void *fdt, int resources_node,
			  const char *instance_name, struct mk_dt_config *config);
int mk_dt_generate_instance_dtb(struct mk_instance *instance,
				 void **out_dtb, size_t *out_size);

/* overlay.c */
extern struct kernfs_node *mk_overlay_root_kn;
int mk_overlay_init(void);
void mk_overlay_exit(void);
int mk_overlay_rmdir(struct kernfs_node *kn);

/* hotplug.c */
int mk_hotplug_init(void);
void mk_hotplug_cleanup(void);
int mk_handle_cpu_remove(struct mk_cpu_resource_payload *payload, u32 payload_len);

/* baseline.c */
int mk_baseline_validate_and_initialize(const void *fdt, size_t fdt_size);
//EO -> retrive: 4 cette fonction etait static
void mk_baseline_clear_resources(struct mk_instance *instance);

//EO -> flex_pool
struct seq_file;
struct kernfs_node;
bool multikernel_flexmem_enabled(void);
int multikernel_flexmem_create_instance_pool(int instance_id, size_t pool_size, int min_alloc_order,
					      void **pool_handle, phys_addr_t *base);
void multikernel_flexmem_destroy_instance_pool(void *pool_handle, phys_addr_t base, size_t size);
int multikernel_flexmem_acquire(size_t size, phys_addr_t *base); 
void multikernel_flexmem_release(phys_addr_t base, size_t size);
size_t multikernel_flexmem_size(void);
int multikernel_flexmem_parse_borrow(const char *str, int *pct);
void multikernel_flexmem_set_borrow(int pct);

int multikernel_flexmem_reclaim(size_t size, const char *policy_name);
void multikernel_flexmem_reclaim_show(struct seq_file *sf);

/* reclaim.c: host reclaim engine, common to every pool resource */

/* An instance holding a resource of the pool, seen by a reclaim policy */
struct mk_reclaim_candidate {
	int id;
	char name[32];
	unsigned long amount;	/* held: pages, CPUs... */
	enum mk_instance_state state;
	bool victim;		/* set by the policy */
};

struct mk_reclaim_policy {
	const char *name;
	void (*select)(struct mk_reclaim_candidate *cand, int nr, unsigned long missing);
};

/* Victims chosen for the last request of a resource */
#define MK_RECLAIM_MAX_VICTIMS	32
struct mk_reclaim_victim {
	struct mk_reclaim_candidate c;
	bool done;		/* stopped and deleted */
};

struct mk_reclaim_victims {
	struct mk_reclaim_victim v[MK_RECLAIM_MAX_VICTIMS];
	int nr;
	bool chosen;		/* for the current request */
};

/* A resource of the pool the host can reclaim */
struct mk_reclaim_resource {
	const char *unit;	/* of shown(), e.g. "MB" */
	/* What @instance holds, 0 if it is not a candidate (mk_instance_mutex held) */
	unsigned long (*amount)(struct mk_instance *instance);
	/* @amount in @unit, for logs and flex_reclaimed */
	unsigned long (*shown)(unsigned long amount);
	/* Called by each run of the monitor: handle the pending request */
	void (*process)(void);
	/* Protected by the lock of the resource */
	struct mk_reclaim_victims victims;
	struct list_head list;
};

static inline void mk_reclaim_victims_reset(struct mk_reclaim_resource *res)
{
	res->victims.nr = 0;
	res->victims.chosen = false;
}

const struct mk_reclaim_policy *mk_reclaim_find_policy(const char *name);
void mk_reclaim_policies_show(struct seq_file *sf);
int mk_reclaim_select_victims(struct mk_reclaim_resource *res,
			      const struct mk_reclaim_policy *policy,
			      unsigned long missing);
int mk_reclaim_remove_victims(struct mk_reclaim_resource *res, const char *policy);
void mk_reclaim_victims_show(struct mk_reclaim_resource *res, struct seq_file *sf);
void mk_reclaim_history_show(struct seq_file *sf);
void mk_reclaim_set_history_node(struct kernfs_node *kn);
void mk_reclaim_wake(void);
unsigned int mk_reclaim_interval(void);
int mk_reclaim_set_interval(unsigned int ms);
void mk_reclaim_register(struct mk_reclaim_resource *res);
