// SPDX-License-Identifier: GPL-2.0
/*
 * Shadow layer, part 5: struct outer_cache_fns.
 *
 * The blob is built with -DWL_PL310_WAR, Broadcom's workaround for the PL310
 * L2 controller on Northstar, and wlc_mctrl_write() calls outer_sync() after
 * touching a register to push the L2 write buffer. outer_sync() is an indirect
 * call through the global `outer_cache`, at an offset the blob compiled in.
 *
 * That offset now means something else:
 *
 *                    2.6.36   6.12
 *   inv_range           0        0
 *   clean_range         4        4
 *   flush_range         8        8
 *   sync               12       20     <-- moved
 *   flush_all           -       12     <-- took sync's slot
 *   sizeof             16       36
 *
 * So the blob asked for a write-buffer sync and got a full L2 cache flush.
 * l2c210_flush_all() opens with BUG_ON(!irqs_disabled()), the blob calls it
 * with interrupts on, and the kernel dies on an undefined instruction - the
 * ARM encoding of BUG(). See TESTING.md for the trace.
 *
 * Note this is not fixable by wrapping the call in local_irq_save(): that
 * would satisfy the BUG_ON while still doing entirely the wrong thing, a full
 * cache flush on every register write. The blob wants sync; give it sync.
 *
 * `outer_cache` is a data symbol rather than a function, but the same objcopy
 * reroute works: --redefine-sym points the blob at our 16-byte 2.6.36-layout
 * copy, which we fill from the real one before the blob's init runs.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <asm/outercache.h>

#include "shadow/offsets-2.6.36.h"
#include "wl_shadow.h"

/* What the blob sees. 2.6.36 layout: four function pointers. */
u8 wl_shim_outer_cache[WL236_OUTER_CACHE_FNS_SIZEOF] __aligned(4);

#define SLOT(off)	(*(void **)(wl_shim_outer_cache + (off)))

void wl_shadow_l2c_init(void)
{
	SLOT(WL236_OUTER_CACHE_FNS_INV_RANGE)   = (void *)outer_cache.inv_range;
	SLOT(WL236_OUTER_CACHE_FNS_CLEAN_RANGE) = (void *)outer_cache.clean_range;
	SLOT(WL236_OUTER_CACHE_FNS_FLUSH_RANGE) = (void *)outer_cache.flush_range;
	SLOT(WL236_OUTER_CACHE_FNS_SYNC)        = (void *)outer_cache.sync;

	pr_info("wl: outer_cache shimmed (sync=%pS, was reading flush_all=%pS)\n",
		outer_cache.sync, outer_cache.flush_all);

	if (!outer_cache.sync)
		pr_warn("wl: outer_cache.sync is NULL; the PL310 WAR will be a no-op\n");
}
