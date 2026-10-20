// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Multikernel Technologies, Inc. All rights reserved
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/multikernel.h>
#include <linux/kexec.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <asm/apic.h>
#include "internal.h"

/* Callback management */
static struct mk_ipi_handler *mk_handlers;
static raw_spinlock_t mk_handlers_lock = __RAW_SPIN_LOCK_UNLOCKED(mk_handlers_lock);

/* Private copy of a ring slot, queued on a handler until its irq_work runs */
struct mk_ipi_msg {
	struct llist_node node;
	struct mk_ipi_data data;	/* only header + data_size bytes are valid */
};

static void handler_work(struct irq_work *work)
{
	struct mk_ipi_handler *handler = container_of(work, struct mk_ipi_handler, work);
	struct llist_node *list;
	struct mk_ipi_msg *msg, *tmp;

	/* llist is LIFO: reverse to deliver messages in arrival order */
	list = llist_reverse_order(llist_del_all(&handler->pending));
	llist_for_each_entry_safe(msg, tmp, list, node) {
		if (handler->callback)
			handler->callback(&msg->data, handler->context);
		kfree(msg);
	}
}

static void handler_drop_pending(struct mk_ipi_handler *handler)
{
	struct mk_ipi_msg *msg, *tmp;

	llist_for_each_entry_safe(msg, tmp, llist_del_all(&handler->pending), node)
		kfree(msg);
}

/**
 * multikernel_register_handler - Register a callback for multikernel IPI
 * @callback: Function to call when IPI is received
 * @ctx: Context pointer passed to the callback
 * @ipi_type: IPI type this handler should process
 *
 * Returns pointer to handler on success, NULL on failure
 */
struct mk_ipi_handler *multikernel_register_handler(mk_ipi_callback_t callback, void *ctx, unsigned int ipi_type)
{
	struct mk_ipi_handler *handler;
	unsigned long flags;

	if (!callback)
		return NULL;

	handler = kzalloc(sizeof(*handler), GFP_KERNEL);
	if (!handler)
		return NULL;

	handler->callback = callback;
	handler->context = ctx;
	handler->ipi_type = ipi_type;
	init_llist_head(&handler->pending);

	init_irq_work(&handler->work, handler_work);

	raw_spin_lock_irqsave(&mk_handlers_lock, flags);
	handler->next = mk_handlers;
	mk_handlers = handler;
	raw_spin_unlock_irqrestore(&mk_handlers_lock, flags);

	return handler;
}
EXPORT_SYMBOL(multikernel_register_handler);

/**
 * multikernel_unregister_handler - Unregister a multikernel IPI callback
 * @handler: Handler pointer returned from multikernel_register_handler
 */
void multikernel_unregister_handler(struct mk_ipi_handler *handler)
{
	struct mk_ipi_handler **pp, *p;
	unsigned long flags;

	if (!handler)
		return;

	raw_spin_lock_irqsave(&mk_handlers_lock, flags);
	pp = &mk_handlers;
	while ((p = *pp) != NULL) {
		if (p == handler) {
			*pp = p->next;
			break;
		}
		pp = &p->next;
	}
	raw_spin_unlock_irqrestore(&mk_handlers_lock, flags);

	/* Wait for pending work to complete */
	irq_work_sync(&handler->work);
	handler_drop_pending(handler);
	kfree(p);
}
EXPORT_SYMBOL(multikernel_unregister_handler);

/**
 * multikernel_send_ipi_data - Send data to another CPU via IPI
 * @instance_id: Target multikernel instance ID
 * @data: Pointer to data to send
 * @data_size: Size of data
 * @type: User-defined type identifier
 *
 * This function enqueues data into the target instance's IPI ring buffer
 * and sends an IPI to notify the target CPU.
 *
 * Returns 0 on success, negative error code on failure
 */
