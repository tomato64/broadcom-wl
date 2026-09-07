// SPDX-License-Identifier: GPL-2.0
/*
 * Shadow layer, part 6: struct tasklet_struct.
 *
 * Same shape as the timer problem. 6.12 inserted `bool use_callback` at offset
 * 12, so:
 *
 *              2.6.36   6.12
 *   next          0        0
 *   state         4        4
 *   count         8        8
 *   use_callback  -       12
 *   func         12       16
 *   data         16       20
 *   sizeof       20       24
 *
 * The blob embeds a 20-byte tasklet_struct in its own state and calls the
 * kernel's tasklet_init() on it. The kernel then wrote ->data at offset 20 -
 * past the end of the blob's allocation - so the cookie was both lost and
 * scribbling on whatever followed. wl_dpc() ran with data == NULL and died
 * dereferencing it, in interrupt context.
 *
 * So the kernel never sees the blob's tasklet. We keep a real one of our own,
 * hash-keyed on the blob's address, exactly as wl_shadow_timer.c does.
 *
 * One extra wrinkle over timers: tasklet_schedule() is a static inline that
 * the blob compiled in, and it tests TASKLET_STATE_SCHED in *its own* struct
 * before calling __tasklet_schedule(). ->state is at offset 4 in both layouts,
 * so that bit is real and ours to maintain - if it is never cleared, the blob
 * schedules once and then silently stops. The trampoline clears it.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/hashtable.h>
#include <linux/bitops.h>

#include "shadow/offsets-2.6.36.h"
#include "wl_shadow.h"

typedef void (*wl236_tasklet_fn)(unsigned long);

static inline unsigned long *blob_state(void *bt)
{
	return (unsigned long *)((u8 *)bt + WL236_TASKLET_STRUCT_STATE);
}

struct wl_tasklet_map {
	struct hlist_node	link;
	void			*blob;	/* the blob's 20-byte tasklet_struct */
	wl236_tasklet_fn	func;
	unsigned long		data;
	struct tasklet_struct	real;	/* the one the kernel knows about */
};

static DEFINE_HASHTABLE(wl_tasklet_ht, 5);
static DEFINE_SPINLOCK(wl_tasklet_lock);

/* caller holds wl_tasklet_lock */
static struct wl_tasklet_map *map_find(void *blob)
{
	struct wl_tasklet_map *m;

	hash_for_each_possible(wl_tasklet_ht, m, link, (unsigned long)blob)
		if (m->blob == blob)
			return m;
	return NULL;
}

static void wl_tasklet_tramp(struct tasklet_struct *t)
{
	struct wl_tasklet_map *m = container_of(t, struct wl_tasklet_map, real);

	/*
	 * Clear the blob's own SCHED bit before running: its inlined
	 * tasklet_schedule() will refuse to re-arm otherwise, and the handler
	 * routinely re-schedules itself.
	 */
	clear_bit(TASKLET_STATE_SCHED, blob_state(m->blob));

	if (m->func)
		m->func(m->data);
	else
		pr_warn_once("wl: tasklet %p ran with no handler\n", m->blob);
}

/* 2.6.36: void tasklet_init(struct tasklet_struct *, void (*)(unsigned long),
 *                           unsigned long data) */
void wl_shim_tasklet_init(void *blob_t, void (*func)(unsigned long),
			  unsigned long data)
{
	struct wl_tasklet_map *m, *dup;
	unsigned long flags;

	if (!blob_t)
		return;

	spin_lock_irqsave(&wl_tasklet_lock, flags);
	dup = map_find(blob_t);
	if (dup) {
		/* Re-init on the same address is normal across up/down. */
		dup->func = (wl236_tasklet_fn)func;
		dup->data = data;
	}
	spin_unlock_irqrestore(&wl_tasklet_lock, flags);

	if (dup) {
		clear_bit(TASKLET_STATE_SCHED, blob_state(blob_t));
		return;
	}

	m = kzalloc(sizeof(*m), GFP_ATOMIC);
	if (!m) {
		pr_err("wl: no memory for tasklet map (%p); it will never run\n",
		       blob_t);
		return;
	}

	m->blob = blob_t;
	m->func = (wl236_tasklet_fn)func;
	m->data = data;
	tasklet_setup(&m->real, wl_tasklet_tramp);

	/* The blob's own struct stays inert: we only ever read and clear its
	 * SCHED bit, so leave the rest as the blob left it. */
	clear_bit(TASKLET_STATE_SCHED, blob_state(blob_t));

	spin_lock_irqsave(&wl_tasklet_lock, flags);
	hash_add(wl_tasklet_ht, &m->link, (unsigned long)blob_t);
	spin_unlock_irqrestore(&wl_tasklet_lock, flags);
}

void wl_shim___tasklet_schedule(void *blob_t)
{
	struct wl_tasklet_map *m;
	unsigned long flags;

	spin_lock_irqsave(&wl_tasklet_lock, flags);
	m = map_find(blob_t);
	spin_unlock_irqrestore(&wl_tasklet_lock, flags);

	if (!m) {
		pr_warn_once("wl: __tasklet_schedule on unmapped tasklet %p\n",
			     blob_t);
		return;
	}
	tasklet_schedule(&m->real);
}

void wl_shim_tasklet_kill(void *blob_t)
{
	struct wl_tasklet_map *m;
	unsigned long flags;

	spin_lock_irqsave(&wl_tasklet_lock, flags);
	m = map_find(blob_t);
	if (m)
		hash_del(&m->link);
	spin_unlock_irqrestore(&wl_tasklet_lock, flags);

	if (!m)
		return;

	tasklet_kill(&m->real);
	clear_bit(TASKLET_STATE_SCHED, blob_state(blob_t));
	kfree(m);
}

void wl_shadow_tasklet_exit(void)
{
	struct wl_tasklet_map *m;
	struct hlist_node *tmp;
	unsigned long flags;
	int bkt, n = 0;

	hash_for_each_safe(wl_tasklet_ht, bkt, tmp, m, link) {
		tasklet_kill(&m->real);
		spin_lock_irqsave(&wl_tasklet_lock, flags);
		hash_del(&m->link);
		spin_unlock_irqrestore(&wl_tasklet_lock, flags);
		kfree(m);
		n++;
	}
	if (n)
		pr_info("wl: shadow tasklet table freed (%d entries)\n", n);
}
