// SPDX-License-Identifier: GPL-2.0
/*
 * Tier-2 shim: kernel APIs the FreshTomato Northstar `wl_apsta.o` blob imports
 * that no longer exist as linkable symbols in Linux 6.12.
 *
 * Status of each symbol in 6.12.94, established by scanning the tree:
 *   ABSENT      create_proc_entry num_physpages __memzero warn_slowpath_null
 *               _set_bit_le _test_and_set_bit_le _test_and_clear_bit_le del_timer
 *   HEADER-ONLY dev_alloc_skb flush_scheduled_work alloc_netdev_mq
 *               __copy_from_user __copy_to_user __arm_ioremap __iounmap
 *               (static inline / macro, so the name is not linkable)
 *
 * IMPORTANT — read before believing any of this "works":
 *
 * Resolving a symbol is not the same as restoring its behaviour. Three of the
 * wrappers below are deliberately marked LANDMINE: they will satisfy the linker
 * and then corrupt memory at runtime, because the blob accesses the returned
 * object's fields at 2.6.36 struct offsets. Fixing those is the Tier-3 shadow
 * layer, not a wrapper.
 *
 * There is also a class of problem this file cannot address at all: symbols that
 * still exist but whose ABI changed. `init_timer_key` is the example — exported
 * in 6.12, but 2.6.36 called it as (timer, name, key) and 6.12 wants
 * (timer, func, flags, name, key). It links silently and misbehaves. The link
 * experiment will NOT surface those; only disassembly will.
 *
 * MODULE_LICENSE is deliberately absent here: the blob's own .modinfo already
 * carries `license=Proprietary`.
 */

/*
 * Six of the symbols below exist in 6.12 as `static inline` functions, not
 * macros, so #undef cannot displace them - defining our own would be a
 * redefinition error. Rename the header's inline out of the way *before*
 * including anything, so the name is free for us to provide as a real linkable
 * symbol. The renamed inline still exists, so headers that call it internally
 * are unaffected.
 *
 * The matching #undef for each appears further down, just before our definition.
 */
#define schedule_work		wl_hidden_schedule_work
#define schedule_work_on	wl_hidden_schedule_work_on
#define dev_get_drvdata		wl_hidden_dev_get_drvdata
#define dev_set_drvdata		wl_hidden_dev_set_drvdata
#define __copy_from_user	wl_hidden___copy_from_user
#define __copy_to_user		wl_hidden___copy_to_user

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/timer.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <linux/uaccess.h>
#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/slab.h>

#include "wl_shadow_netdev.h"

/* Several of these names are macros or static inlines in 6.12 headers; drop the
 * header definition so we can provide a real, linkable symbol. */
#undef flush_scheduled_work
#undef __copy_from_user
#undef __copy_to_user
#undef __arm_ioremap
#undef __iounmap

/* ------------------------------------------------------------------ *
 * Genuinely straightforward: semantics are preserved.
 * ------------------------------------------------------------------ */

/* Removed in 3.12. The blob reads it to size internal pools. Populated from
 * wl_shim_init() below, before the blob's own init runs. */
unsigned long num_physpages;

/* ARM's __memzero went away with the ARM string-op rework. */
void __memzero(void *ptr, __kernel_size_t n)
{
	memset(ptr, 0, n);
}

/* Removed in 5.9 by the WARN() rework. */
void warn_slowpath_null(const char *file, const int line)
{
	printk(KERN_WARNING "wl: WARN at %s:%d\n", file, line);
	dump_stack();
}

/* Removed in 6.6; was flush_workqueue(system_wq). */
void flush_scheduled_work(void)
{
	flush_workqueue(system_wq);
}

/* ARM dropped the __-prefixed ioremap entry points. mtype is ignored: every
 * caller in wl passes MT_DEVICE, which is what ioremap() already gives. */
void __iomem *__arm_ioremap(phys_addr_t phys_addr, size_t size, unsigned int mtype)
{
	return ioremap(phys_addr, size);
}

void __iounmap(volatile void __iomem *addr)
{
	iounmap((void __iomem *)addr);
}

/* Became raw_copy_{from,to}_user in 4.13. Both return bytes NOT copied. */
unsigned long __copy_from_user(void *to, const void __user *from, unsigned long n)
{
	return raw_copy_from_user(to, from, n);
}

unsigned long __copy_to_user(void __user *to, const void *from, unsigned long n)
{
	return raw_copy_to_user(to, from, n);
}

/* ARM's little-endian bitops helpers. On a little-endian ARM build these are
 * bit-for-bit identical to the plain versions. */
/*
 * These three are also the interception point for the blob's open-coded
 * netif_stop_queue()/netif_wake_queue(), which are bit operations on
 * ->_tx[0].state. wl_shadow_netdev_txq_bitop() claims the address if it
 * belongs to a shadow queue; otherwise these stay plain bit ops.
 */
