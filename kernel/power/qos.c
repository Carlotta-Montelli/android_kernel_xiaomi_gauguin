/*
 * This module exposes the interface to kernel space for specifying
 * QoS dependencies.  It provides infrastructure for registration of:
 *
 * Dependents on a QoS value : register requests
 * Watchers of QoS value : get notified when target QoS value changes
 *
 * This QoS design is best effort based.  Dependents register their QoS needs.
 * Watchers register to keep track of the current QoS needs of the system.
 *
 * There are 3 basic classes of QoS parameter: latency, timeout, throughput
 * each have defined units:
 * latency: usec
 * timeout: usec <-- currently not used.
 * throughput: kbs (kilo byte / sec)
 *
 * There are lists of pm_qos_objects each one wrapping requests, notifiers
 *
 * User mode requests on a QOS parameter register themselves to the
 * subsystem by opening the device node /dev/... and writing there request to
 * the node.  As long as the process holds a file handle open to the node the
 * client continues to be accounted for.  Upon file release the usermode
 * request is removed and a new qos target is computed.  This way when the
 * request that the application has is cleaned up when closes the file
 * pointer or exits the pm_qos_object will get an opportunity to clean up.
 *
 * Mark Gross <mgross@linux.intel.com>
 */

/*#define DEBUG*/

#include <linux/pm_qos.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/string.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/cpumask.h>

#include <linux/uaccess.h>
#include <linux/export.h>
#include <trace/events/power.h>

#define CPUMASK_ALL (BIT(NR_CPUS) - 1)

/*
 * locking rule: all changes to constraints or notifiers lists
 * or pm_qos_object list and pm_qos_objects need to happen with pm_qos_lock
 * held.  One lock to rule them all
 */
struct pm_qos_object {
	struct pm_qos_constraints *constraints;
	struct miscdevice pm_qos_power_miscdev;
	char *name;
};

static DEFINE_SPINLOCK(pm_qos_lock);

static struct pm_qos_object null_pm_qos;

static BLOCKING_NOTIFIER_HEAD(cpu_dma_lat_notifier);
static struct pm_qos_constraints cpu_dma_constraints = {
	.list = PLIST_HEAD_INIT(cpu_dma_constraints.list),
	.target_value = PM_QOS_CPU_DMA_LAT_DEFAULT_VALUE,
	.target_per_cpu = { [0 ... (NR_CPUS - 1)] =
				PM_QOS_CPU_DMA_LAT_DEFAULT_VALUE },
	.default_value = PM_QOS_CPU_DMA_LAT_DEFAULT_VALUE,
	.no_constraint_value = PM_QOS_CPU_DMA_LAT_DEFAULT_VALUE,
	.type = PM_QOS_MIN,
	.notifiers = &cpu_dma_lat_notifier,
};
static struct pm_qos_object cpu_dma_pm_qos = {
	.constraints = &cpu_dma_constraints,
	.name = "cpu_dma_latency",
};

static BLOCKING_NOTIFIER_HEAD(network_lat_notifier);
static struct pm_qos_constraints network_lat_constraints = {
	.list = PLIST_HEAD_INIT(network_lat_constraints.list),
	.target_value = PM_QOS_NETWORK_LAT_DEFAULT_VALUE,
	.target_per_cpu = { [0 ... (NR_CPUS - 1)] =
				PM_QOS_NETWORK_LAT_DEFAULT_VALUE },
	.default_value = PM_QOS_NETWORK_LAT_DEFAULT_VALUE,
	.no_constraint_value = PM_QOS_NETWORK_LAT_DEFAULT_VALUE,
	.type = PM_QOS_MIN,
	.notifiers = &network_lat_notifier,
};
static struct pm_qos_object network_lat_pm_qos = {
	.constraints = &network_lat_constraints,
	.name = "network_latency",
};

static BLOCKING_NOTIFIER_HEAD(network_throughput_notifier);
static struct pm_qos_constraints network_tput_constraints = {
	.list = PLIST_HEAD_INIT(network_tput_constraints.list),
	.target_value = PM_QOS_NETWORK_THROUGHPUT_DEFAULT_VALUE,
	.target_per_cpu = { [0 ... (NR_CPUS - 1)] =
				PM_QOS_NETWORK_THROUGHPUT_DEFAULT_VALUE },
	.default_value = PM_QOS_NETWORK_THROUGHPUT_DEFAULT_VALUE,
	.no_constraint_value = PM_QOS_NETWORK_THROUGHPUT_DEFAULT_VALUE,
	.type = PM_QOS_MAX,
	.notifiers = &network_throughput_notifier,
};
static struct pm_qos_object network_throughput_pm_qos = {
	.constraints = &network_tput_constraints,
	.name = "network_throughput",
};


