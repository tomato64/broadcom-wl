/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Interposer for Broadcom's <linux_osl.h>, so the vendored Broadcom sources compile
 * against the shadow sk_buff instead of the real one.
 *
 * This directory goes on the include path AHEAD of vendor/include, so a
 * Broadcom source doing `#include <linux_osl.h>` lands here first; the
 * #include_next below then pulls in the real header, and the overrides are
 * applied on top.
 *
 * -include cannot do this job: it inserts at the very top of the translation
 * unit, before linux_osl.h defines the macros, so the redefinitions would just
 * be overwritten.
 *
 * The problem it solves: vendor/include/linux_osl.h defines PKTDATA and
 * friends as direct struct sk_buff field accesses. Compiled with 6.12 headers
 * they read 6.12 offsets - but the pointers flowing through Broadcom's code
 * are blob-visible *shadows* in 2.6.36 layout. So linux_osl.c would disagree
 * with the blob about where ->len lives, which is precisely the bug the shadow
 * layer exists to prevent.
 *
 * The #undef before each #define is still required: #include_next has just
 * defined them all.
 *
 * Not overridden, deliberately:
 *   PKTNEXT / PKTSETNEXT   ->next, offset 0 in both kernels
 *   PKTLINK / PKTSETLINK   ->prev, offset 4 in both kernels
 * Those two are why the blob's own packet queueing (pktq_*) needs no
 * translation at all - a rare piece of luck in this exercise.
 */
#ifndef WL_BCM_PKT_OVERRIDE_H
#define WL_BCM_PKT_OVERRIDE_H

#include_next <linux_osl.h>

#include "wl_shadow_skb.h"		/* via -I$(src) */

/*
 * ioremap_nocache() was removed in 5.6 - plain ioremap() has been uncached on
 * every architecture for years. Defining it here catches both users: the
 * direct call in linux_osl.c, and the REG_MAP() macro in the real header
 * (macro bodies expand at use, so REG_MAP picks this up too).
 */
#undef  ioremap_nocache
#define ioremap_nocache(pa, size)	ioremap((pa), (size))

/*
 * The pci_* DMA API was removed in 5.18. The dma_* replacements take a
 * struct device *, and osh->pdev is a void * holding a struct pci_dev *.
 */
#include <linux/dma-mapping.h>
#include <linux/pci.h>

#define WL_PDEV_DEV(pdev)	(&((struct pci_dev *)(pdev))->dev)

#undef  PCI_DMA_TODEVICE
#define PCI_DMA_TODEVICE	DMA_TO_DEVICE
#undef  PCI_DMA_FROMDEVICE
#define PCI_DMA_FROMDEVICE	DMA_FROM_DEVICE

#undef  pci_map_single
#define pci_map_single(pdev, va, size, dir) \
	dma_map_single(WL_PDEV_DEV(pdev), (va), (size), (dir))
#undef  pci_unmap_single
#define pci_unmap_single(pdev, pa, size, dir) \
	dma_unmap_single(WL_PDEV_DEV(pdev), (pa), (size), (dir))
#undef  pci_map_sg
#define pci_map_sg(pdev, sg, n, dir) \
	dma_map_sg(WL_PDEV_DEV(pdev), (sg), (n), (dir))
#undef  pci_unmap_sg
#define pci_unmap_sg(pdev, sg, n, dir) \
	dma_unmap_sg(WL_PDEV_DEV(pdev), (sg), (n), (dir))

/*
 * kernel_read() changed shape in 4.14: it took (file, offset, buf, count) and
 * now takes (file, buf, count, &pos), advancing pos rather than the file.
 * osl_os_get_image_block() advances fp->f_pos itself afterwards, so the
 * position here is deliberately local and discarded - advancing the file too
 * would double-count.
 */
#include <linux/fs.h>
static inline int wl_compat_kernel_read(struct file *f, loff_t off,
					char *buf, unsigned long count)
{
	loff_t pos = off;

	return kernel_read(f, buf, count, &pos);
}
#undef  kernel_read
#define kernel_read(f, off, buf, count)	wl_compat_kernel_read((f), (off), (buf), (count))

#undef  PKTDATA
#define PKTDATA(osh, skb)		wl236_data(skb)

#undef  PKTLEN
#define PKTLEN(osh, skb)		wl236_len(skb)

#undef  PKTSETLEN
#define PKTSETLEN(osh, skb, len)	wl_shadow_pktsetlen((skb), (len))

#undef  PKTPUSH
#define PKTPUSH(osh, skb, bytes)	wl_shim_skb_push((skb), (bytes))

#undef  PKTPULL
#define PKTPULL(osh, skb, bytes)	wl_shim_skb_pull((skb), (bytes))

