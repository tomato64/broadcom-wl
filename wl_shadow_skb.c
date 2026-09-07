// SPDX-License-Identifier: GPL-2.0
/*
 * Shadow layer, part 2 of 3: struct sk_buff.
 *
 * See wl_shadow_skb.h for the layout rationale and the sync policy.
 *
 * Boundary set. The blob imports exactly six skb-touching kernel functions,
 * all rerouted by objcopy --redefine-sym in the Makefile:
 *
 *   dev_alloc_skb   skb_put   skb_pull   skb_push   netif_rx   eth_type_trans
 *
 * Everything else it does to packets goes through Broadcom's own osl_ and pkt
 * helpers, which are source we compile (vendor/shared/), so those become
 * translation points for free once they are built for real instead of stubbed.
 *
 * WRAP SITES - one per packet, and only where it crosses in from the kernel:
 *
 *   wl_tramp_start_xmit()      TX: the kernel hands us a real skb
 *   wl_shim_dev_alloc_skb()    the blob allocates
 *   osl_pktget() / osl_pktdup() the driver allocates or clones
 *
 * It is tempting to put it at Broadcom's own conversion boundary instead -
 * PKTFRMNATIVE/PKTTONATIVE exist precisely to convert between a kernel skb and
 * a Broadcom "pkt", so they look like the right place. They are not. That
 * boundary sits INSIDE the blob's world: by the time the blob calls
 * PKTFRMNATIVE the packet has already crossed in through ndo_start_xmit and
 * been wrapped. Wrapping there as well produced a shadow of a shadow, whose
 * ->data read back NULL, and pktsetprio() oopsed on it. osl_pkt_frmnative()
 * now checks with wl_skb_is_shadow() instead of wrapping.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>	/* virt_addr_valid() for wl_skb_is_shadow() */
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/atomic.h>
#include <linux/scatterlist.h>
#include <linux/errno.h>

#include "wl_shadow_skb.h"
#include "wl_shadow_netdev.h"
#include "wl_shadow.h"

static atomic_t wl_skb_live = ATOMIC_INIT(0);

static inline struct wl_skb_shadow *to_shadow(void *blob_skb)
{
	struct wl_skb_shadow *sh = (struct wl_skb_shadow *)blob_skb;

	if (!sh)
		return NULL;
	if (sh->magic != WL_SKB_SHADOW_MAGIC) {
		pr_err_once("wl: %p is not an skb shadow (magic %08x)\n",
			    blob_skb, sh->magic);
		return NULL;
	}
	return sh;
}

/* ---- sync ----------------------------------------------------------- */

/*
 * real -> shadow. head/end/truesize are included because this also runs at
 * wrap time, when they must be established; afterwards they do not change.
 */