static BLOCKING_NOTIFIER_HEAD(memory_bandwidth_notifier);
static struct pm_qos_constraints memory_bw_constraints = {
	.list = PLIST_HEAD_INIT(memory_bw_constraints.list),
	.target_value = PM_QOS_MEMORY_BANDWIDTH_DEFAULT_VALUE,
	.default_value = PM_QOS_MEMORY_BANDWIDTH_DEFAULT_VALUE,
	.no_constraint_value = PM_QOS_MEMORY_BANDWIDTH_DEFAULT_VALUE,
	.type = PM_QOS_SUM,
	.notifiers = &memory_bandwidth_notifier,
};
static struct pm_qos_object memory_bandwidth_pm_qos = {
	.constraints = &memory_bw_constraints,
	.name = "memory_bandwidth",
};


static struct pm_qos_object *pm_qos_array[] = {
	&null_pm_qos,
	&cpu_dma_pm_qos,
	&network_lat_pm_qos,
	&network_throughput_pm_qos,
	&memory_bandwidth_pm_qos,
};

static ssize_t pm_qos_power_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *f_pos);
static ssize_t pm_qos_power_read(struct file *filp, char __user *buf,
		size_t count, loff_t *f_pos);
static int pm_qos_power_open(struct inode *inode, struct file *filp);
static int pm_qos_power_release(struct inode *inode, struct file *filp);

static const struct file_operations pm_qos_power_fops = {
	.write = pm_qos_power_write,
	.read = pm_qos_power_read,
	.open = pm_qos_power_open,
	.release = pm_qos_power_release,
	.llseek = noop_llseek,
};

/* unlocked internal variant */
static inline int pm_qos_get_value(struct pm_qos_constraints *c)
{
	struct plist_node *node;
	int total_value = 0;

	if (plist_head_empty(&c->list))
		return c->no_constraint_value;

	switch (c->type) {
	case PM_QOS_MIN:
		return plist_first(&c->list)->prio;

	case PM_QOS_MAX:
		return plist_last(&c->list)->prio;

	case PM_QOS_SUM:
		plist_for_each(node, &c->list)
			total_value += node->prio;

		return total_value;

	default:
		/* runtime check for not using enum */
		BUG();
		return PM_QOS_DEFAULT_VALUE;
	}
}

s32 pm_qos_read_value(struct pm_qos_constraints *c)
{
	return c->target_value;
}

static inline void pm_qos_set_value(struct pm_qos_constraints *c, s32 value)
{
	c->target_value = value;
}

static int pm_qos_dbg_show_requests(struct seq_file *s, void *unused)
{
	struct pm_qos_object *qos = (struct pm_qos_object *)s->private;
	struct pm_qos_constraints *c;
	struct pm_qos_request *req;
	char *type;
	int tot_reqs = 0;
	int active_reqs = 0;

	if (IS_ERR_OR_NULL(qos)) {
		pr_err("%s: bad qos param!\n", __func__);
		return -EINVAL;
	}
	c = qos->constraints;
	if (IS_ERR_OR_NULL(c)) {
		pr_err("%s: Bad constraints on qos?\n", __func__);
		return -EINVAL;
	}

	/* Lock to ensure we have a snapshot */
	spin_lock(&pm_qos_lock);
	if (plist_head_empty(&c->list)) {
		seq_puts(s, "Empty!\n");
		goto out;
	}

	switch (c->type) {
	case PM_QOS_MIN:
		type = "Minimum";
		break;
	case PM_QOS_MAX:
		type = "Maximum";
		break;
	case PM_QOS_SUM:
		type = "Sum";
		break;
	default:
		type = "Unknown";
	}

	plist_for_each_entry(req, &c->list, node) {
		char *state = "Default";

		if ((req->node).prio != c->default_value) {
			active_reqs++;
			state = "Active";
		}
		tot_reqs++;
		seq_printf(s, "%d: %d: %s\n", tot_reqs,
			   (req->node).prio, state);
	}

	seq_printf(s, "Type=%s, Value=%d, Requests: active=%d / total=%d\n",
		   type, pm_qos_get_value(c), active_reqs, tot_reqs);

out:
	spin_unlock(&pm_qos_lock);
	return 0;
}