int multikernel_send_ipi_data(int instance_id, void *data, size_t data_size, unsigned long type)
{
	struct mk_ipi_data *slot;
	struct mk_instance *instance = mk_instance_find(instance_id);
	unsigned int head, next_head, tail;
	int cpu;

	if (!instance)
		return -EINVAL;
	if (data_size > MK_MAX_DATA_SIZE) {
		mk_instance_put(instance);
		return -EINVAL;
	}

	if (!instance->ipi_data && instance->kimage && instance->kimage->kho.ipi) {
		instance->ipi_data = phys_to_virt(instance->kimage->kho.ipi);
		memset(instance->ipi_data, 0, sizeof(struct mk_shared_data));
		pr_info("Initialized IPI ring buffer for instance %d: phys=0x%llx, virt=%px\n",
			instance->id, (unsigned long long)instance->kimage->kho.ipi,
			instance->ipi_data); //EO -> crash sysreq
	}

	if (!instance->ipi_data) {
		pr_debug("Multikernel IPI buffer not available for instance %d\n", instance_id);
		mk_instance_put(instance);
		return -ENODEV;
	}

	/* Try to enqueue the message in the ring buffer */
	do {
		head = atomic_read(&instance->ipi_data->ring.head);
		next_head = (head + 1) % MK_IPI_RING_SIZE;
		tail = atomic_read(&instance->ipi_data->ring.tail);

		/* Check if ring buffer is full */
		if (next_head == tail) {
			pr_warn("IPI ring buffer full for instance %d (head=%u, tail=%u)\n",
				instance_id, head, tail);
			mk_instance_put(instance);
			return -ENOSPC;
		}

		/* Try to claim this slot atomically */
	} while (atomic_cmpxchg(&instance->ipi_data->ring.head, head, next_head) != head);

	/* We've claimed slot 'head', now fill it */
	slot = &instance->ipi_data->ring.entries[head];

	/* Set header information */
	slot->sender_cpu = arch_cpu_physical_id(smp_processor_id());
	slot->type = type;
	slot->data_size = data_size;

	/* Copy the actual data into the buffer */
	if (data && data_size > 0)
		memcpy(slot->buffer, data, data_size);

	/* Ensure the slot is fully written before sending IPI */
	smp_wmb();

	cpu = find_first_bit(instance->cpus, NR_CPUS);

	/* Send IPI directly to physical APIC ID
	 * instance->cpus contains physical CPU IDs, use directly for APIC */
	apic_icr_write(APIC_DM_FIXED | APIC_DEST_PHYSICAL | MULTIKERNEL_VECTOR,
		       cpu);

	mk_instance_put(instance);
	return 0;
}

//EO -> 4
int multikernel_test_send_ipi_data(int instance_id, void *data, size_t data_size, unsigned long type)
{
	struct mk_ipi_data *slot;
	struct mk_instance *instance = mk_instance_find(instance_id);
	unsigned int head, next_head, tail;
	int cpu;

	if (!instance)
		return -EINVAL;
	if (data_size > MK_MAX_DATA_SIZE) {
		mk_instance_put(instance);
		return -EINVAL;
	}


	if (!instance->ipi_data) {
		pr_debug("Multikernel IPI buffer not available for instance %d\n", instance_id);
		mk_instance_put(instance);
		return -ENODEV;
	}

	do {
		head = atomic_read(&instance->ipi_data->ring.head);
		next_head = (head + 1) % MK_IPI_RING_SIZE;
		tail = atomic_read(&instance->ipi_data->ring.tail);
		pr_info("head=%u, tail=%u", head, tail);

		if (next_head == tail) {
			pr_warn("IPI ring buffer full for instance %d (head=%u, tail=%u)\n",
				instance_id, head, tail);
			mk_instance_put(instance);
			return -ENOSPC;
		}

	} while (atomic_cmpxchg(&instance->ipi_data->ring.head, head, next_head) != head);


	slot = &instance->ipi_data->ring.entries[head];

	slot->sender_cpu = arch_cpu_physical_id(smp_processor_id());
	slot->type = type;
	slot->data_size = data_size;


	if (data && data_size > 0)
		memcpy(slot->buffer, data, data_size);

	smp_wmb();

	cpu = find_first_bit(instance->cpus, NR_CPUS);

	apic_icr_write(APIC_DM_FIXED | APIC_DEST_PHYSICAL | MULTIKERNEL_VECTOR,
		       cpu); 

	mk_instance_put(instance);
	return 0;

}