static void sync_down(struct wl_skb_shadow *sh)
{
	struct sk_buff *r = sh->real;
	void *s = sh->s236;

	wl236_len(s)       = r->len;
	wl236_data_len(s)  = r->data_len;
	wl236_mac_len(s)   = r->mac_len;
	wl236_hdr_len(s)   = r->hdr_len;
	wl236_priority(s)  = r->priority;
	wl236_protocol(s)  = r->protocol;
	wl236_truesize(s)  = r->truesize;
	wl236_head(s)      = r->head;
	wl236_data(s)      = r->data;
	wl236_tail(s)      = skb_tail_pointer(r);
	wl236_end(s)       = skb_end_pointer(r);
	wl236_sk(s)        = r->sk;

	/*
	 * ->dev must be TRANSLATED, not copied. The blob does
	 * WL_DEV_IF(skb->dev) == netdev_priv(dev) == dev + 896 on whatever it
	 * finds here, so handing it a real 6.12 net_device makes it read 448
	 * bytes inside the kernel's own struct. wl_start_txqwork() is one such
	 * reader: it takes the packet's ->dev to recover the interface.
	 *
	 * Left alone when the real skb has no dev, so a value the blob wrote
	 * itself survives - that is the RX case, where the blob sets its own
	 * shadow device on a packet it allocated.
	 */
	if (r->dev)
		wl236_dev(s) = wl_netdev_blob_of(r->dev);

	/*
	 * ->cloned must be mirrored, and missing it was a real bug.
	 *
	 * Broadcom's PKTSHARED() is `((struct sk_buff *)skb)->cloned` inlined
	 * into the blob, and the driver consults it before editing a packet in
	 * place - the same contract skb_cloned() expresses. The shadow never
	 * carried the bit, so the blob saw every packet as exclusively its own
	 * and edited buffers it shared with someone else.
	 *
	 * What that broke: an AP must echo group-addressed frames back out to
	 * the other stations, so on receiving one it PKTDUPs it - osl_pktdup()
	 * is skb_clone(), which shares the data buffer - and converts the copy
	 * back to 802.11 for transmit. That conversion rewrites the 802.11
	 * header and LLC/SNAP straight over the ethernet header the receive
	 * path had just written into the same bytes. The frame handed to the
	 * stack then had an intact SNAP where its source address belonged.
	 *
	 * Only this one bit is mirrored. ip_summed and nohdr share the byte and
	 * are read-modify-written around, deliberately: they are the blob's to
	 * set, and checksum offload being unmirrored costs nothing here.
	 */
	if (skb_cloned(r))
		wl236_flags1(s) |= WL236_SKB_CLONED;
	else
		wl236_flags1(s) &= ~WL236_SKB_CLONED;
}

/*
 * shadow -> real. head/end are omitted on purpose: both point into the same
 * allocation and are fixed for its lifetime, so a write here could only ever
 * be corruption. len/data/tail are the ones the blob really moves, directly,
 * via the inlined __skb_trim() behind PKTSETLEN.
 */
static void sync_up(struct wl_skb_shadow *sh)
{
	struct sk_buff *r = sh->real;
	void *s = sh->s236;

	if (wl236_head(s) != r->head) {
		pr_err_once("wl: shadow %p head moved (%p -> %p); refusing sync\n",
			    s, r->head, wl236_head(s));
		return;
	}

	r->len      = wl236_len(s);
	r->data_len = wl236_data_len(s);
	r->mac_len  = wl236_mac_len(s);
	r->hdr_len  = wl236_hdr_len(s);
	r->priority = wl236_priority(s);
	r->protocol = wl236_protocol(s);
	r->data     = wl236_data(s);
	skb_set_tail_pointer(r, wl236_tail(s) - r->data);
	r->len      = wl236_len(s);	/* set_tail_pointer does not touch len */
}

/* ---- identity -------------------------------------------------------- */

/*
 * Is this pointer one of our shadows rather than a real sk_buff?
 *
 * Needed because osl_pkt_frmnative() has to tell the two apart: wrapping an
 * already-wrapped packet is what oopsed wl_start_txqwork(). The magic sits at
 * offset 228, past the end of a 6.12 sk_buff, so the read has to be bounded -
 * virt_addr_valid() on both the first and last byte keeps it inside the linear
 * map whatever was passed in.
 */
bool wl_skb_is_shadow(const void *p)
{
	const struct wl_skb_shadow *sh = p;

	if (!p || !IS_ALIGNED((unsigned long)p, __alignof__(struct wl_skb_shadow)))
		return false;
	if (!virt_addr_valid(p) ||
	    !virt_addr_valid((const u8 *)p + sizeof(*sh) - 1))
		return false;

	return sh->magic == WL_SKB_SHADOW_MAGIC;
}

/* ---- wrap / unwrap -------------------------------------------------- */

void *wl_skb_wrap(struct sk_buff *real)
{
	struct wl_skb_shadow *sh;

	if (!real)
		return NULL;

	/* GFP_ATOMIC: this runs on the receive and transmit paths. */
	sh = kzalloc(sizeof(*sh), GFP_ATOMIC);
	if (!sh) {
		pr_err_once("wl: no memory for skb shadow\n");
		return NULL;
	}

	sh->real  = real;
	sh->magic = WL_SKB_SHADOW_MAGIC;
	sync_down(sh);
	atomic_inc(&wl_skb_live);

	return sh->s236;
}