void _set_bit_le(int nr, volatile unsigned long *p)
{
	if (wl_shadow_netdev_txq_bitop(nr, p, 0, NULL))
		return;
	set_bit(nr, p);
}

int _test_and_set_bit_le(int nr, volatile unsigned long *p)
{
	int old;

	if (wl_shadow_netdev_txq_bitop(nr, p, 1, &old))
		return old;
	return test_and_set_bit(nr, p);
}

int _test_and_clear_bit_le(int nr, volatile unsigned long *p)
{
	int old;

	if (wl_shadow_netdev_txq_bitop(nr, p, 2, &old))
		return old;
	return test_and_clear_bit(nr, p);
}

/* ------------------------------------------------------------------ *
 * Workqueue: the blob's open-coded 2.6.36 INIT_WORK.
 *
 * wl_schedule_task() does not call INIT_WORK; it lays a work item out by
 * hand and writes 2.6.36's WORK_STRUCT_NO_CPU - WORK_CPU_NONE (6) shifted by
 * that kernel's WORK_STRUCT_FLAG_BITS (7), so 0x300 - into ->data.
 *
 * 6.12 encodes something else entirely in those bits. With the work item off
 * queue, ->data is
 *
 *   [ pool ID : 11 ] [ disable depth : 16 ] [ OFFQ flags : 1 ] [ flags : 4 ]
 *    31        21     20              5      4                  3        0
 *
 * so 0x300 decodes as a disable depth of 24. queue_work_on() sets PENDING,
 * clear_pending_if_disabled() sees the non-zero depth, clears PENDING again
 * and returns false - the work is never queued, and schedule_work() reports
 * failure.
 *
 * For the persistent work items embedded in wl_info_t that is survivable:
 * clear_pending_if_disabled() rewrites ->data on its way out, so only the
 * first schedule of each is lost. wl_schedule_task() is not, because it
 * kmalloc()s a fresh wl_task_t per call and re-writes 0x300 every time, so
 * it fails every time. Everything routed through it - _wl_add_if,
 * _wl_del_if, _wl_add_monitor_if - simply never ran.
 *
 * That is not a quiet failure. wl_alloc_if() links the new wl_if_t into
 * wl->if_list before wl_add_if() schedules _wl_add_if; wl_add_if()'s failure
 * path then MFREE()s the wl_if_t WITHOUT unlinking it (only wl_free_if()
 * unlinks). wl->if_list is left holding a dangling pointer, and the next
 * wl_up() walks it:
 *
 *   Unable to handle kernel paging request at virtual address 58585a78
 *   PC is at wl_txflowcontrol+0x28 [wl]   LR is at wl_up+0x54 [wl]
 *
 * - wl_txflowcontrol() reading ->dev out of a reused slab object. So a
 * driver that is otherwise healthy dies the moment a second BSS is
 * configured on a radio.
 *
 * Normalising ->data before handing the item to the kernel fixes all of it.
 * ------------------------------------------------------------------ */

/*
 * The blob's wl_task_t is 20 bytes: a work_struct followed by ->context at
 * offset 16 (both wl_schedule_task() and _wl_add_if() use that constant).
 * That only holds while work_struct is still exactly 16 bytes - CONFIG_LOCKDEP
 * appends a lockdep_map and every one of those allocations becomes an
 * overflow.
 */
static_assert(sizeof(struct work_struct) == 16,
	      "struct work_struct grew; the blob's 20-byte wl_task_t no longer fits it");

/* 2.6.36's WORK_STRUCT_NO_CPU for that kernel's config: 6 << 7. */
#define WL236_WORK_DATA_INIT	0x300UL

static void wl_fixup_work_data(struct work_struct *work)
{
	unsigned long data = atomic_long_read(&work->data);

	/*
	 * With PWQ set the item is queued and ->data is a pool_workqueue
	 * pointer; never touch that. Otherwise ->data is off-queue info, where
	 * a non-zero disable depth cannot have come from this kernel - nothing
	 * here ever calls disable_work() - so it is 2.6.36 bits landing in a
	 * field that did not exist then.
	 */
	if (data & WORK_STRUCT_PWQ)
		return;
	if (!(data & WORK_OFFQ_DISABLE_MASK))
		return;

	if (data != WL236_WORK_DATA_INIT)
		pr_warn_once("wl: work %p has an unexpected ->data %#lx; "
			     "reinitialising it anyway\n", work, data);

	/* What WORK_DATA_INIT() would have written. ->entry is already
	 * self-linked and ->func already set by the blob. */
	atomic_long_set(&work->data, (unsigned long)WORK_STRUCT_NO_POOL);
}

