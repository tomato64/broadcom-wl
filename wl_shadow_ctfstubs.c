// SPDX-License-Identifier: GPL-2.0
/*
 * Inert definitions for Broadcom's optional acceleration modules: CTF (cut
 * through forwarding), EMF/IGS (multicast forwarding and IGMP snooping) and
 * DPSTA (proxy STA).
 *
 * The blob and vendor/shared reference these, but their implementations live
 * in separate modules whose sources FreshTomato does not ship - ctf/linux is
 * itself a prebuilt 2.6.36 object, with exactly the struct-ABI problem this
 * whole project exists to work around. Linking that in would mean shadowing
 * for a second blob.
 *
 * Leaving them out is not a compromise on correctness, because these are
 * accelerators, not required paths. Broadcom's own header gates CTF on the
 * pointer being non-NULL:
 *
 *     hndctf.h:61  (ctf_attach_fn ? ctf_attach_fn(osh, n, m, c, a) : NULL)
 *
 * so NULL here means "CTF not present" and the driver takes its ordinary slow
 * path - the same thing that happens on a build without CTF. What we must NOT
 * do is drop -DHNDCTF and friends from shadow/wlflags: those change struct
 * layout, and the blob was compiled with them. So the layout stays, the
 * feature is simply switched off.
 *
 * These replace the auto-generated weak stubs, which returned nothing for
 * everything and would have made a non-NULL-looking ctf_attach_fn.
 */

#include <linux/kernel.h>

#include <typedefs.h>
#include <osl.h>
#include <ctf/hndctf.h>

/* NULL disables CTF at every call site. */
ctf_attach_t	ctf_attach_fn;
ctf_t		*kcih;

/* Function-pointer hook the fastpath tests before calling. */
void		*dnsmq_hit_hook;

/*
 * EMF / IGS. Signatures come from the vendored headers; each returns the
 * "nothing here" value, which the callers already handle because these
 * modules are optional at runtime.
 */
struct emfc_info;
struct igsc_info;

struct emfc_info *emfc_init(int8 *inst_id, void *emfi, osl_t *osh, void *wrapper)
{
	return NULL;
}

void emfc_exit(struct emfc_info *emfc) { }

uint32 emfc_input(struct emfc_info *emfc, void *sdu, void *ifp,
		  uint8 *iph, bool rt_port)
{
	return 0;	/* EMF_NOP: let the caller forward normally */
}

void emfc_cfg_request_process(struct emfc_info *emfc, void *cfg) { }

void *igsc_init(int8 *inst_id, void *igs_info, osl_t *osh, void *wrapper)
{
	return NULL;
}

void  igsc_exit(struct igsc_info *igsc_info) { }
int32 igsc_sdb_interface_del(struct igsc_info *igsc_info, void *ifp) { return 0; }
int32 igsc_interface_rtport_del(struct igsc_info *igsc_info, void *ifp) { return 0; }

/* DPSTA (proxy STA). No vendored declaration; the blob only registers and
 * feeds it, so returning failure keeps it disabled. */
int  dpsta_register(int unit, void *info) { return -1; }
void dpsta_recv(void *skb) { }