/*
 * PKTTAG is the driver's 32-byte per-packet scratch, and it lives in ->cb -
 * which is at 40 in the shadow and 24 in 6.12. Missing this override was a
 * real bug: OSL_PKTTAG_CLEAR() in linux_osl.c expands to eight stores through
 * ->cb, so on every packet crossing osl_pkt_frmnative()/osl_pkt_tonative() it
 * zeroed shadow bytes 24-55 instead of 40-71. That is ->tstamp, ->sk and
 * ->dev, and only the first half of the tag the blob actually reads.
 */
#undef  PKTTAG
#define PKTTAG(skb)			((void *)((u8 *)(skb) + WL236_SK_BUFF_CB))

/* Same field, for the sources we compile. See sync_down() in wl_shadow_skb.c. */
#undef  PKTSHARED
#define PKTSHARED(skb)			(wl236_flags1(skb) & WL236_SKB_CLONED)

#undef  PKTPRIO
#define PKTPRIO(skb)			wl236_priority(skb)

#undef  PKTSETPRIO
#define PKTSETPRIO(skb, x)		(wl236_priority(skb) = (x))

#undef  PKTHEADROOM
#define PKTHEADROOM(osh, skb)		(wl236_data(skb) - wl236_head(skb))

#undef  PKTTAILROOM
#define PKTTAILROOM(osh, skb)		(wl236_end(skb) - wl236_tail(skb))

/* 2.6.36's PKTSETLEN was __skb_trim(): set len, and pull tail back to
 * data + len. Operate on the shadow; the real skb picks it up at the next
 * sync_up(). */
static inline void wl_shadow_pktsetlen(void *blob_skb, unsigned int len)
{
	wl236_len(blob_skb)  = len;
	wl236_tail(blob_skb) = wl236_data(blob_skb) + len;
}

/*
 * FreshTomato patches extra fields into 2.6.36's sk_buff for Broadcom CTF and
 * FA, inserted immediately after ->cb - which is why enabling the CTF flags
 * shifted every offset past it. None of them exist in 6.12, so every macro
 * that names one is redirected at the shadow.
 *
 * This is also why shadow/wlflags is shared with the offsets probe: measuring
 * the struct without these defines describes a sk_buff the blob never sees.
 */
#define wl236_pktc_flags(sh)	WL236_FIELD(sh, WL236_SK_BUFF_PKTC_FLAGS,	u32)
#define wl236_ctfpool(sh)	WL236_FIELD(sh, WL236_SK_BUFF_CTFPOOL,		void *)
#define wl236_skb_sp(sh)	WL236_FIELD(sh, WL236_SK_BUFF_SP,		void *)
#define wl236_ctf_ipc_txif(sh)	WL236_FIELD(sh, WL236_SK_BUFF_CTF_IPC_TXIF,	void *)
#define wl236_napt_idx(sh)	WL236_FIELD(sh, WL236_SK_BUFF_NAPT_IDX,		u32)
#define wl236_napt_flags(sh)	WL236_FIELD(sh, WL236_SK_BUFF_NAPT_FLAGS,	u32)

/* pktc_flags */
#undef PKTSETFAST
#undef PKTCLRFAST
#undef PKTSETCTF
#undef PKTCLRCTF
#undef PKTISFAST
#undef PKTISCTF
#undef PKTFAST
#undef PKTSETSKIPCT
#undef PKTCLRSKIPCT
#undef PKTSKIPCT
#undef PKTSETCHAINED
#undef PKTCLRCHAINED
#undef PKTISCHAINED
#undef PKTSETTOBR
#undef PKTCLRTOBR
#undef PKTISTOBR
#define PKTSETFAST(osh, skb)	(wl236_pktc_flags(skb) |= FASTBUF)
#define PKTCLRFAST(osh, skb)	(wl236_pktc_flags(skb) &= ~FASTBUF)
#define PKTSETCTF(osh, skb)	(wl236_pktc_flags(skb) |= CTFBUF)
#define PKTCLRCTF(osh, skb)	(wl236_pktc_flags(skb) &= ~CTFBUF)
#define PKTISFAST(osh, skb)	(wl236_pktc_flags(skb) & FASTBUF)
#define PKTISCTF(osh, skb)	(wl236_pktc_flags(skb) & CTFBUF)
#define PKTFAST(osh, skb)	wl236_pktc_flags(skb)
#define PKTSETSKIPCT(osh, skb)	(wl236_pktc_flags(skb) |= SKIPCT)
#define PKTCLRSKIPCT(osh, skb)	(wl236_pktc_flags(skb) &= ~SKIPCT)
#define PKTSKIPCT(osh, skb)	(wl236_pktc_flags(skb) & SKIPCT)
#define PKTSETCHAINED(osh, skb)	(wl236_pktc_flags(skb) |= CHAINED)
#define PKTCLRCHAINED(osh, skb)	(wl236_pktc_flags(skb) &= ~CHAINED)
#define PKTISCHAINED(skb)	(wl236_pktc_flags(skb) & CHAINED)
#define PKTSETTOBR(osh, skb)	(wl236_pktc_flags(skb) |= TOBR)
#define PKTCLRTOBR(osh, skb)	(wl236_pktc_flags(skb) &= ~TOBR)
#define PKTISTOBR(skb)		(wl236_pktc_flags(skb) & TOBR)

