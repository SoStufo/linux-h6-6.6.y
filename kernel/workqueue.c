// SPDX-License-Identifier: GPL-2.0-only
/*
 * kernel/workqueue.c - generic async execution with shared worker pool
 *
 * Copyright (C) 2002		Ingo Molnar
 *
 *   Derived from the taskqueue/keventd code by:
 *     David Woodhouse <dwmw2@infradead.org>
 *     Andrew Morton
 *     Kai Petzke <wpp@marie.physik.tu-berlin.de>
 *     Theodore Ts'o <tytso@mit.edu>
 *
 * Made to use alloc_percpu by Christoph Lameter.
 *
 * Copyright (C) 2010		SUSE Linux Products GmbH
 * Copyright (C) 2010		Tejun Heo <tj@kernel.org>
 *
 * This is the generic async execution mechanism.  Work items as are
 * executed in process context.  The worker pool is shared and
 * automatically managed.  There are two worker pools for each CPU (one for
 * normal work items and the other for high priority ones) and some extra
 * pools for workqueues which are not bound to any specific CPU - the
 * number of these backing pools is dynamic.
 *
 * Please read Documentation/core-api/workqueue.rst for details.
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/signal.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/kthread.h>
#include <linux/hardirq.h>
#include <linux/mempolicy.h>
#include <linux/freezer.h>
#include <linux/debug_locks.h>
#include <linux/lockdep.h>
#include <linux/idr.h>
#include <linux/jhash.h>
#include <linux/hashtable.h>
#include <linux/rculist.h>
#include <linux/nodemask.h>
#include <linux/moduleparam.h>
#include <linux/uaccess.h>
#include <linux/sched/isolation.h>
#include <linux/sched/debug.h>
#include <linux/nmi.h>
#include <linux/kvm_para.h>
#include <linux/delay.h>

#include "workqueue_internal.h"

enum {
	/*
	 * worker_pool flags
	 *
	 * A bound pool is either associated or disassociated with its CPU.
	 * While associated (!DISASSOCIATED), all workers are bound to the
	 * CPU and none has %WORKER_UNBOUND set and concurrency management
	 * is in effect.
	 *
	 * While DISASSOCIATED, the cpu may be offline and all workers have
	 * %WORKER_UNBOUND set and concurrency management disabled, and may
	 * be executing on any CPU.  The pool behaves as an unbound one.
	 *
	 * Note that DISASSOCIATED should be flipped only while holding
	 * wq_pool_attach_mutex to avoid changing binding state while
	 * worker_attach_to_pool() is in progress.
	 */
	POOL_MANAGER_ACTIVE	= 1 << 0,	/* being managed */
	POOL_DISASSOCIATED	= 1 << 2,	/* cpu can't serve workers */

	/* worker flags */
	WORKER_DIE		= 1 << 1,	/* die die die */
	WORKER_IDLE		= 1 << 2,	/* is idle */
	WORKER_PREP		= 1 << 3,	/* preparing to run works */
	WORKER_CPU_INTENSIVE	= 1 << 6,	/* cpu intensive */
	WORKER_UNBOUND		= 1 << 7,	/* worker is unbound */
	WORKER_REBOUND		= 1 << 8,	/* worker was rebound */

	WORKER_NOT_RUNNING	= WORKER_PREP | WORKER_CPU_INTENSIVE |
				  WORKER_UNBOUND | WORKER_REBOUND,

	NR_STD_WORKER_POOLS	= 2,		/* # standard pools per cpu */

	UNBOUND_POOL_HASH_ORDER	= 6,		/* hashed by pool->attrs */
	BUSY_WORKER_HASH_ORDER	= 6,		/* 64 pointers */

	MAX_IDLE_WORKERS_RATIO	= 4,		/* 1/4 of busy can be idle */
	IDLE_WORKER_TIMEOUT	= 300 * HZ,	/* keep idle ones for 5 mins */

	MAYDAY_INITIAL_TIMEOUT  = HZ / 100 >= 2 ? HZ / 100 : 2,
						/* call for help after 10ms
						   (min two ticks) */
	MAYDAY_INTERVAL		= HZ / 10,	/* and then every 100ms */
	CREATE_COOLDOWN		= HZ,		/* time to breath after fail */

	/*
	 * Rescue workers are used only on emergencies and shared by
	 * all cpus.  Give MIN_NICE.
	 */
	RESCUER_NICE_LEVEL	= MIN_NICE,
	HIGHPRI_NICE_LEVEL	= MIN_NICE,

	WQ_NAME_LEN		= 24,
};

/*
 * Structure fields follow one of the following exclusion rules.
 *
 * I: Modifiable by initialization/destruction paths and read-only for
 *    everyone else.
 *
 * P: Preemption protected.  Disabling preemption is enough and should
 *    only be modified and accessed from the local cpu.
 *
 * L: pool->lock protected.  Access with pool->lock held.
 *
 * K: Only modified by worker while holding pool->lock. Can be safely read by
 *    self, while holding pool->lock or from IRQ context if %current is the
 *    kworker.
 *
 * S: Only modified by worker self.
 *
 * A: wq_pool_attach_mutex protected.
 *
 * PL: wq_pool_mutex protected.
 *
 * PR: wq_pool_mutex protected for writes.  RCU protected for reads.
 *
 * PW: wq_pool_mutex and wq->mutex protected for writes.  Either for reads.
 *
 * PWR: wq_pool_mutex and wq->mutex protected for writes.  Either or
 *      RCU for reads.
 *
 * WQ: wq->mutex protected.
 *
 * WR: wq->mutex protected for writes.  RCU protected for reads.
 *
 * MD: wq_mayday_lock protected.
 *
 * WD: Used internally by the watchdog.
 */

/* struct worker is defined in workqueue_internal.h */

struct worker_pool {
	raw_spinlock_t		lock;		/* the pool lock */
	int			cpu;		/* I: the associated cpu */
	int			node;		/* I: the associated node ID */
	int			id;		/* I: pool ID */
	unsigned int		flags;		/* L: flags */

	unsigned long		watchdog_ts;	/* L: watchdog timestamp */
	bool			cpu_stall;	/* WD: stalled cpu bound pool */

	/*
	 * The counter is incremented in a process context on the associated CPU
	 * w/ preemption disabled, and decremented or reset in the same context
	 * but w/ pool->lock held. The readers grab pool->lock and are
	 * guaranteed to see if the counter reached zero.
	 */
	int			nr_running;

	struct list_head	worklist;	/* L: list of pending works */

	int			nr_workers;	/* L: total number of workers */
	int			nr_idle;	/* L: currently idle workers */

	struct list_head	idle_list;	/* L: list of idle workers */
	struct timer_list	idle_timer;	/* L: worker idle timeout */
	struct work_struct      idle_cull_work; /* L: worker idle cleanup */

	struct timer_list	mayday_timer;	  /* L: SOS timer for workers */

	/* a workers is either on busy_hash or idle_list, or the manager */
	DECLARE_HASHTABLE(busy_hash, BUSY_WORKER_HASH_ORDER);
						/* L: hash of busy workers */

	struct worker		*manager;	/* L: purely informational */
	struct list_head	workers;	/* A: attached workers */
	struct list_head        dying_workers;  /* A: workers about to die */
	struct completion	*detach_completion; /* all workers detached */

	struct ida		worker_ida;	/* worker IDs for task name */

	struct workqueue_attrs	*attrs;		/* I: worker attributes */
	struct hlist_node	hash_node;	/* PL: unbound_pool_hash node */
	int			refcnt;		/* PL: refcnt for unbound pools */

	/*
	 * Destruction of pool is RCU protected to allow dereferences
	 * from get_work_pool().
	 */
	struct rcu_head		rcu;
};

/*
 * Per-pool_workqueue statistics. These can be monitored using
 * tools/workqueue/wq_monitor.py.
 */
enum pool_workqueue_stats {
	PWQ_STAT_STARTED,	/* work items started execution */
	PWQ_STAT_COMPLETED,	/* work items completed execution */
	PWQ_STAT_CPU_TIME,	/* total CPU time consumed */
	PWQ_STAT_CPU_INTENSIVE,	/* wq_cpu_intensive_thresh_us violations */
	PWQ_STAT_CM_WAKEUP,	/* concurrency-management worker wakeups */
	PWQ_STAT_REPATRIATED,	/* unbound workers brought back into scope */
	PWQ_STAT_MAYDAY,	/* maydays to rescuer */
	PWQ_STAT_RESCUED,	/* linked work items executed by rescuer */

	PWQ_NR_STATS,
};

/*
 * The per-pool workqueue.  While queued, the lower WORK_STRUCT_FLAG_BITS
 * of work_struct->data are used for flags and the remaining high bits
 * point to the pwq; thus, pwqs need to be aligned at two's power of the
 * number of flag bits.
 */
struct pool_workqueue {
	struct worker_pool	*pool;		/* I: the associated pool */
	struct workqueue_struct *wq;		/* I: the owning workqueue */
	int			work_color;	/* L: current color */
	int			flush_color;	/* L: flushing color */
	int			refcnt;		/* L: reference count */
	int			nr_in_flight[WORK_NR_COLORS];
						/* L: nr of in_flight works */

	/*
	 * nr_active management and WORK_STRUCT_INACTIVE:
	 *
	 * When pwq->nr_active >= max_active, new work item is queued to
	 * pwq->inactive_works instead of pool->worklist and marked with
	 * WORK_STRUCT_INACTIVE.
	 *
	 * All work items marked with WORK_STRUCT_INACTIVE do not participate
	 * in pwq->nr_active and all work items in pwq->inactive_works are
	 * marked with WORK_STRUCT_INACTIVE.  But not all WORK_STRUCT_INACTIVE
	 * work items are in pwq->inactive_works.  Some of them are ready to
	 * run in pool->worklist or worker->scheduled.  Those work itmes are
	 * only struct wq_barrier which is used for flush_work() and should
	 * not participate in pwq->nr_active.  For non-barrier work item, it
	 * is marked with WORK_STRUCT_INACTIVE iff it is in pwq->inactive_works.
	 */
	int			nr_active;	/* L: nr of active works */
	int			max_active;	/* L: max active works */
	struct list_head	inactive_works;	/* L: inactive works */
	struct list_head	pwqs_node;	/* WR: node on wq->pwqs */
	struct list_head	mayday_node;	/* MD: node on wq->maydays */

	u64			stats[PWQ_NR_STATS];

	/*
	 * Release of unbound pwq is punted to a kthread_worker. See put_pwq()
	 * and pwq_release_workfn() for details. pool_workqueue itself is also
	 * RCU protected so that the first pwq can be determined without
	 * grabbing wq->mutex.
	 */
	struct kthread_work	release_work;
	struct rcu_head		rcu;
} __aligned(1 << WORK_STRUCT_FLAG_BITS);

/*
 * Structure used to wait for workqueue flush.
 */
struct wq_flusher {
	struct list_head	list;		/* WQ: list of flushers */
	int			flush_color;	/* WQ: flush color waiting for */
	struct completion	done;		/* flush completion */
};

struct wq_device;

/*
 * The externally visible workqueue.  It relays the issued work items to
 * the appropriate worker_pool through its pool_workqueues.
 */
struct workqueue_struct {
	struct list_head	pwqs;		/* WR: all pwqs of this wq */
	struct list_head	list;		/* PR: list of all workqueues */

	struct mutex		mutex;		/* protects this wq */
	int			work_color;	/* WQ: current work color */
	int			flush_color;	/* WQ: current flush color */
	atomic_t		nr_pwqs_to_flush; /* flush in progress */
	struct wq_flusher	*first_flusher;	/* WQ: first flusher */
	struct list_head	flusher_queue;	/* WQ: flush waiters */
	struct list_head	flusher_overflow; /* WQ: flush overflow list */

	struct list_head	maydays;	/* MD: pwqs requesting rescue */
	struct worker		*rescuer;	/* MD: rescue worker */

	int			nr_drainers;	/* WQ: drain in progress */
	int			saved_max_active; /* WQ: saved pwq max_active */

	struct workqueue_attrs	*unbound_attrs;	/* PW: only for unbound wqs */
	struct pool_workqueue	*dfl_pwq;	/* PW: only for unbound wqs */

#ifdef CONFIG_SYSFS
	struct wq_device	*wq_dev;	/* I: for sysfs interface */
#endif
#ifdef CONFIG_LOCKDEP
	char			*lock_name;
	struct lock_class_key	key;
	struct lockdep_map	lockdep_map;
#endif
	char			name[WQ_NAME_LEN]; /* I: workqueue name */

	/*
	 * Destruction of workqueue_struct is RCU protected to allow walking
	 * the workqueues list without grabbing wq_pool_mutex.
	 * This is used to dump all workqueues from sysrq.
	 */
	struct rcu_head		rcu;

	/* hot fields used during command issue, aligned to cacheline */
	unsigned int		flags ____cacheline_aligned; /* WQ: WQ_* flags */
	struct pool_workqueue __percpu __rcu **cpu_pwq; /* I: per-cpu pwqs */
};

static struct kmem_cache *pwq_cache;

/*
 * Each pod type describes how CPUs should be grouped for unbound workqueues.
 * See the comment above workqueue_attrs->affn_scope.
 */
struct wq_pod_type {
	int			nr_pods;	/* number of pods */
	cpumask_var_t		*pod_cpus;	/* pod -> cpus */
	int			*pod_node;	/* pod -> node */
	int			*cpu_pod;	/* cpu -> pod */
};

static struct wq_pod_type wq_pod_types[WQ_AFFN_NR_TYPES];
static enum wq_affn_scope wq_affn_dfl = WQ_AFFN_CACHE;

static const char *wq_affn_names[WQ_AFFN_NR_TYPES] = {
	[WQ_AFFN_DFL]			= "default",
	[WQ_AFFN_CPU]			= "cpu",
	[WQ_AFFN_SMT]			= "smt",
	[WQ_AFFN_CACHE]			= "cache",
	[WQ_AFFN_NUMA]			= "numa",
	[WQ_AFFN_SYSTEM]		= "system",
};

/*
 * Per-cpu work items which run for longer than the following threshold are
 * automatically considered CPU intensive and excluded from concurrency
 * management to prevent them from noticeably delaying other per-cpu work items.
 * ULONG_MAX indicates that the user hasn't overridden it with a boot parameter.
 * The actual value is initialized in wq_cpu_intensive_thresh_init().
 */
static unsigned long wq_cpu_intensive_thresh_us = ULONG_MAX;
module_param_named(cpu_intensive_thresh_us, wq_cpu_intensive_thresh_us, ulong, 0644);

/* see the comment above the definition of WQ_POWER_EFFICIENT */
static bool wq_power_efficient = IS_ENABLED(CONFIG_WQ_POWER_EFFICIENT_DEFAULT);
module_param_named(power_efficient, wq_power_efficient, bool, 0444);

static bool wq_online;			/* can kworkers be created yet? */

/* buf for wq_update_unbound_pod_attrs(), protected by CPU hotplug exclusion */
static struct workqueue_attrs *wq_update_pod_attrs_buf;

static DEFINE_MUTEX(wq_pool_mutex);	/* protects pools and workqueues list */
static DEFINE_MUTEX(wq_pool_attach_mutex); /* protects worker attach/detach */
static DEFINE_RAW_SPINLOCK(wq_mayday_lock);	/* protects wq->maydays list */
/* wait for manager to go away */
static struct rcuwait manager_wait = __RCUWAIT_INITIALIZER(manager_wait);

static LIST_HEAD(workqueues);		/* PR: list of all workqueues */
static bool workqueue_freezing;		/* PL: have wqs started freezing? */

/* PL&A: allowable cpus for unbound wqs and work items */
static cpumask_var_t wq_unbound_cpumask;

/* for further constrain wq_unbound_cpumask by cmdline parameter*/
static struct cpumask wq_cmdline_cpumask __initdata;

/* CPU where unbound work was last round robin scheduled from this CPU */
static DEFINE_PER_CPU(int, wq_rr_cpu_last);

/*
 * Local execution of unbound work items is no longer guaranteed.  The
 * following always forces round-robin CPU selection on unbound work items
 * to uncover usages which depend on it.
 */
#ifdef CONFIG_DEBUG_WQ_FORCE_RR_CPU
static bool wq_debug_force_rr_cpu = true;
#else
static bool wq_debug_force_rr_cpu = false;
#endif
module_param_named(debug_force_rr_cpu, wq_debug_force_rr_cpu, bool, 0644);

/* the per-cpu worker pools */
static DEFINE_PER_CPU_SHARED_ALIGNED(struct worker_pool [NR_STD_WORKER_POOLS], cpu_worker_pools);

static DEFINE_IDR(worker_pool_idr);	/* PR: idr of all pools */

/* PL: hash of all unbound pools keyed by pool->attrs */
static DEFINE_HASHTABLE(unbound_pool_hash, UNBOUND_POOL_HASH_ORDER);

/* I: attributes used when instantiating standard unbound pools on demand */
static struct workqueue_attrs *unbound_std_wq_attrs[NR_STD_WORKER_POOLS];

/* I: attributes used when instantiating ordered pools on demand */
static struct workqueue_attrs *ordered_wq_attrs[NR_STD_WORKER_POOLS];

/*
 * I: kthread_worker to release pwq's. pwq release needs to be bounced to a
 * process context while holding a pool lock. Bounce to a dedicated kthread
 * worker to avoid A-A deadlocks.
 */
static struct kthread_worker *pwq_release_worker;

struct workqueue_struct *system_wq __read_mostly;
EXPORT_SYMBOL(system_wq);
struct workqueue_struct *system_highpri_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_highpri_wq);
struct workqueue_struct *system_long_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_long_wq);
struct workqueue_struct *system_unbound_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_unbound_wq);
struct workqueue_struct *system_freezable_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_freezable_wq);
struct workqueue_struct *system_power_efficient_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_power_efficient_wq);
struct workqueue_struct *system_freezable_power_efficient_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_freezable_power_efficient_wq);

static int worker_thread(void *__worker);
static void workqueue_sysfs_unregister(struct workqueue_struct *wq);
static void show_pwq(struct pool_workqueue *pwq);
static void show_one_worker_pool(struct worker_pool *pool);

#define CREATE_TRACE_POINTS
#include <trace/events/workqueue.h>

#define assert_rcu_or_pool_mutex()					\
	RCU_LOCKDEP_WARN(!rcu_read_lock_held() &&			\
			 !lockdep_is_held(&wq_pool_mutex),		\
			 "RCU or wq_pool_mutex should be held")

#define assert_rcu_or_wq_mutex_or_pool_mutex(wq)			\
	RCU_LOCKDEP_WARN(!rcu_read_lock_held() &&			\
			 !lockdep_is_held(&wq->mutex) &&		\
			 !lockdep_is_held(&wq_pool_mutex),		\
			 "RCU, wq->mutex or wq_pool_mutex should be held")

#define for_each_cpu_worker_pool(pool, cpu)				\
	for ((pool) = &per_cpu(cpu_worker_pools, cpu)[0];		\
	     (pool) < &per_cpu(cpu_worker_pools, cpu)[NR_STD_WORKER_POOLS]; \
	     (pool)++)

/**
 * for_each_pool - iterate through all worker_pools in the system
 * @pool: iteration cursor
 * @pi: integer used for iteration
 *
 * This must be called either with wq_pool_mutex held or RCU read
 * locked.  If the pool needs to be used beyond the locking in effect, the
 * caller is responsible for guaranteeing that the pool stays online.
 *
 * The if/else clause exists only for the lockdep assertion and can be
 * ignored.
 */
#define for_each_pool(pool, pi)						\
	idr_for_each_entry(&worker_pool_idr, pool, pi)			\
		if (({ assert_rcu_or_pool_mutex(); false; })) { }	\
		else

/**
 * for_each_pool_worker - iterate through all workers of a worker_pool
 * @worker: iteration cursor
 * @pool: worker_pool to iterate workers of
 *
 * This must be called with wq_pool_attach_mutex.
 *
 * The if/else clause exists only for the lockdep assertion and can be
 * ignored.
 */
#define for_each_pool_worker(worker, pool)				\
	list_for_each_entry((worker), &(pool)->workers, node)		\
		if (({ lockdep_assert_held(&wq_pool_attach_mutex); false; })) { } \
		else

/**
 * for_each_pwq - iterate through all pool_workqueues of the specified workqueue
 * @pwq: iteration cursor
 * @wq: the target workqueue
 *
 * This must be called either with wq->mutex held or RCU read locked.
 * If the pwq needs to be used beyond the locking in effect, the caller is
 * responsible for guaranteeing that the pwq stays online.
 *
 * The if/else clause exists only for the lockdep assertion and can be
 * ignored.
 */
#define for_each_pwq(pwq, wq)						\
	list_for_each_entry_rcu((pwq), &(wq)->pwqs, pwqs_node,		\
				 lockdep_is_held(&(wq->mutex)))

#ifdef CONFIG_DEBUG_OBJECTS_WORK

static const struct debug_obj_descr work_debug_descr;

static void *work_debug_hint(void *addr)
{
	return ((struct work_struct *) addr)->func;
}

static bool work_is_static_object(void *addr)
{
	struct work_struct *work = addr;

	return test_bit(WORK_STRUCT_STATIC_BIT, work_data_bits(work));
}

/*
 * fixup_init is called when:
 * - an active object is initialized
 */
static bool work_fixup_init(void *addr, enum debug_obj_state state)
{
	struct work_struct *work = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		cancel_work_sync(work);
		debug_object_init(work, &work_debug_descr);
		return true;
	default:
		return false;
	}
}

/*
 * fixup_free is called when:
 * - an active object is freed
 */
static bool work_fixup_free(void *addr, enum debug_obj_state state)
{
	struct work_struct *work = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		cancel_work_sync(work);
		debug_object_free(work, &work_debug_descr);
		return true;
	default:
		return false;
	}
}

static const struct debug_obj_descr work_debug_descr = {
	.name		= "work_struct",
	.debug_hint	= work_debug_hint,
	.is_static_object = work_is_static_object,
	.fixup_init	= work_fixup_init,
	.fixup_free	= work_fixup_free,
};

static inline void debug_work_activate(struct work_struct *work)
{
	debug_object_activate(work, &work_debug_descr);
}

static inline void debug_work_deactivate(struct work_struct *work)
{
	debug_object_deactivate(work, &work_debug_descr);
}

void __init_work(struct work_struct *work, int onstack)
{
	if (onstack)
		debug_object_init_on_stack(work, &work_debug_descr);
	else
		debug_object_init(work, &work_debug_descr);
}
EXPORT_SYMBOL_GPL(__init_work);