/* ------------------------------------------------------------------ *
 * LANDMINES: these link, but do not work. Tier-3 territory.
 *
 * Was 3, now 1. sk_buff, net_device and timer_list all moved out to the
 * wl_shadow_*.c layers, which handle them properly rather than papering over
 * them; their entry points are rerouted there by objcopy. Only procfs is left,
 * and that one is genuinely just switched off.
 * ------------------------------------------------------------------ */

/*
 * LANDMINE — struct proc_dir_entry is private in 6.12 and lost
 * read_proc/write_proc entirely (proc_ops, 5.6). The blob fills those fields in
 * directly after this returns. wl's procfs surface is diagnostic only
 * (/proc/net/wl%d), so returning NULL is survivable for a bring-up attempt:
 * the blob checks for NULL and skips.
 */
struct proc_dir_entry *create_proc_entry(const char *name, umode_t mode,
					 struct proc_dir_entry *parent)
{
	pr_warn_once("wl: create_proc_entry stubbed out (procfs disabled)\n");
	return NULL;
}

/*
 * Paired with the above: since nothing is ever created, the blob's cleanup
 * call would ask the kernel to remove an entry that does not exist, which
 * warns ("remove_proc_entry: name 'net/wl0'"). Rerouted by objcopy so only the
 * blob's calls are absorbed - ours, if we ever make any, still work.
 */
void wl_shim_remove_proc_entry(const char *name, struct proc_dir_entry *parent)
{
	pr_debug("wl: remove_proc_entry('%s') absorbed; procfs was never created\n",
		 name ? name : "(null)");
}

/* ------------------------------------------------------------------ *
 * Module entry. The blob's own init_module/cleanup_module were renamed to
 * wl_blob_* by objcopy in the Makefile so that we run first.
 * ------------------------------------------------------------------ */

#include "wl_shadow.h"

static int __init wl_shim_init(void)
{
	num_physpages = totalram_pages();
	wl_shadow_l2c_init();	/* must precede the blob's init */
	wl_shadow_debug_init();	/* likewise: sets the blob's msg levels */
	pr_info("wl: shim init (num_physpages=%lu)\n", num_physpages);
	return wl_blob_init_module();
}

static void __exit wl_shim_exit(void)
{
	wl_blob_cleanup_module();
	wl_shadow_timer_exit();
	wl_shadow_tasklet_exit();
	wl_shadow_skb_exit();
	wl_shadow_netdev_exit();
	wl_shadow_nvram_exit();
	wl_shadow_pci_exit();
}

module_init(wl_shim_init);
module_exit(wl_shim_exit);

/* ------------------------------------------------------------------ *
 * Round 2. These were NOT obvious by inspection - scripts/classify.sh
 * found them. All six exist in 6.12 only as macros or static inlines,
 * so the *symbol* the blob imports is not linkable.
 *
 * Kept at the bottom of the file on purpose: `#undef printk` must come
 * after this file's own pr_*() calls, which expand to printk().
 * ------------------------------------------------------------------ */

#undef cpu_online_mask
#undef schedule_work
#undef schedule_work_on
#undef dev_get_drvdata
#undef dev_set_drvdata
#undef printk

/* 2.6.36 had `extern const struct cpumask *const cpu_online_mask;` as a real
 * variable; 6.12 made it a macro over __cpu_online_mask. */
const struct cpumask *const cpu_online_mask = (const struct cpumask *)&__cpu_online_mask;

/* NOTE: system_wq and flush_workqueue are EXPORT_SYMBOL_GPL in 6.12, and this
 * module inherits license=Proprietary from the blob's own .modinfo. Expect
 * modpost to refuse these - see out/gpl-only-blocked.txt. That is a real
 * finding, not a bug in this file. */
int schedule_work(struct work_struct *work)
{
	wl_fixup_work_data(work);
	return queue_work_on(WORK_CPU_UNBOUND, system_wq, work);
}

int schedule_work_on(int cpu, struct work_struct *work)
{
	wl_fixup_work_data(work);
	return queue_work_on(cpu, system_wq, work);
}

void *dev_get_drvdata(const struct device *dev)
{
	return dev->driver_data;
}

void dev_set_drvdata(struct device *dev, void *data)
{
	dev->driver_data = data;
}

/* printk() became a macro over _printk() in 5.15. */
int printk(const char *fmt, ...)
{
	va_list args;
	int r;

	va_start(args, fmt);
	r = vprintk(fmt, args);
	va_end(args);
	return r;
}

/* ------------------------------------------------------------------ *
 * Round 3. Found only once Module.symvers was available: this kernel's
 * config inlines _raw_spin_unlock (CONFIG_INLINE_SPIN_UNLOCK), so unlike
 * the _raw_spin_lock* variants it is not an exported symbol here. This one
 * is config-dependent, not version-dependent.
 * ------------------------------------------------------------------ */

#include <linux/spinlock.h>

#undef _raw_spin_unlock

void _raw_spin_unlock(raw_spinlock_t *lock)
{
	__raw_spin_unlock(lock);
}