struct sk_buff *wl_skb_unwrap(void *blob_skb)
{
	struct wl_skb_shadow *sh = to_shadow(blob_skb);

	if (!sh)
		return NULL;
	sync_up(sh);
	return sh->real;
}

void wl_skb_sync_to_blob(void *blob_skb)
{
	struct wl_skb_shadow *sh = to_shadow(blob_skb);

	if (sh)
		sync_down(sh);
}

void wl_skb_shadow_put(void *blob_skb)
{
	struct wl_skb_shadow *sh = to_shadow(blob_skb);

	if (!sh)
		return;
	sh->magic = 0;
	atomic_dec(&wl_skb_live);
	kfree(sh);
}

/* ---- the six rerouted entry points ---------------------------------- */

void *wl_shim_dev_alloc_skb(unsigned int length)
{
	struct sk_buff *real;
	void *blob_skb;

	real = __netdev_alloc_skb(NULL, length, GFP_ATOMIC);
	if (!real)
		return NULL;

	blob_skb = wl_skb_wrap(real);
	if (!blob_skb) {
		kfree_skb(real);
		return NULL;
	}
	return blob_skb;
}

/* Record a header move for later, rather than printing it now. */
#ifdef WL_SKB_OPS_TRACE
static void wl_skb_note_op(struct wl_skb_shadow *sh, bool push, unsigned int len)
{
	struct sk_buff *r = sh->real;

	if (sh->nops >= WL_SKB_OPS)
		return;
	sh->ops[sh->nops].push = push;
	sh->ops[sh->nops].len  = min_t(unsigned int, len, 255);
	sh->ops[sh->nops].off  = (u16)(r->data - r->head);
	sh->nops++;
}
#else
static inline void wl_skb_note_op(struct wl_skb_shadow *sh, bool push,
				  unsigned int len) { }
#endif

void *wl_shim_skb_put(void *blob_skb, unsigned int len)
{
	struct wl_skb_shadow *sh = to_shadow(blob_skb);
	void *ret;

	if (!sh)
		return NULL;
	sync_up(sh);
	ret = skb_put(sh->real, len);
	sync_down(sh);
	return ret;
}

void *wl_shim_skb_push(void *blob_skb, unsigned int len)
{
	struct wl_skb_shadow *sh = to_shadow(blob_skb);
	void *ret;

	if (!sh)
		return NULL;
	sync_up(sh);
	ret = skb_push(sh->real, len);
	sync_down(sh);
	wl_skb_note_op(sh, true, len);
	return ret;
}

void *wl_shim_skb_pull(void *blob_skb, unsigned int len)
{
	struct wl_skb_shadow *sh = to_shadow(blob_skb);
	void *ret;

	if (!sh)
		return NULL;
	sync_up(sh);
	ret = skb_pull(sh->real, len);
	sync_down(sh);
	wl_skb_note_op(sh, false, len);
	return ret;
}

/*
 * netif_rx() consumes the skb, so the wrapper dies here and the blob's pointer
 * is dangling on return - which matches 2.6.36 semantics, where the blob also
 * must not touch an skb after handing it up.
 */