void destroy_work_on_stack(struct work_struct *work)
{
	debug_object_free(work, &work_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_work_on_stack);

void destroy_delayed_work_on_stack(struct delayed_work *work)
{
	destroy_timer_on_stack(&work->timer);
	debug_object_free(&work->work, &work_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_delayed_work_on_stack);

#else
static inline void debug_work_activate(struct work_struct *work) { }
static inline void debug_work_deactivate(struct work_struct *work) { }
#endif

/**
 * worker_pool_assign_id - allocate ID and assign it to @pool
 * @pool: the pool pointer of interest
 *
 * Returns 0 if ID in [0, WORK_OFFQ_POOL_NONE) is allocated and assigned
 * successfully, -errno on failure.
 */
static int worker_pool_assign_id(struct worker_pool *pool)
{
	int ret;

	lockdep_assert_held(&wq_pool_mutex);

	ret = idr_alloc(&worker_pool_idr, pool, 0, WORK_OFFQ_POOL_NONE,
			GFP_KERNEL);
	if (ret >= 0) {
		pool->id = ret;
		return 0;
	}
	return ret;
}

static unsigned int work_color_to_flags(int color)
{
	return color << WORK_STRUCT_COLOR_SHIFT;
}

static int get_work_color(unsigned long work_data)
{
	return (work_data >> WORK_STRUCT_COLOR_SHIFT) &
		((1 << WORK_STRUCT_COLOR_BITS) - 1);
}

static int work_next_color(int color)
{
	return (color + 1) % WORK_NR_COLORS;
}

/*
 * While queued, %WORK_STRUCT_PWQ is set and non flag bits of a work's data
 * contain the pointer to the queued pwq.  Once execution starts, the flag
 * is cleared and the high bits contain OFFQ flags and pool ID.
 *
 * set_work_pwq(), set_work_pool_and_clear_pending(), mark_work_canceling()
 * and clear_work_data() can be used to set the pwq, pool or clear
 * work->data.  These functions should only be called while the work is
 * owned - ie. while the PENDING bit is set.
 *
 * get_work_pool() and get_work_pwq() can be used to obtain the pool or pwq
 * corresponding to a work.  Pool is available once the work has been
 * queued anywhere after initialization until it is sync canceled.  pwq is
 * available only while the work item is queued.
 *
 * %WORK_OFFQ_CANCELING is used to mark a work item which is being
 * canceled.  While being canceled, a work item may have its PENDING set
 * but stay off timer and worklist for arbitrarily long and nobody should
 * try to steal the PENDING bit.
 */
static inline void set_work_data(struct work_struct *work, unsigned long data,
				 unsigned long flags)
{
	WARN_ON_ONCE(!work_pending(work));
	atomic_long_set(&work->data, data | flags | work_static(work));
}

static void set_work_pwq(struct work_struct *work, struct pool_workqueue *pwq,
			 unsigned long extra_flags)
{
	set_work_data(work, (unsigned long)pwq,
		      WORK_STRUCT_PENDING | WORK_STRUCT_PWQ | extra_flags);
}

static void set_work_pool_and_keep_pending(struct work_struct *work,
					   int pool_id)
{
	set_work_data(work, (unsigned long)pool_id << WORK_OFFQ_POOL_SHIFT,
		      WORK_STRUCT_PENDING);
}

static void set_work_pool_and_clear_pending(struct work_struct *work,
					    int pool_id)
{
	/*
	 * The following wmb is paired with the implied mb in
	 * test_and_set_bit(PENDING) and ensures all updates to @work made
	 * here are visible to and precede any updates by the next PENDING
	 * owner.
	 */
	smp_wmb();
	set_work_data(work, (unsigned long)pool_id << WORK_OFFQ_POOL_SHIFT, 0);
	/*
	 * The following mb guarantees that previous clear of a PENDING bit
	 * will not be reordered with any speculative LOADS or STORES from
	 * work->current_func, which is executed afterwards.  This possible
	 * reordering can lead to a missed execution on attempt to queue
	 * the same @work.  E.g. consider this case:
	 *
	 *   CPU#0                         CPU#1
	 *   ----------------------------  --------------------------------
	 *
	 * 1  STORE event_indicated
	 * 2  queue_work_on() {
	 * 3    test_and_set_bit(PENDING)
	 * 4 }                             set_..._and_clear_pending() {
	 * 5                                 set_work_data() # clear bit
	 * 6                                 smp_mb()
	 * 7                               work->current_func() {
	 * 8				      LOAD event_indicated
	 *				   }
	 *
	 * Without an explicit full barrier speculative LOAD on line 8 can
	 * be executed before CPU#0 does STORE on line 1.  If that happens,
	 * CPU#0 observes the PENDING bit is still set and new execution of
	 * a @work is not queued in a hope, that CPU#1 will eventually
	 * finish the queued @work.  Meanwhile CPU#1 does not see
	 * event_indicated is set, because speculative LOAD was executed
	 * before actual STORE.
	 */
	smp_mb();
}

static void clear_work_data(struct work_struct *work)
{
	smp_wmb();	/* see set_work_pool_and_clear_pending() */
	set_work_data(work, WORK_STRUCT_NO_POOL, 0);
}

static inline struct pool_workqueue *work_struct_pwq(unsigned long data)
{
	return (struct pool_workqueue *)(data & WORK_STRUCT_WQ_DATA_MASK);
}

static struct pool_workqueue *get_work_pwq(struct work_struct *work)
{
	unsigned long data = atomic_long_read(&work->data);

	if (data & WORK_STRUCT_PWQ)
		return work_struct_pwq(data);
	else
		return NULL;
}

/**
 * get_work_pool - return the worker_pool a given work was associated with
 * @work: the work item of interest
 *
 * Pools are created and destroyed under wq_pool_mutex, and allows read
 * access under RCU read lock.  As such, this function should be
 * called under wq_pool_mutex or inside of a rcu_read_lock() region.
 *
 * All fields of the returned pool are accessible as long as the above
 * mentioned locking is in effect.  If the returned pool needs to be used
 * beyond the critical section, the caller is responsible for ensuring the
 * returned pool is and stays online.
 *
 * Return: The worker_pool @work was last associated with.  %NULL if none.
 */
static struct worker_pool *get_work_pool(struct work_struct *work)
{
	unsigned long data = atomic_long_read(&work->data);
	int pool_id;

	assert_rcu_or_pool_mutex();

	if (data & WORK_STRUCT_PWQ)
		return work_struct_pwq(data)->pool;

	pool_id = data >> WORK_OFFQ_POOL_SHIFT;
	if (pool_id == WORK_OFFQ_POOL_NONE)
		return NULL;

	return idr_find(&worker_pool_idr, pool_id);
}

/**
 * get_work_pool_id - return the worker pool ID a given work is associated with
 * @work: the work item of interest
 *
 * Return: The worker_pool ID @work was last associated with.
 * %WORK_OFFQ_POOL_NONE if none.
 */
static int get_work_pool_id(struct work_struct *work)
{
	unsigned long data = atomic_long_read(&work->data);

	if (data & WORK_STRUCT_PWQ)
		return work_struct_pwq(data)->pool->id;

	return data >> WORK_OFFQ_POOL_SHIFT;
}

static void mark_work_canceling(struct work_struct *work)
{
	unsigned long pool_id = get_work_pool_id(work);

	pool_id <<= WORK_OFFQ_POOL_SHIFT;
	set_work_data(work, pool_id | WORK_OFFQ_CANCELING, WORK_STRUCT_PENDING);
}

static bool work_is_canceling(struct work_struct *work)
{
	unsigned long data = atomic_long_read(&work->data);

	return !(data & WORK_STRUCT_PWQ) && (data & WORK_OFFQ_CANCELING);
}

/*
 * Policy functions.  These define the policies on how the global worker
 * pools are managed.  Unless noted otherwise, these functions assume that
 * they're being called with pool->lock held.
 */

/*
 * Need to wake up a worker?  Called from anything but currently
 * running workers.
 *
 * Note that, because unbound workers never contribute to nr_running, this
 * function will always return %true for unbound pools as long as the
 * worklist isn't empty.
 */
static bool need_more_worker(struct worker_pool *pool)
{
	return !list_empty(&pool->worklist) && !pool->nr_running;
}

/* Can I start working?  Called from busy but !running workers. */
static bool may_start_working(struct worker_pool *pool)
{
	return pool->nr_idle;
}

/* Do I need to keep working?  Called from currently running workers. */
static bool keep_working(struct worker_pool *pool)
{
	return !list_empty(&pool->worklist) && (pool->nr_running <= 1);
}

/* Do we need a new worker?  Called from manager. */
static bool need_to_create_worker(struct worker_pool *pool)
{
	return need_more_worker(pool) && !may_start_working(pool);
}

/* Do we have too many workers and should some go away? */
static bool too_many_workers(struct worker_pool *pool)
{
	bool managing = pool->flags & POOL_MANAGER_ACTIVE;
	int nr_idle = pool->nr_idle + managing; /* manager is considered idle */
	int nr_busy = pool->nr_workers - nr_idle;

	return nr_idle > 2 && (nr_idle - 2) * MAX_IDLE_WORKERS_RATIO >= nr_busy;
}

/**
 * worker_set_flags - set worker flags and adjust nr_running accordingly
 * @worker: self
 * @flags: flags to set
 *
 * Set @flags in @worker->flags and adjust nr_running accordingly.
 */
static inline void worker_set_flags(struct worker *worker, unsigned int flags)
{
	struct worker_pool *pool = worker->pool;

	lockdep_assert_held(&pool->lock);

	/* If transitioning into NOT_RUNNING, adjust nr_running. */
	if ((flags & WORKER_NOT_RUNNING) &&
	    !(worker->flags & WORKER_NOT_RUNNING)) {
		pool->nr_running--;
	}

	worker->flags |= flags;
}

/**
 * worker_clr_flags - clear worker flags and adjust nr_running accordingly
 * @worker: self
 * @flags: flags to clear
 *
 * Clear @flags in @worker->flags and adjust nr_running accordingly.
 */
static inline void worker_clr_flags(struct worker *worker, unsigned int flags)
{
	struct worker_pool *pool = worker->pool;
	unsigned int oflags = worker->flags;

	lockdep_assert_held(&pool->lock);

	worker->flags &= ~flags;

	/*
	 * If transitioning out of NOT_RUNNING, increment nr_running.  Note
	 * that the nested NOT_RUNNING is not a noop.  NOT_RUNNING is mask
	 * of multiple flags, not a single flag.
	 */
	if ((flags & WORKER_NOT_RUNNING) && (oflags & WORKER_NOT_RUNNING))
		if (!(worker->flags & WORKER_NOT_RUNNING))
			pool->nr_running++;
}

/* Return the first idle worker.  Called with pool->lock held. */
static struct worker *first_idle_worker(struct worker_pool *pool)
{
	if (unlikely(list_empty(&pool->idle_list)))
		return NULL;

	return list_first_entry(&pool->idle_list, struct worker, entry);
}

/**
 * worker_enter_idle - enter idle state
 * @worker: worker which is entering idle state
 *
 * @worker is entering idle state.  Update stats and idle timer if
 * necessary.
 *
 * LOCKING:
 * raw_spin_lock_irq(pool->lock).
 */
static void worker_enter_idle(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;

	if (WARN_ON_ONCE(worker->flags & WORKER_IDLE) ||
	    WARN_ON_ONCE(!list_empty(&worker->entry) &&
			 (worker->hentry.next || worker->hentry.pprev)))
		return;

	/* can't use worker_set_flags(), also called from create_worker() */
	worker->flags |= WORKER_IDLE;
	pool->nr_idle++;
	worker->last_active = jiffies;

	/* idle_list is LIFO */
	list_add(&worker->entry, &pool->idle_list);

	if (too_many_workers(pool) && !timer_pending(&pool->idle_timer))
		mod_timer(&pool->idle_timer, jiffies + IDLE_WORKER_TIMEOUT);

	/* Sanity check nr_running. */
	WARN_ON_ONCE(pool->nr_workers == pool->nr_idle && pool->nr_running);
}

/**
 * worker_leave_idle - leave idle state
 * @worker: worker which is leaving idle state
 *
 * @worker is leaving idle state.  Update stats.
 *
 * LOCKING:
 * raw_spin_lock_irq(pool->lock).
 */
static void worker_leave_idle(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;

	if (WARN_ON_ONCE(!(worker->flags & WORKER_IDLE)))
		return;
	worker_clr_flags(worker, WORKER_IDLE);
	pool->nr_idle--;
	list_del_init(&worker->entry);
}

/**
 * find_worker_executing_work - find worker which is executing a work
 * @pool: pool of interest
 * @work: work to find worker for
 *
 * Find a worker which is executing @work on @pool by searching
 * @pool->busy_hash which is keyed by the address of @work.  For a worker
 * to match, its current execution should match the address of @work and
 * its work function.  This is to avoid unwanted dependency between
 * unrelated work executions through a work item being recycled while still
 * being executed.
 *
 * This is a bit tricky.  A work item may be freed once its execution
 * starts and nothing prevents the freed area from being recycled for
 * another work item.  If the same work item address ends up being reused
 * before the original execution finishes, workqueue will identify the
 * recycled work item as currently executing and make it wait until the
 * current execution finishes, introducing an unwanted dependency.
 *
 * This function checks the work item address and work function to avoid
 * false positives.  Note that this isn't complete as one may construct a
 * work function which can introduce dependency onto itself through a
 * recycled work item.  Well, if somebody wants to shoot oneself in the
 * foot that badly, there's only so much we can do, and if such deadlock
 * actually occurs, it should be easy to locate the culprit work function.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 *
 * Return:
 * Pointer to worker which is executing @work if found, %NULL
 * otherwise.
 */
static struct worker *find_worker_executing_work(struct worker_pool *pool,
						 struct work_struct *work)
{
	struct worker *worker;

	hash_for_each_possible(pool->busy_hash, worker, hentry,
			       (unsigned long)work)
		if (worker->current_work == work &&
		    worker->current_func == work->func)
			return worker;

	return NULL;
}

/**
 * move_linked_works - move linked works to a list
 * @work: start of series of works to be scheduled
 * @head: target list to append @work to
 * @nextp: out parameter for nested worklist walking
 *
 * Schedule linked works starting from @work to @head. Work series to be
 * scheduled starts at @work and includes any consecutive work with
 * WORK_STRUCT_LINKED set in its predecessor. See assign_work() for details on
 * @nextp.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 */
static void move_linked_works(struct work_struct *work, struct list_head *head,
			      struct work_struct **nextp)
{
	struct work_struct *n;

	/*
	 * Linked worklist will always end before the end of the list,
	 * use NULL for list head.
	 */
	list_for_each_entry_safe_from(work, n, NULL, entry) {
		list_move_tail(&work->entry, head);
		if (!(*work_data_bits(work) & WORK_STRUCT_LINKED))
			break;
	}

	/*
	 * If we're already inside safe list traversal and have moved
	 * multiple works to the scheduled queue, the next position
	 * needs to be updated.
	 */
	if (nextp)
		*nextp = n;
}

/**
 * assign_work - assign a work item and its linked work items to a worker
 * @work: work to assign
 * @worker: worker to assign to
 * @nextp: out parameter for nested worklist walking
 *
 * Assign @work and its linked work items to @worker. If @work is already being
 * executed by another worker in the same pool, it'll be punted there.
 *
 * If @nextp is not NULL, it's updated to point to the next work of the last
 * scheduled work. This allows assign_work() to be nested inside
 * list_for_each_entry_safe().
 *
 * Returns %true if @work was successfully assigned to @worker. %false if @work
 * was punted to another worker already executing it.
 */
static bool assign_work(struct work_struct *work, struct worker *worker,
			struct work_struct **nextp)
{
	struct worker_pool *pool = worker->pool;
	struct worker *collision;

	lockdep_assert_held(&pool->lock);

	/*
	 * A single work shouldn't be executed concurrently by multiple workers.
	 * __queue_work() ensures that @work doesn't jump to a different pool
	 * while still running in the previous pool. Here, we should ensure that
	 * @work is not executed concurrently by multiple workers from the same
	 * pool. Check whether anyone is already processing the work. If so,
	 * defer the work to the currently executing one.
	 */
	collision = find_worker_executing_work(pool, work);
	if (unlikely(collision)) {
		move_linked_works(work, &collision->scheduled, nextp);
		return false;
	}

	move_linked_works(work, &worker->scheduled, nextp);
	return true;
}

/**
 * kick_pool - wake up an idle worker if necessary
 * @pool: pool to kick
 *
 * @pool may have pending work items. Wake up worker if necessary. Returns
 * whether a worker was woken up.
 */
static bool kick_pool(struct worker_pool *pool)
{
	struct worker *worker = first_idle_worker(pool);
	struct task_struct *p;

	lockdep_assert_held(&pool->lock);

	if (!need_more_worker(pool) || !worker)
		return false;

	p = worker->task;

#ifdef CONFIG_SMP
	/*
	 * Idle @worker is about to execute @work and waking up provides an
	 * opportunity to migrate @worker at a lower cost by setting the task's
	 * wake_cpu field. Let's see if we want to move @worker to improve
	 * execution locality.
	 *
	 * We're waking the worker that went idle the latest and there's some
	 * chance that @worker is marked idle but hasn't gone off CPU yet. If
	 * so, setting the wake_cpu won't do anything. As this is a best-effort
	 * optimization and the race window is narrow, let's leave as-is for
	 * now. If this becomes pronounced, we can skip over workers which are
	 * still on cpu when picking an idle worker.
	 *
	 * If @pool has non-strict affinity, @worker might have ended up outside
	 * its affinity scope. Repatriate.
	 */
	if (!pool->attrs->affn_strict &&
	    !cpumask_test_cpu(p->wake_cpu, pool->attrs->__pod_cpumask)) {
		struct work_struct *work = list_first_entry(&pool->worklist,
						struct work_struct, entry);
		int wake_cpu = cpumask_any_and_distribute(pool->attrs->__pod_cpumask,
							  cpu_online_mask);
		if (wake_cpu < nr_cpu_ids) {
			p->wake_cpu = wake_cpu;
			get_work_pwq(work)->stats[PWQ_STAT_REPATRIATED]++;
		}
	}
#endif
	wake_up_process(p);
	return true;
}

#ifdef CONFIG_WQ_CPU_INTENSIVE_REPORT

/*
 * Concurrency-managed per-cpu work items that hog CPU for longer than
 * wq_cpu_intensive_thresh_us trigger the automatic CPU_INTENSIVE mechanism,
 * which prevents them from stalling other concurrency-managed work items. If a
 * work function keeps triggering this mechanism, it's likely that the work item
 * should be using an unbound workqueue instead.
 *
 * wq_cpu_intensive_report() tracks work functions which trigger such conditions
 * and report them so that they can be examined and converted to use unbound
 * workqueues as appropriate. To avoid flooding the console, each violating work
 * function is tracked and reported with exponential backoff.
 */
#define WCI_MAX_ENTS 128

struct wci_ent {
	work_func_t		func;
	atomic64_t		cnt;
	struct hlist_node	hash_node;
};

static struct wci_ent wci_ents[WCI_MAX_ENTS];
static int wci_nr_ents;
static DEFINE_RAW_SPINLOCK(wci_lock);
static DEFINE_HASHTABLE(wci_hash, ilog2(WCI_MAX_ENTS));

static struct wci_ent *wci_find_ent(work_func_t func)
{
	struct wci_ent *ent;

	hash_for_each_possible_rcu(wci_hash, ent, hash_node,
				   (unsigned long)func) {
		if (ent->func == func)
			return ent;
	}
	return NULL;
}

static void wq_cpu_intensive_report(work_func_t func)
{
	struct wci_ent *ent;

restart:
	ent = wci_find_ent(func);
	if (ent) {
		u64 cnt;

		/*
		 * Start reporting from the fourth time and back off
		 * exponentially.
		 */
		cnt = atomic64_inc_return_relaxed(&ent->cnt);
		if (cnt >= 4 && is_power_of_2(cnt))
			printk_deferred(KERN_WARNING "workqueue: %ps hogged CPU for >%luus %llu times, consider switching to WQ_UNBOUND\n",
					ent->func, wq_cpu_intensive_thresh_us,
					atomic64_read(&ent->cnt));
		return;
	}

	/*
	 * @func is a new violation. Allocate a new entry for it. If wcn_ents[]
	 * is exhausted, something went really wrong and we probably made enough
	 * noise already.
	 */
	if (wci_nr_ents >= WCI_MAX_ENTS)
		return;

	raw_spin_lock(&wci_lock);

	if (wci_nr_ents >= WCI_MAX_ENTS) {
		raw_spin_unlock(&wci_lock);
		return;
	}

	if (wci_find_ent(func)) {
		raw_spin_unlock(&wci_lock);
		goto restart;
	}

	ent = &wci_ents[wci_nr_ents++];
	ent->func = func;
	atomic64_set(&ent->cnt, 1);
	hash_add_rcu(wci_hash, &ent->hash_node, (unsigned long)func);

	raw_spin_unlock(&wci_lock);
}

#else	/* CONFIG_WQ_CPU_INTENSIVE_REPORT */
static void wq_cpu_intensive_report(work_func_t func) {}
#endif	/* CONFIG_WQ_CPU_INTENSIVE_REPORT */

/**
 * wq_worker_running - a worker is running again
 * @task: task waking up
 *
 * This function is called when a worker returns from schedule()
 */
void wq_worker_running(struct task_struct *task)
{
	struct worker *worker = kthread_data(task);

	if (!READ_ONCE(worker->sleeping))
		return;

	/*
	 * If preempted by unbind_workers() between the WORKER_NOT_RUNNING check
	 * and the nr_running increment below, we may ruin the nr_running reset
	 * and leave with an unexpected pool->nr_running == 1 on the newly unbound
	 * pool. Protect against such race.
	 */
	preempt_disable();
	if (!(worker->flags & WORKER_NOT_RUNNING))
		worker->pool->nr_running++;
	preempt_enable();

	/*
	 * CPU intensive auto-detection cares about how long a work item hogged
	 * CPU without sleeping. Reset the starting timestamp on wakeup.
	 */
	worker->current_at = worker->task->se.sum_exec_runtime;

	WRITE_ONCE(worker->sleeping, 0);
}

/**
 * wq_worker_sleeping - a worker is going to sleep
 * @task: task going to sleep
 *
 * This function is called from schedule() when a busy worker is
 * going to sleep.
 */
void wq_worker_sleeping(struct task_struct *task)
{
	struct worker *worker = kthread_data(task);
	struct worker_pool *pool;

	/*
	 * Rescuers, which may not have all the fields set up like normal
	 * workers, also reach here, let's not access anything before
	 * checking NOT_RUNNING.
	 */
	if (worker->flags & WORKER_NOT_RUNNING)
		return;

	pool = worker->pool;

	/* Return if preempted before wq_worker_running() was reached */
	if (READ_ONCE(worker->sleeping))
		return;

	WRITE_ONCE(worker->sleeping, 1);
	raw_spin_lock_irq(&pool->lock);

	/*
	 * Recheck in case unbind_workers() preempted us. We don't
	 * want to decrement nr_running after the worker is unbound
	 * and nr_running has been reset.
	 */
	if (worker->flags & WORKER_NOT_RUNNING) {
		raw_spin_unlock_irq(&pool->lock);
		return;
	}

	pool->nr_running--;
	if (kick_pool(pool))
		worker->current_pwq->stats[PWQ_STAT_CM_WAKEUP]++;

	raw_spin_unlock_irq(&pool->lock);
}

/**
 * wq_worker_tick - a scheduler tick occurred while a kworker is running
 * @task: task currently running
 *
 * Called from scheduler_tick(). We're in the IRQ context and the current
 * worker's fields which follow the 'K' locking rule can be accessed safely.
 */
void wq_worker_tick(struct task_struct *task)
{
	struct worker *worker = kthread_data(task);
	struct pool_workqueue *pwq = worker->current_pwq;
	struct worker_pool *pool = worker->pool;

	if (!pwq)
		return;

	pwq->stats[PWQ_STAT_CPU_TIME] += TICK_USEC;

	if (!wq_cpu_intensive_thresh_us)
		return;

	/*
	 * If the current worker is concurrency managed and hogged the CPU for
	 * longer than wq_cpu_intensive_thresh_us, it's automatically marked
	 * CPU_INTENSIVE to avoid stalling other concurrency-managed work items.
	 *
	 * Set @worker->sleeping means that @worker is in the process of
	 * switching out voluntarily and won't be contributing to
	 * @pool->nr_running until it wakes up. As wq_worker_sleeping() also
	 * decrements ->nr_running, setting CPU_INTENSIVE here can lead to
	 * double decrements. The task is releasing the CPU anyway. Let's skip.
	 * We probably want to make this prettier in the future.
	 */
	if ((worker->flags & WORKER_NOT_RUNNING) || READ_ONCE(worker->sleeping) ||
	    worker->task->se.sum_exec_runtime - worker->current_at <
	    wq_cpu_intensive_thresh_us * NSEC_PER_USEC)
		return;

	raw_spin_lock(&pool->lock);

	worker_set_flags(worker, WORKER_CPU_INTENSIVE);
	wq_cpu_intensive_report(worker->current_func);
	pwq->stats[PWQ_STAT_CPU_INTENSIVE]++;

	if (kick_pool(pool))
		pwq->stats[PWQ_STAT_CM_WAKEUP]++;

	raw_spin_unlock(&pool->lock);
}

/**
 * wq_worker_last_func - retrieve worker's last work function
 * @task: Task to retrieve last work function of.
 *
 * Determine the last function a worker executed. This is called from
 * the scheduler to get a worker's last known identity.
 *
 * CONTEXT:
 * raw_spin_lock_irq(rq->lock)
 *
 * This function is called during schedule() when a kworker is going
 * to sleep. It's used by psi to identify aggregation workers during
 * dequeuing, to allow periodic aggregation to shut-off when that
 * worker is the last task in the system or cgroup to go to sleep.
 *
 * As this function doesn't involve any workqueue-related locking, it
 * only returns stable values when called from inside the scheduler's
 * queuing and dequeuing paths, when @task, which must be a kworker,
 * is guaranteed to not be processing any works.
 *
 * Return:
 * The last work function %current executed as a worker, NULL if it
 * hasn't executed any work yet.
 */
work_func_t wq_worker_last_func(struct task_struct *task)
{
	struct worker *worker = kthread_data(task);

	return worker->last_func;
}

/**
 * get_pwq - get an extra reference on the specified pool_workqueue
 * @pwq: pool_workqueue to get
 *
 * Obtain an extra reference on @pwq.  The caller should guarantee that
 * @pwq has positive refcnt and be holding the matching pool->lock.
 */
static void get_pwq(struct pool_workqueue *pwq)
{
	lockdep_assert_held(&pwq->pool->lock);
	WARN_ON_ONCE(pwq->refcnt <= 0);
	pwq->refcnt++;
}

/**
 * put_pwq - put a pool_workqueue reference
 * @pwq: pool_workqueue to put
 *
 * Drop a reference of @pwq.  If its refcnt reaches zero, schedule its
 * destruction.  The caller should be holding the matching pool->lock.
 */
static void put_pwq(struct pool_workqueue *pwq)
{
	lockdep_assert_held(&pwq->pool->lock);
	if (likely(--pwq->refcnt))
		return;
	/*
	 * @pwq can't be released under pool->lock, bounce to a dedicated
	 * kthread_worker to avoid A-A deadlocks.
	 */
	kthread_queue_work(pwq_release_worker, &pwq->release_work);
}

/**
 * put_pwq_unlocked - put_pwq() with surrounding pool lock/unlock
 * @pwq: pool_workqueue to put (can be %NULL)
 *
 * put_pwq() with locking.  This function also allows %NULL @pwq.
 */
static void put_pwq_unlocked(struct pool_workqueue *pwq)
{
	if (pwq) {
		/*
		 * As both pwqs and pools are RCU protected, the
		 * following lock operations are safe.
		 */
		raw_spin_lock_irq(&pwq->pool->lock);
		put_pwq(pwq);
		raw_spin_unlock_irq(&pwq->pool->lock);
	}
}

static void pwq_activate_inactive_work(struct work_struct *work)
{
	struct pool_workqueue *pwq = get_work_pwq(work);

	trace_workqueue_activate_work(work);
	if (list_empty(&pwq->pool->worklist))
		pwq->pool->watchdog_ts = jiffies;
	move_linked_works(work, &pwq->pool->worklist, NULL);
	__clear_bit(WORK_STRUCT_INACTIVE_BIT, work_data_bits(work));
	pwq->nr_active++;
}

static void pwq_activate_first_inactive(struct pool_workqueue *pwq)
{
	struct work_struct *work = list_first_entry(&pwq->inactive_works,
						    struct work_struct, entry);

	pwq_activate_inactive_work(work);
}

/**
 * pwq_dec_nr_in_flight - decrement pwq's nr_in_flight
 * @pwq: pwq of interest
 * @work_data: work_data of work which left the queue
 *
 * A work either has completed or is removed from pending queue,
 * decrement nr_in_flight of its pwq and handle workqueue flushing.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 */
static void pwq_dec_nr_in_flight(struct pool_workqueue *pwq, unsigned long work_data)
{
	int color = get_work_color(work_data);

	if (!(work_data & WORK_STRUCT_INACTIVE)) {
		pwq->nr_active--;
		if (!list_empty(&pwq->inactive_works)) {
			/* one down, submit an inactive one */
			if (pwq->nr_active < pwq->max_active)
				pwq_activate_first_inactive(pwq);
		}
	}

	pwq->nr_in_flight[color]--;

	/* is flush in progress and are we at the flushing tip? */
	if (likely(pwq->flush_color != color))
		goto out_put;

	/* are there still in-flight works? */
	if (pwq->nr_in_flight[color])
		goto out_put;

	/* this pwq is done, clear flush_color */
	pwq->flush_color = -1;

	/*
	 * If this was the last pwq, wake up the first flusher.  It
	 * will handle the rest.
	 */
	if (atomic_dec_and_test(&pwq->wq->nr_pwqs_to_flush))
		complete(&pwq->wq->first_flusher->done);
out_put:
	put_pwq(pwq);
}

/**
 * try_to_grab_pending - steal work item from worklist and disable irq
 * @work: work item to steal
 * @is_dwork: @work is a delayed_work
 * @flags: place to store irq state
 *
 * Try to grab PENDING bit of @work.  This function can handle @work in any
 * stable state - idle, on timer or on worklist.
 *
 * Return:
 *
 *  ========	================================================================
 *  1		if @work was pending and we successfully stole PENDING
 *  0		if @work was idle and we claimed PENDING
 *  -EAGAIN	if PENDING couldn't be grabbed at the moment, safe to busy-retry
 *  -ENOENT	if someone else is canceling @work, this state may persist
 *		for arbitrarily long
 *  ========	================================================================
 *
 * Note:
 * On >= 0 return, the caller owns @work's PENDING bit.  To avoid getting
 * interrupted while holding PENDING and @work off queue, irq must be
 * disabled on entry.  This, combined with delayed_work->timer being
 * irqsafe, ensures that we return -EAGAIN for finite short period of time.
 *
 * On successful return, >= 0, irq is disabled and the caller is
 * responsible for releasing it using local_irq_restore(*@flags).
 *
 * This function is safe to call from any context including IRQ handler.
 */
static int try_to_grab_pending(struct work_struct *work, bool is_dwork,
			       unsigned long *flags)
{
	struct worker_pool *pool;
	struct pool_workqueue *pwq;

	local_irq_save(*flags);

	/* try to steal the timer if it exists */
	if (is_dwork) {
		struct delayed_work *dwork = to_delayed_work(work);

		/*
		 * dwork->timer is irqsafe.  If del_timer() fails, it's
		 * guaranteed that the timer is not queued anywhere and not
		 * running on the local CPU.
		 */
		if (likely(del_timer(&dwork->timer)))
			return 1;
	}

	/* try to claim PENDING the normal way */
	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work)))
		return 0;

	rcu_read_lock();
	/*
	 * The queueing is in progress, or it is already queued. Try to
	 * steal it from ->worklist without clearing WORK_STRUCT_PENDING.
	 */
	pool = get_work_pool(work);
	if (!pool)
		goto fail;

	raw_spin_lock(&pool->lock);
	/*
	 * work->data is guaranteed to point to pwq only while the work
	 * item is queued on pwq->wq, and both updating work->data to point
	 * to pwq on queueing and to pool on dequeueing are done under
	 * pwq->pool->lock.  This in turn guarantees that, if work->data
	 * points to pwq which is associated with a locked pool, the work
	 * item is currently queued on that pool.
	 */
	pwq = get_work_pwq(work);
	if (pwq && pwq->pool == pool) {
		debug_work_deactivate(work);

		/*
		 * A cancelable inactive work item must be in the
		 * pwq->inactive_works since a queued barrier can't be
		 * canceled (see the comments in insert_wq_barrier()).
		 *
		 * An inactive work item cannot be grabbed directly because
		 * it might have linked barrier work items which, if left
		 * on the inactive_works list, will confuse pwq->nr_active
		 * management later on and cause stall.  Make sure the work
		 * item is activated before grabbing.
		 */
		if (*work_data_bits(work) & WORK_STRUCT_INACTIVE)
			pwq_activate_inactive_work(work);

		list_del_init(&work->entry);
		pwq_dec_nr_in_flight(pwq, *work_data_bits(work));

		/* work->data points to pwq iff queued, point to pool */
		set_work_pool_and_keep_pending(work, pool->id);

		raw_spin_unlock(&pool->lock);
		rcu_read_unlock();
		return 1;
	}
	raw_spin_unlock(&pool->lock);