/* ctfpool / secpath-as-ctfmap / ctf_ipc_txif */
#undef CTFPOOLPTR
#undef CTFPOOLHEAD
#undef CTFMAPPTR
#undef PKTSETCTFIPCTXIF
/*
 * Dormant, not live: the body only runs when PKTISCTF() is true and nothing
 * sets CTFBUF while CTF is inert. But it names ->end directly (180 in the
 * shadow, 160 in 6.12), so it is the same trap as PKTTAG was. Close it now
 * rather than discover it the day CTF is switched on.
 */
#undef PKTCTFMAP
#define PKTCTFMAP(osh, p) \
do { \
	if (PKTISCTF(osh, p)) { \
		int32 sz = (int32)((uintptr)wl236_end(p) - \
				   (uintptr)CTFMAPPTR(osh, p)); \
		if (sz > 0) { \
			sz = (sz + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1); \
			_DMA_MAP(osh, (void *)CTFMAPPTR(osh, p), sz, DMA_RX, p, NULL); \
		} \
		PKTCLRCTF(osh, p); \
		CTFMAPPTR(osh, p) = NULL; \
	} \
} while (0)

#define CTFPOOLPTR(osh, skb)		wl236_ctfpool(skb)
#define CTFPOOLHEAD(osh, skb)		(((ctfpool_t *)wl236_ctfpool(skb))->head)
#define CTFMAPPTR(osh, skb)		wl236_skb_sp(skb)
#define PKTSETCTFIPCTXIF(skb, ifp)	(wl236_ctf_ipc_txif(skb) = (ifp))

/* BCMFA napt fields */
#undef PKTSETFAHIDX
#undef PKTGETFAHIDX
#undef PKTSETFADEV
#undef PKTSETRXDEV
#undef PKTSETFAAUX
#undef PKTCLRFAAUX
#undef PKTISFAAUX
#undef PKTSETFAFREED
#undef PKTCLRFAFREED
#undef PKTISFAFREED
#undef PKTISFABRIDGED
#ifdef BCMFA_HW_HASH
#define PKTSETFAHIDX(skb, idx)	(wl236_napt_idx(skb) = (idx))
#else
#define PKTSETFAHIDX(skb, idx)
#endif
#define PKTGETFAHIDX(skb)	wl236_napt_idx(skb)
#define PKTSETFADEV(skb, imp)	(wl236_dev(skb) = (imp))
/* rxdev is absent even from FreshTomato's patched 2.6.36 header. */
#define PKTSETRXDEV(skb)
#define PKTSETFAAUX(skb)	(wl236_napt_flags(skb) |= AUX_TCP_FIN_RST)
#define PKTCLRFAAUX(skb)	(wl236_napt_flags(skb) &= ~AUX_TCP_FIN_RST)
#define PKTISFAAUX(skb)		(wl236_napt_flags(skb) & AUX_TCP_FIN_RST)
#define PKTSETFAFREED(skb)	(wl236_napt_flags(skb) |= AUX_FREED)
#define PKTCLRFAFREED(skb)	(wl236_napt_flags(skb) &= ~AUX_FREED)
#define PKTISFAFREED(skb)	(wl236_napt_flags(skb) & AUX_FREED)
#define PKTISFABRIDGED(skb)	PKTISFAAUX(skb)

/*
 * ->users is atomic_t at 196 on the blob's side and refcount_t at 176 in 6.12
 * - wrong offset AND wrong type. linux_osl.c touches it directly rather than
 * through a macro, so those few sites are patched (marked WL-SHADOW-PATCH) to
 * use this instead. `make vendor-diff` lists every such divergence.
 */
#define WL236_USERS(skb)	WL236_FIELD(skb, WL236_SK_BUFF_USERS, atomic_t)
#define WL236_TSTAMP(skb)	WL236_FIELD(skb, WL236_SK_BUFF_TSTAMP, s64)

/* PKTC chain scratch: unsigned char pktc_cb[8], immediately after ->prev. */
#undef  CHAIN_NODE
#define CHAIN_NODE(skb) \
	((struct chain_node *)((u8 *)(skb) + WL236_SK_BUFF_PKTC_CB))

/*
 * skb_to_sgvec() is handed a shadow, so it needs unwrapping regardless of
 * anything else - a wrapper was always required here. Given a wrapper, calling
 * the kernel's own implementation beats reimplementing it.
 */
#undef  skb_to_sgvec
#define skb_to_sgvec(skb, sg, off, len)	wl_shim_skb_to_sgvec((skb), (sg), (off), (len))

#endif /* WL_BCM_PKT_OVERRIDE_H */