static int pm_qos_dbg_open(struct inode *inode, struct file *file)
{
	return single_open(file, pm_qos_dbg_show_requests,
			   inode->i_private);
}

static const struct file_operations pm_qos_debug_fops = {
	.open           = pm_qos_dbg_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = single_release,
};

static inline int pm_qos_set_value_for_cpus(struct pm_qos_request *new_req,
					    struct pm_qos_constraints *c,
					    unsigned long *cpus,
					    unsigned long new_cpus,
					    enum pm_qos_req_action new_action,
						bool dev_req)
{
	struct pm_qos_request *req;
	unsigned long new_req_cpus;
	int cpu;

	/*
	 * pm_qos_constraints can be from different classes,
	 * Update cpumask only only for CPU_DMA_LATENCY classes
	 */

	if (c != pm_qos_array[PM_QOS_CPU_DMA_LATENCY]->constraints)
		return -EINVAL;
	/*
	 * pm_qos_set_value_for_cpus expects all c->list elements to be of type
	 * pm_qos_request, however requests from device will contain elements
	 * of type dev_pm_qos_request.
	 * pm_qos_constraints.target_per_cpu can be accessed only for
	 * constraints associated with one of the pm_qos_class and present in
	 * pm_qos_array. Device requests are not associated with any of
	 * pm_qos_class, therefore their target_per_cpu cannot be accessed. We
	 * can safely skip updating target_per_cpu for device requests.
	 */
	if (dev_req)
		return 0;

	if (new_cpus) {
		/* cpus_affine changed, so the old CPUs need to be refreshed */
		new_req_cpus = atomic_read(&new_req->cpus_affine) | new_cpus;
		atomic_set(&new_req->cpus_affine, new_cpus);
	} else {
		new_req_cpus = atomic_read(&new_req->cpus_affine);
	}

	if (new_action != PM_QOS_REMOVE_REQ) {
		bool changed = false;

		for_each_cpu(cpu, to_cpumask(&new_req_cpus)) {
			if (c->target_per_cpu[cpu] != new_req->node.prio) {
				changed = true;
				break;
			}
		}

		if (!changed)
			return 0;
	}

	plist_for_each_entry(req, &c->list, node) {
		unsigned long affected_cpus;

		affected_cpus = atomic_read(&req->cpus_affine) & new_req_cpus;
		if (!affected_cpus)
			continue;

		for_each_cpu(cpu, to_cpumask(&affected_cpus)) {
			if (c->target_per_cpu[cpu] != req->node.prio) {
				c->target_per_cpu[cpu] = req->node.prio;
				*cpus |= BIT(cpu);
			}
		}

		if (!(new_req_cpus &= ~affected_cpus))
			return 0;
	}

	for_each_cpu(cpu, to_cpumask(&new_req_cpus)) {
		if (c->target_per_cpu[cpu] != PM_QOS_CPU_DMA_LAT_DEFAULT_VALUE) {
			c->target_per_cpu[cpu] = PM_QOS_CPU_DMA_LAT_DEFAULT_VALUE;
			*cpus |= BIT(cpu);
		}
	}

	return 0;
}

static int pm_qos_update_target_cpus(struct pm_qos_constraints *c,
				     struct plist_node *node,
				     enum pm_qos_req_action action, int value,
				     unsigned long new_cpus,
					 bool dev_req)
{
	struct pm_qos_request *req = container_of(node, typeof(*req), node);
	int prev_value, curr_value, new_value;
	unsigned long cpus = 0;
	int ret;

	spin_lock(&pm_qos_lock);
	prev_value = pm_qos_get_value(c);
	if (value == PM_QOS_DEFAULT_VALUE)
		new_value = c->default_value;
	else
		new_value = value;

	switch (action) {
	case PM_QOS_REMOVE_REQ:
		plist_del(node, &c->list);
		break;
	case PM_QOS_UPDATE_REQ:
		/*
		 * to change the list, we atomically remove, reinit
		 * with new value and add, then see if the extremal
		 * changed
		 */
		plist_del(node, &c->list);
		/* fall through */
	case PM_QOS_ADD_REQ:
		plist_node_init(node, new_value);
		plist_add(node, &c->list);
		break;
	default:
		/* no action */
		;
	}

	curr_value = pm_qos_get_value(c);
	pm_qos_set_value(c, curr_value);
	ret = pm_qos_set_value_for_cpus(req, c, &cpus, new_cpus, action, dev_req);

	spin_unlock(&pm_qos_lock);

	trace_pm_qos_update_target(action, prev_value, curr_value);

	/*
	 * if cpu mask bits are set, call the notifier call chain
	 * to update the new qos restriction for the cores
	 */

	if (cpus ||
	   (ret && prev_value != curr_value)) {
		ret = 1;
		if (c->notifiers)
			blocking_notifier_call_chain(c->notifiers,
				     (unsigned long)curr_value, &cpus);
	} else {
		ret = 0;
	}
	return ret;
}