fail:
	rcu_read_unlock();
	local_irq_restore(*flags);
	if (work_is_canceling(work))
		return -ENOENT;
	cpu_relax();
	return -EAGAIN;
}

/**
 * insert_work - insert a work into a pool
 * @pwq: pwq @work belongs to
 * @work: work to insert
 * @head: insertion point
 * @extra_flags: extra WORK_STRUCT_* flags to set
 *
 * Insert @work which belongs to @pwq after @head.  @extra_flags is or'd to
 * work_struct flags.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 */
static void insert_work(struct pool_workqueue *pwq, struct work_struct *work,
			struct list_head *head, unsigned int extra_flags)
{
	debug_work_activate(work);

	/* record the work call stack in order to print it in KASAN reports */
	kasan_record_aux_stack_noalloc(work);

	/* we own @work, set data and link */
	set_work_pwq(work, pwq, extra_flags);
	list_add_tail(&work->entry, head);
	get_pwq(pwq);
}

/*
 * Test whether @work is being queued from another work executing on the
 * same workqueue.
 */
static bool is_chained_work(struct workqueue_struct *wq)
{
	struct worker *worker;

	worker = current_wq_worker();
	/*
	 * Return %true iff I'm a worker executing a work item on @wq.  If
	 * I'm @worker, it's safe to dereference it without locking.
	 */
	return worker && worker->current_pwq->wq == wq;
}

/*
 * When queueing an unbound work item to a wq, prefer local CPU if allowed
 * by wq_unbound_cpumask.  Otherwise, round robin among the allowed ones to
 * avoid perturbing sensitive tasks.
 */
static int wq_select_unbound_cpu(int cpu)
{
	int new_cpu;

	if (likely(!wq_debug_force_rr_cpu)) {
		if (cpumask_test_cpu(cpu, wq_unbound_cpumask))
			return cpu;
	} else {
		pr_warn_once("workqueue: round-robin CPU selection forced, expect performance impact\n");
	}

	new_cpu = __this_cpu_read(wq_rr_cpu_last);
	new_cpu = cpumask_next_and(new_cpu, wq_unbound_cpumask, cpu_online_mask);
	if (unlikely(new_cpu >= nr_cpu_ids)) {
		new_cpu = cpumask_first_and(wq_unbound_cpumask, cpu_online_mask);
		if (unlikely(new_cpu >= nr_cpu_ids))
			return cpu;
	}
	__this_cpu_write(wq_rr_cpu_last, new_cpu);

	return new_cpu;
}

static void __queue_work(int cpu, struct workqueue_struct *wq,
			 struct work_struct *work)
{
	struct pool_workqueue *pwq;
	struct worker_pool *last_pool, *pool;
	unsigned int work_flags;
	unsigned int req_cpu = cpu;

	/*
	 * While a work item is PENDING && off queue, a task trying to
	 * steal the PENDING will busy-loop waiting for it to either get
	 * queued or lose PENDING.  Grabbing PENDING and queueing should
	 * happen with IRQ disabled.
	 */
	lockdep_assert_irqs_disabled();


	/*
	 * For a draining wq, only works from the same workqueue are
	 * allowed. The __WQ_DESTROYING helps to spot the issue that
	 * queues a new work item to a wq after destroy_workqueue(wq).
	 */
	if (unlikely(wq->flags & (__WQ_DESTROYING | __WQ_DRAINING) &&
		     WARN_ON_ONCE(!is_chained_work(wq))))
		return;
	rcu_read_lock();
retry:
	/* pwq which will be used unless @work is executing elsewhere */
	if (req_cpu == WORK_CPU_UNBOUND) {
		if (wq->flags & WQ_UNBOUND)
			cpu = wq_select_unbound_cpu(raw_smp_processor_id());
		else
			cpu = raw_smp_processor_id();
	}

	pwq = rcu_dereference(*per_cpu_ptr(wq->cpu_pwq, cpu));
	pool = pwq->pool;

	/*
	 * If @work was previously on a different pool, it might still be
	 * running there, in which case the work needs to be queued on that
	 * pool to guarantee non-reentrancy.
	 */
	last_pool = get_work_pool(work);
	if (last_pool && last_pool != pool) {
		struct worker *worker;

		raw_spin_lock(&last_pool->lock);

		worker = find_worker_executing_work(last_pool, work);

		if (worker && worker->current_pwq->wq == wq) {
			pwq = worker->current_pwq;
			pool = pwq->pool;
			WARN_ON_ONCE(pool != last_pool);
		} else {
			/* meh... not running there, queue here */
			raw_spin_unlock(&last_pool->lock);
			raw_spin_lock(&pool->lock);
		}
	} else {
		raw_spin_lock(&pool->lock);
	}

	/*
	 * pwq is determined and locked. For unbound pools, we could have raced
	 * with pwq release and it could already be dead. If its refcnt is zero,
	 * repeat pwq selection. Note that unbound pwqs never die without
	 * another pwq replacing it in cpu_pwq or while work items are executing
	 * on it, so the retrying is guaranteed to make forward-progress.
	 */
	if (unlikely(!pwq->refcnt)) {
		if (wq->flags & WQ_UNBOUND) {
			raw_spin_unlock(&pool->lock);
			cpu_relax();
			goto retry;
		}
		/* oops */
		WARN_ONCE(true, "workqueue: per-cpu pwq for %s on cpu%d has 0 refcnt",
			  wq->name, cpu);
	}

	/* pwq determined, queue */
	trace_workqueue_queue_work(req_cpu, pwq, work);

	if (WARN_ON(!list_empty(&work->entry)))
		goto out;

	pwq->nr_in_flight[pwq->work_color]++;
	work_flags = work_color_to_flags(pwq->work_color);

	if (likely(pwq->nr_active < pwq->max_active)) {
		if (list_empty(&pool->worklist))
			pool->watchdog_ts = jiffies;

		trace_workqueue_activate_work(work);
		pwq->nr_active++;
		insert_work(pwq, work, &pool->worklist, work_flags);
		kick_pool(pool);
	} else {
		work_flags |= WORK_STRUCT_INACTIVE;
		insert_work(pwq, work, &pwq->inactive_works, work_flags);
	}

out:
	raw_spin_unlock(&pool->lock);
	rcu_read_unlock();
}

/**
 * queue_work_on - queue work on specific cpu
 * @cpu: CPU number to execute work on
 * @wq: workqueue to use
 * @work: work to queue
 *
 * We queue the work to a specific CPU, the caller must ensure it
 * can't go away.  Callers that fail to ensure that the specified
 * CPU cannot go away will execute on a randomly chosen CPU.
 * But note well that callers specifying a CPU that never has been
 * online will get a splat.
 *
 * Return: %false if @work was already on a queue, %true otherwise.
 */
bool queue_work_on(int cpu, struct workqueue_struct *wq,
		   struct work_struct *work)
{
	bool ret = false;
	unsigned long flags;

	local_irq_save(flags);

	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work))) {
		__queue_work(cpu, wq, work);
		ret = true;
	}

	local_irq_restore(flags);
	return ret;
}
EXPORT_SYMBOL(queue_work_on);

/**
 * select_numa_node_cpu - Select a CPU based on NUMA node
 * @node: NUMA node ID that we want to select a CPU from
 *
 * This function will attempt to find a "random" cpu available on a given
 * node. If there are no CPUs available on the given node it will return
 * WORK_CPU_UNBOUND indicating that we should just schedule to any
 * available CPU if we need to schedule this work.
 */
static int select_numa_node_cpu(int node)
{
	int cpu;

	/* Delay binding to CPU if node is not valid or online */
	if (node < 0 || node >= MAX_NUMNODES || !node_online(node))
		return WORK_CPU_UNBOUND;

	/* Use local node/cpu if we are already there */
	cpu = raw_smp_processor_id();
	if (node == cpu_to_node(cpu))
		return cpu;

	/* Use "random" otherwise know as "first" online CPU of node */
	cpu = cpumask_any_and(cpumask_of_node(node), cpu_online_mask);

	/* If CPU is valid return that, otherwise just defer */
	return cpu < nr_cpu_ids ? cpu : WORK_CPU_UNBOUND;
}

/**
 * queue_work_node - queue work on a "random" cpu for a given NUMA node
 * @node: NUMA node that we are targeting the work for
 * @wq: workqueue to use
 * @work: work to queue
 *
 * We queue the work to a "random" CPU within a given NUMA node. The basic
 * idea here is to provide a way to somehow associate work with a given
 * NUMA node.
 *
 * This function will only make a best effort attempt at getting this onto
 * the right NUMA node. If no node is requested or the requested node is
 * offline then we just fall back to standard queue_work behavior.
 *
 * Currently the "random" CPU ends up being the first available CPU in the
 * intersection of cpu_online_mask and the cpumask of the node, unless we
 * are running on the node. In that case we just use the current CPU.
 *
 * Return: %false if @work was already on a queue, %true otherwise.
 */
bool queue_work_node(int node, struct workqueue_struct *wq,
		     struct work_struct *work)
{
	unsigned long flags;
	bool ret = false;

	/*
	 * This current implementation is specific to unbound workqueues.
	 * Specifically we only return the first available CPU for a given
	 * node instead of cycling through individual CPUs within the node.
	 *
	 * If this is used with a per-cpu workqueue then the logic in
	 * workqueue_select_cpu_near would need to be updated to allow for
	 * some round robin type logic.
	 */
	WARN_ON_ONCE(!(wq->flags & WQ_UNBOUND));

	local_irq_save(flags);

	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work))) {
		int cpu = select_numa_node_cpu(node);

		__queue_work(cpu, wq, work);
		ret = true;
	}

	local_irq_restore(flags);
	return ret;
}
EXPORT_SYMBOL_GPL(queue_work_node);

void delayed_work_timer_fn(struct timer_list *t)
{
	struct delayed_work *dwork = from_timer(dwork, t, timer);

	/* should have been called from irqsafe timer with irq already off */
	__queue_work(dwork->cpu, dwork->wq, &dwork->work);
}
EXPORT_SYMBOL(delayed_work_timer_fn);

static void __queue_delayed_work(int cpu, struct workqueue_struct *wq,
				struct delayed_work *dwork, unsigned long delay)
{
	struct timer_list *timer = &dwork->timer;
	struct work_struct *work = &dwork->work;

	WARN_ON_ONCE(!wq);
	WARN_ON_ONCE(timer->function != delayed_work_timer_fn);
	WARN_ON_ONCE(timer_pending(timer));
	WARN_ON_ONCE(!list_empty(&work->entry));

	/*
	 * If @delay is 0, queue @dwork->work immediately.  This is for
	 * both optimization and correctness.  The earliest @timer can
	 * expire is on the closest next tick and delayed_work users depend
	 * on that there's no such delay when @delay is 0.
	 */
	if (!delay) {
		__queue_work(cpu, wq, &dwork->work);
		return;
	}

	dwork->wq = wq;
	dwork->cpu = cpu;
	timer->expires = jiffies + delay;

	if (unlikely(cpu != WORK_CPU_UNBOUND))
		add_timer_on(timer, cpu);
	else
		add_timer(timer);
}

/**
 * queue_delayed_work_on - queue work on specific CPU after delay
 * @cpu: CPU number to execute work on
 * @wq: workqueue to use
 * @dwork: work to queue
 * @delay: number of jiffies to wait before queueing
 *
 * Return: %false if @work was already on a queue, %true otherwise.  If
 * @delay is zero and @dwork is idle, it will be scheduled for immediate
 * execution.
 */
bool queue_delayed_work_on(int cpu, struct workqueue_struct *wq,
			   struct delayed_work *dwork, unsigned long delay)
{
	struct work_struct *work = &dwork->work;
	bool ret = false;
	unsigned long flags;

	/* read the comment in __queue_work() */
	local_irq_save(flags);

	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work))) {
		__queue_delayed_work(cpu, wq, dwork, delay);
		ret = true;
	}

	local_irq_restore(flags);
	return ret;
}
EXPORT_SYMBOL(queue_delayed_work_on);

/**
 * mod_delayed_work_on - modify delay of or queue a delayed work on specific CPU
 * @cpu: CPU number to execute work on
 * @wq: workqueue to use
 * @dwork: work to queue
 * @delay: number of jiffies to wait before queueing
 *
 * If @dwork is idle, equivalent to queue_delayed_work_on(); otherwise,
 * modify @dwork's timer so that it expires after @delay.  If @delay is
 * zero, @work is guaranteed to be scheduled immediately regardless of its
 * current state.
 *
 * Return: %false if @dwork was idle and queued, %true if @dwork was
 * pending and its timer was modified.
 *
 * This function is safe to call from any context including IRQ handler.
 * See try_to_grab_pending() for details.
 */
bool mod_delayed_work_on(int cpu, struct workqueue_struct *wq,
			 struct delayed_work *dwork, unsigned long delay)
{
	unsigned long flags;
	int ret;

	do {
		ret = try_to_grab_pending(&dwork->work, true, &flags);
	} while (unlikely(ret == -EAGAIN));

	if (likely(ret >= 0)) {
		__queue_delayed_work(cpu, wq, dwork, delay);
		local_irq_restore(flags);
	}

	/* -ENOENT from try_to_grab_pending() becomes %true */
	return ret;
}
EXPORT_SYMBOL_GPL(mod_delayed_work_on);

static void rcu_work_rcufn(struct rcu_head *rcu)
{
	struct rcu_work *rwork = container_of(rcu, struct rcu_work, rcu);

	/* read the comment in __queue_work() */
	local_irq_disable();
	__queue_work(WORK_CPU_UNBOUND, rwork->wq, &rwork->work);
	local_irq_enable();
}

/**
 * queue_rcu_work - queue work after a RCU grace period
 * @wq: workqueue to use
 * @rwork: work to queue
 *
 * Return: %false if @rwork was already pending, %true otherwise.  Note
 * that a full RCU grace period is guaranteed only after a %true return.
 * While @rwork is guaranteed to be executed after a %false return, the
 * execution may happen before a full RCU grace period has passed.
 */
bool queue_rcu_work(struct workqueue_struct *wq, struct rcu_work *rwork)
{
	struct work_struct *work = &rwork->work;

	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work))) {
		rwork->wq = wq;
		call_rcu_hurry(&rwork->rcu, rcu_work_rcufn);
		return true;
	}

	return false;
}
EXPORT_SYMBOL(queue_rcu_work);

static struct worker *alloc_worker(int node)
{
	struct worker *worker;

	worker = kzalloc_node(sizeof(*worker), GFP_KERNEL, node);
	if (worker) {
		INIT_LIST_HEAD(&worker->entry);
		INIT_LIST_HEAD(&worker->scheduled);
		INIT_LIST_HEAD(&worker->node);
		/* on creation a worker is in !idle && prep state */
		worker->flags = WORKER_PREP;
	}
	return worker;
}

static cpumask_t *pool_allowed_cpus(struct worker_pool *pool)
{
	if (pool->cpu < 0 && pool->attrs->affn_strict)
		return pool->attrs->__pod_cpumask;
	else
		return pool->attrs->cpumask;
}

/**
 * worker_attach_to_pool() - attach a worker to a pool
 * @worker: worker to be attached
 * @pool: the target pool
 *
 * Attach @worker to @pool.  Once attached, the %WORKER_UNBOUND flag and
 * cpu-binding of @worker are kept coordinated with the pool across
 * cpu-[un]hotplugs.
 */
static void worker_attach_to_pool(struct worker *worker,
				   struct worker_pool *pool)
{
	mutex_lock(&wq_pool_attach_mutex);

	/*
	 * The wq_pool_attach_mutex ensures %POOL_DISASSOCIATED remains
	 * stable across this function.  See the comments above the flag
	 * definition for details.
	 */
	if (pool->flags & POOL_DISASSOCIATED)
		worker->flags |= WORKER_UNBOUND;
	else
		kthread_set_per_cpu(worker->task, pool->cpu);

	if (worker->rescue_wq)
		set_cpus_allowed_ptr(worker->task, pool_allowed_cpus(pool));

	list_add_tail(&worker->node, &pool->workers);
	worker->pool = pool;

	mutex_unlock(&wq_pool_attach_mutex);
}

/**
 * worker_detach_from_pool() - detach a worker from its pool
 * @worker: worker which is attached to its pool
 *
 * Undo the attaching which had been done in worker_attach_to_pool().  The
 * caller worker shouldn't access to the pool after detached except it has
 * other reference to the pool.
 */
static void worker_detach_from_pool(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;
	struct completion *detach_completion = NULL;

	mutex_lock(&wq_pool_attach_mutex);

	kthread_set_per_cpu(worker->task, -1);
	list_del(&worker->node);
	worker->pool = NULL;

	if (list_empty(&pool->workers) && list_empty(&pool->dying_workers))
		detach_completion = pool->detach_completion;
	mutex_unlock(&wq_pool_attach_mutex);

	/* clear leftover flags without pool->lock after it is detached */
	worker->flags &= ~(WORKER_UNBOUND | WORKER_REBOUND);

	if (detach_completion)
		complete(detach_completion);
}

/**
 * create_worker - create a new workqueue worker
 * @pool: pool the new worker will belong to
 *
 * Create and start a new worker which is attached to @pool.
 *
 * CONTEXT:
 * Might sleep.  Does GFP_KERNEL allocations.
 *
 * Return:
 * Pointer to the newly created worker.
 */
static struct worker *create_worker(struct worker_pool *pool)
{
	struct worker *worker;
	int id;
	char id_buf[23];

	/* ID is needed to determine kthread name */
	id = ida_alloc(&pool->worker_ida, GFP_KERNEL);
	if (id < 0) {
		pr_err_once("workqueue: Failed to allocate a worker ID: %pe\n",
			    ERR_PTR(id));
		return NULL;
	}

	worker = alloc_worker(pool->node);
	if (!worker) {
		pr_err_once("workqueue: Failed to allocate a worker\n");
		goto fail;
	}

	worker->id = id;

	if (pool->cpu >= 0)
		snprintf(id_buf, sizeof(id_buf), "%d:%d%s", pool->cpu, id,
			 pool->attrs->nice < 0  ? "H" : "");
	else
		snprintf(id_buf, sizeof(id_buf), "u%d:%d", pool->id, id);

	worker->task = kthread_create_on_node(worker_thread, worker, pool->node,
					      "kworker/%s", id_buf);
	if (IS_ERR(worker->task)) {
		if (PTR_ERR(worker->task) == -EINTR) {
			pr_err("workqueue: Interrupted when creating a worker thread \"kworker/%s\"\n",
			       id_buf);
		} else {
			pr_err_once("workqueue: Failed to create a worker thread: %pe",
				    worker->task);
		}
		goto fail;
	}

	set_user_nice(worker->task, pool->attrs->nice);
	kthread_bind_mask(worker->task, pool_allowed_cpus(pool));

	/* successful, attach the worker to the pool */
	worker_attach_to_pool(worker, pool);

	/* start the newly created worker */
	raw_spin_lock_irq(&pool->lock);

	worker->pool->nr_workers++;
	worker_enter_idle(worker);
	kick_pool(pool);

	/*
	 * @worker is waiting on a completion in kthread() and will trigger hung
	 * check if not woken up soon. As kick_pool() might not have waken it
	 * up, wake it up explicitly once more.
	 */
	wake_up_process(worker->task);

	raw_spin_unlock_irq(&pool->lock);

	return worker;

fail:
	ida_free(&pool->worker_ida, id);
	kfree(worker);
	return NULL;
}

static void unbind_worker(struct worker *worker)
{
	lockdep_assert_held(&wq_pool_attach_mutex);

	kthread_set_per_cpu(worker->task, -1);
	if (cpumask_intersects(wq_unbound_cpumask, cpu_active_mask))
		WARN_ON_ONCE(set_cpus_allowed_ptr(worker->task, wq_unbound_cpumask) < 0);
	else
		WARN_ON_ONCE(set_cpus_allowed_ptr(worker->task, cpu_possible_mask) < 0);
}

static void wake_dying_workers(struct list_head *cull_list)
{
	struct worker *worker, *tmp;

	list_for_each_entry_safe(worker, tmp, cull_list, entry) {
		list_del_init(&worker->entry);
		unbind_worker(worker);
		/*
		 * If the worker was somehow already running, then it had to be
		 * in pool->idle_list when set_worker_dying() happened or we
		 * wouldn't have gotten here.
		 *
		 * Thus, the worker must either have observed the WORKER_DIE
		 * flag, or have set its state to TASK_IDLE. Either way, the
		 * below will be observed by the worker and is safe to do
		 * outside of pool->lock.
		 */
		wake_up_process(worker->task);
	}
}

/**
 * set_worker_dying - Tag a worker for destruction
 * @worker: worker to be destroyed
 * @list: transfer worker away from its pool->idle_list and into list
 *
 * Tag @worker for destruction and adjust @pool stats accordingly.  The worker
 * should be idle.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 */
static void set_worker_dying(struct worker *worker, struct list_head *list)
{
	struct worker_pool *pool = worker->pool;

	lockdep_assert_held(&pool->lock);
	lockdep_assert_held(&wq_pool_attach_mutex);

	/* sanity check frenzy */
	if (WARN_ON(worker->current_work) ||
	    WARN_ON(!list_empty(&worker->scheduled)) ||
	    WARN_ON(!(worker->flags & WORKER_IDLE)))
		return;

	pool->nr_workers--;
	pool->nr_idle--;

	worker->flags |= WORKER_DIE;

	list_move(&worker->entry, list);
	list_move(&worker->node, &pool->dying_workers);
}

/**
 * idle_worker_timeout - check if some idle workers can now be deleted.
 * @t: The pool's idle_timer that just expired
 *
 * The timer is armed in worker_enter_idle(). Note that it isn't disarmed in
 * worker_leave_idle(), as a worker flicking between idle and active while its
 * pool is at the too_many_workers() tipping point would cause too much timer
 * housekeeping overhead. Since IDLE_WORKER_TIMEOUT is long enough, we just let
 * it expire and re-evaluate things from there.
 */
static void idle_worker_timeout(struct timer_list *t)
{
	struct worker_pool *pool = from_timer(pool, t, idle_timer);
	bool do_cull = false;

	if (work_pending(&pool->idle_cull_work))
		return;

	raw_spin_lock_irq(&pool->lock);

	if (too_many_workers(pool)) {
		struct worker *worker;
		unsigned long expires;

		/* idle_list is kept in LIFO order, check the last one */
		worker = list_entry(pool->idle_list.prev, struct worker, entry);
		expires = worker->last_active + IDLE_WORKER_TIMEOUT;
		do_cull = !time_before(jiffies, expires);

		if (!do_cull)
			mod_timer(&pool->idle_timer, expires);
	}
	raw_spin_unlock_irq(&pool->lock);

	if (do_cull)
		queue_work(system_unbound_wq, &pool->idle_cull_work);
}

/**
 * idle_cull_fn - cull workers that have been idle for too long.
 * @work: the pool's work for handling these idle workers
 *
 * This goes through a pool's idle workers and gets rid of those that have been
 * idle for at least IDLE_WORKER_TIMEOUT seconds.
 *
 * We don't want to disturb isolated CPUs because of a pcpu kworker being
 * culled, so this also resets worker affinity. This requires a sleepable
 * context, hence the split between timer callback and work item.
 */
static void idle_cull_fn(struct work_struct *work)
{
	struct worker_pool *pool = container_of(work, struct worker_pool, idle_cull_work);
	LIST_HEAD(cull_list);

	/*
	 * Grabbing wq_pool_attach_mutex here ensures an already-running worker
	 * cannot proceed beyong worker_detach_from_pool() in its self-destruct
	 * path. This is required as a previously-preempted worker could run after
	 * set_worker_dying() has happened but before wake_dying_workers() did.
	 */
	mutex_lock(&wq_pool_attach_mutex);
	raw_spin_lock_irq(&pool->lock);

	while (too_many_workers(pool)) {
		struct worker *worker;
		unsigned long expires;

		worker = list_entry(pool->idle_list.prev, struct worker, entry);
		expires = worker->last_active + IDLE_WORKER_TIMEOUT;

		if (time_before(jiffies, expires)) {
			mod_timer(&pool->idle_timer, expires);
			break;
		}

		set_worker_dying(worker, &cull_list);
	}

	raw_spin_unlock_irq(&pool->lock);
	wake_dying_workers(&cull_list);
	mutex_unlock(&wq_pool_attach_mutex);
}

static void send_mayday(struct work_struct *work)
{
	struct pool_workqueue *pwq = get_work_pwq(work);
	struct workqueue_struct *wq = pwq->wq;

	lockdep_assert_held(&wq_mayday_lock);

	if (!wq->rescuer)
		return;

	/* mayday mayday mayday */
	if (list_empty(&pwq->mayday_node)) {
		/*
		 * If @pwq is for an unbound wq, its base ref may be put at
		 * any time due to an attribute change.  Pin @pwq until the
		 * rescuer is done with it.
		 */
		get_pwq(pwq);
		list_add_tail(&pwq->mayday_node, &wq->maydays);
		wake_up_process(wq->rescuer->task);
		pwq->stats[PWQ_STAT_MAYDAY]++;
	}
}

static void pool_mayday_timeout(struct timer_list *t)
{
	struct worker_pool *pool = from_timer(pool, t, mayday_timer);
	struct work_struct *work;

	raw_spin_lock_irq(&pool->lock);
	raw_spin_lock(&wq_mayday_lock);		/* for wq->maydays */

	if (need_to_create_worker(pool)) {
		/*
		 * We've been trying to create a new worker but
		 * haven't been successful.  We might be hitting an
		 * allocation deadlock.  Send distress signals to
		 * rescuers.
		 */
		list_for_each_entry(work, &pool->worklist, entry)
			send_mayday(work);
	}

	raw_spin_unlock(&wq_mayday_lock);
	raw_spin_unlock_irq(&pool->lock);

	mod_timer(&pool->mayday_timer, jiffies + MAYDAY_INTERVAL);
}

/**
 * maybe_create_worker - create a new worker if necessary
 * @pool: pool to create a new worker for
 *
 * Create a new worker for @pool if necessary.  @pool is guaranteed to
 * have at least one idle worker on return from this function.  If
 * creating a new worker takes longer than MAYDAY_INTERVAL, mayday is
 * sent to all rescuers with works scheduled on @pool to resolve
 * possible allocation deadlock.
 *
 * On return, need_to_create_worker() is guaranteed to be %false and
 * may_start_working() %true.
 *
 * LOCKING:
 * raw_spin_lock_irq(pool->lock) which may be released and regrabbed
 * multiple times.  Does GFP_KERNEL allocations.  Called only from
 * manager.
 */
static void maybe_create_worker(struct worker_pool *pool)
__releases(&pool->lock)
__acquires(&pool->lock)
{
restart:
	raw_spin_unlock_irq(&pool->lock);

	/* if we don't make progress in MAYDAY_INITIAL_TIMEOUT, call for help */
	mod_timer(&pool->mayday_timer, jiffies + MAYDAY_INITIAL_TIMEOUT);

	while (true) {
		if (create_worker(pool) || !need_to_create_worker(pool))
			break;

		schedule_timeout_interruptible(CREATE_COOLDOWN);

		if (!need_to_create_worker(pool))
			break;
	}

	del_timer_sync(&pool->mayday_timer);
	raw_spin_lock_irq(&pool->lock);
	/*
	 * This is necessary even after a new worker was just successfully
	 * created as @pool->lock was dropped and the new worker might have
	 * already become busy.
	 */
	if (need_to_create_worker(pool))
		goto restart;
}

