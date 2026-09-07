/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shadow struct pci_dev and struct pci_driver.
 *
 * The blob registers a 2.6.36-laid-out struct pci_driver, and 2.6.36 had a
 * struct list_head at the front of it:
 *
 *              2.6.36   6.12
 *   name          8       0
 *   id_table     12       4
 *   probe        16       8
 *   driver       48      52
 *   sizeof      116     144
 *
 * so __pci_register_driver() read ->node.next as ->name and handed that to
 * strcmp(). That was the second hardware crash.
 *
 * struct pci_device_id also grew, 28 -> 32 (6.12 added ->override_only), so
 * the id table has the wrong stride as well and must be rebuilt.
 *
 * And wl_pci_probe() dereferences the pci_dev it is handed - bus, devfn,
 * vendor, device, irq and resource[0].start - so it gets a shadow of that too.
 */
#ifndef WL_SHADOW_PCI_H
#define WL_SHADOW_PCI_H

#include <linux/pci.h>
#include <linux/build_bug.h>
#include "shadow/offsets-2.6.36.h"

#define WL_PCI_DEV_SHADOW_MAGIC 0x57315044u	/* "W1PD" */

struct wl_pci_dev_shadow {
	u8		s236[WL236_PCI_DEV_SIZEOF];	/* what the blob sees */
	struct pci_dev	*real;
	u32		magic;
};

static_assert(offsetof(struct wl_pci_dev_shadow, s236) == 0,
	      "s236 must be first: the blob's pointer is cast, not offset");

/*
 * The first 28 bytes of struct pci_device_id are identical in both kernels;
 * 6.12 only appends ->override_only. So translating the table is a 28-byte
 * copy per entry into a wider slot, not a field-by-field rebuild.
 */
static_assert(sizeof(struct pci_device_id) >= WL236_PCI_DEVICE_ID_SIZEOF,
	      "pci_device_id shrank; the id table translation assumes it grew");

/* Recover the real device from a pointer the blob is holding. */
struct pci_dev *wl_pci_dev_real(void *blob_pdev);

/*
 * The blob calls SET_NETDEV_DEV(dev, &pdev->dev) with its shadow, so the
 * "parent" it stores is a pointer into our zeroed shadow bytes rather than a
 * real struct device. Map it back. Takes the interior pointer, not the base.
 */
struct device *wl_pci_embedded_dev_real(void *blob_embedded_dev);

void wl_shadow_pci_exit(void);

/* See wl_shadow_debug.c. Used by the WL-SHADOW-PATCH in osl_pci_bus(). */
extern int wl_pci_domain_offset;

#endif /* WL_SHADOW_PCI_H */