/**
 * pm_qos_update_target - manages the constraints list and calls the notifiers
 *  if needed
 * @c: constraints data struct
 * @node: request to add to the list, to update or to remove
 * @action: action to take on the constraints list
 * @value: value of the request to add or update
 *
 * This function returns 1 if the aggregated constraint value has changed, 0
 *  otherwise.
 */
__always_inline int pm_qos_update_target(struct pm_qos_constraints *c, struct plist_node *node,
			 enum pm_qos_req_action action, int value, bool dev_req)
{
	return pm_qos_update_target_cpus(c, node, action, value, 0, dev_req);
}

/**
 * pm_qos_flags_remove_req - Remove device PM QoS flags request.
 * @pqf: Device PM QoS flags set to remove the request from.
 * @req: Request to remove from the set.
 */
static void pm_qos_flags_remove_req(struct pm_qos_flags *pqf,
				    struct pm_qos_flags_request *req)
{
	s32 val = 0;

	list_del(&req->node);
	list_for_each_entry(req, &pqf->list, node)
		val |= req->flags;

	pqf->effective_flags = val;
}

/**
 * pm_qos_update_flags - Update a set of PM QoS flags.
 * @pqf: Set of flags to update.
 * @req: Request to add to the set, to modify, or to remove from the set.
 * @action: Action to take on the set.
 * @val: Value of the request to add or modify.
 *
 * Update the given set of PM QoS flags and call notifiers if the aggregate
 * value has changed.  Returns 1 if the aggregate constraint value has changed,
 * 0 otherwise.
 */
bool pm_qos_update_flags(struct pm_qos_flags *pqf,
			 struct pm_qos_flags_request *req,
			 enum pm_qos_req_action action, s32 val)
{
	s32 prev_value, curr_value;

	spin_lock(&pm_qos_lock);

	prev_value = list_empty(&pqf->list) ? 0 : pqf->effective_flags;

	switch (action) {
	case PM_QOS_REMOVE_REQ:
		pm_qos_flags_remove_req(pqf, req);
		break;
	case PM_QOS_UPDATE_REQ:
		pm_qos_flags_remove_req(pqf, req);
		/* fall through */
	case PM_QOS_ADD_REQ:
		req->flags = val;
		INIT_LIST_HEAD(&req->node);
		list_add_tail(&req->node, &pqf->list);
		pqf->effective_flags |= val;
		break;
	default:
		/* no action */
		;
	}

	curr_value = list_empty(&pqf->list) ? 0 : pqf->effective_flags;

	spin_unlock(&pm_qos_lock);

	trace_pm_qos_update_flags(action, prev_value, curr_value);
	return prev_value != curr_value;
}

/**
 * pm_qos_request - returns current system wide qos expectation
 * @pm_qos_class: identification of which qos value is requested
 *
 * This function returns the current target value.
 */
int pm_qos_request(int pm_qos_class)
{
	return pm_qos_read_value(pm_qos_array[pm_qos_class]->constraints);
}
EXPORT_SYMBOL_GPL(pm_qos_request);

int pm_qos_request_for_cpu(int pm_qos_class, int cpu)
{
	if (cpu_isolated(cpu))
		return INT_MAX;

	return pm_qos_array[pm_qos_class]->constraints->target_per_cpu[cpu];
}
EXPORT_SYMBOL(pm_qos_request_for_cpu);