/**
 * manage_workers - manage worker pool
 * @worker: self
 *
 * Assume the manager role and manage the worker pool @worker belongs
 * to.  At any given time, there can be only zero or one manager per
 * pool.  The exclusion is handled automatically by this function.
 *
 * The caller can safely start processing works on false return.  On
 * true return, it's guaranteed that need_to_create_worker() is false
 * and may_start_working() is true.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock) which may be released and regrabbed
 * multiple times.  Does GFP_KERNEL allocations.
 *
 * Return:
 * %false if the pool doesn't need management and the caller can safely
 * start processing works, %true if management function was performed and
 * the conditions that the caller verified before calling the function may
 * no longer be true.
 */
static bool manage_workers(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;

	if (pool->flags & POOL_MANAGER_ACTIVE)
		return false;

	pool->flags |= POOL_MANAGER_ACTIVE;
	pool->manager = worker;

	maybe_create_worker(pool);

	pool->manager = NULL;
	pool->flags &= ~POOL_MANAGER_ACTIVE;
	rcuwait_wake_up(&manager_wait);
	return true;
}

/**
 * process_one_work - process single work
 * @worker: self
 * @work: work to process
 *
 * Process @work.  This function contains all the logics necessary to
 * process a single work including synchronization against and
 * interaction with other workers on the same cpu, queueing and
 * flushing.  As long as context requirement is met, any worker can
 * call this function to process a work.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock) which is released and regrabbed.
 */
static void process_one_work(struct worker *worker, struct work_struct *work)
__releases(&pool->lock)
__acquires(&pool->lock)
{
	struct pool_workqueue *pwq = get_work_pwq(work);
	struct worker_pool *pool = worker->pool;
	unsigned long work_data;
#ifdef CONFIG_LOCKDEP
	/*
	 * It is permissible to free the struct work_struct from
	 * inside the function that is called from it, this we need to
	 * take into account for lockdep too.  To avoid bogus "held
	 * lock freed" warnings as well as problems when looking into
	 * work->lockdep_map, make a copy and use that here.
	 */
	struct lockdep_map lockdep_map;

	lockdep_copy_map(&lockdep_map, &work->lockdep_map);
#endif
	/* ensure we're on the correct CPU */
	WARN_ON_ONCE(!(pool->flags & POOL_DISASSOCIATED) &&
		     raw_smp_processor_id() != pool->cpu);

	/* claim and dequeue */
	debug_work_deactivate(work);
	hash_add(pool->busy_hash, &worker->hentry, (unsigned long)work);
	worker->current_work = work;
	worker->current_func = work->func;
	worker->current_pwq = pwq;
	worker->current_at = worker->task->se.sum_exec_runtime;
	work_data = *work_data_bits(work);
	worker->current_color = get_work_color(work_data);

	/*
	 * Record wq name for cmdline and debug reporting, may get
	 * overridden through set_worker_desc().
	 */
	strscpy(worker->desc, pwq->wq->name, WORKER_DESC_LEN);

	list_del_init(&work->entry);

	/*
	 * CPU intensive works don't participate in concurrency management.
	 * They're the scheduler's responsibility.  This takes @worker out
	 * of concurrency management and the next code block will chain
	 * execution of the pending work items.
	 */
	if (unlikely(pwq->wq->flags & WQ_CPU_INTENSIVE))
		worker_set_flags(worker, WORKER_CPU_INTENSIVE);

	/*
	 * Kick @pool if necessary. It's always noop for per-cpu worker pools
	 * since nr_running would always be >= 1 at this point. This is used to
	 * chain execution of the pending work items for WORKER_NOT_RUNNING
	 * workers such as the UNBOUND and CPU_INTENSIVE ones.
	 */
	kick_pool(pool);

	/*
	 * Record the last pool and clear PENDING which should be the last
	 * update to @work.  Also, do this inside @pool->lock so that
	 * PENDING and queued state changes happen together while IRQ is
	 * disabled.
	 */
	set_work_pool_and_clear_pending(work, pool->id);

	pwq->stats[PWQ_STAT_STARTED]++;
	raw_spin_unlock_irq(&pool->lock);

	lock_map_acquire(&pwq->wq->lockdep_map);
	lock_map_acquire(&lockdep_map);
	/*
	 * Strictly speaking we should mark the invariant state without holding
	 * any locks, that is, before these two lock_map_acquire()'s.
	 *
	 * However, that would result in:
	 *
	 *   A(W1)
	 *   WFC(C)
	 *		A(W1)
	 *		C(C)
	 *
	 * Which would create W1->C->W1 dependencies, even though there is no
	 * actual deadlock possible. There are two solutions, using a
	 * read-recursive acquire on the work(queue) 'locks', but this will then
	 * hit the lockdep limitation on recursive locks, or simply discard
	 * these locks.
	 *
	 * AFAICT there is no possible deadlock scenario between the
	 * flush_work() and complete() primitives (except for single-threaded
	 * workqueues), so hiding them isn't a problem.
	 */
	lockdep_invariant_state(true);
	trace_workqueue_execute_start(work);
	worker->current_func(work);
	/*
	 * While we must be careful to not use "work" after this, the trace
	 * point will only record its address.
	 */
	trace_workqueue_execute_end(work, worker->current_func);
	pwq->stats[PWQ_STAT_COMPLETED]++;
	lock_map_release(&lockdep_map);
	lock_map_release(&pwq->wq->lockdep_map);

	if (unlikely(in_atomic() || lockdep_depth(current) > 0)) {
		pr_err("BUG: workqueue leaked lock or atomic: %s/0x%08x/%d\n"
		       "     last function: %ps\n",
		       current->comm, preempt_count(), task_pid_nr(current),
		       worker->current_func);
		debug_show_held_locks(current);
		dump_stack();
	}

	/*
	 * The following prevents a kworker from hogging CPU on !PREEMPTION
	 * kernels, where a requeueing work item waiting for something to
	 * happen could deadlock with stop_machine as such work item could
	 * indefinitely requeue itself while all other CPUs are trapped in
	 * stop_machine. At the same time, report a quiescent RCU state so
	 * the same condition doesn't freeze RCU.
	 */
	cond_resched();

	raw_spin_lock_irq(&pool->lock);

	/*
	 * In addition to %WQ_CPU_INTENSIVE, @worker may also have been marked
	 * CPU intensive by wq_worker_tick() if @work hogged CPU longer than
	 * wq_cpu_intensive_thresh_us. Clear it.
	 */
	worker_clr_flags(worker, WORKER_CPU_INTENSIVE);

	/* tag the worker for identification in schedule() */
	worker->last_func = worker->current_func;

	/* we're done with it, release */
	hash_del(&worker->hentry);
	worker->current_work = NULL;
	worker->current_func = NULL;
	worker->current_pwq = NULL;
	worker->current_color = INT_MAX;
	pwq_dec_nr_in_flight(pwq, work_data);
}

/**
 * process_scheduled_works - process scheduled works
 * @worker: self
 *
 * Process all scheduled works.  Please note that the scheduled list
 * may change while processing a work, so this function repeatedly
 * fetches a work from the top and executes it.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock) which may be released and regrabbed
 * multiple times.
 */
static void process_scheduled_works(struct worker *worker)
{
	struct work_struct *work;
	bool first = true;

	while ((work = list_first_entry_or_null(&worker->scheduled,
						struct work_struct, entry))) {
		if (first) {
			worker->pool->watchdog_ts = jiffies;
			first = false;
		}
		process_one_work(worker, work);
	}
}

static void set_pf_worker(bool val)
{
	mutex_lock(&wq_pool_attach_mutex);
	if (val)
		current->flags |= PF_WQ_WORKER;
	else
		current->flags &= ~PF_WQ_WORKER;
	mutex_unlock(&wq_pool_attach_mutex);
}

/**
 * worker_thread - the worker thread function
 * @__worker: self
 *
 * The worker thread function.  All workers belong to a worker_pool -
 * either a per-cpu one or dynamic unbound one.  These workers process all
 * work items regardless of their specific target workqueue.  The only
 * exception is work items which belong to workqueues with a rescuer which
 * will be explained in rescuer_thread().
 *
 * Return: 0
 */
static int worker_thread(void *__worker)
{
	struct worker *worker = __worker;
	struct worker_pool *pool = worker->pool;

	/* tell the scheduler that this is a workqueue worker */
	set_pf_worker(true);
woke_up:
	raw_spin_lock_irq(&pool->lock);

	/* am I supposed to die? */
	if (unlikely(worker->flags & WORKER_DIE)) {
		raw_spin_unlock_irq(&pool->lock);
		set_pf_worker(false);

		set_task_comm(worker->task, "kworker/dying");
		ida_free(&pool->worker_ida, worker->id);
		worker_detach_from_pool(worker);
		WARN_ON_ONCE(!list_empty(&worker->entry));
		kfree(worker);
		return 0;
	}

	worker_leave_idle(worker);
recheck:
	/* no more worker necessary? */
	if (!need_more_worker(pool))
		goto sleep;

	/* do we need to manage? */
	if (unlikely(!may_start_working(pool)) && manage_workers(worker))
		goto recheck;

	/*
	 * ->scheduled list can only be filled while a worker is
	 * preparing to process a work or actually processing it.
	 * Make sure nobody diddled with it while I was sleeping.
	 */
	WARN_ON_ONCE(!list_empty(&worker->scheduled));

	/*
	 * Finish PREP stage.  We're guaranteed to have at least one idle
	 * worker or that someone else has already assumed the manager
	 * role.  This is where @worker starts participating in concurrency
	 * management if applicable and concurrency management is restored
	 * after being rebound.  See rebind_workers() for details.
	 */
	worker_clr_flags(worker, WORKER_PREP | WORKER_REBOUND);

	do {
		struct work_struct *work =
			list_first_entry(&pool->worklist,
					 struct work_struct, entry);

		if (assign_work(work, worker, NULL))
			process_scheduled_works(worker);
	} while (keep_working(pool));

	worker_set_flags(worker, WORKER_PREP);
sleep:
	/*
	 * pool->lock is held and there's no work to process and no need to
	 * manage, sleep.  Workers are woken up only while holding
	 * pool->lock or from local cpu, so setting the current state
	 * before releasing pool->lock is enough to prevent losing any
	 * event.
	 */
	worker_enter_idle(worker);
	__set_current_state(TASK_IDLE);
	raw_spin_unlock_irq(&pool->lock);
	schedule();
	goto woke_up;
}

/**
 * rescuer_thread - the rescuer thread function
 * @__rescuer: self
 *
 * Workqueue rescuer thread function.  There's one rescuer for each
 * workqueue which has WQ_MEM_RECLAIM set.
 *
 * Regular work processing on a pool may block trying to create a new
 * worker which uses GFP_KERNEL allocation which has slight chance of
 * developing into deadlock if some works currently on the same queue
 * need to be processed to satisfy the GFP_KERNEL allocation.  This is
 * the problem rescuer solves.
 *
 * When such condition is possible, the pool summons rescuers of all
 * workqueues which have works queued on the pool and let them process
 * those works so that forward progress can be guaranteed.
 *
 * This should happen rarely.
 *
 * Return: 0
 */
static int rescuer_thread(void *__rescuer)
{
	struct worker *rescuer = __rescuer;
	struct workqueue_struct *wq = rescuer->rescue_wq;
	bool should_stop;

	set_user_nice(current, RESCUER_NICE_LEVEL);

	/*
	 * Mark rescuer as worker too.  As WORKER_PREP is never cleared, it
	 * doesn't participate in concurrency management.
	 */
	set_pf_worker(true);
repeat:
	set_current_state(TASK_IDLE);

	/*
	 * By the time the rescuer is requested to stop, the workqueue
	 * shouldn't have any work pending, but @wq->maydays may still have
	 * pwq(s) queued.  This can happen by non-rescuer workers consuming
	 * all the work items before the rescuer got to them.  Go through
	 * @wq->maydays processing before acting on should_stop so that the
	 * list is always empty on exit.
	 */
	should_stop = kthread_should_stop();

	/* see whether any pwq is asking for help */
	raw_spin_lock_irq(&wq_mayday_lock);

	while (!list_empty(&wq->maydays)) {
		struct pool_workqueue *pwq = list_first_entry(&wq->maydays,
					struct pool_workqueue, mayday_node);
		struct worker_pool *pool = pwq->pool;
		struct work_struct *work, *n;

		__set_current_state(TASK_RUNNING);
		list_del_init(&pwq->mayday_node);

		raw_spin_unlock_irq(&wq_mayday_lock);

		worker_attach_to_pool(rescuer, pool);

		raw_spin_lock_irq(&pool->lock);

		/*
		 * Slurp in all works issued via this workqueue and
		 * process'em.
		 */
		WARN_ON_ONCE(!list_empty(&rescuer->scheduled));
		list_for_each_entry_safe(work, n, &pool->worklist, entry) {
			if (get_work_pwq(work) == pwq &&
			    assign_work(work, rescuer, &n))
				pwq->stats[PWQ_STAT_RESCUED]++;
		}

		if (!list_empty(&rescuer->scheduled)) {
			process_scheduled_works(rescuer);

			/*
			 * The above execution of rescued work items could
			 * have created more to rescue through
			 * pwq_activate_first_inactive() or chained
			 * queueing.  Let's put @pwq back on mayday list so
			 * that such back-to-back work items, which may be
			 * being used to relieve memory pressure, don't
			 * incur MAYDAY_INTERVAL delay inbetween.
			 */
			if (pwq->nr_active && need_to_create_worker(pool)) {
				raw_spin_lock(&wq_mayday_lock);
				/*
				 * Queue iff we aren't racing destruction
				 * and somebody else hasn't queued it already.
				 */
				if (wq->rescuer && list_empty(&pwq->mayday_node)) {
					get_pwq(pwq);
					list_add_tail(&pwq->mayday_node, &wq->maydays);
				}
				raw_spin_unlock(&wq_mayday_lock);
			}
		}

		/*
		 * Put the reference grabbed by send_mayday().  @pool won't
		 * go away while we're still attached to it.
		 */
		put_pwq(pwq);

		/*
		 * Leave this pool. Notify regular workers; otherwise, we end up
		 * with 0 concurrency and stalling the execution.
		 */
		kick_pool(pool);

		raw_spin_unlock_irq(&pool->lock);

		worker_detach_from_pool(rescuer);

		raw_spin_lock_irq(&wq_mayday_lock);
	}

	raw_spin_unlock_irq(&wq_mayday_lock);

	if (should_stop) {
		__set_current_state(TASK_RUNNING);
		set_pf_worker(false);
		return 0;
	}

	/* rescuers should never participate in concurrency management */
	WARN_ON_ONCE(!(rescuer->flags & WORKER_NOT_RUNNING));
	schedule();
	goto repeat;
}

/**
 * check_flush_dependency - check for flush dependency sanity
 * @target_wq: workqueue being flushed
 * @target_work: work item being flushed (NULL for workqueue flushes)
 *
 * %current is trying to flush the whole @target_wq or @target_work on it.
 * If @target_wq doesn't have %WQ_MEM_RECLAIM, verify that %current is not
 * reclaiming memory or running on a workqueue which doesn't have
 * %WQ_MEM_RECLAIM as that can break forward-progress guarantee leading to
 * a deadlock.
 */
static void check_flush_dependency(struct workqueue_struct *target_wq,
				   struct work_struct *target_work)
{
	work_func_t target_func = target_work ? target_work->func : NULL;
	struct worker *worker;

	if (target_wq->flags & WQ_MEM_RECLAIM)
		return;

	worker = current_wq_worker();

	WARN_ONCE(current->flags & PF_MEMALLOC,
		  "workqueue: PF_MEMALLOC task %d(%s) is flushing !WQ_MEM_RECLAIM %s:%ps",
		  current->pid, current->comm, target_wq->name, target_func);
	WARN_ONCE(worker && ((worker->current_pwq->wq->flags &
			      (WQ_MEM_RECLAIM | __WQ_LEGACY)) == WQ_MEM_RECLAIM),
		  "workqueue: WQ_MEM_RECLAIM %s:%ps is flushing !WQ_MEM_RECLAIM %s:%ps",
		  worker->current_pwq->wq->name, worker->current_func,
		  target_wq->name, target_func);
}

struct wq_barrier {
	struct work_struct	work;
	struct completion	done;
	struct task_struct	*task;	/* purely informational */
};

static void wq_barrier_func(struct work_struct *work)
{
	struct wq_barrier *barr = container_of(work, struct wq_barrier, work);
	complete(&barr->done);
}

/**
 * insert_wq_barrier - insert a barrier work
 * @pwq: pwq to insert barrier into
 * @barr: wq_barrier to insert
 * @target: target work to attach @barr to
 * @worker: worker currently executing @target, NULL if @target is not executing
 *
 * @barr is linked to @target such that @barr is completed only after
 * @target finishes execution.  Please note that the ordering
 * guarantee is observed only with respect to @target and on the local
 * cpu.
 *
 * Currently, a queued barrier can't be canceled.  This is because
 * try_to_grab_pending() can't determine whether the work to be
 * grabbed is at the head of the queue and thus can't clear LINKED
 * flag of the previous work while there must be a valid next work
 * after a work with LINKED flag set.
 *
 * Note that when @worker is non-NULL, @target may be modified
 * underneath us, so we can't reliably determine pwq from @target.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 */
static void insert_wq_barrier(struct pool_workqueue *pwq,
			      struct wq_barrier *barr,
			      struct work_struct *target, struct worker *worker)
{
	unsigned int work_flags = 0;
	unsigned int work_color;
	struct list_head *head;

	/*
	 * debugobject calls are safe here even with pool->lock locked
	 * as we know for sure that this will not trigger any of the
	 * checks and call back into the fixup functions where we
	 * might deadlock.
	 */
	INIT_WORK_ONSTACK(&barr->work, wq_barrier_func);
	__set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(&barr->work));

	init_completion_map(&barr->done, &target->lockdep_map);

	barr->task = current;

	/* The barrier work item does not participate in pwq->nr_active. */
	work_flags |= WORK_STRUCT_INACTIVE;

	/*
	 * If @target is currently being executed, schedule the
	 * barrier to the worker; otherwise, put it after @target.
	 */
	if (worker) {
		head = worker->scheduled.next;
		work_color = worker->current_color;
	} else {
		unsigned long *bits = work_data_bits(target);

		head = target->entry.next;
		/* there can already be other linked works, inherit and set */
		work_flags |= *bits & WORK_STRUCT_LINKED;
		work_color = get_work_color(*bits);
		__set_bit(WORK_STRUCT_LINKED_BIT, bits);
	}

	pwq->nr_in_flight[work_color]++;
	work_flags |= work_color_to_flags(work_color);

	insert_work(pwq, &barr->work, head, work_flags);
}

/**
 * flush_workqueue_prep_pwqs - prepare pwqs for workqueue flushing
 * @wq: workqueue being flushed
 * @flush_color: new flush color, < 0 for no-op
 * @work_color: new work color, < 0 for no-op
 *
 * Prepare pwqs for workqueue flushing.
 *
 * If @flush_color is non-negative, flush_color on all pwqs should be
 * -1.  If no pwq has in-flight commands at the specified color, all
 * pwq->flush_color's stay at -1 and %false is returned.  If any pwq
 * has in flight commands, its pwq->flush_color is set to
 * @flush_color, @wq->nr_pwqs_to_flush is updated accordingly, pwq
 * wakeup logic is armed and %true is returned.
 *
 * The caller should have initialized @wq->first_flusher prior to
 * calling this function with non-negative @flush_color.  If
 * @flush_color is negative, no flush color update is done and %false
 * is returned.
 *
 * If @work_color is non-negative, all pwqs should have the same
 * work_color which is previous to @work_color and all will be
 * advanced to @work_color.
 *
 * CONTEXT:
 * mutex_lock(wq->mutex).
 *
 * Return:
 * %true if @flush_color >= 0 and there's something to flush.  %false
 * otherwise.
 */
static bool flush_workqueue_prep_pwqs(struct workqueue_struct *wq,
				      int flush_color, int work_color)
{
	bool wait = false;
	struct pool_workqueue *pwq;

	if (flush_color >= 0) {
		WARN_ON_ONCE(atomic_read(&wq->nr_pwqs_to_flush));
		atomic_set(&wq->nr_pwqs_to_flush, 1);
	}

	for_each_pwq(pwq, wq) {
		struct worker_pool *pool = pwq->pool;

		raw_spin_lock_irq(&pool->lock);

		if (flush_color >= 0) {
			WARN_ON_ONCE(pwq->flush_color != -1);

			if (pwq->nr_in_flight[flush_color]) {
				pwq->flush_color = flush_color;
				atomic_inc(&wq->nr_pwqs_to_flush);
				wait = true;
			}
		}

		if (work_color >= 0) {
			WARN_ON_ONCE(work_color != work_next_color(pwq->work_color));
			pwq->work_color = work_color;
		}

		raw_spin_unlock_irq(&pool->lock);
	}

	if (flush_color >= 0 && atomic_dec_and_test(&wq->nr_pwqs_to_flush))
		complete(&wq->first_flusher->done);

	return wait;
}

/**
 * __flush_workqueue - ensure that any scheduled work has run to completion.
 * @wq: workqueue to flush
 *
 * This function sleeps until all work items which were queued on entry
 * have finished execution, but it is not livelocked by new incoming ones.
 */
void __flush_workqueue(struct workqueue_struct *wq)
{
	struct wq_flusher this_flusher = {
		.list = LIST_HEAD_INIT(this_flusher.list),
		.flush_color = -1,
		.done = COMPLETION_INITIALIZER_ONSTACK_MAP(this_flusher.done, wq->lockdep_map),
	};
	int next_color;

	if (WARN_ON(!wq_online))
		return;

	lock_map_acquire(&wq->lockdep_map);
	lock_map_release(&wq->lockdep_map);

	mutex_lock(&wq->mutex);

	/*
	 * Start-to-wait phase
	 */
	next_color = work_next_color(wq->work_color);

	if (next_color != wq->flush_color) {
		/*
		 * Color space is not full.  The current work_color
		 * becomes our flush_color and work_color is advanced
		 * by one.
		 */
		WARN_ON_ONCE(!list_empty(&wq->flusher_overflow));
		this_flusher.flush_color = wq->work_color;
		wq->work_color = next_color;

		if (!wq->first_flusher) {
			/* no flush in progress, become the first flusher */
			WARN_ON_ONCE(wq->flush_color != this_flusher.flush_color);

			wq->first_flusher = &this_flusher;

			if (!flush_workqueue_prep_pwqs(wq, wq->flush_color,
						       wq->work_color)) {
				/* nothing to flush, done */
				wq->flush_color = next_color;
				wq->first_flusher = NULL;
				goto out_unlock;
			}
		} else {
			/* wait in queue */
			WARN_ON_ONCE(wq->flush_color == this_flusher.flush_color);
			list_add_tail(&this_flusher.list, &wq->flusher_queue);
			flush_workqueue_prep_pwqs(wq, -1, wq->work_color);
		}
	} else {
		/*
		 * Oops, color space is full, wait on overflow queue.
		 * The next flush completion will assign us
		 * flush_color and transfer to flusher_queue.
		 */
		list_add_tail(&this_flusher.list, &wq->flusher_overflow);
	}

	check_flush_dependency(wq, NULL);

	mutex_unlock(&wq->mutex);

	wait_for_completion(&this_flusher.done);

	/*
	 * Wake-up-and-cascade phase
	 *
	 * First flushers are responsible for cascading flushes and
	 * handling overflow.  Non-first flushers can simply return.
	 */
	if (READ_ONCE(wq->first_flusher) != &this_flusher)
		return;

	mutex_lock(&wq->mutex);

	/* we might have raced, check again with mutex held */
	if (wq->first_flusher != &this_flusher)
		goto out_unlock;

	WRITE_ONCE(wq->first_flusher, NULL);

	WARN_ON_ONCE(!list_empty(&this_flusher.list));
	WARN_ON_ONCE(wq->flush_color != this_flusher.flush_color);

	while (true) {
		struct wq_flusher *next, *tmp;

		/* complete all the flushers sharing the current flush color */
		list_for_each_entry_safe(next, tmp, &wq->flusher_queue, list) {
			if (next->flush_color != wq->flush_color)
				break;
			list_del_init(&next->list);
			complete(&next->done);
		}

		WARN_ON_ONCE(!list_empty(&wq->flusher_overflow) &&
			     wq->flush_color != work_next_color(wq->work_color));

		/* this flush_color is finished, advance by one */
		wq->flush_color = work_next_color(wq->flush_color);

		/* one color has been freed, handle overflow queue */
		if (!list_empty(&wq->flusher_overflow)) {
			/*
			 * Assign the same color to all overflowed
			 * flushers, advance work_color and append to
			 * flusher_queue.  This is the start-to-wait
			 * phase for these overflowed flushers.
			 */
			list_for_each_entry(tmp, &wq->flusher_overflow, list)
				tmp->flush_color = wq->work_color;

			wq->work_color = work_next_color(wq->work_color);

			list_splice_tail_init(&wq->flusher_overflow,
					      &wq->flusher_queue);
			flush_workqueue_prep_pwqs(wq, -1, wq->work_color);
		}

		if (list_empty(&wq->flusher_queue)) {
			WARN_ON_ONCE(wq->flush_color != wq->work_color);
			break;
		}

		/*
		 * Need to flush more colors.  Make the next flusher
		 * the new first flusher and arm pwqs.
		 */
		WARN_ON_ONCE(wq->flush_color == wq->work_color);
		WARN_ON_ONCE(wq->flush_color != next->flush_color);

		list_del_init(&next->list);
		wq->first_flusher = next;

		if (flush_workqueue_prep_pwqs(wq, wq->flush_color, -1))
			break;

		/*
		 * Meh... this color is already done, clear first
		 * flusher and repeat cascading.
		 */
		wq->first_flusher = NULL;
	}

out_unlock:
	mutex_unlock(&wq->mutex);
}
EXPORT_SYMBOL(__flush_workqueue);

/**
 * drain_workqueue - drain a workqueue
 * @wq: workqueue to drain
 *
 * Wait until the workqueue becomes empty.  While draining is in progress,
 * only chain queueing is allowed.  IOW, only currently pending or running
 * work items on @wq can queue further work items on it.  @wq is flushed
 * repeatedly until it becomes empty.  The number of flushing is determined
 * by the depth of chaining and should be relatively short.  Whine if it
 * takes too long.
 */
void drain_workqueue(struct workqueue_struct *wq)
{
	unsigned int flush_cnt = 0;
	struct pool_workqueue *pwq;

	/*
	 * __queue_work() needs to test whether there are drainers, is much
	 * hotter than drain_workqueue() and already looks at @wq->flags.
	 * Use __WQ_DRAINING so that queue doesn't have to check nr_drainers.
	 */
	mutex_lock(&wq->mutex);
	if (!wq->nr_drainers++)
		wq->flags |= __WQ_DRAINING;
	mutex_unlock(&wq->mutex);
reflush:
	__flush_workqueue(wq);

	mutex_lock(&wq->mutex);

	for_each_pwq(pwq, wq) {
		bool drained;

		raw_spin_lock_irq(&pwq->pool->lock);
		drained = !pwq->nr_active && list_empty(&pwq->inactive_works);
		raw_spin_unlock_irq(&pwq->pool->lock);

		if (drained)
			continue;

		if (++flush_cnt == 10 ||
		    (flush_cnt % 100 == 0 && flush_cnt <= 1000))
			pr_warn("workqueue %s: %s() isn't complete after %u tries\n",
				wq->name, __func__, flush_cnt);

		mutex_unlock(&wq->mutex);
		goto reflush;
	}

	if (!--wq->nr_drainers)
		wq->flags &= ~__WQ_DRAINING;
	mutex_unlock(&wq->mutex);
}
EXPORT_SYMBOL_GPL(drain_workqueue);

