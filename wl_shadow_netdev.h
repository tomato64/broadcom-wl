/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shadow struct net_device.
 *
 * The blob was compiled against 2.6.36, where sizeof(struct net_device) is 896
 * and netdev_priv() is inlined as dev + ALIGN(896, 32) = dev + 896. In 6.12
 * sizeof is 1344, so the blob's private pointer would land 448 bytes inside the
 * kernel's own struct. It also touches nine fields whose offsets all moved.
 *
 * So the blob gets a 900-byte shadow (896 + its 4-byte private area) and never
 * sees a real net_device. A real 6.12 device is registered alongside it, and
 * netdev_priv(real) holds a back-pointer to the shadow so the trampolines can
 * find it in O(1).
 *
 * Which nine fields, and why each is handled the way it is, is documented in
 * shadow/netdev-findings.md - derived from disassembly, reproducible with
 * `make scan-netdev`.
 */
#ifndef WL_SHADOW_NETDEV_H
#define WL_SHADOW_NETDEV_H

#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/build_bug.h>
#include "shadow/offsets-2.6.36.h"

#define WL_NETDEV_SHADOW_MAGIC 0x57314E44u	/* "W1ND" */

/* The blob called alloc_netdev_mq() with sizeof_priv = 4: the private area is
 * a single pointer back to its own wl interface struct. */
#define WL236_NETDEV_PRIV_SIZE	4

struct wl_netdev_shadow {
	/* Must be first: the blob only ever holds a pointer to this, and
	 * recovering the wrapper is a plain cast. */
	u8	s236[WL236_NET_DEVICE_SIZEOF + WL236_NETDEV_PRIV_SIZE];

	/* What the shadow's ->_tx points at. The blob reads ->_tx[0].state and
	 * ->_tx[0].qdisc directly; state moved from offset 8 to 204 and the
	 * struct grew 96 -> 256, so it cannot be the real queue. Only [0]
	 * exists because alloc_netdev_mq() was called with queue_count == 1. */
	u8	fake_txq[WL236_NETDEV_QUEUE_SIZEOF];

	/* What the shadow's ->dev_addr points at. The blob reads that pointer
	 * and memcpy()s the MAC into it, so it has to address real storage. */
	u8	mac[MAX_ADDR_LEN];

	struct net_device	*real;
	void			*blob_ops;	/* the blob's 2.6.36 net_device_ops */
	void			*blob_ethtool_ops;/* likewise, its ethtool_ops */
	struct net_device_ops	real_ops;	/* trampolines, handed to the kernel */
	struct ethtool_ops	real_ethtool_ops;
	struct net_device_stats	stats;		/* ndo_get_stats return buffer */
	struct list_head	link;
	u32			magic;
};

static_assert(offsetof(struct wl_netdev_shadow, s236) == 0,
	      "s236 must be first: the blob's pointer is cast, not offset");

/*
 * net_device_stats turned out to be byte-identical between 2.6.36 and 6.12
 * (sizeof 92, every probed member at the same offset), so ndo_get_stats can
 * memcpy rather than translate. Enforce that rather than trusting it.
 */
static_assert(sizeof(struct net_device_stats) == WL236_NET_DEVICE_STATS_SIZEOF,
	      "net_device_stats layout diverged; ndo_get_stats needs real translation");

/*
 * ->state, which offsets-2.6.36.h does NOT carry: the probe never measured it,
 * because netdev-findings.md was derived from the primary interface's paths and
 * none of those read it. wl_sendup() does, but only for a VIF - the primary is
 * sent up with wlif == NULL and skips the check entirely - so it stayed
 * invisible until virtual interfaces started working:
 *
 *   1cb0:  ldr r3, [r8, #8]     ; wlif->dev
 *   1cb4:  cmp r3, #0
 *   1cb8:  beq <drop>
 *   1cbc:  ldr r3, [r3, #72]    ; dev->state
 *   1cc0:  tst r3, #2           ; __LINK_STATE_PRESENT
 *   1cc4:  bne <deliver>        ; else "wl%d: wl_sendup: interface not ready"
 *
 * 72 rather than the 68 a stock 2.6.36 layout gives: the measured anchors
 * (irq 64, features 100, ifindex 104) are 4 bytes further along than stock,
 * so one extra word sits between ->irq and ->features in the tree the blob was
 * built against. It has to be before ->state rather than after ->unreg_list,
 * because at 68 this read would be dev_list.next - a pointer, whose bit 1 is
 * always 0 - and every VIF frame would be dropped on FreshTomato too.
 *
 * Derived from disassembly rather than measured, but confirmed on hardware:
 * setting it is what stopped "wl0: wl_sendup: interface not ready" and got
 * frames flowing on a virtual BSS. Move it into the generated header next time
 * the offsets probe is run.
 */