int pm_qos_request_active(struct pm_qos_request *req)
{
	return req->pm_qos_class != 0;
}
EXPORT_SYMBOL_GPL(pm_qos_request_active);

int pm_qos_request_for_cpumask(int pm_qos_class, struct cpumask *mask)
{
	unsigned long irqflags;
	int cpu;
	struct pm_qos_constraints *c = NULL;
	int val;

	spin_lock_irqsave(&pm_qos_lock, irqflags);
	c = pm_qos_array[pm_qos_class]->constraints;
	val = c->default_value;

	for_each_cpu(cpu, mask) {

		switch (c->type) {
		case PM_QOS_MIN:
			if (c->target_per_cpu[cpu] < val)
				val = c->target_per_cpu[cpu];
			break;
		case PM_QOS_MAX:
			if (c->target_per_cpu[cpu] > val)
				val = c->target_per_cpu[cpu];
			break;
		default:
			break;
		}
	}
	spin_unlock_irqrestore(&pm_qos_lock, irqflags);

	return val;
}
EXPORT_SYMBOL(pm_qos_request_for_cpumask);

static __always_inline void __pm_qos_update_request(struct pm_qos_request *req,
			   s32 new_value)
{
	trace_pm_qos_update_request(req->pm_qos_class, new_value);

	if (new_value != req->node.prio)
		pm_qos_update_target(
			pm_qos_array[req->pm_qos_class]->constraints,
			&req->node, PM_QOS_UPDATE_REQ, new_value, false);
}

#ifdef CONFIG_SMP
static void pm_qos_irq_release(struct kref *ref)
{
	struct irq_affinity_notify *notify = container_of(ref,
					struct irq_affinity_notify, kref);
	struct pm_qos_request *req = container_of(notify,
					struct pm_qos_request, irq_notify);
	struct pm_qos_constraints *c =
				pm_qos_array[req->pm_qos_class]->constraints;

	pm_qos_update_target_cpus(c, &req->node, PM_QOS_UPDATE_REQ,
				  c->default_value, CPUMASK_ALL, false);
}

static void pm_qos_irq_notify(struct irq_affinity_notify *notify,
		const cpumask_t *mask)
{
	struct pm_qos_request *req = container_of(notify,
					struct pm_qos_request, irq_notify);
	struct pm_qos_constraints *c =
				pm_qos_array[req->pm_qos_class]->constraints;

	pm_qos_update_target_cpus(c, &req->node, PM_QOS_UPDATE_REQ,
				  req->node.prio, *cpumask_bits(mask), false);
}
#endif

/**
 * pm_qos_add_request - inserts new qos request into the list
 * @req: pointer to a preallocated handle
 * @pm_qos_class: identifies which list of qos request to use
 * @value: defines the qos request
 *
 * This function inserts a new entry in the pm_qos_class list of requested qos
 * performance characteristics.  It recomputes the aggregate QoS expectations
 * for the pm_qos_class of parameters and initializes the pm_qos_request
 * handle.  Caller needs to save this handle for later use in updates and
 * removal.
 */