static bool start_flush_work(struct work_struct *work, struct wq_barrier *barr,
			     bool from_cancel)
{
	struct worker *worker = NULL;
	struct worker_pool *pool;
	struct pool_workqueue *pwq;

	might_sleep();

	rcu_read_lock();
	pool = get_work_pool(work);
	if (!pool) {
		rcu_read_unlock();
		return false;
	}

	raw_spin_lock_irq(&pool->lock);
	/* see the comment in try_to_grab_pending() with the same code */
	pwq = get_work_pwq(work);
	if (pwq) {
		if (unlikely(pwq->pool != pool))
			goto already_gone;
	} else {
		worker = find_worker_executing_work(pool, work);
		if (!worker)
			goto already_gone;
		pwq = worker->current_pwq;
	}

	check_flush_dependency(pwq->wq, work);

	insert_wq_barrier(pwq, barr, work, worker);
	raw_spin_unlock_irq(&pool->lock);

	/*
	 * Force a lock recursion deadlock when using flush_work() inside a
	 * single-threaded or rescuer equipped workqueue.
	 *
	 * For single threaded workqueues the deadlock happens when the work
	 * is after the work issuing the flush_work(). For rescuer equipped
	 * workqueues the deadlock happens when the rescuer stalls, blocking
	 * forward progress.
	 */
	if (!from_cancel &&
	    (pwq->wq->saved_max_active == 1 || pwq->wq->rescuer)) {
		lock_map_acquire(&pwq->wq->lockdep_map);
		lock_map_release(&pwq->wq->lockdep_map);
	}
	rcu_read_unlock();
	return true;
already_gone:
	raw_spin_unlock_irq(&pool->lock);
	rcu_read_unlock();
	return false;
}

static bool __flush_work(struct work_struct *work, bool from_cancel)
{
	struct wq_barrier barr;

	if (WARN_ON(!wq_online))
		return false;

	if (WARN_ON(!work->func))
		return false;

	lock_map_acquire(&work->lockdep_map);
	lock_map_release(&work->lockdep_map);

	if (start_flush_work(work, &barr, from_cancel)) {
		wait_for_completion(&barr.done);
		destroy_work_on_stack(&barr.work);
		return true;
	} else {
		return false;
	}
}

/**
 * flush_work - wait for a work to finish executing the last queueing instance
 * @work: the work to flush
 *
 * Wait until @work has finished execution.  @work is guaranteed to be idle
 * on return if it hasn't been requeued since flush started.
 *
 * Return:
 * %true if flush_work() waited for the work to finish execution,
 * %false if it was already idle.
 */
bool flush_work(struct work_struct *work)
{
	return __flush_work(work, false);
}
EXPORT_SYMBOL_GPL(flush_work);

struct cwt_wait {
	wait_queue_entry_t		wait;
	struct work_struct	*work;
};

static int cwt_wakefn(wait_queue_entry_t *wait, unsigned mode, int sync, void *key)
{
	struct cwt_wait *cwait = container_of(wait, struct cwt_wait, wait);

	if (cwait->work != key)
		return 0;
	return autoremove_wake_function(wait, mode, sync, key);
}

static bool __cancel_work_timer(struct work_struct *work, bool is_dwork)
{
	static DECLARE_WAIT_QUEUE_HEAD(cancel_waitq);
	unsigned long flags;
	int ret;

	do {
		ret = try_to_grab_pending(work, is_dwork, &flags);
		/*
		 * If someone else is already canceling, wait for it to
		 * finish.  flush_work() doesn't work for PREEMPT_NONE
		 * because we may get scheduled between @work's completion
		 * and the other canceling task resuming and clearing
		 * CANCELING - flush_work() will return false immediately
		 * as @work is no longer busy, try_to_grab_pending() will
		 * return -ENOENT as @work is still being canceled and the
		 * other canceling task won't be able to clear CANCELING as
		 * we're hogging the CPU.
		 *
		 * Let's wait for completion using a waitqueue.  As this
		 * may lead to the thundering herd problem, use a custom
		 * wake function which matches @work along with exclusive
		 * wait and wakeup.
		 */
		if (unlikely(ret == -ENOENT)) {
			struct cwt_wait cwait;

			init_wait(&cwait.wait);
			cwait.wait.func = cwt_wakefn;
			cwait.work = work;

			prepare_to_wait_exclusive(&cancel_waitq, &cwait.wait,
						  TASK_UNINTERRUPTIBLE);
			if (work_is_canceling(work))
				schedule();
			finish_wait(&cancel_waitq, &cwait.wait);
		}
	} while (unlikely(ret < 0));

	/* tell other tasks trying to grab @work to back off */
	mark_work_canceling(work);
	local_irq_restore(flags);

	/*
	 * This allows canceling during early boot.  We know that @work
	 * isn't executing.
	 */
	if (wq_online)
		__flush_work(work, true);

	clear_work_data(work);

	/*
	 * Paired with prepare_to_wait() above so that either
	 * waitqueue_active() is visible here or !work_is_canceling() is
	 * visible there.
	 */
	smp_mb();
	if (waitqueue_active(&cancel_waitq))
		__wake_up(&cancel_waitq, TASK_NORMAL, 1, work);

	return ret;
}

/**
 * cancel_work_sync - cancel a work and wait for it to finish
 * @work: the work to cancel
 *
 * Cancel @work and wait for its execution to finish.  This function
 * can be used even if the work re-queues itself or migrates to
 * another workqueue.  On return from this function, @work is
 * guaranteed to be not pending or executing on any CPU.
 *
 * cancel_work_sync(&delayed_work->work) must not be used for
 * delayed_work's.  Use cancel_delayed_work_sync() instead.
 *
 * The caller must ensure that the workqueue on which @work was last
 * queued can't be destroyed before this function returns.
 *
 * Return:
 * %true if @work was pending, %false otherwise.
 */
bool cancel_work_sync(struct work_struct *work)
{
	return __cancel_work_timer(work, false);
}
EXPORT_SYMBOL_GPL(cancel_work_sync);

/**
 * flush_delayed_work - wait for a dwork to finish executing the last queueing
 * @dwork: the delayed work to flush
 *
 * Delayed timer is cancelled and the pending work is queued for
 * immediate execution.  Like flush_work(), this function only
 * considers the last queueing instance of @dwork.
 *
 * Return:
 * %true if flush_work() waited for the work to finish execution,
 * %false if it was already idle.
 */
bool flush_delayed_work(struct delayed_work *dwork)
{
	local_irq_disable();
	if (del_timer_sync(&dwork->timer))
		__queue_work(dwork->cpu, dwork->wq, &dwork->work);
	local_irq_enable();
	return flush_work(&dwork->work);
}
EXPORT_SYMBOL(flush_delayed_work);

/**
 * flush_rcu_work - wait for a rwork to finish executing the last queueing
 * @rwork: the rcu work to flush
 *
 * Return:
 * %true if flush_rcu_work() waited for the work to finish execution,
 * %false if it was already idle.
 */
bool flush_rcu_work(struct rcu_work *rwork)
{
	if (test_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(&rwork->work))) {
		rcu_barrier();
		flush_work(&rwork->work);
		return true;
	} else {
		return flush_work(&rwork->work);
	}
}
EXPORT_SYMBOL(flush_rcu_work);

static bool __cancel_work(struct work_struct *work, bool is_dwork)
{
	unsigned long flags;
	int ret;

	do {
		ret = try_to_grab_pending(work, is_dwork, &flags);
	} while (unlikely(ret == -EAGAIN));

	if (unlikely(ret < 0))
		return false;

	set_work_pool_and_clear_pending(work, get_work_pool_id(work));
	local_irq_restore(flags);
	return ret;
}

/*
 * See cancel_delayed_work()
 */
bool cancel_work(struct work_struct *work)
{
	return __cancel_work(work, false);
}
EXPORT_SYMBOL(cancel_work);

/**
 * cancel_delayed_work - cancel a delayed work
 * @dwork: delayed_work to cancel
 *
 * Kill off a pending delayed_work.
 *
 * Return: %true if @dwork was pending and canceled; %false if it wasn't
 * pending.
 *
 * Note:
 * The work callback function may still be running on return, unless
 * it returns %true and the work doesn't re-arm itself.  Explicitly flush or
 * use cancel_delayed_work_sync() to wait on it.
 *
 * This function is safe to call from any context including IRQ handler.
 */
bool cancel_delayed_work(struct delayed_work *dwork)
{
	return __cancel_work(&dwork->work, true);
}
EXPORT_SYMBOL(cancel_delayed_work);

/**
 * cancel_delayed_work_sync - cancel a delayed work and wait for it to finish
 * @dwork: the delayed work cancel
 *
 * This is cancel_work_sync() for delayed works.
 *
 * Return:
 * %true if @dwork was pending, %false otherwise.
 */
bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
	return __cancel_work_timer(&dwork->work, true);
}
EXPORT_SYMBOL(cancel_delayed_work_sync);

/**
 * schedule_on_each_cpu - execute a function synchronously on each online CPU
 * @func: the function to call
 *
 * schedule_on_each_cpu() executes @func on each online CPU using the
 * system workqueue and blocks until all CPUs have completed.
 * schedule_on_each_cpu() is very slow.
 *
 * Return:
 * 0 on success, -errno on failure.
 */
int schedule_on_each_cpu(work_func_t func)
{
	int cpu;
	struct work_struct __percpu *works;

	works = alloc_percpu(struct work_struct);
	if (!works)
		return -ENOMEM;

	cpus_read_lock();

	for_each_online_cpu(cpu) {
		struct work_struct *work = per_cpu_ptr(works, cpu);

		INIT_WORK(work, func);
		schedule_work_on(cpu, work);
	}

	for_each_online_cpu(cpu)
		flush_work(per_cpu_ptr(works, cpu));

	cpus_read_unlock();
	free_percpu(works);
	return 0;
}

/**
 * execute_in_process_context - reliably execute the routine with user context
 * @fn:		the function to execute
 * @ew:		guaranteed storage for the execute work structure (must
 *		be available when the work executes)
 *
 * Executes the function immediately if process context is available,
 * otherwise schedules the function for delayed execution.
 *
 * Return:	0 - function was executed
 *		1 - function was scheduled for execution
 */
int execute_in_process_context(work_func_t fn, struct execute_work *ew)
{
	if (!in_interrupt()) {
		fn(&ew->work);
		return 0;
	}

	INIT_WORK(&ew->work, fn);
	schedule_work(&ew->work);

	return 1;
}
EXPORT_SYMBOL_GPL(execute_in_process_context);

/**
 * free_workqueue_attrs - free a workqueue_attrs
 * @attrs: workqueue_attrs to free
 *
 * Undo alloc_workqueue_attrs().
 */
void free_workqueue_attrs(struct workqueue_attrs *attrs)
{
	if (attrs) {
		free_cpumask_var(attrs->cpumask);
		free_cpumask_var(attrs->__pod_cpumask);
		kfree(attrs);
	}
}

/**
 * alloc_workqueue_attrs - allocate a workqueue_attrs
 *
 * Allocate a new workqueue_attrs, initialize with default settings and
 * return it.
 *
 * Return: The allocated new workqueue_attr on success. %NULL on failure.
 */
struct workqueue_attrs *alloc_workqueue_attrs(void)
{
	struct workqueue_attrs *attrs;

	attrs = kzalloc(sizeof(*attrs), GFP_KERNEL);
	if (!attrs)
		goto fail;
	if (!alloc_cpumask_var(&attrs->cpumask, GFP_KERNEL))
		goto fail;
	if (!alloc_cpumask_var(&attrs->__pod_cpumask, GFP_KERNEL))
		goto fail;

	cpumask_copy(attrs->cpumask, cpu_possible_mask);
	attrs->affn_scope = WQ_AFFN_DFL;
	return attrs;
fail:
	free_workqueue_attrs(attrs);
	return NULL;
}

static void copy_workqueue_attrs(struct workqueue_attrs *to,
				 const struct workqueue_attrs *from)
{
	to->nice = from->nice;
	cpumask_copy(to->cpumask, from->cpumask);
	cpumask_copy(to->__pod_cpumask, from->__pod_cpumask);
	to->affn_strict = from->affn_strict;

	/*
	 * Unlike hash and equality test, copying shouldn't ignore wq-only
	 * fields as copying is used for both pool and wq attrs. Instead,
	 * get_unbound_pool() explicitly clears the fields.
	 */
	to->affn_scope = from->affn_scope;
	to->ordered = from->ordered;
}

/*
 * Some attrs fields are workqueue-only. Clear them for worker_pool's. See the
 * comments in 'struct workqueue_attrs' definition.
 */
static void wqattrs_clear_for_pool(struct workqueue_attrs *attrs)
{
	attrs->affn_scope = WQ_AFFN_NR_TYPES;
	attrs->ordered = false;
}

/* hash value of the content of @attr */
static u32 wqattrs_hash(const struct workqueue_attrs *attrs)
{
	u32 hash = 0;

	hash = jhash_1word(attrs->nice, hash);
	hash = jhash(cpumask_bits(attrs->cpumask),
		     BITS_TO_LONGS(nr_cpumask_bits) * sizeof(long), hash);
	hash = jhash(cpumask_bits(attrs->__pod_cpumask),
		     BITS_TO_LONGS(nr_cpumask_bits) * sizeof(long), hash);
	hash = jhash_1word(attrs->affn_strict, hash);
	return hash;
}

/* content equality test */
static bool wqattrs_equal(const struct workqueue_attrs *a,
			  const struct workqueue_attrs *b)
{
	if (a->nice != b->nice)
		return false;
	if (!cpumask_equal(a->cpumask, b->cpumask))
		return false;
	if (!cpumask_equal(a->__pod_cpumask, b->__pod_cpumask))
		return false;
	if (a->affn_strict != b->affn_strict)
		return false;
	return true;
}

/* Update @attrs with actually available CPUs */
static void wqattrs_actualize_cpumask(struct workqueue_attrs *attrs,
				      const cpumask_t *unbound_cpumask)
{
	/*
	 * Calculate the effective CPU mask of @attrs given @unbound_cpumask. If
	 * @attrs->cpumask doesn't overlap with @unbound_cpumask, we fallback to
	 * @unbound_cpumask.
	 */
	cpumask_and(attrs->cpumask, attrs->cpumask, unbound_cpumask);
	if (unlikely(cpumask_empty(attrs->cpumask)))
		cpumask_copy(attrs->cpumask, unbound_cpumask);
}

/* find wq_pod_type to use for @attrs */
static const struct wq_pod_type *
wqattrs_pod_type(const struct workqueue_attrs *attrs)
{
	enum wq_affn_scope scope;
	struct wq_pod_type *pt;

	/* to synchronize access to wq_affn_dfl */
	lockdep_assert_held(&wq_pool_mutex);

	if (attrs->affn_scope == WQ_AFFN_DFL)
		scope = wq_affn_dfl;
	else
		scope = attrs->affn_scope;

	pt = &wq_pod_types[scope];

	if (!WARN_ON_ONCE(attrs->affn_scope == WQ_AFFN_NR_TYPES) &&
	    likely(pt->nr_pods))
		return pt;

	/*
	 * Before workqueue_init_topology(), only SYSTEM is available which is
	 * initialized in workqueue_init_early().
	 */
	pt = &wq_pod_types[WQ_AFFN_SYSTEM];
	BUG_ON(!pt->nr_pods);
	return pt;
}

/**
 * init_worker_pool - initialize a newly zalloc'd worker_pool
 * @pool: worker_pool to initialize
 *
 * Initialize a newly zalloc'd @pool.  It also allocates @pool->attrs.
 *
 * Return: 0 on success, -errno on failure.  Even on failure, all fields
 * inside @pool proper are initialized and put_unbound_pool() can be called
 * on @pool safely to release it.
 */
static int init_worker_pool(struct worker_pool *pool)
{
	raw_spin_lock_init(&pool->lock);
	pool->id = -1;
	pool->cpu = -1;
	pool->node = NUMA_NO_NODE;
	pool->flags |= POOL_DISASSOCIATED;
	pool->watchdog_ts = jiffies;
	INIT_LIST_HEAD(&pool->worklist);
	INIT_LIST_HEAD(&pool->idle_list);
	hash_init(pool->busy_hash);

	timer_setup(&pool->idle_timer, idle_worker_timeout, TIMER_DEFERRABLE);
	INIT_WORK(&pool->idle_cull_work, idle_cull_fn);

	timer_setup(&pool->mayday_timer, pool_mayday_timeout, 0);

	INIT_LIST_HEAD(&pool->workers);
	INIT_LIST_HEAD(&pool->dying_workers);

	ida_init(&pool->worker_ida);
	INIT_HLIST_NODE(&pool->hash_node);
	pool->refcnt = 1;

	/* shouldn't fail above this point */
	pool->attrs = alloc_workqueue_attrs();
	if (!pool->attrs)
		return -ENOMEM;

	wqattrs_clear_for_pool(pool->attrs);

	return 0;
}

#ifdef CONFIG_LOCKDEP
static void wq_init_lockdep(struct workqueue_struct *wq)
{
	char *lock_name;

	lockdep_register_key(&wq->key);
	lock_name = kasprintf(GFP_KERNEL, "%s%s", "(wq_completion)", wq->name);
	if (!lock_name)
		lock_name = wq->name;

	wq->lock_name = lock_name;
	lockdep_init_map(&wq->lockdep_map, lock_name, &wq->key, 0);
}

static void wq_unregister_lockdep(struct workqueue_struct *wq)
{
	lockdep_unregister_key(&wq->key);
}

static void wq_free_lockdep(struct workqueue_struct *wq)
{
	if (wq->lock_name != wq->name)
		kfree(wq->lock_name);
}
#else
static void wq_init_lockdep(struct workqueue_struct *wq)
{
}

static void wq_unregister_lockdep(struct workqueue_struct *wq)
{
}

static void wq_free_lockdep(struct workqueue_struct *wq)
{
}
#endif

static void rcu_free_wq(struct rcu_head *rcu)
{
	struct workqueue_struct *wq =
		container_of(rcu, struct workqueue_struct, rcu);

	wq_free_lockdep(wq);
	free_percpu(wq->cpu_pwq);
	free_workqueue_attrs(wq->unbound_attrs);
	kfree(wq);
}

static void rcu_free_pool(struct rcu_head *rcu)
{
	struct worker_pool *pool = container_of(rcu, struct worker_pool, rcu);

	ida_destroy(&pool->worker_ida);
	free_workqueue_attrs(pool->attrs);
	kfree(pool);
}

/**
 * put_unbound_pool - put a worker_pool
 * @pool: worker_pool to put
 *
 * Put @pool.  If its refcnt reaches zero, it gets destroyed in RCU
 * safe manner.  get_unbound_pool() calls this function on its failure path
 * and this function should be able to release pools which went through,
 * successfully or not, init_worker_pool().
 *
 * Should be called with wq_pool_mutex held.
 */
static void put_unbound_pool(struct worker_pool *pool)
{
	DECLARE_COMPLETION_ONSTACK(detach_completion);
	struct worker *worker;
	LIST_HEAD(cull_list);

	lockdep_assert_held(&wq_pool_mutex);

	if (--pool->refcnt)
		return;

	/* sanity checks */
	if (WARN_ON(!(pool->cpu < 0)) ||
	    WARN_ON(!list_empty(&pool->worklist)))
		return;

	/* release id and unhash */
	if (pool->id >= 0)
		idr_remove(&worker_pool_idr, pool->id);
	hash_del(&pool->hash_node);

	/*
	 * Become the manager and destroy all workers.  This prevents
	 * @pool's workers from blocking on attach_mutex.  We're the last
	 * manager and @pool gets freed with the flag set.
	 *
	 * Having a concurrent manager is quite unlikely to happen as we can
	 * only get here with
	 *   pwq->refcnt == pool->refcnt == 0
	 * which implies no work queued to the pool, which implies no worker can
	 * become the manager. However a worker could have taken the role of
	 * manager before the refcnts dropped to 0, since maybe_create_worker()
	 * drops pool->lock
	 */
	while (true) {
		rcuwait_wait_event(&manager_wait,
				   !(pool->flags & POOL_MANAGER_ACTIVE),
				   TASK_UNINTERRUPTIBLE);

		mutex_lock(&wq_pool_attach_mutex);
		raw_spin_lock_irq(&pool->lock);
		if (!(pool->flags & POOL_MANAGER_ACTIVE)) {
			pool->flags |= POOL_MANAGER_ACTIVE;
			break;
		}
		raw_spin_unlock_irq(&pool->lock);
		mutex_unlock(&wq_pool_attach_mutex);
	}

	while ((worker = first_idle_worker(pool)))
		set_worker_dying(worker, &cull_list);
	WARN_ON(pool->nr_workers || pool->nr_idle);
	raw_spin_unlock_irq(&pool->lock);

	wake_dying_workers(&cull_list);

	if (!list_empty(&pool->workers) || !list_empty(&pool->dying_workers))
		pool->detach_completion = &detach_completion;
	mutex_unlock(&wq_pool_attach_mutex);

	if (pool->detach_completion)
		wait_for_completion(pool->detach_completion);

	/* shut down the timers */
	del_timer_sync(&pool->idle_timer);
	cancel_work_sync(&pool->idle_cull_work);
	del_timer_sync(&pool->mayday_timer);

	/* RCU protected to allow dereferences from get_work_pool() */
	call_rcu(&pool->rcu, rcu_free_pool);
}

/**
 * get_unbound_pool - get a worker_pool with the specified attributes
 * @attrs: the attributes of the worker_pool to get
 *
 * Obtain a worker_pool which has the same attributes as @attrs, bump the
 * reference count and return it.  If there already is a matching
 * worker_pool, it will be used; otherwise, this function attempts to
 * create a new one.
 *
 * Should be called with wq_pool_mutex held.
 *
 * Return: On success, a worker_pool with the same attributes as @attrs.
 * On failure, %NULL.
 */
static struct worker_pool *get_unbound_pool(const struct workqueue_attrs *attrs)
{
	struct wq_pod_type *pt = &wq_pod_types[WQ_AFFN_NUMA];
	u32 hash = wqattrs_hash(attrs);
	struct worker_pool *pool;
	int pod, node = NUMA_NO_NODE;

	lockdep_assert_held(&wq_pool_mutex);

	/* do we already have a matching pool? */
	hash_for_each_possible(unbound_pool_hash, pool, hash_node, hash) {
		if (wqattrs_equal(pool->attrs, attrs)) {
			pool->refcnt++;
			return pool;
		}
	}

	/* If __pod_cpumask is contained inside a NUMA pod, that's our node */
	for (pod = 0; pod < pt->nr_pods; pod++) {
		if (cpumask_subset(attrs->__pod_cpumask, pt->pod_cpus[pod])) {
			node = pt->pod_node[pod];
			break;
		}
	}

	/* nope, create a new one */
	pool = kzalloc_node(sizeof(*pool), GFP_KERNEL, node);
	if (!pool || init_worker_pool(pool) < 0)
		goto fail;

	pool->node = node;
	copy_workqueue_attrs(pool->attrs, attrs);
	wqattrs_clear_for_pool(pool->attrs);

	if (worker_pool_assign_id(pool) < 0)
		goto fail;

	/* create and start the initial worker */
	if (wq_online && !create_worker(pool))
		goto fail;

	/* install */
	hash_add(unbound_pool_hash, &pool->hash_node, hash);

	return pool;
fail:
	if (pool)
		put_unbound_pool(pool);
	return NULL;
}

static void rcu_free_pwq(struct rcu_head *rcu)
{
	kmem_cache_free(pwq_cache,
			container_of(rcu, struct pool_workqueue, rcu));
}

/*
 * Scheduled on pwq_release_worker by put_pwq() when an unbound pwq hits zero
 * refcnt and needs to be destroyed.
 */
static void pwq_release_workfn(struct kthread_work *work)
{
	struct pool_workqueue *pwq = container_of(work, struct pool_workqueue,
						  release_work);
	struct workqueue_struct *wq = pwq->wq;
	struct worker_pool *pool = pwq->pool;
	bool is_last = false;

	/*
	 * When @pwq is not linked, it doesn't hold any reference to the
	 * @wq, and @wq is invalid to access.
	 */
	if (!list_empty(&pwq->pwqs_node)) {
		mutex_lock(&wq->mutex);
		list_del_rcu(&pwq->pwqs_node);
		is_last = list_empty(&wq->pwqs);
		mutex_unlock(&wq->mutex);
	}

	if (wq->flags & WQ_UNBOUND) {
		mutex_lock(&wq_pool_mutex);
		put_unbound_pool(pool);
		mutex_unlock(&wq_pool_mutex);
	}

	call_rcu(&pwq->rcu, rcu_free_pwq);

	/*
	 * If we're the last pwq going away, @wq is already dead and no one
	 * is gonna access it anymore.  Schedule RCU free.
	 */
	if (is_last) {
		wq_unregister_lockdep(wq);
		call_rcu(&wq->rcu, rcu_free_wq);
	}
}

/**
 * pwq_adjust_max_active - update a pwq's max_active to the current setting
 * @pwq: target pool_workqueue
 *
 * If @pwq isn't freezing, set @pwq->max_active to the associated
 * workqueue's saved_max_active and activate inactive work items
 * accordingly.  If @pwq is freezing, clear @pwq->max_active to zero.
 */
static void pwq_adjust_max_active(struct pool_workqueue *pwq)
{
	struct workqueue_struct *wq = pwq->wq;
	bool freezable = wq->flags & WQ_FREEZABLE;
	unsigned long flags;

	/* for @wq->saved_max_active */
	lockdep_assert_held(&wq->mutex);

	/* fast exit for non-freezable wqs */
	if (!freezable && pwq->max_active == wq->saved_max_active)
		return;

	/* this function can be called during early boot w/ irq disabled */
	raw_spin_lock_irqsave(&pwq->pool->lock, flags);

	/*
	 * During [un]freezing, the caller is responsible for ensuring that
	 * this function is called at least once after @workqueue_freezing
	 * is updated and visible.
	 */
	if (!freezable || !workqueue_freezing) {
		pwq->max_active = wq->saved_max_active;

		while (!list_empty(&pwq->inactive_works) &&
		       pwq->nr_active < pwq->max_active)
			pwq_activate_first_inactive(pwq);

		kick_pool(pwq->pool);
	} else {
		pwq->max_active = 0;
	}

	raw_spin_unlock_irqrestore(&pwq->pool->lock, flags);
}

/* initialize newly allocated @pwq which is associated with @wq and @pool */
static void init_pwq(struct pool_workqueue *pwq, struct workqueue_struct *wq,
		     struct worker_pool *pool)
{
	BUG_ON((unsigned long)pwq & WORK_STRUCT_FLAG_MASK);

	memset(pwq, 0, sizeof(*pwq));

	pwq->pool = pool;
	pwq->wq = wq;
	pwq->flush_color = -1;
	pwq->refcnt = 1;
	INIT_LIST_HEAD(&pwq->inactive_works);
	INIT_LIST_HEAD(&pwq->pwqs_node);
	INIT_LIST_HEAD(&pwq->mayday_node);
	kthread_init_work(&pwq->release_work, pwq_release_workfn);
}

/* sync @pwq with the current state of its associated wq and link it */
static void link_pwq(struct pool_workqueue *pwq)
{
	struct workqueue_struct *wq = pwq->wq;

	lockdep_assert_held(&wq->mutex);

	/* may be called multiple times, ignore if already linked */
	if (!list_empty(&pwq->pwqs_node))
		return;

	/* set the matching work_color */
	pwq->work_color = wq->work_color;

	/* sync max_active to the current setting */
	pwq_adjust_max_active(pwq);

	/* link in @pwq */
	list_add_rcu(&pwq->pwqs_node, &wq->pwqs);
}

/* obtain a pool matching @attr and create a pwq associating the pool and @wq */
static struct pool_workqueue *alloc_unbound_pwq(struct workqueue_struct *wq,
					const struct workqueue_attrs *attrs)
{
	struct worker_pool *pool;
	struct pool_workqueue *pwq;

	lockdep_assert_held(&wq_pool_mutex);

	pool = get_unbound_pool(attrs);
	if (!pool)
		return NULL;

	pwq = kmem_cache_alloc_node(pwq_cache, GFP_KERNEL, pool->node);
	if (!pwq) {
		put_unbound_pool(pool);
		return NULL;
	}

	init_pwq(pwq, wq, pool);
	return pwq;
}

