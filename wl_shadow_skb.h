/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shadow struct sk_buff.
 *
 * The blob reads and writes sk_buff fields at 2.6.36 offsets, inlined, via
 * Broadcom's PKT* macros in vendor/include/linux_osl.h:
 *
 *   PKTDATA/PKTLEN      direct reads of ->data / ->len          (both MOVED)
 *   PKTSETLEN           __skb_trim(), a 2.6.36 static inline that
 *                       writes ->len and ->tail directly        (both MOVED)
 *   PKTPRIO/PKTSETPRIO  direct access to ->priority             (MOVED)
 *   PKTHEADROOM         PKTDATA - ->head                        (both MOVED)
 *   PKTNEXT/PKTSETNEXT  ->next   (offset 0 in BOTH - no work needed)
 *   PKTLINK/PKTSETLINK  ->prev   (offset 4 in BOTH - no work needed)
 *
 * So the blob cannot be handed a real 6.12 skb. It gets a shadow in 2.6.36
 * layout instead, sized by WL236_SK_BUFF_SIZEOF, and we translate at the
 * boundaries.
 *
 * CORRECTION: this originally claimed sizeof was 184 in both kernels. It is
 * not. FreshTomato patches CTF and FA fields into 2.6.36's sk_buff directly
 * after ->cb, so with the real build flags the blob's sk_buff is 224 bytes and
 * every offset past ->cb shifts. The first measurement was taken without those
 * defines and described a struct the blob never sees. Nothing here hardcodes a
 * number - the size and every offset come from shadow/offsets-2.6.36.h - so
 * regenerating with the correct flags fixed this automatically.
 *
 * The shadow and the real skb alias the SAME data buffer - only the metadata is
 * duplicated. head/end/truesize are set once at wrap time; len/data/tail and
 * friends are synced in both directions at each crossing.
 *
 * Deliberately NOT synced:
 *   next, prev   the blob queues shadows on its own lists (pktq_*, PKTLINK)
 *                while the kernel queues the real skb on its own. Same
 *                offsets, opposite owners - syncing would corrupt both.
 *   cb           the blob's per-packet scratch; the kernel's qdisc/layer
 *                state. Independent by design. (Also once claimed to sit at
 *                offset 24 in both - it does not, it is at 40 on the blob's
 *                side. Independence is why that never mattered.)
 *   users        atomic_t at 156 vs refcount_t at 176. Different types with
 *                different semantics; refcounting stays entirely on the real
 *                skb.
 *
 * Synced one way only:
 *   cloned       real -> shadow. The blob reads it through PKTSHARED() before
 *                editing a packet in place; it never sets it. Leaving it out
 *                corrupted every group-addressed received frame - see
 *                sync_down() and TESTING.md.
 *   destructor   kernel-owned.
 */
#ifndef WL_SHADOW_SKB_H
#define WL_SHADOW_SKB_H

#include <linux/skbuff.h>
#include <linux/build_bug.h>
#include <linux/types.h>
#include <linux/stddef.h>
#include "shadow/offsets-2.6.36.h"

/*
 * The generated offset table is for 32-bit ARM. On any other word size every
 * number in it is wrong, and silently so - guard rather than corrupt.
 */
static_assert(BITS_PER_LONG == 32,
	      "shadow/offsets-2.6.36.h is 32-bit ARM only");

#define WL_SKB_SHADOW_MAGIC 0x5731534Bu	/* "W1SK" */

/*
 * s236 must stay at offset 0: the blob only ever holds a pointer to it, and
 * recovering the wrapper is a plain cast.
 */
/*
 * A recorded skb_push()/skb_pull() on this packet, for chasing receive-path
 * bugs. Tracing those calls as they happen is useless: beacons dominate both
 * directions at 100ms intervals per radio and swamp any budget long before a
 * client frame appears. Recording them per-packet and dumping only for frames
 * that reach netif_rx() shows the blob's walk from 802.11 frame to ethernet
 * frame with no noise at all - which is how the ->cloned bug was found.
 *
 * Compile-time, off by default: `make WL_SKB_OPS_TRACE=1`. The ring lives in
 * every shadow, and carrying it pushes the allocation out of kmalloc-256 into
 * kmalloc-512 - doubling the per-packet footprint on both hot paths. That is
 * the only piece of the instrumentation with a cost worth caring about; the
 * dma_trace and rx_trace switches stay compiled in and cost a branch.
 */
#ifdef WL_SKB_OPS_TRACE
struct wl_skb_op {
	u8	push;		/* 1 = push, 0 = pull */
	u8	len;
	u16	off;		/* data offset within the buffer afterwards */
};

#define WL_SKB_OPS	10
#endif

struct wl_skb_shadow {
	u8			s236[WL236_SK_BUFF_SIZEOF];
	struct sk_buff		*real;
	u32			magic;
#ifdef WL_SKB_OPS_TRACE
	u8			nops;
	struct wl_skb_op	ops[WL_SKB_OPS];
#endif
};

/*
 * Guard the slab bucket. Without the op ring this must stay inside kmalloc-256,
 * because it is allocated once per packet on both hot paths. If a future field
 * pushes it over, that is a doubling of per-packet memory and should be a
 * deliberate decision, not a surprise.
 */
#ifndef WL_SKB_OPS_TRACE
static_assert(sizeof(struct wl_skb_shadow) <= 256,
	      "skb shadow no longer fits the kmalloc-256 slab; "
	      "per-packet footprint would double");
#endif

/* Recovering the wrapper from the blob's pointer is a plain cast, so s236 must
 * sit at offset 0 and be exactly one 2.6.36 sk_buff wide. */