void pm_qos_add_request(struct pm_qos_request *req,
			int pm_qos_class, s32 value)
{
	if (!req) /*guard against callers passing in null */
		return;

	if (pm_qos_request_active(req)) {
		WARN(1, KERN_ERR "pm_qos_add_request() called for already added request\n");
		return;
	}

	switch (req->type) {
	case PM_QOS_REQ_AFFINE_CORES:
		if (!atomic_cmpxchg_relaxed(&req->cpus_affine, 0, CPUMASK_ALL)) {
			req->type = PM_QOS_REQ_ALL_CORES;
			WARN(1, "Affine cores not set for request with affinity flag\n");
		}
		break;
#ifdef CONFIG_SMP
	case PM_QOS_REQ_AFFINE_IRQ:
		if (irq_can_set_affinity(req->irq)) {
			struct irq_desc *desc = irq_to_desc(req->irq);
			struct cpumask *mask;

			if (!desc)
				return;

			mask = desc->irq_data.common->affinity;

			/* Get the current affinity */
			atomic_set(&req->cpus_affine, *cpumask_bits(mask));
			req->irq_notify.irq = req->irq;
			req->irq_notify.notify = pm_qos_irq_notify;
			req->irq_notify.release = pm_qos_irq_release;

		} else {
			req->type = PM_QOS_REQ_ALL_CORES;
			atomic_set(&req->cpus_affine, CPUMASK_ALL);
			WARN(1, "IRQ-%d not set for request with affinity flag\n",
					req->irq);
		}
		break;
#endif
	default:
		WARN(1, "Unknown request type %d\n", req->type);
		/* fall through */
	case PM_QOS_REQ_ALL_CORES:
		atomic_set(&req->cpus_affine, CPUMASK_ALL);
		break;
	}

	req->pm_qos_class = pm_qos_class;
	trace_pm_qos_add_request(pm_qos_class, value);
	pm_qos_update_target(pm_qos_array[pm_qos_class]->constraints,
			     &req->node, PM_QOS_ADD_REQ, value, false);

#ifdef CONFIG_SMP
	if (req->type == PM_QOS_REQ_AFFINE_IRQ &&
			irq_can_set_affinity(req->irq)) {
		int ret = 0;

		ret = irq_set_affinity_notifier(req->irq,
					&req->irq_notify);
		if (ret) {
			WARN(1, "IRQ affinity notify set failed\n");
			req->type = PM_QOS_REQ_ALL_CORES;
			atomic_set(&req->cpus_affine, CPUMASK_ALL);
			pm_qos_update_target(
				pm_qos_array[pm_qos_class]->constraints,
				&req->node, PM_QOS_UPDATE_REQ, value, false);
		}
	}
#endif
}
EXPORT_SYMBOL_GPL(pm_qos_add_request);

/**
 * pm_qos_update_request - modifies an existing qos request
 * @req : handle to list element holding a pm_qos request to use
 * @value: defines the qos request
 *
 * Updates an existing qos request for the pm_qos_class of parameters along
 * with updating the target pm_qos_class value.
 *
 * Attempts are made to make this code callable on hot code paths.
 */
__always_inline void pm_qos_update_request(struct pm_qos_request *req,
			   s32 new_value)
{
	if (!req) /*guard against callers passing in null */
		return;

	if (!pm_qos_request_active(req)) {
		WARN(1, KERN_ERR "pm_qos_update_request() called for unknown object\n");
		return;
	}

	__pm_qos_update_request(req, new_value);
}
EXPORT_SYMBOL_GPL(pm_qos_update_request);

/**
 * pm_qos_remove_request - modifies an existing qos request
 * @req: handle to request list element
 *
 * Will remove pm qos request from the list of constraints and
 * recompute the current target value for the pm_qos_class.  Call this
 * on slow code paths.
 */
void pm_qos_remove_request(struct pm_qos_request *req)
{
	if (!req) /*guard against callers passing in null */
		return;
		/* silent return to keep pcm code cleaner */

	if (!pm_qos_request_active(req)) {
		WARN(1, "%s called for unknown object\n", __func__);
		return;
	}

#ifdef CONFIG_SMP
	if (req->type == PM_QOS_REQ_AFFINE_IRQ) {
		int ret = 0;
		/* Get the current affinity */
		ret = irq_set_affinity_notifier(req->irq, NULL);
		if (ret)
			WARN(1, "IRQ affinity notify set failed\n");
	}
#endif

	trace_pm_qos_remove_request(req->pm_qos_class, PM_QOS_DEFAULT_VALUE);
	pm_qos_update_target(pm_qos_array[req->pm_qos_class]->constraints,
			     &req->node, PM_QOS_REMOVE_REQ,
			     PM_QOS_DEFAULT_VALUE, false);
	memset(req, 0, sizeof(*req));
}
EXPORT_SYMBOL_GPL(pm_qos_remove_request);

/**
 * pm_qos_add_notifier - sets notification entry for changes to target value
 * @pm_qos_class: identifies which qos target changes should be notified.
 * @notifier: notifier block managed by caller.
 *
 * will register the notifier into a notification chain that gets called
 * upon changes to the pm_qos_class target value.
 */
int pm_qos_add_notifier(int pm_qos_class, struct notifier_block *notifier)
{
	int retval;

	retval = blocking_notifier_chain_register(
			pm_qos_array[pm_qos_class]->constraints->notifiers,
			notifier);

	return retval;
}
EXPORT_SYMBOL_GPL(pm_qos_add_notifier);

/**
 * pm_qos_remove_notifier - deletes notification entry from chain.
 * @pm_qos_class: identifies which qos target changes are notified.
 * @notifier: notifier block to be removed.
 *
 * will remove the notifier from the notification chain that gets called
 * upon changes to the pm_qos_class target value.
 */