/**
 * wq_calc_pod_cpumask - calculate a wq_attrs' cpumask for a pod
 * @attrs: the wq_attrs of the default pwq of the target workqueue
 * @cpu: the target CPU
 * @cpu_going_down: if >= 0, the CPU to consider as offline
 *
 * Calculate the cpumask a workqueue with @attrs should use on @pod. If
 * @cpu_going_down is >= 0, that cpu is considered offline during calculation.
 * The result is stored in @attrs->__pod_cpumask.
 *
 * If pod affinity is not enabled, @attrs->cpumask is always used. If enabled
 * and @pod has online CPUs requested by @attrs, the returned cpumask is the
 * intersection of the possible CPUs of @pod and @attrs->cpumask.
 *
 * The caller is responsible for ensuring that the cpumask of @pod stays stable.
 */
static void wq_calc_pod_cpumask(struct workqueue_attrs *attrs, int cpu,
				int cpu_going_down)
{
	const struct wq_pod_type *pt = wqattrs_pod_type(attrs);
	int pod = pt->cpu_pod[cpu];

	/* does @pod have any online CPUs @attrs wants? */
	cpumask_and(attrs->__pod_cpumask, pt->pod_cpus[pod], attrs->cpumask);
	cpumask_and(attrs->__pod_cpumask, attrs->__pod_cpumask, cpu_online_mask);
	if (cpu_going_down >= 0)
		cpumask_clear_cpu(cpu_going_down, attrs->__pod_cpumask);

	if (cpumask_empty(attrs->__pod_cpumask)) {
		cpumask_copy(attrs->__pod_cpumask, attrs->cpumask);
		return;
	}

	/* yeap, return possible CPUs in @pod that @attrs wants */
	cpumask_and(attrs->__pod_cpumask, attrs->cpumask, pt->pod_cpus[pod]);

	if (cpumask_empty(attrs->__pod_cpumask))
		pr_warn_once("WARNING: workqueue cpumask: online intersect > "
				"possible intersect\n");
}

/* install @pwq into @wq's cpu_pwq and return the old pwq */
static struct pool_workqueue *install_unbound_pwq(struct workqueue_struct *wq,
					int cpu, struct pool_workqueue *pwq)
{
	struct pool_workqueue *old_pwq;

	lockdep_assert_held(&wq_pool_mutex);
	lockdep_assert_held(&wq->mutex);

	/* link_pwq() can handle duplicate calls */
	link_pwq(pwq);

	old_pwq = rcu_access_pointer(*per_cpu_ptr(wq->cpu_pwq, cpu));
	rcu_assign_pointer(*per_cpu_ptr(wq->cpu_pwq, cpu), pwq);
	return old_pwq;
}

/* context to store the prepared attrs & pwqs before applying */
struct apply_wqattrs_ctx {
	struct workqueue_struct	*wq;		/* target workqueue */
	struct workqueue_attrs	*attrs;		/* attrs to apply */
	struct list_head	list;		/* queued for batching commit */
	struct pool_workqueue	*dfl_pwq;
	struct pool_workqueue	*pwq_tbl[];
};

/* free the resources after success or abort */
static void apply_wqattrs_cleanup(struct apply_wqattrs_ctx *ctx)
{
	if (ctx) {
		int cpu;

		for_each_possible_cpu(cpu)
			put_pwq_unlocked(ctx->pwq_tbl[cpu]);
		put_pwq_unlocked(ctx->dfl_pwq);

		free_workqueue_attrs(ctx->attrs);

		kfree(ctx);
	}
}

/* allocate the attrs and pwqs for later installation */
static struct apply_wqattrs_ctx *
apply_wqattrs_prepare(struct workqueue_struct *wq,
		      const struct workqueue_attrs *attrs,
		      const cpumask_var_t unbound_cpumask)
{
	struct apply_wqattrs_ctx *ctx;
	struct workqueue_attrs *new_attrs;
	int cpu;

	lockdep_assert_held(&wq_pool_mutex);

	if (WARN_ON(attrs->affn_scope < 0 ||
		    attrs->affn_scope >= WQ_AFFN_NR_TYPES))
		return ERR_PTR(-EINVAL);

	ctx = kzalloc(struct_size(ctx, pwq_tbl, nr_cpu_ids), GFP_KERNEL);

	new_attrs = alloc_workqueue_attrs();
	if (!ctx || !new_attrs)
		goto out_free;

	/*
	 * If something goes wrong during CPU up/down, we'll fall back to
	 * the default pwq covering whole @attrs->cpumask.  Always create
	 * it even if we don't use it immediately.
	 */
	copy_workqueue_attrs(new_attrs, attrs);
	wqattrs_actualize_cpumask(new_attrs, unbound_cpumask);
	cpumask_copy(new_attrs->__pod_cpumask, new_attrs->cpumask);
	ctx->dfl_pwq = alloc_unbound_pwq(wq, new_attrs);
	if (!ctx->dfl_pwq)
		goto out_free;

	for_each_possible_cpu(cpu) {
		if (new_attrs->ordered) {
			ctx->dfl_pwq->refcnt++;
			ctx->pwq_tbl[cpu] = ctx->dfl_pwq;
		} else {
			wq_calc_pod_cpumask(new_attrs, cpu, -1);
			ctx->pwq_tbl[cpu] = alloc_unbound_pwq(wq, new_attrs);
			if (!ctx->pwq_tbl[cpu])
				goto out_free;
		}
	}

	/* save the user configured attrs and sanitize it. */
	copy_workqueue_attrs(new_attrs, attrs);
	cpumask_and(new_attrs->cpumask, new_attrs->cpumask, cpu_possible_mask);
	cpumask_copy(new_attrs->__pod_cpumask, new_attrs->cpumask);
	ctx->attrs = new_attrs;

	ctx->wq = wq;
	return ctx;

out_free:
	free_workqueue_attrs(new_attrs);
	apply_wqattrs_cleanup(ctx);
	return ERR_PTR(-ENOMEM);
}

/* set attrs and install prepared pwqs, @ctx points to old pwqs on return */
static void apply_wqattrs_commit(struct apply_wqattrs_ctx *ctx)
{
	int cpu;

	/* all pwqs have been created successfully, let's install'em */
	mutex_lock(&ctx->wq->mutex);

	copy_workqueue_attrs(ctx->wq->unbound_attrs, ctx->attrs);

	/* save the previous pwq and install the new one */
	for_each_possible_cpu(cpu)
		ctx->pwq_tbl[cpu] = install_unbound_pwq(ctx->wq, cpu,
							ctx->pwq_tbl[cpu]);

	/* @dfl_pwq might not have been used, ensure it's linked */
	link_pwq(ctx->dfl_pwq);
	swap(ctx->wq->dfl_pwq, ctx->dfl_pwq);

	mutex_unlock(&ctx->wq->mutex);
}

static void apply_wqattrs_lock(void)
{
	/* CPUs should stay stable across pwq creations and installations */
	cpus_read_lock();
	mutex_lock(&wq_pool_mutex);
}

static void apply_wqattrs_unlock(void)
{
	mutex_unlock(&wq_pool_mutex);
	cpus_read_unlock();
}

static int apply_workqueue_attrs_locked(struct workqueue_struct *wq,
					const struct workqueue_attrs *attrs)
{
	struct apply_wqattrs_ctx *ctx;

	/* only unbound workqueues can change attributes */
	if (WARN_ON(!(wq->flags & WQ_UNBOUND)))
		return -EINVAL;

	/* creating multiple pwqs breaks ordering guarantee */
	if (!list_empty(&wq->pwqs)) {
		if (WARN_ON(wq->flags & __WQ_ORDERED_EXPLICIT))
			return -EINVAL;

		wq->flags &= ~__WQ_ORDERED;
	}

	ctx = apply_wqattrs_prepare(wq, attrs, wq_unbound_cpumask);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	/* the ctx has been prepared successfully, let's commit it */
	apply_wqattrs_commit(ctx);
	apply_wqattrs_cleanup(ctx);

	return 0;
}

/**
 * apply_workqueue_attrs - apply new workqueue_attrs to an unbound workqueue
 * @wq: the target workqueue
 * @attrs: the workqueue_attrs to apply, allocated with alloc_workqueue_attrs()
 *
 * Apply @attrs to an unbound workqueue @wq. Unless disabled, this function maps
 * a separate pwq to each CPU pod with possibles CPUs in @attrs->cpumask so that
 * work items are affine to the pod it was issued on. Older pwqs are released as
 * in-flight work items finish. Note that a work item which repeatedly requeues
 * itself back-to-back will stay on its current pwq.
 *
 * Performs GFP_KERNEL allocations.
 *
 * Assumes caller has CPU hotplug read exclusion, i.e. cpus_read_lock().
 *
 * Return: 0 on success and -errno on failure.
 */
int apply_workqueue_attrs(struct workqueue_struct *wq,
			  const struct workqueue_attrs *attrs)
{
	int ret;

	lockdep_assert_cpus_held();

	mutex_lock(&wq_pool_mutex);
	ret = apply_workqueue_attrs_locked(wq, attrs);
	mutex_unlock(&wq_pool_mutex);

	return ret;
}

/**
 * wq_update_pod - update pod affinity of a wq for CPU hot[un]plug
 * @wq: the target workqueue
 * @cpu: the CPU to update pool association for
 * @hotplug_cpu: the CPU coming up or going down
 * @online: whether @cpu is coming up or going down
 *
 * This function is to be called from %CPU_DOWN_PREPARE, %CPU_ONLINE and
 * %CPU_DOWN_FAILED.  @cpu is being hot[un]plugged, update pod affinity of
 * @wq accordingly.
 *
 *
 * If pod affinity can't be adjusted due to memory allocation failure, it falls
 * back to @wq->dfl_pwq which may not be optimal but is always correct.
 *
 * Note that when the last allowed CPU of a pod goes offline for a workqueue
 * with a cpumask spanning multiple pods, the workers which were already
 * executing the work items for the workqueue will lose their CPU affinity and
 * may execute on any CPU. This is similar to how per-cpu workqueues behave on
 * CPU_DOWN. If a workqueue user wants strict affinity, it's the user's
 * responsibility to flush the work item from CPU_DOWN_PREPARE.
 */
static void wq_update_pod(struct workqueue_struct *wq, int cpu,
			  int hotplug_cpu, bool online)
{
	int off_cpu = online ? -1 : hotplug_cpu;
	struct pool_workqueue *old_pwq = NULL, *pwq;
	struct workqueue_attrs *target_attrs;

	lockdep_assert_held(&wq_pool_mutex);

	if (!(wq->flags & WQ_UNBOUND) || wq->unbound_attrs->ordered)
		return;

	/*
	 * We don't wanna alloc/free wq_attrs for each wq for each CPU.
	 * Let's use a preallocated one.  The following buf is protected by
	 * CPU hotplug exclusion.
	 */
	target_attrs = wq_update_pod_attrs_buf;

	copy_workqueue_attrs(target_attrs, wq->unbound_attrs);
	wqattrs_actualize_cpumask(target_attrs, wq_unbound_cpumask);

	/* nothing to do if the target cpumask matches the current pwq */
	wq_calc_pod_cpumask(target_attrs, cpu, off_cpu);
	pwq = rcu_dereference_protected(*per_cpu_ptr(wq->cpu_pwq, cpu),
					lockdep_is_held(&wq_pool_mutex));
	if (wqattrs_equal(target_attrs, pwq->pool->attrs))
		return;

	/* create a new pwq */
	pwq = alloc_unbound_pwq(wq, target_attrs);
	if (!pwq) {
		pr_warn("workqueue: allocation failed while updating CPU pod affinity of \"%s\"\n",
			wq->name);
		goto use_dfl_pwq;
	}

	/* Install the new pwq. */
	mutex_lock(&wq->mutex);
	old_pwq = install_unbound_pwq(wq, cpu, pwq);
	goto out_unlock;

use_dfl_pwq:
	mutex_lock(&wq->mutex);
	raw_spin_lock_irq(&wq->dfl_pwq->pool->lock);
	get_pwq(wq->dfl_pwq);
	raw_spin_unlock_irq(&wq->dfl_pwq->pool->lock);
	old_pwq = install_unbound_pwq(wq, cpu, wq->dfl_pwq);
out_unlock:
	mutex_unlock(&wq->mutex);
	put_pwq_unlocked(old_pwq);
}

static int alloc_and_link_pwqs(struct workqueue_struct *wq)
{
	bool highpri = wq->flags & WQ_HIGHPRI;
	int cpu, ret;

	wq->cpu_pwq = alloc_percpu(struct pool_workqueue *);
	if (!wq->cpu_pwq)
		goto enomem;

	if (!(wq->flags & WQ_UNBOUND)) {
		for_each_possible_cpu(cpu) {
			struct pool_workqueue **pwq_p =
				per_cpu_ptr(wq->cpu_pwq, cpu);
			struct worker_pool *pool =
				&(per_cpu_ptr(cpu_worker_pools, cpu)[highpri]);

			*pwq_p = kmem_cache_alloc_node(pwq_cache, GFP_KERNEL,
						       pool->node);
			if (!*pwq_p)
				goto enomem;

			init_pwq(*pwq_p, wq, pool);

			mutex_lock(&wq->mutex);
			link_pwq(*pwq_p);
			mutex_unlock(&wq->mutex);
		}
		return 0;
	}

	cpus_read_lock();
	if (wq->flags & __WQ_ORDERED) {
		ret = apply_workqueue_attrs(wq, ordered_wq_attrs[highpri]);
		/* there should only be single pwq for ordering guarantee */
		WARN(!ret && (wq->pwqs.next != &wq->dfl_pwq->pwqs_node ||
			      wq->pwqs.prev != &wq->dfl_pwq->pwqs_node),
		     "ordering guarantee broken for workqueue %s\n", wq->name);
	} else {
		ret = apply_workqueue_attrs(wq, unbound_std_wq_attrs[highpri]);
	}
	cpus_read_unlock();

	/* for unbound pwq, flush the pwq_release_worker ensures that the
	 * pwq_release_workfn() completes before calling kfree(wq).
	 */
	if (ret)
		kthread_flush_worker(pwq_release_worker);

	return ret;

enomem:
	if (wq->cpu_pwq) {
		for_each_possible_cpu(cpu) {
			struct pool_workqueue *pwq = *per_cpu_ptr(wq->cpu_pwq, cpu);

			if (pwq)
				kmem_cache_free(pwq_cache, pwq);
		}
		free_percpu(wq->cpu_pwq);
		wq->cpu_pwq = NULL;
	}
	return -ENOMEM;
}

static int wq_clamp_max_active(int max_active, unsigned int flags,
			       const char *name)
{
	if (max_active < 1 || max_active > WQ_MAX_ACTIVE)
		pr_warn("workqueue: max_active %d requested for %s is out of range, clamping between %d and %d\n",
			max_active, name, 1, WQ_MAX_ACTIVE);

	return clamp_val(max_active, 1, WQ_MAX_ACTIVE);
}

/*
 * Workqueues which may be used during memory reclaim should have a rescuer
 * to guarantee forward progress.
 */
static int init_rescuer(struct workqueue_struct *wq)
{
	struct worker *rescuer;
	int ret;

	if (!(wq->flags & WQ_MEM_RECLAIM))
		return 0;

	rescuer = alloc_worker(NUMA_NO_NODE);
	if (!rescuer) {
		pr_err("workqueue: Failed to allocate a rescuer for wq \"%s\"\n",
		       wq->name);
		return -ENOMEM;
	}

	rescuer->rescue_wq = wq;
	rescuer->task = kthread_create(rescuer_thread, rescuer, "kworker/R-%s", wq->name);
	if (IS_ERR(rescuer->task)) {
		ret = PTR_ERR(rescuer->task);
		pr_err("workqueue: Failed to create a rescuer kthread for wq \"%s\": %pe",
		       wq->name, ERR_PTR(ret));
		kfree(rescuer);
		return ret;
	}

	wq->rescuer = rescuer;
	kthread_bind_mask(rescuer->task, cpu_possible_mask);
	wake_up_process(rescuer->task);

	return 0;
}

__printf(1, 4)
struct workqueue_struct *alloc_workqueue(const char *fmt,
					 unsigned int flags,
					 int max_active, ...)
{
	va_list args;
	struct workqueue_struct *wq;
	struct pool_workqueue *pwq;

	/*
	 * Unbound && max_active == 1 used to imply ordered, which is no longer
	 * the case on many machines due to per-pod pools. While
	 * alloc_ordered_workqueue() is the right way to create an ordered
	 * workqueue, keep the previous behavior to avoid subtle breakages.
	 */
	if ((flags & WQ_UNBOUND) && max_active == 1)
		flags |= __WQ_ORDERED;

	/* see the comment above the definition of WQ_POWER_EFFICIENT */
	if ((flags & WQ_POWER_EFFICIENT) && wq_power_efficient)
		flags |= WQ_UNBOUND;

	/* allocate wq and format name */
	wq = kzalloc(sizeof(*wq), GFP_KERNEL);
	if (!wq)
		return NULL;

	if (flags & WQ_UNBOUND) {
		wq->unbound_attrs = alloc_workqueue_attrs();
		if (!wq->unbound_attrs)
			goto err_free_wq;
	}

	va_start(args, max_active);
	vsnprintf(wq->name, sizeof(wq->name), fmt, args);
	va_end(args);

	max_active = max_active ?: WQ_DFL_ACTIVE;
	max_active = wq_clamp_max_active(max_active, flags, wq->name);

	/* init wq */
	wq->flags = flags;
	wq->saved_max_active = max_active;
	mutex_init(&wq->mutex);
	atomic_set(&wq->nr_pwqs_to_flush, 0);
	INIT_LIST_HEAD(&wq->pwqs);
	INIT_LIST_HEAD(&wq->flusher_queue);
	INIT_LIST_HEAD(&wq->flusher_overflow);
	INIT_LIST_HEAD(&wq->maydays);

	wq_init_lockdep(wq);
	INIT_LIST_HEAD(&wq->list);

	if (alloc_and_link_pwqs(wq) < 0)
		goto err_unreg_lockdep;

	if (wq_online && init_rescuer(wq) < 0)
		goto err_destroy;

	if ((wq->flags & WQ_SYSFS) && workqueue_sysfs_register(wq))
		goto err_destroy;

	/*
	 * wq_pool_mutex protects global freeze state and workqueues list.
	 * Grab it, adjust max_active and add the new @wq to workqueues
	 * list.
	 */
	mutex_lock(&wq_pool_mutex);

	mutex_lock(&wq->mutex);
	for_each_pwq(pwq, wq)
		pwq_adjust_max_active(pwq);
	mutex_unlock(&wq->mutex);

	list_add_tail_rcu(&wq->list, &workqueues);

	mutex_unlock(&wq_pool_mutex);

	return wq;

err_unreg_lockdep:
	wq_unregister_lockdep(wq);
	wq_free_lockdep(wq);
err_free_wq:
	free_workqueue_attrs(wq->unbound_attrs);
	kfree(wq);
	return NULL;
err_destroy:
	destroy_workqueue(wq);
	return NULL;
}
EXPORT_SYMBOL_GPL(alloc_workqueue);

static bool pwq_busy(struct pool_workqueue *pwq)
{
	int i;

	for (i = 0; i < WORK_NR_COLORS; i++)
		if (pwq->nr_in_flight[i])
			return true;

	if ((pwq != pwq->wq->dfl_pwq) && (pwq->refcnt > 1))
		return true;
	if (pwq->nr_active || !list_empty(&pwq->inactive_works))
		return true;

	return false;
}

/**
 * destroy_workqueue - safely terminate a workqueue
 * @wq: target workqueue
 *
 * Safely destroy a workqueue. All work currently pending will be done first.
 */