/**
 * multikernel_interrupt_handler - Handle the multikernel IPI
 *
 * This function is called when a multikernel IPI is received.
 * It processes all pending messages in the ring buffer and invokes
 * registered callbacks for each message.
 */
static void multikernel_interrupt_handler(void)
{
	struct mk_ipi_data *slot;
	struct mk_ipi_handler *handler;
	unsigned int head, tail, next_tail;
	int current_cpu = smp_processor_id();
	int messages_processed = 0;

	if (!root_instance->ipi_data)
		return;

	/* Process all pending messages in the ring buffer */
	while (1) {
		tail = atomic_read(&root_instance->ipi_data->ring.tail);
		head = atomic_read(&root_instance->ipi_data->ring.head);

		/* Check if ring buffer is empty */
		if (tail == head)
			break;

		/* Ensure we read the message after checking it exists */
		smp_rmb();

		/* Get the message at tail */
		slot = &root_instance->ipi_data->ring.entries[tail];

		/* Validate message */
		if (slot->data_size == 0 || slot->data_size > MK_MAX_DATA_SIZE) {
			pr_debug("Multikernel IPI: invalid message at slot %u (size=%zu)\n",
				 tail, slot->data_size);
			/* Advance tail to skip this message */
			next_tail = (tail + 1) % MK_IPI_RING_SIZE;
			atomic_set(&root_instance->ipi_data->ring.tail, next_tail);
			continue;
		}

		//pr_debug("Multikernel IPI received on CPU %d from CPU %d (slot %u, type=%u, size=%zu)\n",
			 //current_cpu, slot->sender_cpu, tail, slot->type, slot->data_size);
		pr_info("Multikernel IPI received on CPU %d from CPU %d (slot %u, type=%u, size=%zu)\n",
			 current_cpu, slot->sender_cpu, tail, slot->type, slot->data_size);

		/*
		 * Dispatch to registered handlers. Each handler gets its own
		 * copy of the slot: the slot is given back to the producer as
		 * soon as tail moves, and several messages of the same type can
		 * arrive before the irq_work runs.
		 */
		raw_spin_lock(&mk_handlers_lock);
		for (handler = mk_handlers; handler; handler = handler->next) {
			struct mk_ipi_msg *msg;
			size_t len;

			if (handler->ipi_type != slot->type)
				continue;

			len = offsetof(struct mk_ipi_data, buffer) + slot->data_size;
			msg = kmalloc(offsetof(struct mk_ipi_msg, data) + len, GFP_ATOMIC);
			if (!msg) {
				pr_warn_ratelimited("Multikernel IPI: dropping message type %u (no memory)\n",
						    slot->type);
				continue;
			}
			memcpy(&msg->data, slot, len);
			llist_add(&msg->node, &handler->pending);
			irq_work_queue(&handler->work); // EO -> 6
		}
		raw_spin_unlock(&mk_handlers_lock);

		/* Advance tail to consume this message */
		next_tail = (tail + 1) % MK_IPI_RING_SIZE;
		atomic_set(&root_instance->ipi_data->ring.tail, next_tail);

		messages_processed++;

		/* Prevent infinite loop in case of bugs */
		if (messages_processed >= MK_IPI_RING_SIZE) {
			pr_warn("Processed %d IPI messages, breaking to prevent infinite loop\n",
				messages_processed);
			break;
		}
	}

	if (messages_processed > 0) {
		pr_debug("Processed %d IPI message(s) on CPU %d\n",
			 messages_processed, current_cpu);
	}
}

/**
 * Generic multikernel interrupt handler - called by the IPI vector
 *
 * This is the function that gets called by the IPI vector handler.
 */
void generic_multikernel_interrupt(void) // EO -> 5
{
	multikernel_interrupt_handler(); 
}