int pm_qos_remove_notifier(int pm_qos_class, struct notifier_block *notifier)
{
	int retval;

	retval = blocking_notifier_chain_unregister(
			pm_qos_array[pm_qos_class]->constraints->notifiers,
			notifier);

	return retval;
}
EXPORT_SYMBOL_GPL(pm_qos_remove_notifier);

/* User space interface to PM QoS classes via misc devices */
static int register_pm_qos_misc(struct pm_qos_object *qos, struct dentry *d)
{
	qos->pm_qos_power_miscdev.minor = MISC_DYNAMIC_MINOR;
	qos->pm_qos_power_miscdev.name = qos->name;
	qos->pm_qos_power_miscdev.fops = &pm_qos_power_fops;

	debugfs_create_file(qos->name, S_IRUGO, d, (void *)qos,
			    &pm_qos_debug_fops);

	return misc_register(&qos->pm_qos_power_miscdev);
}

static int find_pm_qos_object_by_minor(int minor)
{
	int pm_qos_class;

	for (pm_qos_class = PM_QOS_CPU_DMA_LATENCY;
		pm_qos_class < PM_QOS_NUM_CLASSES; pm_qos_class++) {
		if (minor ==
			pm_qos_array[pm_qos_class]->pm_qos_power_miscdev.minor)
			return pm_qos_class;
	}
	return -1;
}

static int pm_qos_power_open(struct inode *inode, struct file *filp)
{
	long pm_qos_class;

	pm_qos_class = find_pm_qos_object_by_minor(iminor(inode));
	if (pm_qos_class >= PM_QOS_CPU_DMA_LATENCY) {
		struct pm_qos_request *req = kzalloc(sizeof(*req), GFP_KERNEL);
		if (!req)
			return -ENOMEM;

		pm_qos_add_request(req, pm_qos_class, PM_QOS_DEFAULT_VALUE);
		filp->private_data = req;

		return 0;
	}
	return -EPERM;
}

static int pm_qos_power_release(struct inode *inode, struct file *filp)
{
	struct pm_qos_request *req;

	req = filp->private_data;
	pm_qos_remove_request(req);
	kfree(req);

	return 0;
}


static ssize_t pm_qos_power_read(struct file *filp, char __user *buf,
		size_t count, loff_t *f_pos)
{
	s32 value;
	struct pm_qos_request *req = filp->private_data;

	if (!req)
		return -EINVAL;
	if (!pm_qos_request_active(req))
		return -EINVAL;

	spin_lock(&pm_qos_lock);
	value = pm_qos_get_value(pm_qos_array[req->pm_qos_class]->constraints);
	spin_unlock(&pm_qos_lock);

	return simple_read_from_buffer(buf, count, f_pos, &value, sizeof(s32));
}

static ssize_t pm_qos_power_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *f_pos)
{
	s32 value;
	struct pm_qos_request *req;

	/* Don't let userspace impose restrictions on CPU idle levels */
	return count;

	if (count == sizeof(s32)) {
		if (copy_from_user(&value, buf, sizeof(s32)))
			return -EFAULT;
	} else {
		int ret;

		ret = kstrtos32_from_user(buf, count, 16, &value);
		if (ret)
			return ret;
	}

	switch (value) {
		case 0x44:
			value = 44;
			break;
		case 0x100:
			return count;
			break;
	}

	req = filp->private_data;
	pm_qos_update_request(req, value);

	return count;
}


static int __init pm_qos_power_init(void)
{
	int ret = 0;
	int i;
	struct dentry *d;

	BUILD_BUG_ON(ARRAY_SIZE(pm_qos_array) != PM_QOS_NUM_CLASSES);

	/* Don't let userspace impose restrictions on CPU idle levels */
	return 0;

	d = debugfs_create_dir("pm_qos", NULL);

	for (i = PM_QOS_CPU_DMA_LATENCY; i < PM_QOS_NUM_CLASSES; i++) {
		ret = register_pm_qos_misc(pm_qos_array[i], d);
		if (ret < 0) {
			pr_err("%s: %s setup failed\n",
			       __func__, pm_qos_array[i]->name);
			return ret;
		}
	}

	return ret;
}

late_initcall(pm_qos_power_init);