void destroy_workqueue(struct workqueue_struct *wq)
{
	struct pool_workqueue *pwq;
	int cpu;

	/*
	 * Remove it from sysfs first so that sanity check failure doesn't
	 * lead to sysfs name conflicts.
	 */
	workqueue_sysfs_unregister(wq);

	/* mark the workqueue destruction is in progress */
	mutex_lock(&wq->mutex);
	wq->flags |= __WQ_DESTROYING;
	mutex_unlock(&wq->mutex);

	/* drain it before proceeding with destruction */
	drain_workqueue(wq);

	/* kill rescuer, if sanity checks fail, leave it w/o rescuer */
	if (wq->rescuer) {
		struct worker *rescuer = wq->rescuer;

		/* this prevents new queueing */
		raw_spin_lock_irq(&wq_mayday_lock);
		wq->rescuer = NULL;
		raw_spin_unlock_irq(&wq_mayday_lock);

		/* rescuer will empty maydays list before exiting */
		kthread_stop(rescuer->task);
		kfree(rescuer);
	}

	/*
	 * Sanity checks - grab all the locks so that we wait for all
	 * in-flight operations which may do put_pwq().
	 */
	mutex_lock(&wq_pool_mutex);
	mutex_lock(&wq->mutex);
	for_each_pwq(pwq, wq) {
		raw_spin_lock_irq(&pwq->pool->lock);
		if (WARN_ON(pwq_busy(pwq))) {
			pr_warn("%s: %s has the following busy pwq\n",
				__func__, wq->name);
			show_pwq(pwq);
			raw_spin_unlock_irq(&pwq->pool->lock);
			mutex_unlock(&wq->mutex);
			mutex_unlock(&wq_pool_mutex);
			show_one_workqueue(wq);
			return;
		}
		raw_spin_unlock_irq(&pwq->pool->lock);
	}
	mutex_unlock(&wq->mutex);

	/*
	 * wq list is used to freeze wq, remove from list after
	 * flushing is complete in case freeze races us.
	 */
	list_del_rcu(&wq->list);
	mutex_unlock(&wq_pool_mutex);

	/*
	 * We're the sole accessor of @wq. Directly access cpu_pwq and dfl_pwq
	 * to put the base refs. @wq will be auto-destroyed from the last
	 * pwq_put. RCU read lock prevents @wq from going away from under us.
	 */
	rcu_read_lock();

	for_each_possible_cpu(cpu) {
		pwq = rcu_access_pointer(*per_cpu_ptr(wq->cpu_pwq, cpu));
		RCU_INIT_POINTER(*per_cpu_ptr(wq->cpu_pwq, cpu), NULL);
		put_pwq_unlocked(pwq);
	}

	put_pwq_unlocked(wq->dfl_pwq);
	wq->dfl_pwq = NULL;

	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(destroy_workqueue);

/**
 * workqueue_set_max_active - adjust max_active of a workqueue
 * @wq: target workqueue
 * @max_active: new max_active value.
 *
 * Set max_active of @wq to @max_active.
 *
 * CONTEXT:
 * Don't call from IRQ context.
 */
void workqueue_set_max_active(struct workqueue_struct *wq, int max_active)
{
	struct pool_workqueue *pwq;

	/* disallow meddling with max_active for ordered workqueues */
	if (WARN_ON(wq->flags & __WQ_ORDERED_EXPLICIT))
		return;

	max_active = wq_clamp_max_active(max_active, wq->flags, wq->name);

	mutex_lock(&wq->mutex);

	wq->flags &= ~__WQ_ORDERED;
	wq->saved_max_active = max_active;

	for_each_pwq(pwq, wq)
		pwq_adjust_max_active(pwq);

	mutex_unlock(&wq->mutex);
}
EXPORT_SYMBOL_GPL(workqueue_set_max_active);

/**
 * current_work - retrieve %current task's work struct
 *
 * Determine if %current task is a workqueue worker and what it's working on.
 * Useful to find out the context that the %current task is running in.
 *
 * Return: work struct if %current task is a workqueue worker, %NULL otherwise.
 */
struct work_struct *current_work(void)
{
	struct worker *worker = current_wq_worker();

	return worker ? worker->current_work : NULL;
}
EXPORT_SYMBOL(current_work);

/**
 * current_is_workqueue_rescuer - is %current workqueue rescuer?
 *
 * Determine whether %current is a workqueue rescuer.  Can be used from
 * work functions to determine whether it's being run off the rescuer task.
 *
 * Return: %true if %current is a workqueue rescuer. %false otherwise.
 */
bool current_is_workqueue_rescuer(void)
{
	struct worker *worker = current_wq_worker();

	return worker && worker->rescue_wq;
}

/**
 * workqueue_congested - test whether a workqueue is congested
 * @cpu: CPU in question
 * @wq: target workqueue
 *
 * Test whether @wq's cpu workqueue for @cpu is congested.  There is
 * no synchronization around this function and the test result is
 * unreliable and only useful as advisory hints or for debugging.
 *
 * If @cpu is WORK_CPU_UNBOUND, the test is performed on the local CPU.
 *
 * With the exception of ordered workqueues, all workqueues have per-cpu
 * pool_workqueues, each with its own congested state. A workqueue being
 * congested on one CPU doesn't mean that the workqueue is contested on any
 * other CPUs.
 *
 * Return:
 * %true if congested, %false otherwise.
 */
bool workqueue_congested(int cpu, struct workqueue_struct *wq)
{
	struct pool_workqueue *pwq;
	bool ret;

	rcu_read_lock();
	preempt_disable();

	if (cpu == WORK_CPU_UNBOUND)
		cpu = smp_processor_id();

	pwq = *per_cpu_ptr(wq->cpu_pwq, cpu);
	ret = !list_empty(&pwq->inactive_works);

	preempt_enable();
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(workqueue_congested);

/**
 * work_busy - test whether a work is currently pending or running
 * @work: the work to be tested
 *
 * Test whether @work is currently pending or running.  There is no
 * synchronization around this function and the test result is
 * unreliable and only useful as advisory hints or for debugging.
 *
 * Return:
 * OR'd bitmask of WORK_BUSY_* bits.
 */
unsigned int work_busy(struct work_struct *work)
{
	struct worker_pool *pool;
	unsigned long flags;
	unsigned int ret = 0;

	if (work_pending(work))
		ret |= WORK_BUSY_PENDING;

	rcu_read_lock();
	pool = get_work_pool(work);
	if (pool) {
		raw_spin_lock_irqsave(&pool->lock, flags);
		if (find_worker_executing_work(pool, work))
			ret |= WORK_BUSY_RUNNING;
		raw_spin_unlock_irqrestore(&pool->lock, flags);
	}
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(work_busy);

/**
 * set_worker_desc - set description for the current work item
 * @fmt: printf-style format string
 * @...: arguments for the format string
 *
 * This function can be called by a running work function to describe what
 * the work item is about.  If the worker task gets dumped, this
 * information will be printed out together to help debugging.  The
 * description can be at most WORKER_DESC_LEN including the trailing '\0'.
 */
void set_worker_desc(const char *fmt, ...)
{
	struct worker *worker = current_wq_worker();
	va_list args;

	if (worker) {
		va_start(args, fmt);
		vsnprintf(worker->desc, sizeof(worker->desc), fmt, args);
		va_end(args);
	}
}
EXPORT_SYMBOL_GPL(set_worker_desc);

/**
 * print_worker_info - print out worker information and description
 * @log_lvl: the log level to use when printing
 * @task: target task
 *
 * If @task is a worker and currently executing a work item, print out the
 * name of the workqueue being serviced and worker description set with
 * set_worker_desc() by the currently executing work item.
 *
 * This function can be safely called on any task as long as the
 * task_struct itself is accessible.  While safe, this function isn't
 * synchronized and may print out mixups or garbages of limited length.
 */
void print_worker_info(const char *log_lvl, struct task_struct *task)
{
	work_func_t *fn = NULL;
	char name[WQ_NAME_LEN] = { };
	char desc[WORKER_DESC_LEN] = { };
	struct pool_workqueue *pwq = NULL;
	struct workqueue_struct *wq = NULL;
	struct worker *worker;

	if (!(task->flags & PF_WQ_WORKER))
		return;

	/*
	 * This function is called without any synchronization and @task
	 * could be in any state.  Be careful with dereferences.
	 */
	worker = kthread_probe_data(task);

	/*
	 * Carefully copy the associated workqueue's workfn, name and desc.
	 * Keep the original last '\0' in case the original is garbage.
	 */
	copy_from_kernel_nofault(&fn, &worker->current_func, sizeof(fn));
	copy_from_kernel_nofault(&pwq, &worker->current_pwq, sizeof(pwq));
	copy_from_kernel_nofault(&wq, &pwq->wq, sizeof(wq));
	copy_from_kernel_nofault(name, wq->name, sizeof(name) - 1);
	copy_from_kernel_nofault(desc, worker->desc, sizeof(desc) - 1);

	if (fn || name[0] || desc[0]) {
		printk("%sWorkqueue: %s %ps", log_lvl, name, fn);
		if (strcmp(name, desc))
			pr_cont(" (%s)", desc);
		pr_cont("\n");
	}
}

static void pr_cont_pool_info(struct worker_pool *pool)
{
	pr_cont(" cpus=%*pbl", nr_cpumask_bits, pool->attrs->cpumask);
	if (pool->node != NUMA_NO_NODE)
		pr_cont(" node=%d", pool->node);
	pr_cont(" flags=0x%x nice=%d", pool->flags, pool->attrs->nice);
}

struct pr_cont_work_struct {
	bool comma;
	work_func_t func;
	long ctr;
};

static void pr_cont_work_flush(bool comma, work_func_t func, struct pr_cont_work_struct *pcwsp)
{
	if (!pcwsp->ctr)
		goto out_record;
	if (func == pcwsp->func) {
		pcwsp->ctr++;
		return;
	}
	if (pcwsp->ctr == 1)
		pr_cont("%s %ps", pcwsp->comma ? "," : "", pcwsp->func);
	else
		pr_cont("%s %ld*%ps", pcwsp->comma ? "," : "", pcwsp->ctr, pcwsp->func);
	pcwsp->ctr = 0;
out_record:
	if ((long)func == -1L)
		return;
	pcwsp->comma = comma;
	pcwsp->func = func;
	pcwsp->ctr = 1;
}

static void pr_cont_work(bool comma, struct work_struct *work, struct pr_cont_work_struct *pcwsp)
{
	if (work->func == wq_barrier_func) {
		struct wq_barrier *barr;

		barr = container_of(work, struct wq_barrier, work);

		pr_cont_work_flush(comma, (work_func_t)-1, pcwsp);
		pr_cont("%s BAR(%d)", comma ? "," : "",
			task_pid_nr(barr->task));
	} else {
		if (!comma)
			pr_cont_work_flush(comma, (work_func_t)-1, pcwsp);
		pr_cont_work_flush(comma, work->func, pcwsp);
	}
}

static void show_pwq(struct pool_workqueue *pwq)
{
	struct pr_cont_work_struct pcws = { .ctr = 0, };
	struct worker_pool *pool = pwq->pool;
	struct work_struct *work;
	struct worker *worker;
	bool has_in_flight = false, has_pending = false;
	int bkt;

	pr_info("  pwq %d:", pool->id);
	pr_cont_pool_info(pool);

	pr_cont(" active=%d/%d refcnt=%d%s\n",
		pwq->nr_active, pwq->max_active, pwq->refcnt,
		!list_empty(&pwq->mayday_node) ? " MAYDAY" : "");

	hash_for_each(pool->busy_hash, bkt, worker, hentry) {
		if (worker->current_pwq == pwq) {
			has_in_flight = true;
			break;
		}
	}
	if (has_in_flight) {
		bool comma = false;

		pr_info("    in-flight:");
		hash_for_each(pool->busy_hash, bkt, worker, hentry) {
			if (worker->current_pwq != pwq)
				continue;

			pr_cont("%s %d%s:%ps", comma ? "," : "",
				task_pid_nr(worker->task),
				worker->rescue_wq ? "(RESCUER)" : "",
				worker->current_func);
			list_for_each_entry(work, &worker->scheduled, entry)
				pr_cont_work(false, work, &pcws);
			pr_cont_work_flush(comma, (work_func_t)-1L, &pcws);
			comma = true;
		}
		pr_cont("\n");
	}

	list_for_each_entry(work, &pool->worklist, entry) {
		if (get_work_pwq(work) == pwq) {
			has_pending = true;
			break;
		}
	}
	if (has_pending) {
		bool comma = false;

		pr_info("    pending:");
		list_for_each_entry(work, &pool->worklist, entry) {
			if (get_work_pwq(work) != pwq)
				continue;

			pr_cont_work(comma, work, &pcws);
			comma = !(*work_data_bits(work) & WORK_STRUCT_LINKED);
		}
		pr_cont_work_flush(comma, (work_func_t)-1L, &pcws);
		pr_cont("\n");
	}

	if (!list_empty(&pwq->inactive_works)) {
		bool comma = false;

		pr_info("    inactive:");
		list_for_each_entry(work, &pwq->inactive_works, entry) {
			pr_cont_work(comma, work, &pcws);
			comma = !(*work_data_bits(work) & WORK_STRUCT_LINKED);
		}
		pr_cont_work_flush(comma, (work_func_t)-1L, &pcws);
		pr_cont("\n");
	}
}

/**
 * show_one_workqueue - dump state of specified workqueue
 * @wq: workqueue whose state will be printed
 */
void show_one_workqueue(struct workqueue_struct *wq)
{
	struct pool_workqueue *pwq;
	bool idle = true;
	unsigned long flags;

	for_each_pwq(pwq, wq) {
		if (pwq->nr_active || !list_empty(&pwq->inactive_works)) {
			idle = false;
			break;
		}
	}
	if (idle) /* Nothing to print for idle workqueue */
		return;

	pr_info("workqueue %s: flags=0x%x\n", wq->name, wq->flags);

	for_each_pwq(pwq, wq) {
		raw_spin_lock_irqsave(&pwq->pool->lock, flags);
		if (pwq->nr_active || !list_empty(&pwq->inactive_works)) {
			/*
			 * Defer printing to avoid deadlocks in console
			 * drivers that queue work while holding locks
			 * also taken in their write paths.
			 */
			printk_deferred_enter();
			show_pwq(pwq);
			printk_deferred_exit();
		}
		raw_spin_unlock_irqrestore(&pwq->pool->lock, flags);
		/*
		 * We could be printing a lot from atomic context, e.g.
		 * sysrq-t -> show_all_workqueues(). Avoid triggering
		 * hard lockup.
		 */
		touch_nmi_watchdog();
	}

}

/**
 * show_one_worker_pool - dump state of specified worker pool
 * @pool: worker pool whose state will be printed
 */
static void show_one_worker_pool(struct worker_pool *pool)
{
	struct worker *worker;
	bool first = true;
	unsigned long flags;
	unsigned long hung = 0;

	raw_spin_lock_irqsave(&pool->lock, flags);
	if (pool->nr_workers == pool->nr_idle)
		goto next_pool;

	/* How long the first pending work is waiting for a worker. */
	if (!list_empty(&pool->worklist))
		hung = jiffies_to_msecs(jiffies - pool->watchdog_ts) / 1000;

	/*
	 * Defer printing to avoid deadlocks in console drivers that
	 * queue work while holding locks also taken in their write
	 * paths.
	 */
	printk_deferred_enter();
	pr_info("pool %d:", pool->id);
	pr_cont_pool_info(pool);
	pr_cont(" hung=%lus workers=%d", hung, pool->nr_workers);
	if (pool->manager)
		pr_cont(" manager: %d",
			task_pid_nr(pool->manager->task));
	list_for_each_entry(worker, &pool->idle_list, entry) {
		pr_cont(" %s%d", first ? "idle: " : "",
			task_pid_nr(worker->task));
		first = false;
	}
	pr_cont("\n");
	printk_deferred_exit();
next_pool:
	raw_spin_unlock_irqrestore(&pool->lock, flags);
	/*
	 * We could be printing a lot from atomic context, e.g.
	 * sysrq-t -> show_all_workqueues(). Avoid triggering
	 * hard lockup.
	 */
	touch_nmi_watchdog();

}

/**
 * show_all_workqueues - dump workqueue state
 *
 * Called from a sysrq handler and prints out all busy workqueues and pools.
 */
void show_all_workqueues(void)
{
	struct workqueue_struct *wq;
	struct worker_pool *pool;
	int pi;

	rcu_read_lock();

	pr_info("Showing busy workqueues and worker pools:\n");

	list_for_each_entry_rcu(wq, &workqueues, list)
		show_one_workqueue(wq);

	for_each_pool(pool, pi)
		show_one_worker_pool(pool);

	rcu_read_unlock();
}

/**
 * show_freezable_workqueues - dump freezable workqueue state
 *
 * Called from try_to_freeze_tasks() and prints out all freezable workqueues
 * still busy.
 */
void show_freezable_workqueues(void)
{
	struct workqueue_struct *wq;

	rcu_read_lock();

	pr_info("Showing freezable workqueues that are still busy:\n");

	list_for_each_entry_rcu(wq, &workqueues, list) {
		if (!(wq->flags & WQ_FREEZABLE))
			continue;
		show_one_workqueue(wq);
	}

	rcu_read_unlock();
}

/* used to show worker information through /proc/PID/{comm,stat,status} */
void wq_worker_comm(char *buf, size_t size, struct task_struct *task)
{
	int off;

	/* always show the actual comm */
	off = strscpy(buf, task->comm, size);
	if (off < 0)
		return;

	/* stabilize PF_WQ_WORKER and worker pool association */
	mutex_lock(&wq_pool_attach_mutex);

	if (task->flags & PF_WQ_WORKER) {
		struct worker *worker = kthread_data(task);
		struct worker_pool *pool = worker->pool;

		if (pool) {
			raw_spin_lock_irq(&pool->lock);
			/*
			 * ->desc tracks information (wq name or
			 * set_worker_desc()) for the latest execution.  If
			 * current, prepend '+', otherwise '-'.
			 */
			if (worker->desc[0] != '\0') {
				if (worker->current_work)
					scnprintf(buf + off, size - off, "+%s",
						  worker->desc);
				else
					scnprintf(buf + off, size - off, "-%s",
						  worker->desc);
			}
			raw_spin_unlock_irq(&pool->lock);
		}
	}

	mutex_unlock(&wq_pool_attach_mutex);
}

#ifdef CONFIG_SMP

/*
 * CPU hotplug.
 *
 * There are two challenges in supporting CPU hotplug.  Firstly, there
 * are a lot of assumptions on strong associations among work, pwq and
 * pool which make migrating pending and scheduled works very
 * difficult to implement without impacting hot paths.  Secondly,
 * worker pools serve mix of short, long and very long running works making
 * blocked draining impractical.
 *
 * This is solved by allowing the pools to be disassociated from the CPU
 * running as an unbound one and allowing it to be reattached later if the
 * cpu comes back online.
 */

static void unbind_workers(int cpu)
{
	struct worker_pool *pool;
	struct worker *worker;

	for_each_cpu_worker_pool(pool, cpu) {
		mutex_lock(&wq_pool_attach_mutex);
		raw_spin_lock_irq(&pool->lock);

		/*
		 * We've blocked all attach/detach operations. Make all workers
		 * unbound and set DISASSOCIATED.  Before this, all workers
		 * must be on the cpu.  After this, they may become diasporas.
		 * And the preemption disabled section in their sched callbacks
		 * are guaranteed to see WORKER_UNBOUND since the code here
		 * is on the same cpu.
		 */
		for_each_pool_worker(worker, pool)
			worker->flags |= WORKER_UNBOUND;

		pool->flags |= POOL_DISASSOCIATED;

		/*
		 * The handling of nr_running in sched callbacks are disabled
		 * now.  Zap nr_running.  After this, nr_running stays zero and
		 * need_more_worker() and keep_working() are always true as
		 * long as the worklist is not empty.  This pool now behaves as
		 * an unbound (in terms of concurrency management) pool which
		 * are served by workers tied to the pool.
		 */
		pool->nr_running = 0;

		/*
		 * With concurrency management just turned off, a busy
		 * worker blocking could lead to lengthy stalls.  Kick off
		 * unbound chain execution of currently pending work items.
		 */
		kick_pool(pool);

		raw_spin_unlock_irq(&pool->lock);

		for_each_pool_worker(worker, pool)
			unbind_worker(worker);

		mutex_unlock(&wq_pool_attach_mutex);
	}
}

/**
 * rebind_workers - rebind all workers of a pool to the associated CPU
 * @pool: pool of interest
 *
 * @pool->cpu is coming online.  Rebind all workers to the CPU.
 */
static void rebind_workers(struct worker_pool *pool)
{
	struct worker *worker;

	lockdep_assert_held(&wq_pool_attach_mutex);

	/*
	 * Restore CPU affinity of all workers.  As all idle workers should
	 * be on the run-queue of the associated CPU before any local
	 * wake-ups for concurrency management happen, restore CPU affinity
	 * of all workers first and then clear UNBOUND.  As we're called
	 * from CPU_ONLINE, the following shouldn't fail.
	 */
	for_each_pool_worker(worker, pool) {
		kthread_set_per_cpu(worker->task, pool->cpu);
		WARN_ON_ONCE(set_cpus_allowed_ptr(worker->task,
						  pool_allowed_cpus(pool)) < 0);
	}

	raw_spin_lock_irq(&pool->lock);

	pool->flags &= ~POOL_DISASSOCIATED;

	for_each_pool_worker(worker, pool) {
		unsigned int worker_flags = worker->flags;

		/*
		 * We want to clear UNBOUND but can't directly call
		 * worker_clr_flags() or adjust nr_running.  Atomically
		 * replace UNBOUND with another NOT_RUNNING flag REBOUND.
		 * @worker will clear REBOUND using worker_clr_flags() when
		 * it initiates the next execution cycle thus restoring
		 * concurrency management.  Note that when or whether
		 * @worker clears REBOUND doesn't affect correctness.
		 *
		 * WRITE_ONCE() is necessary because @worker->flags may be
		 * tested without holding any lock in
		 * wq_worker_running().  Without it, NOT_RUNNING test may
		 * fail incorrectly leading to premature concurrency
		 * management operations.
		 */
		WARN_ON_ONCE(!(worker_flags & WORKER_UNBOUND));
		worker_flags |= WORKER_REBOUND;
		worker_flags &= ~WORKER_UNBOUND;
		WRITE_ONCE(worker->flags, worker_flags);
	}

	raw_spin_unlock_irq(&pool->lock);
}

/**
 * restore_unbound_workers_cpumask - restore cpumask of unbound workers
 * @pool: unbound pool of interest
 * @cpu: the CPU which is coming up
 *
 * An unbound pool may end up with a cpumask which doesn't have any online
 * CPUs.  When a worker of such pool get scheduled, the scheduler resets
 * its cpus_allowed.  If @cpu is in @pool's cpumask which didn't have any
 * online CPU before, cpus_allowed of all its workers should be restored.
 */
static void restore_unbound_workers_cpumask(struct worker_pool *pool, int cpu)
{
	static cpumask_t cpumask;
	struct worker *worker;

	lockdep_assert_held(&wq_pool_attach_mutex);

	/* is @cpu allowed for @pool? */
	if (!cpumask_test_cpu(cpu, pool->attrs->cpumask))
		return;

	cpumask_and(&cpumask, pool->attrs->cpumask, cpu_online_mask);

	/* as we're called from CPU_ONLINE, the following shouldn't fail */
	for_each_pool_worker(worker, pool)
		WARN_ON_ONCE(set_cpus_allowed_ptr(worker->task, &cpumask) < 0);
}

int workqueue_prepare_cpu(unsigned int cpu)
{
	struct worker_pool *pool;

	for_each_cpu_worker_pool(pool, cpu) {
		if (pool->nr_workers)
			continue;
		if (!create_worker(pool))
			return -ENOMEM;
	}
	return 0;
}

int workqueue_online_cpu(unsigned int cpu)
{
	struct worker_pool *pool;
	struct workqueue_struct *wq;
	int pi;

	mutex_lock(&wq_pool_mutex);

	for_each_pool(pool, pi) {
		mutex_lock(&wq_pool_attach_mutex);

		if (pool->cpu == cpu)
			rebind_workers(pool);
		else if (pool->cpu < 0)
			restore_unbound_workers_cpumask(pool, cpu);

		mutex_unlock(&wq_pool_attach_mutex);
	}

	/* update pod affinity of unbound workqueues */
	list_for_each_entry(wq, &workqueues, list) {
		struct workqueue_attrs *attrs = wq->unbound_attrs;

		if (attrs) {
			const struct wq_pod_type *pt = wqattrs_pod_type(attrs);
			int tcpu;

			for_each_cpu(tcpu, pt->pod_cpus[pt->cpu_pod[cpu]])
				wq_update_pod(wq, tcpu, cpu, true);
		}
	}

	mutex_unlock(&wq_pool_mutex);
	return 0;
}

int workqueue_offline_cpu(unsigned int cpu)
{
	struct workqueue_struct *wq;

	/* unbinding per-cpu workers should happen on the local CPU */
	if (WARN_ON(cpu != smp_processor_id()))
		return -1;

	unbind_workers(cpu);

	/* update pod affinity of unbound workqueues */
	mutex_lock(&wq_pool_mutex);
	list_for_each_entry(wq, &workqueues, list) {
		struct workqueue_attrs *attrs = wq->unbound_attrs;

		if (attrs) {
			const struct wq_pod_type *pt = wqattrs_pod_type(attrs);
			int tcpu;

			for_each_cpu(tcpu, pt->pod_cpus[pt->cpu_pod[cpu]])
				wq_update_pod(wq, tcpu, cpu, false);
		}
	}
	mutex_unlock(&wq_pool_mutex);

	return 0;
}

struct work_for_cpu {
	struct work_struct work;
	long (*fn)(void *);
	void *arg;
	long ret;
};

static void work_for_cpu_fn(struct work_struct *work)
{
	struct work_for_cpu *wfc = container_of(work, struct work_for_cpu, work);

	wfc->ret = wfc->fn(wfc->arg);
}

/**
 * work_on_cpu_key - run a function in thread context on a particular cpu
 * @cpu: the cpu to run on
 * @fn: the function to run
 * @arg: the function arg
 * @key: The lock class key for lock debugging purposes
 *
 * It is up to the caller to ensure that the cpu doesn't go offline.
 * The caller must not hold any locks which would prevent @fn from completing.
 *
 * Return: The value @fn returns.
 */
long work_on_cpu_key(int cpu, long (*fn)(void *),
		     void *arg, struct lock_class_key *key)
{
	struct work_for_cpu wfc = { .fn = fn, .arg = arg };

	INIT_WORK_ONSTACK_KEY(&wfc.work, work_for_cpu_fn, key);
	schedule_work_on(cpu, &wfc.work);
	flush_work(&wfc.work);
	destroy_work_on_stack(&wfc.work);
	return wfc.ret;
}
EXPORT_SYMBOL_GPL(work_on_cpu_key);

/**
 * work_on_cpu_safe_key - run a function in thread context on a particular cpu
 * @cpu: the cpu to run on
 * @fn:  the function to run
 * @arg: the function argument
 * @key: The lock class key for lock debugging purposes
 *
 * Disables CPU hotplug and calls work_on_cpu(). The caller must not hold
 * any locks which would prevent @fn from completing.
 *
 * Return: The value @fn returns.
 */
long work_on_cpu_safe_key(int cpu, long (*fn)(void *),
			  void *arg, struct lock_class_key *key)
{
	long ret = -ENODEV;

	cpus_read_lock();
	if (cpu_online(cpu))
		ret = work_on_cpu_key(cpu, fn, arg, key);
	cpus_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(work_on_cpu_safe_key);
#endif /* CONFIG_SMP */

#ifdef CONFIG_FREEZER

/**
 * freeze_workqueues_begin - begin freezing workqueues
 *
 * Start freezing workqueues.  After this function returns, all freezable
 * workqueues will queue new works to their inactive_works list instead of
 * pool->worklist.
 *
 * CONTEXT:
 * Grabs and releases wq_pool_mutex, wq->mutex and pool->lock's.
 */
void freeze_workqueues_begin(void)
{
	struct workqueue_struct *wq;
	struct pool_workqueue *pwq;

	mutex_lock(&wq_pool_mutex);

	WARN_ON_ONCE(workqueue_freezing);
	workqueue_freezing = true;

	list_for_each_entry(wq, &workqueues, list) {
		mutex_lock(&wq->mutex);
		for_each_pwq(pwq, wq)
			pwq_adjust_max_active(pwq);
		mutex_unlock(&wq->mutex);
	}

	mutex_unlock(&wq_pool_mutex);
}

/**
 * freeze_workqueues_busy - are freezable workqueues still busy?
 *
 * Check whether freezing is complete.  This function must be called
 * between freeze_workqueues_begin() and thaw_workqueues().
 *
 * CONTEXT:
 * Grabs and releases wq_pool_mutex.
 *
 * Return:
 * %true if some freezable workqueues are still busy.  %false if freezing
 * is complete.
 */
bool freeze_workqueues_busy(void)
{
	bool busy = false;
	struct workqueue_struct *wq;
	struct pool_workqueue *pwq;

	mutex_lock(&wq_pool_mutex);

	WARN_ON_ONCE(!workqueue_freezing);

	list_for_each_entry(wq, &workqueues, list) {
		if (!(wq->flags & WQ_FREEZABLE))
			continue;
		/*
		 * nr_active is monotonically decreasing.  It's safe
		 * to peek without lock.
		 */
		rcu_read_lock();
		for_each_pwq(pwq, wq) {
			WARN_ON_ONCE(pwq->nr_active < 0);
			if (pwq->nr_active) {
				busy = true;
				rcu_read_unlock();
				goto out_unlock;
			}
		}
		rcu_read_unlock();
	}
out_unlock:
	mutex_unlock(&wq_pool_mutex);
	return busy;
}

/**
 * thaw_workqueues - thaw workqueues
 *
 * Thaw workqueues.  Normal queueing is restored and all collected
 * frozen works are transferred to their respective pool worklists.
 *
 * CONTEXT:
 * Grabs and releases wq_pool_mutex, wq->mutex and pool->lock's.
 */
void thaw_workqueues(void)
{
	struct workqueue_struct *wq;
	struct pool_workqueue *pwq;

	mutex_lock(&wq_pool_mutex);

	if (!workqueue_freezing)
		goto out_unlock;

	workqueue_freezing = false;

	/* restore max_active and repopulate worklist */
	list_for_each_entry(wq, &workqueues, list) {
		mutex_lock(&wq->mutex);
		for_each_pwq(pwq, wq)
			pwq_adjust_max_active(pwq);
		mutex_unlock(&wq->mutex);
	}

out_unlock:
	mutex_unlock(&wq_pool_mutex);
}
#endif /* CONFIG_FREEZER */

static int workqueue_apply_unbound_cpumask(const cpumask_var_t unbound_cpumask)
{
	LIST_HEAD(ctxs);
	int ret = 0;
	struct workqueue_struct *wq;
	struct apply_wqattrs_ctx *ctx, *n;

	lockdep_assert_held(&wq_pool_mutex);

	list_for_each_entry(wq, &workqueues, list) {
		if (!(wq->flags & WQ_UNBOUND))
			continue;
		/* creating multiple pwqs breaks ordering guarantee */
		if (wq->flags & __WQ_ORDERED)
			continue;

		ctx = apply_wqattrs_prepare(wq, wq->unbound_attrs, unbound_cpumask);
		if (IS_ERR(ctx)) {
			ret = PTR_ERR(ctx);
			break;
		}

		list_add_tail(&ctx->list, &ctxs);
	}

	list_for_each_entry_safe(ctx, n, &ctxs, list) {
		if (!ret)
			apply_wqattrs_commit(ctx);
		apply_wqattrs_cleanup(ctx);
	}

	if (!ret) {
		mutex_lock(&wq_pool_attach_mutex);
		cpumask_copy(wq_unbound_cpumask, unbound_cpumask);
		mutex_unlock(&wq_pool_attach_mutex);
	}
	return ret;
}

/**
 *  workqueue_set_unbound_cpumask - Set the low-level unbound cpumask
 *  @cpumask: the cpumask to set
 *
 *  The low-level workqueues cpumask is a global cpumask that limits
 *  the affinity of all unbound workqueues.  This function check the @cpumask
 *  and apply it to all unbound workqueues and updates all pwqs of them.
 *
 *  Return:	0	- Success
 *  		-EINVAL	- Invalid @cpumask
 *  		-ENOMEM	- Failed to allocate memory for attrs or pwqs.
 */
int workqueue_set_unbound_cpumask(cpumask_var_t cpumask)
{
	int ret = -EINVAL;

	/*
	 * Not excluding isolated cpus on purpose.
	 * If the user wishes to include them, we allow that.
	 */
	cpumask_and(cpumask, cpumask, cpu_possible_mask);
	if (!cpumask_empty(cpumask)) {
		apply_wqattrs_lock();
		if (cpumask_equal(cpumask, wq_unbound_cpumask)) {
			ret = 0;
			goto out_unlock;
		}

		ret = workqueue_apply_unbound_cpumask(cpumask);

out_unlock:
		apply_wqattrs_unlock();
	}

	return ret;
}

static int parse_affn_scope(const char *val)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(wq_affn_names); i++) {
		if (!strncasecmp(val, wq_affn_names[i], strlen(wq_affn_names[i])))
			return i;
	}
	return -EINVAL;
}

static int wq_affn_dfl_set(const char *val, const struct kernel_param *kp)
{
	struct workqueue_struct *wq;
	int affn, cpu;

	affn = parse_affn_scope(val);
	if (affn < 0)
		return affn;
	if (affn == WQ_AFFN_DFL)
		return -EINVAL;

	cpus_read_lock();
	mutex_lock(&wq_pool_mutex);

	wq_affn_dfl = affn;

	list_for_each_entry(wq, &workqueues, list) {
		for_each_online_cpu(cpu) {
			wq_update_pod(wq, cpu, cpu, true);
		}
	}

	mutex_unlock(&wq_pool_mutex);
	cpus_read_unlock();

	return 0;
}

static int wq_affn_dfl_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%s\n", wq_affn_names[wq_affn_dfl]);
}

static const struct kernel_param_ops wq_affn_dfl_ops = {
	.set	= wq_affn_dfl_set,
	.get	= wq_affn_dfl_get,
};

module_param_cb(default_affinity_scope, &wq_affn_dfl_ops, NULL, 0644);

#ifdef CONFIG_SYSFS
/*
 * Workqueues with WQ_SYSFS flag set is visible to userland via
 * /sys/bus/workqueue/devices/WQ_NAME.  All visible workqueues have the
 * following attributes.
 *
 *  per_cpu		RO bool	: whether the workqueue is per-cpu or unbound
 *  max_active		RW int	: maximum number of in-flight work items
 *
 * Unbound workqueues have the following extra attributes.
 *
 *  nice		RW int	: nice value of the workers
 *  cpumask		RW mask	: bitmask of allowed CPUs for the workers
 *  affinity_scope	RW str  : worker CPU affinity scope (cache, numa, none)
 *  affinity_strict	RW bool : worker CPU affinity is strict
 */
struct wq_device {
	struct workqueue_struct		*wq;
	struct device			dev;
};

static struct workqueue_struct *dev_to_wq(struct device *dev)
{
	struct wq_device *wq_dev = container_of(dev, struct wq_device, dev);

	return wq_dev->wq;
}

static ssize_t per_cpu_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", (bool)!(wq->flags & WQ_UNBOUND));
}
static DEVICE_ATTR_RO(per_cpu);

static ssize_t max_active_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", wq->saved_max_active);
}

static ssize_t max_active_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	int val;

	if (sscanf(buf, "%d", &val) != 1 || val <= 0)
		return -EINVAL;

	workqueue_set_max_active(wq, val);
	return count;
}
static DEVICE_ATTR_RW(max_active);

static struct attribute *wq_sysfs_attrs[] = {
	&dev_attr_per_cpu.attr,
	&dev_attr_max_active.attr,
	NULL,
};
ATTRIBUTE_GROUPS(wq_sysfs);

