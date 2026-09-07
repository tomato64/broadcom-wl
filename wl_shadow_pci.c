// SPDX-License-Identifier: GPL-2.0
/*
 * Shadow layer, part 4: struct pci_driver and struct pci_dev.
 *
 * See wl_shadow_pci.h for the layout rationale.
 *
 * Rerouted by objcopy --redefine-sym in the Makefile:
 *   __pci_register_driver  pci_unregister_driver
 *   pci_enable_device      pci_disable_device      pci_set_master
 *
 * There is exactly one wl PCI driver, so a single static registration is
 * enough; a second one is refused loudly rather than silently mishandled.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/errno.h>

#include "wl_shadow_pci.h"
#include "wl_shadow.h"

/* ---- the blob's 2.6.36 struct pci_driver ---------------------------- */

#define B236(p, off, type)	(*(type *)((u8 *)(p) + (off)))

#define bdrv_name(p)		B236(p, WL236_PCI_DRIVER_NAME,		const char *)
#define bdrv_id_table(p)	B236(p, WL236_PCI_DRIVER_ID_TABLE,	const void *)
#define bdrv_probe(p)		B236(p, WL236_PCI_DRIVER_PROBE,		void *)
#define bdrv_remove(p)		B236(p, WL236_PCI_DRIVER_REMOVE,	void *)
#define bdrv_shutdown(p)	B236(p, WL236_PCI_DRIVER_SHUTDOWN,	void *)

typedef int  (*blob_probe_fn)(void *pdev, const void *id);
typedef void (*blob_void_fn)(void *pdev);

static struct {
	void			*blob_drv;
	struct pci_driver	real;
	struct pci_device_id	*ids;		/* translated table */
	unsigned int		nids;
} wl_pci;

/* ---- pci_dev shadow -------------------------------------------------- */

static struct wl_pci_dev_shadow *pdev_shadow_of(void *blob_pdev)
{
	struct wl_pci_dev_shadow *sh = (struct wl_pci_dev_shadow *)blob_pdev;

	if (!sh)
		return NULL;
	if (sh->magic != WL_PCI_DEV_SHADOW_MAGIC) {
		pr_err_once("wl: %p is not a pci_dev shadow (magic %08x)\n",
			    blob_pdev, sh->magic);
		return NULL;
	}
	return sh;
}

struct pci_dev *wl_pci_dev_real(void *blob_pdev)
{
	struct wl_pci_dev_shadow *sh = pdev_shadow_of(blob_pdev);

	return sh ? sh->real : NULL;
}

struct device *wl_pci_embedded_dev_real(void *blob_embedded_dev)
{
	struct wl_pci_dev_shadow *sh;

	if (!blob_embedded_dev)
		return NULL;
	/* The blob formed this as shadow + offsetof(2.6.36 pci_dev, dev); the
	 * magic check below validates that the subtraction landed on one. */
	sh = pdev_shadow_of((u8 *)blob_embedded_dev - WL236_PCI_DEV_DEV);
	return sh ? &sh->real->dev : NULL;
}

/*
 * Populate the six fields wl_pci_probe() actually reads. Established by
 * disassembly, not guesswork - see shadow/netdev-findings.md for the method.
 *
 * ->bus is passed through as the real pointer. The blob only hands it to
 * printk on the pci_enable_device failure path; if it ever dereferenced
 * ->bus->number the number would be wrong, which is cosmetic and confined to
 * an error message.
 */
static struct wl_pci_dev_shadow *pdev_shadow_new(struct pci_dev *real)
{
	struct wl_pci_dev_shadow *sh;

	sh = kzalloc(sizeof(*sh), GFP_KERNEL);
	if (!sh)
		return NULL;

	sh->real  = real;
	sh->magic = WL_PCI_DEV_SHADOW_MAGIC;

	B236(sh->s236, WL236_PCI_DEV_BUS,    void *)   = real->bus;
	B236(sh->s236, WL236_PCI_DEV_DEVFN,  unsigned int) = real->devfn;
	B236(sh->s236, WL236_PCI_DEV_VENDOR, u16)      = real->vendor;
	B236(sh->s236, WL236_PCI_DEV_DEVICE, u16)      = real->device;
	B236(sh->s236, WL236_PCI_DEV_IRQ,    unsigned int) = real->irq;

	/* resource[0], the register BAR. struct resource also differs, so copy
	 * the three members the blob could reach rather than the struct. */
	B236(sh->s236, WL236_PCI_DEV_RESOURCE + WL236_RESOURCE_START, u32) =
		(u32)pci_resource_start(real, 0);
	B236(sh->s236, WL236_PCI_DEV_RESOURCE + WL236_RESOURCE_END, u32) =
		(u32)pci_resource_end(real, 0);
	B236(sh->s236, WL236_PCI_DEV_RESOURCE + WL236_RESOURCE_FLAGS, unsigned long) =
		pci_resource_flags(real, 0);

	return sh;
}

/* ---- trampolines ----------------------------------------------------- */