#define WL236_NET_DEVICE_STATE	72

/*
 * The blob's 2.6.36 struct ethtool_ops, of which it fills exactly one slot.
 * Verified against the relocations rather than assumed: wl_netdev_ops sits at
 * .rodata+0x13cf6c and its ethtool_ops at +0x6c = 0x13cfd8, and the only
 * relocation in that table is wl_get_driver_info at 0x13cfe0 - offset 8, which
 * is where get_drvinfo lands in a stock 2.6.36 layout (get_settings,
 * set_settings, get_drvinfo).
 *
 * struct ethtool_drvinfo is 196 bytes with ->driver at 4 and ->version at 36 in
 * BOTH kernels, and the blob memsets all 196 before writing those two.
 */
#define WL236_ETHTOOL_OPS_GET_DRVINFO	8
#define WL236_ETHTOOL_DRVINFO_SIZEOF	196

/* ---- typed access into the 2.6.36-layout shadow --------------------- */

#define WL236_ND(sh, off, type)	(*(type *)((u8 *)(sh) + (off)))

#define wl236_nd_type(sh)	WL236_ND(sh, WL236_NET_DEVICE_TYPE,		u16)
#define wl236_nd_mtu(sh)	WL236_ND(sh, WL236_NET_DEVICE_MTU,		unsigned int)
#define wl236_nd_flags(sh)	WL236_ND(sh, WL236_NET_DEVICE_FLAGS,		unsigned int)
#define wl236_nd_dev_addr(sh)	WL236_ND(sh, WL236_NET_DEVICE_DEV_ADDR,		void *)
#define wl236_nd_netdev_ops(sh)	WL236_ND(sh, WL236_NET_DEVICE_NETDEV_OPS,	void *)
#define wl236_nd_ethtool_ops(sh) WL236_ND(sh, WL236_NET_DEVICE_ETHTOOL_OPS,	void *)
#define wl236_nd_tx(sh)		WL236_ND(sh, WL236_NET_DEVICE__TX,		void *)
#define wl236_nd_parent(sh)	WL236_ND(sh, WL236_NET_DEVICE_DEV + WL236_DEVICE_PARENT, void *)
#define wl236_nd_priv(sh)	WL236_ND(sh, WL236_NET_DEVICE_SIZEOF,		void *)
#define wl236_nd_state(sh)	WL236_ND(sh, WL236_NET_DEVICE_STATE,		unsigned long)

#define wl236_txq_state(q)	WL236_ND(q, WL236_NETDEV_QUEUE_STATE,		unsigned long)
#define wl236_txq_qdisc(q)	WL236_ND(q, WL236_NETDEV_QUEUE_QDISC,		void *)
#define wl236_txq_dev(q)	WL236_ND(q, WL236_NETDEV_QUEUE_DEV,		void *)

/* ---- API ------------------------------------------------------------ */

/*
 * Hook for the _*_bit_le shims in wl_shim_removed.c. The blob open-codes
 * netif_stop_queue()/netif_wake_queue() as bit operations on
 * ->_tx[0].state, which is why those shims - which we already own - are the
 * interception point, with no new machinery needed.
 *
 * op: 0 = set, 1 = test_and_set, 2 = test_and_clear.
 * Returns 1 if the address belonged to a shadow queue and was handled, with
 * the previous bit value in *old; 0 to fall through to the plain bit op.
 */
int wl_shadow_netdev_txq_bitop(int nr, volatile unsigned long *p, int op, int *old);

/*
 * Translate a net_device across the boundary, in either direction. Both
 * return NULL for a device that is not one of ours, so they are safe to call
 * on any pointer - which the packet paths need, since an skb's ->dev may be
 * some other interface entirely.
 */
void *wl_netdev_blob_of(struct net_device *real);
struct net_device *wl_netdev_real_of(void *blob_dev);

void wl_shadow_netdev_exit(void);

#endif /* WL_SHADOW_NETDEV_H */