static ssize_t wq_nice_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	int written;

	mutex_lock(&wq->mutex);
	written = scnprintf(buf, PAGE_SIZE, "%d\n", wq->unbound_attrs->nice);
	mutex_unlock(&wq->mutex);

	return written;
}

/* prepare workqueue_attrs for sysfs store operations */
static struct workqueue_attrs *wq_sysfs_prep_attrs(struct workqueue_struct *wq)
{
	struct workqueue_attrs *attrs;

	lockdep_assert_held(&wq_pool_mutex);

	attrs = alloc_workqueue_attrs();
	if (!attrs)
		return NULL;

	copy_workqueue_attrs(attrs, wq->unbound_attrs);
	return attrs;
}

static ssize_t wq_nice_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	struct workqueue_attrs *attrs;
	int ret = -ENOMEM;

	apply_wqattrs_lock();

	attrs = wq_sysfs_prep_attrs(wq);
	if (!attrs)
		goto out_unlock;

	if (sscanf(buf, "%d", &attrs->nice) == 1 &&
	    attrs->nice >= MIN_NICE && attrs->nice <= MAX_NICE)
		ret = apply_workqueue_attrs_locked(wq, attrs);
	else
		ret = -EINVAL;

out_unlock:
	apply_wqattrs_unlock();
	free_workqueue_attrs(attrs);
	return ret ?: count;
}

static ssize_t wq_cpumask_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	int written;

	mutex_lock(&wq->mutex);
	written = scnprintf(buf, PAGE_SIZE, "%*pb\n",
			    cpumask_pr_args(wq->unbound_attrs->cpumask));
	mutex_unlock(&wq->mutex);
	return written;
}

static ssize_t wq_cpumask_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	struct workqueue_attrs *attrs;
	int ret = -ENOMEM;

	apply_wqattrs_lock();

	attrs = wq_sysfs_prep_attrs(wq);
	if (!attrs)
		goto out_unlock;

	ret = cpumask_parse(buf, attrs->cpumask);
	if (!ret)
		ret = apply_workqueue_attrs_locked(wq, attrs);

out_unlock:
	apply_wqattrs_unlock();
	free_workqueue_attrs(attrs);
	return ret ?: count;
}

static ssize_t wq_affn_scope_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	int written;

	mutex_lock(&wq->mutex);
	if (wq->unbound_attrs->affn_scope == WQ_AFFN_DFL)
		written = scnprintf(buf, PAGE_SIZE, "%s (%s)\n",
				    wq_affn_names[WQ_AFFN_DFL],
				    wq_affn_names[wq_affn_dfl]);
	else
		written = scnprintf(buf, PAGE_SIZE, "%s\n",
				    wq_affn_names[wq->unbound_attrs->affn_scope]);
	mutex_unlock(&wq->mutex);

	return written;
}

static ssize_t wq_affn_scope_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	struct workqueue_attrs *attrs;
	int affn, ret = -ENOMEM;

	affn = parse_affn_scope(buf);
	if (affn < 0)
		return affn;

	apply_wqattrs_lock();
	attrs = wq_sysfs_prep_attrs(wq);
	if (attrs) {
		attrs->affn_scope = affn;
		ret = apply_workqueue_attrs_locked(wq, attrs);
	}
	apply_wqattrs_unlock();
	free_workqueue_attrs(attrs);
	return ret ?: count;
}

static ssize_t wq_affinity_strict_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 wq->unbound_attrs->affn_strict);
}

static ssize_t wq_affinity_strict_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	struct workqueue_attrs *attrs;
	int v, ret = -ENOMEM;

	if (sscanf(buf, "%d", &v) != 1)
		return -EINVAL;

	apply_wqattrs_lock();
	attrs = wq_sysfs_prep_attrs(wq);
	if (attrs) {
		attrs->affn_strict = (bool)v;
		ret = apply_workqueue_attrs_locked(wq, attrs);
	}
	apply_wqattrs_unlock();
	free_workqueue_attrs(attrs);
	return ret ?: count;
}

static struct device_attribute wq_sysfs_unbound_attrs[] = {
	__ATTR(nice, 0644, wq_nice_show, wq_nice_store),
	__ATTR(cpumask, 0644, wq_cpumask_show, wq_cpumask_store),
	__ATTR(affinity_scope, 0644, wq_affn_scope_show, wq_affn_scope_store),
	__ATTR(affinity_strict, 0644, wq_affinity_strict_show, wq_affinity_strict_store),
	__ATTR_NULL,
};

static struct bus_type wq_subsys = {
	.name				= "workqueue",
	.dev_groups			= wq_sysfs_groups,
};

static ssize_t wq_unbound_cpumask_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int written;

	mutex_lock(&wq_pool_mutex);
	written = scnprintf(buf, PAGE_SIZE, "%*pb\n",
			    cpumask_pr_args(wq_unbound_cpumask));
	mutex_unlock(&wq_pool_mutex);

	return written;
}

static ssize_t wq_unbound_cpumask_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	cpumask_var_t cpumask;
	int ret;

	if (!zalloc_cpumask_var(&cpumask, GFP_KERNEL))
		return -ENOMEM;

	ret = cpumask_parse(buf, cpumask);
	if (!ret)
		ret = workqueue_set_unbound_cpumask(cpumask);

	free_cpumask_var(cpumask);
	return ret ? ret : count;
}

static struct device_attribute wq_sysfs_cpumask_attr =
	__ATTR(cpumask, 0644, wq_unbound_cpumask_show,
	       wq_unbound_cpumask_store);

static int __init wq_sysfs_init(void)
{
	struct device *dev_root;
	int err;

	err = subsys_virtual_register(&wq_subsys, NULL);
	if (err)
		return err;

	dev_root = bus_get_dev_root(&wq_subsys);
	if (dev_root) {
		err = device_create_file(dev_root, &wq_sysfs_cpumask_attr);
		put_device(dev_root);
	}
	return err;
}
core_initcall(wq_sysfs_init);

static void wq_device_release(struct device *dev)
{
	struct wq_device *wq_dev = container_of(dev, struct wq_device, dev);

	kfree(wq_dev);
}

/**
 * workqueue_sysfs_register - make a workqueue visible in sysfs
 * @wq: the workqueue to register
 *
 * Expose @wq in sysfs under /sys/bus/workqueue/devices.
 * alloc_workqueue*() automatically calls this function if WQ_SYSFS is set
 * which is the preferred method.
 *
 * Workqueue user should use this function directly iff it wants to apply
 * workqueue_attrs before making the workqueue visible in sysfs; otherwise,
 * apply_workqueue_attrs() may race against userland updating the
 * attributes.
 *
 * Return: 0 on success, -errno on failure.
 */
int workqueue_sysfs_register(struct workqueue_struct *wq)
{
	struct wq_device *wq_dev;
	int ret;

	/*
	 * Adjusting max_active or creating new pwqs by applying
	 * attributes breaks ordering guarantee.  Disallow exposing ordered
	 * workqueues.
	 */
	if (WARN_ON(wq->flags & __WQ_ORDERED_EXPLICIT))
		return -EINVAL;

	wq->wq_dev = wq_dev = kzalloc(sizeof(*wq_dev), GFP_KERNEL);
	if (!wq_dev)
		return -ENOMEM;

	wq_dev->wq = wq;
	wq_dev->dev.bus = &wq_subsys;
	wq_dev->dev.release = wq_device_release;
	dev_set_name(&wq_dev->dev, "%s", wq->name);

	/*
	 * unbound_attrs are created separately.  Suppress uevent until
	 * everything is ready.
	 */
	dev_set_uevent_suppress(&wq_dev->dev, true);

	ret = device_register(&wq_dev->dev);
	if (ret) {
		put_device(&wq_dev->dev);
		wq->wq_dev = NULL;
		return ret;
	}

	if (wq->flags & WQ_UNBOUND) {
		struct device_attribute *attr;

		for (attr = wq_sysfs_unbound_attrs; attr->attr.name; attr++) {
			ret = device_create_file(&wq_dev->dev, attr);
			if (ret) {
				device_unregister(&wq_dev->dev);
				wq->wq_dev = NULL;
				return ret;
			}
		}
	}

	dev_set_uevent_suppress(&wq_dev->dev, false);
	kobject_uevent(&wq_dev->dev.kobj, KOBJ_ADD);
	return 0;
}

/**
 * workqueue_sysfs_unregister - undo workqueue_sysfs_register()
 * @wq: the workqueue to unregister
 *
 * If @wq is registered to sysfs by workqueue_sysfs_register(), unregister.
 */
static void workqueue_sysfs_unregister(struct workqueue_struct *wq)
{
	struct wq_device *wq_dev = wq->wq_dev;

	if (!wq->wq_dev)
		return;

	wq->wq_dev = NULL;
	device_unregister(&wq_dev->dev);
}
#else	/* CONFIG_SYSFS */
static void workqueue_sysfs_unregister(struct workqueue_struct *wq)	{ }
#endif	/* CONFIG_SYSFS */

/*
 * Workqueue watchdog.
 *
 * Stall may be caused by various bugs - missing WQ_MEM_RECLAIM, illegal
 * flush dependency, a concurrency managed work item which stays RUNNING
 * indefinitely.  Workqueue stalls can be very difficult to debug as the
 * usual warning mechanisms don't trigger and internal workqueue state is
 * largely opaque.
 *
 * Workqueue watchdog monitors all worker pools periodically and dumps
 * state if some pools failed to make forward progress for a while where
 * forward progress is defined as the first item on ->worklist changing.
 *
 * This mechanism is controlled through the kernel parameter
 * "workqueue.watchdog_thresh" which can be updated at runtime through the
 * corresponding sysfs parameter file.
 */
#ifdef CONFIG_WQ_WATCHDOG

static unsigned long wq_watchdog_thresh = 30;
static struct timer_list wq_watchdog_timer;

static unsigned long wq_watchdog_touched = INITIAL_JIFFIES;
static DEFINE_PER_CPU(unsigned long, wq_watchdog_touched_cpu) = INITIAL_JIFFIES;

/*
 * Show workers that might prevent the processing of pending work items.
 * The only candidates are CPU-bound workers in the running state.
 * Pending work items should be handled by another idle worker
 * in all other situations.
 */
static void show_cpu_pool_hog(struct worker_pool *pool)
{
	struct worker *worker;
	unsigned long flags;
	int bkt;

	raw_spin_lock_irqsave(&pool->lock, flags);

	hash_for_each(pool->busy_hash, bkt, worker, hentry) {
		if (task_is_running(worker->task)) {
			/*
			 * Defer printing to avoid deadlocks in console
			 * drivers that queue work while holding locks
			 * also taken in their write paths.
			 */
			printk_deferred_enter();

			pr_info("pool %d:\n", pool->id);
			sched_show_task(worker->task);

			printk_deferred_exit();
		}
	}

	raw_spin_unlock_irqrestore(&pool->lock, flags);
}

static void show_cpu_pools_hogs(void)
{
	struct worker_pool *pool;
	int pi;

	pr_info("Showing backtraces of running workers in stalled CPU-bound worker pools:\n");

	rcu_read_lock();

	for_each_pool(pool, pi) {
		if (pool->cpu_stall)
			show_cpu_pool_hog(pool);

	}

	rcu_read_unlock();
}

static void wq_watchdog_reset_touched(void)
{
	int cpu;

	wq_watchdog_touched = jiffies;
	for_each_possible_cpu(cpu)
		per_cpu(wq_watchdog_touched_cpu, cpu) = jiffies;
}

static void wq_watchdog_timer_fn(struct timer_list *unused)
{
	unsigned long thresh = READ_ONCE(wq_watchdog_thresh) * HZ;
	bool lockup_detected = false;
	bool cpu_pool_stall = false;
	unsigned long now = jiffies;
	struct worker_pool *pool;
	int pi;

	if (!thresh)
		return;

	rcu_read_lock();

	for_each_pool(pool, pi) {
		unsigned long pool_ts, touched, ts;

		pool->cpu_stall = false;
		if (list_empty(&pool->worklist))
			continue;

		/*
		 * If a virtual machine is stopped by the host it can look to
		 * the watchdog like a stall.
		 */
		kvm_check_and_clear_guest_paused();

		/* get the latest of pool and touched timestamps */
		if (pool->cpu >= 0)
			touched = READ_ONCE(per_cpu(wq_watchdog_touched_cpu, pool->cpu));
		else
			touched = READ_ONCE(wq_watchdog_touched);
		pool_ts = READ_ONCE(pool->watchdog_ts);

		if (time_after(pool_ts, touched))
			ts = pool_ts;
		else
			ts = touched;

		/* did we stall? */
		if (time_after(now, ts + thresh)) {
			lockup_detected = true;
			if (pool->cpu >= 0) {
				pool->cpu_stall = true;
				cpu_pool_stall = true;
			}
			pr_emerg("BUG: workqueue lockup - pool");
			pr_cont_pool_info(pool);
			pr_cont(" stuck for %us!\n",
				jiffies_to_msecs(now - pool_ts) / 1000);
		}


	}

	rcu_read_unlock();

	if (lockup_detected)
		show_all_workqueues();

	if (cpu_pool_stall)
		show_cpu_pools_hogs();

	wq_watchdog_reset_touched();
	mod_timer(&wq_watchdog_timer, jiffies + thresh);
}

notrace void wq_watchdog_touch(int cpu)
{
	unsigned long thresh = READ_ONCE(wq_watchdog_thresh) * HZ;
	unsigned long touch_ts = READ_ONCE(wq_watchdog_touched);
	unsigned long now = jiffies;

	if (cpu >= 0)
		per_cpu(wq_watchdog_touched_cpu, cpu) = now;
	else
		WARN_ONCE(1, "%s should be called with valid CPU", __func__);

	/* Don't unnecessarily store to global cacheline */
	if (time_after(now, touch_ts + thresh / 4))
		WRITE_ONCE(wq_watchdog_touched, jiffies);
}

static void wq_watchdog_set_thresh(unsigned long thresh)
{
	wq_watchdog_thresh = 0;
	del_timer_sync(&wq_watchdog_timer);

	if (thresh) {
		wq_watchdog_thresh = thresh;
		wq_watchdog_reset_touched();
		mod_timer(&wq_watchdog_timer, jiffies + thresh * HZ);
	}
}

static int wq_watchdog_param_set_thresh(const char *val,
					const struct kernel_param *kp)
{
	unsigned long thresh;
	int ret;

	ret = kstrtoul(val, 0, &thresh);
	if (ret)
		return ret;

	if (system_wq)
		wq_watchdog_set_thresh(thresh);
	else
		wq_watchdog_thresh = thresh;

	return 0;
}

static const struct kernel_param_ops wq_watchdog_thresh_ops = {
	.set	= wq_watchdog_param_set_thresh,
	.get	= param_get_ulong,
};

module_param_cb(watchdog_thresh, &wq_watchdog_thresh_ops, &wq_watchdog_thresh,
		0644);

static void wq_watchdog_init(void)
{
	timer_setup(&wq_watchdog_timer, wq_watchdog_timer_fn, TIMER_DEFERRABLE);
	wq_watchdog_set_thresh(wq_watchdog_thresh);
}

#else	/* CONFIG_WQ_WATCHDOG */

static inline void wq_watchdog_init(void) { }

#endif	/* CONFIG_WQ_WATCHDOG */

static void __init restrict_unbound_cpumask(const char *name, const struct cpumask *mask)
{
	if (!cpumask_intersects(wq_unbound_cpumask, mask)) {
		pr_warn("workqueue: Restricting unbound_cpumask (%*pb) with %s (%*pb) leaves no CPU, ignoring\n",
			cpumask_pr_args(wq_unbound_cpumask), name, cpumask_pr_args(mask));
		return;
	}

	cpumask_and(wq_unbound_cpumask, wq_unbound_cpumask, mask);
}

/**
 * workqueue_init_early - early init for workqueue subsystem
 *
 * This is the first step of three-staged workqueue subsystem initialization and
 * invoked as soon as the bare basics - memory allocation, cpumasks and idr are
 * up. It sets up all the data structures and system workqueues and allows early
 * boot code to create workqueues and queue/cancel work items. Actual work item
 * execution starts only after kthreads can be created and scheduled right
 * before early initcalls.
 */
void __init workqueue_init_early(void)
{
	struct wq_pod_type *pt = &wq_pod_types[WQ_AFFN_SYSTEM];
	int std_nice[NR_STD_WORKER_POOLS] = { 0, HIGHPRI_NICE_LEVEL };
	int i, cpu;

	BUILD_BUG_ON(__alignof__(struct pool_workqueue) < __alignof__(long long));

	BUG_ON(!alloc_cpumask_var(&wq_unbound_cpumask, GFP_KERNEL));
	cpumask_copy(wq_unbound_cpumask, cpu_possible_mask);
	restrict_unbound_cpumask("HK_TYPE_WQ", housekeeping_cpumask(HK_TYPE_WQ));
	restrict_unbound_cpumask("HK_TYPE_DOMAIN", housekeeping_cpumask(HK_TYPE_DOMAIN));
	if (!cpumask_empty(&wq_cmdline_cpumask))
		restrict_unbound_cpumask("workqueue.unbound_cpus", &wq_cmdline_cpumask);

	pwq_cache = KMEM_CACHE(pool_workqueue, SLAB_PANIC);

	wq_update_pod_attrs_buf = alloc_workqueue_attrs();
	BUG_ON(!wq_update_pod_attrs_buf);

	/* initialize WQ_AFFN_SYSTEM pods */
	pt->pod_cpus = kcalloc(1, sizeof(pt->pod_cpus[0]), GFP_KERNEL);
	pt->pod_node = kcalloc(1, sizeof(pt->pod_node[0]), GFP_KERNEL);
	pt->cpu_pod = kcalloc(nr_cpu_ids, sizeof(pt->cpu_pod[0]), GFP_KERNEL);
	BUG_ON(!pt->pod_cpus || !pt->pod_node || !pt->cpu_pod);

	BUG_ON(!zalloc_cpumask_var_node(&pt->pod_cpus[0], GFP_KERNEL, NUMA_NO_NODE));

	pt->nr_pods = 1;
	cpumask_copy(pt->pod_cpus[0], cpu_possible_mask);
	pt->pod_node[0] = NUMA_NO_NODE;
	pt->cpu_pod[0] = 0;

	/* initialize CPU pools */
	for_each_possible_cpu(cpu) {
		struct worker_pool *pool;

		i = 0;
		for_each_cpu_worker_pool(pool, cpu) {
			BUG_ON(init_worker_pool(pool));
			pool->cpu = cpu;
			cpumask_copy(pool->attrs->cpumask, cpumask_of(cpu));
			cpumask_copy(pool->attrs->__pod_cpumask, cpumask_of(cpu));
			pool->attrs->nice = std_nice[i++];
			pool->attrs->affn_strict = true;
			pool->node = cpu_to_node(cpu);

			/* alloc pool ID */
			mutex_lock(&wq_pool_mutex);
			BUG_ON(worker_pool_assign_id(pool));
			mutex_unlock(&wq_pool_mutex);
		}
	}

	/* create default unbound and ordered wq attrs */
	for (i = 0; i < NR_STD_WORKER_POOLS; i++) {
		struct workqueue_attrs *attrs;

		BUG_ON(!(attrs = alloc_workqueue_attrs()));
		attrs->nice = std_nice[i];
		unbound_std_wq_attrs[i] = attrs;

		/*
		 * An ordered wq should have only one pwq as ordering is
		 * guaranteed by max_active which is enforced by pwqs.
		 */
		BUG_ON(!(attrs = alloc_workqueue_attrs()));
		attrs->nice = std_nice[i];
		attrs->ordered = true;
		ordered_wq_attrs[i] = attrs;
	}

	system_wq = alloc_workqueue("events", 0, 0);
	system_highpri_wq = alloc_workqueue("events_highpri", WQ_HIGHPRI, 0);
	system_long_wq = alloc_workqueue("events_long", 0, 0);
	system_unbound_wq = alloc_workqueue("events_unbound", WQ_UNBOUND,
					    WQ_MAX_ACTIVE);
	system_freezable_wq = alloc_workqueue("events_freezable",
					      WQ_FREEZABLE, 0);
	system_power_efficient_wq = alloc_workqueue("events_power_efficient",
					      WQ_POWER_EFFICIENT, 0);
	system_freezable_power_efficient_wq = alloc_workqueue("events_freezable_power_efficient",
					      WQ_FREEZABLE | WQ_POWER_EFFICIENT,
					      0);
	BUG_ON(!system_wq || !system_highpri_wq || !system_long_wq ||
	       !system_unbound_wq || !system_freezable_wq ||
	       !system_power_efficient_wq ||
	       !system_freezable_power_efficient_wq);
}

static void __init wq_cpu_intensive_thresh_init(void)
{
	unsigned long thresh;
	unsigned long bogo;

	pwq_release_worker = kthread_create_worker(0, "pool_workqueue_release");
	BUG_ON(IS_ERR(pwq_release_worker));

	/* if the user set it to a specific value, keep it */
	if (wq_cpu_intensive_thresh_us != ULONG_MAX)
		return;

	/*
	 * The default of 10ms is derived from the fact that most modern (as of
	 * 2023) processors can do a lot in 10ms and that it's just below what
	 * most consider human-perceivable. However, the kernel also runs on a
	 * lot slower CPUs including microcontrollers where the threshold is way
	 * too low.
	 *
	 * Let's scale up the threshold upto 1 second if BogoMips is below 4000.
	 * This is by no means accurate but it doesn't have to be. The mechanism
	 * is still useful even when the threshold is fully scaled up. Also, as
	 * the reports would usually be applicable to everyone, some machines
	 * operating on longer thresholds won't significantly diminish their
	 * usefulness.
	 */
	thresh = 10 * USEC_PER_MSEC;

	/* see init/calibrate.c for lpj -> BogoMIPS calculation */
	bogo = max_t(unsigned long, loops_per_jiffy / 500000 * HZ, 1);
	if (bogo < 4000)
		thresh = min_t(unsigned long, thresh * 4000 / bogo, USEC_PER_SEC);

	pr_debug("wq_cpu_intensive_thresh: lpj=%lu BogoMIPS=%lu thresh_us=%lu\n",
		 loops_per_jiffy, bogo, thresh);

	wq_cpu_intensive_thresh_us = thresh;
}

/**
 * workqueue_init - bring workqueue subsystem fully online
 *
 * This is the second step of three-staged workqueue subsystem initialization
 * and invoked as soon as kthreads can be created and scheduled. Workqueues have
 * been created and work items queued on them, but there are no kworkers
 * executing the work items yet. Populate the worker pools with the initial
 * workers and enable future kworker creations.
 */
void __init workqueue_init(void)
{
	struct workqueue_struct *wq;
	struct worker_pool *pool;
	int cpu, bkt;

	wq_cpu_intensive_thresh_init();

	mutex_lock(&wq_pool_mutex);

	/*
	 * Per-cpu pools created earlier could be missing node hint. Fix them
	 * up. Also, create a rescuer for workqueues that requested it.
	 */
	for_each_possible_cpu(cpu) {
		for_each_cpu_worker_pool(pool, cpu) {
			pool->node = cpu_to_node(cpu);
		}
	}

	list_for_each_entry(wq, &workqueues, list) {
		WARN(init_rescuer(wq),
		     "workqueue: failed to create early rescuer for %s",
		     wq->name);
	}

	mutex_unlock(&wq_pool_mutex);

	/* create the initial workers */
	for_each_online_cpu(cpu) {
		for_each_cpu_worker_pool(pool, cpu) {
			pool->flags &= ~POOL_DISASSOCIATED;
			BUG_ON(!create_worker(pool));
		}
	}

	hash_for_each(unbound_pool_hash, bkt, pool, hash_node)
		BUG_ON(!create_worker(pool));

	wq_online = true;
	wq_watchdog_init();
}

/*
 * Initialize @pt by first initializing @pt->cpu_pod[] with pod IDs according to
 * @cpu_shares_pod(). Each subset of CPUs that share a pod is assigned a unique
 * and consecutive pod ID. The rest of @pt is initialized accordingly.
 */
static void __init init_pod_type(struct wq_pod_type *pt,
				 bool (*cpus_share_pod)(int, int))
{
	int cur, pre, cpu, pod;

	pt->nr_pods = 0;

	/* init @pt->cpu_pod[] according to @cpus_share_pod() */
	pt->cpu_pod = kcalloc(nr_cpu_ids, sizeof(pt->cpu_pod[0]), GFP_KERNEL);
	BUG_ON(!pt->cpu_pod);

	for_each_possible_cpu(cur) {
		for_each_possible_cpu(pre) {
			if (pre >= cur) {
				pt->cpu_pod[cur] = pt->nr_pods++;
				break;
			}
			if (cpus_share_pod(cur, pre)) {
				pt->cpu_pod[cur] = pt->cpu_pod[pre];
				break;
			}
		}
	}

	/* init the rest to match @pt->cpu_pod[] */
	pt->pod_cpus = kcalloc(pt->nr_pods, sizeof(pt->pod_cpus[0]), GFP_KERNEL);
	pt->pod_node = kcalloc(pt->nr_pods, sizeof(pt->pod_node[0]), GFP_KERNEL);
	BUG_ON(!pt->pod_cpus || !pt->pod_node);

	for (pod = 0; pod < pt->nr_pods; pod++)
		BUG_ON(!zalloc_cpumask_var(&pt->pod_cpus[pod], GFP_KERNEL));

	for_each_possible_cpu(cpu) {
		cpumask_set_cpu(cpu, pt->pod_cpus[pt->cpu_pod[cpu]]);
		pt->pod_node[pt->cpu_pod[cpu]] = cpu_to_node(cpu);
	}
}

static bool __init cpus_dont_share(int cpu0, int cpu1)
{
	return false;
}

static bool __init cpus_share_smt(int cpu0, int cpu1)
{
#ifdef CONFIG_SCHED_SMT
	return cpumask_test_cpu(cpu0, cpu_smt_mask(cpu1));
#else
	return false;
#endif
}

static bool __init cpus_share_numa(int cpu0, int cpu1)
{
	return cpu_to_node(cpu0) == cpu_to_node(cpu1);
}

/**
 * workqueue_init_topology - initialize CPU pods for unbound workqueues
 *
 * This is the third step of there-staged workqueue subsystem initialization and
 * invoked after SMP and topology information are fully initialized. It
 * initializes the unbound CPU pods accordingly.
 */
void __init workqueue_init_topology(void)
{
	struct workqueue_struct *wq;
	int cpu;

	init_pod_type(&wq_pod_types[WQ_AFFN_CPU], cpus_dont_share);
	init_pod_type(&wq_pod_types[WQ_AFFN_SMT], cpus_share_smt);
	init_pod_type(&wq_pod_types[WQ_AFFN_CACHE], cpus_share_cache);
	init_pod_type(&wq_pod_types[WQ_AFFN_NUMA], cpus_share_numa);

	mutex_lock(&wq_pool_mutex);

	/*
	 * Workqueues allocated earlier would have all CPUs sharing the default
	 * worker pool. Explicitly call wq_update_pod() on all workqueue and CPU
	 * combinations to apply per-pod sharing.
	 */
	list_for_each_entry(wq, &workqueues, list) {
		for_each_online_cpu(cpu) {
			wq_update_pod(wq, cpu, cpu, true);
		}
	}

	mutex_unlock(&wq_pool_mutex);
}

void __warn_flushing_systemwide_wq(void)
{
	pr_warn("WARNING: Flushing system-wide workqueues will be prohibited in near future.\n");
	dump_stack();
}
EXPORT_SYMBOL(__warn_flushing_systemwide_wq);

static int __init workqueue_unbound_cpus_setup(char *str)
{
	if (cpulist_parse(str, &wq_cmdline_cpumask) < 0) {
		cpumask_clear(&wq_cmdline_cpumask);
		pr_warn("workqueue.unbound_cpus: incorrect CPU range, using default\n");
	}

	return 1;
}
__setup("workqueue.unbound_cpus=", workqueue_unbound_cpus_setup);