static int wl_pci_probe_tramp(struct pci_dev *real,
			      const struct pci_device_id *id)
{
	struct wl_pci_dev_shadow *sh;
	blob_probe_fn fn = (blob_probe_fn)bdrv_probe(wl_pci.blob_drv);
	const void *blob_id = NULL;
	int ret;

	if (!fn)
		return -ENODEV;

	sh = pdev_shadow_new(real);
	if (!sh)
		return -ENOMEM;

	/* Hand the blob the matching entry from its OWN table, at the same
	 * index as the 6.12 entry the core matched. */
	if (id && wl_pci.ids && wl_pci.nids) {
		unsigned int i = id - wl_pci.ids;

		if (i < wl_pci.nids)
			blob_id = (const u8 *)bdrv_id_table(wl_pci.blob_drv) +
				  i * WL236_PCI_DEVICE_ID_SIZEOF;
	}

	pci_set_drvdata(real, sh);

	ret = fn(sh->s236, blob_id);
	if (ret) {
		pci_set_drvdata(real, NULL);
		kfree(sh);
	}
	return ret;
}

static void wl_pci_remove_tramp(struct pci_dev *real)
{
	struct wl_pci_dev_shadow *sh = pci_get_drvdata(real);
	blob_void_fn fn = (blob_void_fn)bdrv_remove(wl_pci.blob_drv);

	if (fn && sh)
		fn(sh->s236);
	pci_set_drvdata(real, NULL);
	kfree(sh);
}

static void wl_pci_shutdown_tramp(struct pci_dev *real)
{
	struct wl_pci_dev_shadow *sh = pci_get_drvdata(real);
	blob_void_fn fn = (blob_void_fn)bdrv_shutdown(wl_pci.blob_drv);

	if (fn && sh)
		fn(sh->s236);
}

/* ---- id table -------------------------------------------------------- */

/*
 * Rebuild the blob's table at 6.12's wider stride. The terminator is an
 * all-zero entry, which is how the length is found.
 */
static int build_id_table(const void *blob_tbl)
{
	const u8 *p = blob_tbl;
	unsigned int n = 0, i;

	if (!blob_tbl)
		return -EINVAL;

	for (;;) {
		const u32 *w = (const u32 *)(p + n * WL236_PCI_DEVICE_ID_SIZEOF);
		unsigned int j, zero = 1;

		for (j = 0; j < WL236_PCI_DEVICE_ID_SIZEOF / 4; j++)
			if (w[j]) { zero = 0; break; }
		if (zero)
			break;
		if (++n > 256) {
			pr_err("wl: pci id table has no terminator\n");
			return -EINVAL;
		}
	}

	/* +1 for the terminator, which kzalloc leaves zeroed. */
	wl_pci.ids = kcalloc(n + 1, sizeof(*wl_pci.ids), GFP_KERNEL);
	if (!wl_pci.ids)
		return -ENOMEM;

	for (i = 0; i < n; i++)
		memcpy(&wl_pci.ids[i], p + i * WL236_PCI_DEVICE_ID_SIZEOF,
		       WL236_PCI_DEVICE_ID_SIZEOF);

	wl_pci.nids = n;
	pr_info("wl: translated %u pci id(s) to the 6.12 layout\n", n);
	return 0;
}

/* ---- rerouted entry points ------------------------------------------ */

int wl_shim___pci_register_driver(void *blob_drv, struct module *owner,
				  const char *mod_name)
{
	int err;

	if (wl_pci.blob_drv) {
		pr_err("wl: a second pci_driver was registered; not supported\n");
		return -EBUSY;
	}
	if (!blob_drv)
		return -EINVAL;

	wl_pci.blob_drv = blob_drv;

	err = build_id_table(bdrv_id_table(blob_drv));
	if (err) {
		wl_pci.blob_drv = NULL;
		return err;
	}

	wl_pci.real.name      = bdrv_name(blob_drv);
	wl_pci.real.id_table  = wl_pci.ids;
	wl_pci.real.probe     = wl_pci_probe_tramp;
	wl_pci.real.remove    = wl_pci_remove_tramp;
	if (bdrv_shutdown(blob_drv))
		wl_pci.real.shutdown = wl_pci_shutdown_tramp;

	/*
	 * suspend/resume are deliberately not forwarded. 2.6.36's took a
	 * pm_message_t by value and the whole PM model has been replaced since;
	 * this router does not suspend, and a wrong signature here would be a
	 * silent stack mismatch rather than a missing feature.
	 */

	pr_info("wl: registering pci driver '%s'\n",
		wl_pci.real.name ? wl_pci.real.name : "(null)");

	err = __pci_register_driver(&wl_pci.real, owner, mod_name);
	if (err) {
		kfree(wl_pci.ids);
		wl_pci.ids = NULL;
		wl_pci.blob_drv = NULL;
	}
	return err;
}

void wl_shim_pci_unregister_driver(void *blob_drv)
{
	if (!wl_pci.blob_drv)
		return;
	pci_unregister_driver(&wl_pci.real);
	kfree(wl_pci.ids);
	wl_pci.ids = NULL;
	wl_pci.nids = 0;
	wl_pci.blob_drv = NULL;
}

/* The blob calls these with its shadow; unwrap to the real device. */
int wl_shim_pci_enable_device(void *blob_pdev)
{
	struct pci_dev *real = wl_pci_dev_real(blob_pdev);

	return real ? pci_enable_device(real) : -ENODEV;
}

void wl_shim_pci_disable_device(void *blob_pdev)
{
	struct pci_dev *real = wl_pci_dev_real(blob_pdev);

	if (real)
		pci_disable_device(real);
}

void wl_shim_pci_set_master(void *blob_pdev)
{
	struct pci_dev *real = wl_pci_dev_real(blob_pdev);

	if (real)
		pci_set_master(real);
}

void wl_shadow_pci_exit(void)
{
	kfree(wl_pci.ids);
	wl_pci.ids = NULL;
	wl_pci.blob_drv = NULL;
}