int wl_shim_netif_rx(void *blob_skb)
{
	struct wl_skb_shadow *sh = to_shadow(blob_skb);
	struct sk_buff *real;

	if (!sh)
		return NET_RX_DROP;

	sync_up(sh);
	real = sh->real;

	/*
	 * Dump the frame exactly as the stack will see it. eth_type_trans() has
	 * already pulled the 14-byte ethernet header by now, so print from the
	 * mac header - that is what the bridge reads to learn a source address,
	 * and reading it here is the only way to see why it learned an LLC/SNAP
	 * header as one.
	 */
	if (wl_rx_trace > 0) {
		const u8 *m = skb_mac_header_was_set(real)
			      ? skb_mac_header(real) : real->data;
		wl_rx_trace--;
#ifdef WL_SKB_OPS_TRACE
		{
			char ops[WL_SKB_OPS * 16 + 1];
			int n = 0, i;

			for (i = 0; i < sh->nops; i++)
				n += scnprintf(ops + n, sizeof(ops) - n,
					       " %s%u@%u",
					       sh->ops[i].push ? "+" : "-",
					       sh->ops[i].len, sh->ops[i].off);
			ops[n] = '\0';
			printk(KERN_INFO "wl: rxops%s\n", n ? ops : " (none)");
		}
#endif
		printk(KERN_INFO "wl: rx %s len=%u proto=%04x mac_set=%d "
		       "%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x "
		       "%02x%02x | %02x%02x%02x%02x%02x%02x%02x%02x\n",
		       real->dev ? real->dev->name : "(nodev)",
		       real->len, ntohs(real->protocol),
		       skb_mac_header_was_set(real) ? 1 : 0,
		       m[0], m[1], m[2], m[3], m[4], m[5],
		       m[6], m[7], m[8], m[9], m[10], m[11],
		       m[12], m[13],
		       m[14], m[15], m[16], m[17], m[18], m[19], m[20], m[21]);
	}

	wl_skb_shadow_put(blob_skb);

	return netif_rx(real);
}

/*
 * The blob passes its own net_device, which is a 2.6.36-layout shadow. This
 * used to be a known gap - the shadow was ignored and the real skb's ->dev
 * used instead - because the net_device layer did not exist yet. It does now,
 * so translate properly: eth_type_trans() sets skb->dev itself, and on RX the
 * blob allocated the packet, so the real skb's ->dev is still NULL and the
 * blob's argument is the only statement of which interface received it.
 */
__be16 wl_shim_eth_type_trans(void *blob_skb, void *blob_dev)
{
	struct wl_skb_shadow *sh = to_shadow(blob_skb);
	struct net_device *dev;
	__be16 proto;

	if (!sh)
		return htons(ETH_P_802_2);

	dev = wl_netdev_real_of(blob_dev);
	if (!dev) {
		/* Not one of our shadows. Fall back to whatever the real skb
		 * carries rather than guessing. */
		dev = sh->real->dev;
		pr_warn_once("wl: eth_type_trans: %p is not a net_device shadow; "
			     "falling back to the skb's own dev %p\n",
			     blob_dev, dev);
	}
	if (!dev) {
		pr_warn_once("wl: eth_type_trans with no usable dev\n");
		return htons(ETH_P_802_2);
	}

	sync_up(sh);
	proto = eth_type_trans(sh->real, dev);
	sync_down(sh);
	return proto;
}

/*
 * Scatter-gather DMA mapping. linux_osl.c calls this on a shadow, so the
 * unwrap is mandatory - passing the shadow straight to the kernel would have
 * it read fragment state at 2.6.36 offsets.
 */
int wl_shim_skb_to_sgvec(void *blob_skb, struct scatterlist *sg,
			 int offset, int len)
{
	struct sk_buff *real = wl_skb_unwrap(blob_skb);

	if (!real)
		return -EINVAL;
	return skb_to_sgvec(real, sg, offset, len);
}

/* ---- lifecycle ------------------------------------------------------ */

void wl_shadow_skb_exit(void)
{
	int live = atomic_read(&wl_skb_live);

	/*
	 * Shadows are freed at netif_rx() or by whoever called wl_skb_wrap().
	 * A non-zero count at unload means some path leaks one - most likely a
	 * free path still going through a stubbed osl_pktfree(). Report rather
	 * than sweep: there is no registry to walk, by design (recovering the
	 * wrapper is a cast, so no hash table is needed).
	 */
	if (live)
		pr_warn("wl: %d skb shadow(s) still live at unload\n", live);
}