static_assert(offsetof(struct wl_skb_shadow, s236) == 0,
	      "s236 must be first: wl_skb_unwrap() casts rather than offsets");
static_assert(sizeof(((struct wl_skb_shadow *)0)->s236) == WL236_SK_BUFF_SIZEOF,
	      "shadow must be exactly one 2.6.36 sk_buff wide");

/*
 * The flags1 bitfield byte. `offsetof` cannot address a bitfield, so this is
 * derived: 2.6.36's sk_buff has
 *
 *     __u32 priority;                                    <- probed, 136
 *     kmemcheck_bitfield_begin(flags1);                  <- zero-sized
 *     __u8  local_df:1, cloned:1, ip_summed:2, nohdr:1, nfctinfo:3;
 *     __u8  pkt_type:3, fclone:2, ipvs_property:1, peeked:1, nf_trace:1;
 *     kmemcheck_bitfield_end(flags1);
 *     __be16 protocol;                                   <- probed, 142
 *
 * so the two flag bytes sit exactly between priority and protocol. The
 * static_assert below is a real check of that, not decoration: if either
 * probed offset ever moves, the derivation stops compiling rather than
 * silently addressing the wrong byte. GCC allocates bitfields from the least
 * significant bit on little-endian ARM, so cloned is bit 1.
 */
#define WL236_SK_BUFF_FLAGS1	(WL236_SK_BUFF_PRIORITY + 4)
#define WL236_SKB_CLONED	0x02

static_assert(WL236_SK_BUFF_FLAGS1 + 2 == WL236_SK_BUFF_PROTOCOL,
	      "flags1/flags2 do not sit between priority and protocol; "
	      "the cloned-bit derivation is wrong");

/* ---- typed access into the 2.6.36-layout shadow --------------------- */

#define WL236_FIELD(sh, off, type)	(*(type *)((u8 *)(sh) + (off)))

#define wl236_len(sh)		WL236_FIELD(sh, WL236_SK_BUFF_LEN,	unsigned int)
#define wl236_data_len(sh)	WL236_FIELD(sh, WL236_SK_BUFF_DATA_LEN,	unsigned int)
#define wl236_mac_len(sh)	WL236_FIELD(sh, WL236_SK_BUFF_MAC_LEN,	u16)
#define wl236_hdr_len(sh)	WL236_FIELD(sh, WL236_SK_BUFF_HDR_LEN,	u16)
#define wl236_priority(sh)	WL236_FIELD(sh, WL236_SK_BUFF_PRIORITY,	u32)
#define wl236_protocol(sh)	WL236_FIELD(sh, WL236_SK_BUFF_PROTOCOL,	__be16)
#define wl236_truesize(sh)	WL236_FIELD(sh, WL236_SK_BUFF_TRUESIZE,	unsigned int)
#define wl236_head(sh)		WL236_FIELD(sh, WL236_SK_BUFF_HEAD,	unsigned char *)
#define wl236_data(sh)		WL236_FIELD(sh, WL236_SK_BUFF_DATA,	unsigned char *)
#define wl236_tail(sh)		WL236_FIELD(sh, WL236_SK_BUFF_TAIL,	unsigned char *)
#define wl236_end(sh)		WL236_FIELD(sh, WL236_SK_BUFF_END,	unsigned char *)
#define wl236_dev(sh)		WL236_FIELD(sh, WL236_SK_BUFF_DEV,	void *)
#define wl236_sk(sh)		WL236_FIELD(sh, WL236_SK_BUFF_SK,	void *)
#define wl236_flags1(sh)	WL236_FIELD(sh, WL236_SK_BUFF_FLAGS1,	u8)

/* ---- the shadow API ------------------------------------------------- */

/*
 * True if this pointer is one of our shadows rather than a real sk_buff.
 * Safe on any pointer. The single wrap site per packet is the kernel boundary
 * - wl_tramp_start_xmit() on TX, wl_shim_dev_alloc_skb() and osl_pktget() for
 * packets the driver allocates - so anything further in should already be a
 * shadow, and this is how the deeper layers check rather than assume.
 */
bool wl_skb_is_shadow(const void *p);

/* Wrap a real skb and return the pointer the blob should see, or NULL. */
void *wl_skb_wrap(struct sk_buff *real);

/* Recover the real skb from a blob-visible pointer, pushing the blob's
 * metadata into it first. NULL if the pointer is not a shadow. */
struct sk_buff *wl_skb_unwrap(void *blob_skb);

/* Refresh the shadow from the real skb (kernel-side change -> blob). */
void wl_skb_sync_to_blob(void *blob_skb);

/* Release the wrapper. Does NOT touch the real skb - the caller decides
 * whether that is freed, consumed by the stack, or still in flight. */
void wl_skb_shadow_put(void *blob_skb);

void wl_shadow_skb_exit(void);

/*
 * The objcopy reroute targets. Declared here rather than in wl_shadow.h
 * because shadow/bcm-override/linux_osl.h redirects Broadcom's PKTPUSH and
 * PKTPULL onto them, and that header includes this one.
 */
void   *wl_shim_dev_alloc_skb(unsigned int length);
void   *wl_shim_skb_put(void *blob_skb, unsigned int len);
void   *wl_shim_skb_push(void *blob_skb, unsigned int len);
void   *wl_shim_skb_pull(void *blob_skb, unsigned int len);
int     wl_shim_netif_rx(void *blob_skb);
__be16  wl_shim_eth_type_trans(void *blob_skb, void *blob_dev);

struct scatterlist;
int     wl_shim_skb_to_sgvec(void *blob_skb, struct scatterlist *sg,
			     int offset, int len);

#endif /* WL_SHADOW_SKB_H */
