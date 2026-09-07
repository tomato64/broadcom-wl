/*
 * wldiag - dump what the wl driver actually thinks its state is.
 *
 * wlconf exits 0 even when individual ioctls fail, so "wlconf up OK" says
 * very little. This asks the driver directly, through the same private-ioctl
 * path (libshared's wl_ioctl -> SIOCDEVPRIVATE -> ndo_siocdevprivate ->
 * the net_device shadow -> the blob).
 *
 * FreshTomato's own `wl` tool would do this, but it ships prebuilt against
 * uClibc and its source is not in the tree, so this covers the subset that
 * matters for bring-up.
 *
 *	wldiag <ifname> [ioctl <cmd> | iovar <name>]
 *
 * With no subcommand it prints the full state dump.
 */

#include <typedefs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <bcmnvram.h>
#include <bcmutils.h>
#include <shutils.h>
#include <wlutils.h>
#include <wlioctl.h>

/*
 * The driver maps every BCME_* error except BCME_UNSUPPORTED onto -EINVAL, so
 * "Invalid argument" from an ioctl means "the driver said no" and nothing
 * more. The real reason is still available: the driver keeps the last BCME_*
 * code in its "bcmerror" iovar, which is how wlconf tells a range error from a
 * genuine failure. Read it back and every "Invalid argument" becomes a
 * diagnosis.
 */
static const char *bcme_str(int err)
{
	static const char *tbl[] = {
		"OK", "ERROR", "BADARG", "BADOPTION", "NOTUP", "NOTDOWN",
		"NOTAP", "NOTSTA", "BADKEYIDX", "RADIOOFF", "NOTBANDLOCKED",
		"NOCLK", "BADRATESET", "BADBAND", "BUFTOOSHORT", "BUFTOOLONG",
		"BUSY", "NOTASSOCIATED", "BADSSIDLEN", "OUTOFRANGECHAN",
		"BADCHAN", "BADADDR", "NORESOURCE", "UNSUPPORTED", "BADLEN",
		"NOTREADY", "EPERM", "NOMEM", "ASSOCIATED", "RANGE",
		"NOTFOUND", "WME_NOT_ENABLED", "TSPEC_NOTFOUND",
		"ACM_NOTSUPPORTED", "NOT_WME_ASSOCIATION", "SDIO_ERROR",
		"DONGLE_DOWN", "VERSION", "TXFAIL", "RXFAIL", "NODEVICE",
		"NMODE_DISABLED", "NONRESIDENT", "SCANREJECT",
	};
	int i = -err;

	if (i < 0 || i >= (int)(sizeof(tbl) / sizeof(tbl[0])))
		return "?";
	return tbl[i];
}

static char *ifname_for_bcmerror;

static const char *e(int ret)
{
	static char buf[96];
	int bcmerr = 0;
	int saved = errno;

	if (ret == 0)
		return "ok";

	/*
	 * Ask before anything else can overwrite it - this get is itself an
	 * ioctl, but a successful one does not disturb the stored code.
	 */
	if (ifname_for_bcmerror &&
	    wl_iovar_getint(ifname_for_bcmerror, "bcmerror", &bcmerr) == 0 && bcmerr)
		snprintf(buf, sizeof(buf), "FAIL(%s; BCME_%s %d)",
			 strerror(saved), bcme_str(bcmerr), bcmerr);
	else
		snprintf(buf, sizeof(buf), "FAIL(%s)", strerror(saved));
	return buf;
}

static void p_int(char *ifname, const char *label, int cmd)
{
	int val = 0, ret;

	ret = wl_ioctl(ifname, cmd, &val, sizeof(val));
	if (ret)
		printf("  %-18s %s\n", label, e(ret));
	else
		printf("  %-18s %d (0x%x)\n", label, val, val);
}

static void p_iovar_int(char *ifname, const char *label, const char *iovar)
{
	int val = 0, ret;

	ret = wl_iovar_getint(ifname, (char *)iovar, &val);
	if (ret)
		printf("  %-18s %s\n", label, e(ret));
	else
		printf("  %-18s %d (0x%x)\n", label, val, val);
}

static void p_mac(char *ifname, const char *label, int cmd)
{
	struct ether_addr ea;
	int ret;

	memset(&ea, 0, sizeof(ea));
	ret = wl_ioctl(ifname, cmd, &ea, sizeof(ea));
	if (ret) {
		printf("  %-18s %s\n", label, e(ret));
		return;
	}
	printf("  %-18s %02x:%02x:%02x:%02x:%02x:%02x\n", label,
	       ea.octet[0], ea.octet[1], ea.octet[2],
	       ea.octet[3], ea.octet[4], ea.octet[5]);
}

static void p_ssid(char *ifname)
{
	wlc_ssid_t ssid;
	char buf[36];
	int ret;

	memset(&ssid, 0, sizeof(ssid));
	ret = wl_ioctl(ifname, WLC_GET_SSID, &ssid, sizeof(ssid));
	if (ret) {
		printf("  %-18s %s\n", "ssid", e(ret));
		return;
	}
	if (ssid.SSID_len > 32)
		ssid.SSID_len = 32;
	memcpy(buf, ssid.SSID, ssid.SSID_len);
	buf[ssid.SSID_len] = '\0';
	printf("  %-18s \"%s\" (len %u)%s\n", "ssid", buf, ssid.SSID_len,
	       ssid.SSID_len ? "" : "   <-- empty: no beacon");
}

static void p_country(char *ifname)
{
	char cc[WLC_CNTRY_BUF_SZ];
	int ret;

	memset(cc, 0, sizeof(cc));
	ret = wl_ioctl(ifname, WLC_GET_COUNTRY, cc, sizeof(cc));
	printf("  %-18s %s\n", "country", ret ? e(ret) : cc);
}

static void p_revinfo(char *ifname)
{
	wlc_rev_info_t rev;
	int ret;

	memset(&rev, 0, sizeof(rev));
	ret = wl_ioctl(ifname, WLC_GET_REVINFO, &rev, sizeof(rev));
	if (ret) {
		printf("  %-18s %s\n", "revinfo", e(ret));
		return;
	}
	printf("  %-18s chip 0x%x rev %d corerev %d radiorev 0x%x "
	       "phytype %d phyrev %d\n", "revinfo",
	       rev.chipnum, rev.chiprev, rev.corerev, rev.radiorev,
	       rev.phytype, rev.phyrev);
}

static void p_phylist(char *ifname)
{
	char var[16];
	int ret;

	memset(var, 0, sizeof(var));
	ret = wl_ioctl(ifname, WLC_GET_PHYLIST, var, sizeof(var));
	printf("  %-18s %s\n", "phylist", ret ? e(ret) : var);
}

static void p_chanspec(char *ifname)
{
	channel_info_t ci;
	int val = 0, ret;

	memset(&ci, 0, sizeof(ci));
	ret = wl_ioctl(ifname, WLC_GET_CHANNEL, &ci, sizeof(ci));
	if (ret)
		printf("  %-18s %s\n", "channel", e(ret));
	else
		printf("  %-18s hw %d target %d scan %d\n", "channel",
		       ci.hw_channel, ci.target_channel, ci.scan_channel);

	ret = wl_iovar_getint(ifname, "chanspec", &val);
	if (ret)
		printf("  %-18s %s\n", "chanspec", e(ret));
	else
		printf("  %-18s 0x%04x\n", "chanspec", val & 0xffff);
}

/*
 * bsscfg 0 is the primary BSS. If "bss" reads back 0 the radio is up but the
 * BSS is not started, which looks exactly like a working AP that no client
 * can see.
 */
static void p_bss(char *ifname)
{
	int val = 0, ret;

	ret = wl_bssiovar_get(ifname, "bss", 0, &val, sizeof(val));
	if (ret)
		printf("  %-18s %s\n", "bss[0]", e(ret));
	else
		printf("  %-18s %d%s\n", "bss[0]", val,
		       val ? "  (BSS up)" : "  <-- BSS DOWN: not beaconing");
}

static void p_assoclist(char *ifname)
{
	char buf[8192];
	maclist_t *ml = (maclist_t *)buf;
	uint i;
	int ret;

	memset(buf, 0, sizeof(buf));
	ml->count = (sizeof(buf) - sizeof(uint)) / ETHER_ADDR_LEN;
	ret = wl_ioctl(ifname, WLC_GET_ASSOCLIST, buf, sizeof(buf));
	if (ret) {
		printf("  %-18s %s\n", "assoclist", e(ret));
		return;
	}
	printf("  %-18s %u station(s)\n", "assoclist", ml->count);
	for (i = 0; i < ml->count && i < 64; i++)
		printf("      %02x:%02x:%02x:%02x:%02x:%02x\n",
		       ml->ea[i].octet[0], ml->ea[i].octet[1], ml->ea[i].octet[2],
		       ml->ea[i].octet[3], ml->ea[i].octet[4], ml->ea[i].octet[5]);
}

/*
 * WLC_GET_RADIO returns a mask of the reasons the radio is OFF, so 0 is the
 * good answer and reads confusingly as a bare number.
 */
static void p_radio(char *ifname)
{
	int val = 0, ret;

	ret = wl_ioctl(ifname, WLC_GET_RADIO, &val, sizeof(val));
	if (ret) {
		printf("  %-18s %s\n", "radio", e(ret));
		return;
	}
	printf("  %-18s 0x%x  %s%s%s%s\n", "radio", val,
	       val ? "OFF:" : "on (no disable bits)",
	       (val & WL_RADIO_SW_DISABLE)  ? " sw"  : "",
	       (val & WL_RADIO_HW_DISABLE)  ? " hw"  : "",
	       (val & WL_RADIO_MPC_DISABLE) ? " mpc" : "");
}

/*
 * Exactly what wlconf does at the end of its "up" path: the "bss" iovar with
 * a {bsscfg_idx, enable} pair. Repeating it by hand is how to see the driver's
 * actual objection - wlconf's own call is fire-and-forget, and the read-back
 * is the only thing that shows it did not take.
 */
static int bss_set(char *ifname, int idx, int enable)
{
	struct { int bsscfg_idx; int enable; } setbuf;
	int ret;

	setbuf.bsscfg_idx = idx;
	setbuf.enable = enable;
	ret = wl_iovar_set(ifname, "bss", &setbuf, sizeof(setbuf));
	printf("  bss[%d] <- %d: %s\n", idx, enable, ret ? e(ret) : "ok");
	p_bss(ifname);
	return ret;
}

static void dump(char *ifname)
{
	printf("=== %s ===\n", ifname);

	p_int(ifname, "magic", WLC_GET_MAGIC);
	p_int(ifname, "version", WLC_GET_VERSION);
	p_int(ifname, "instance", WLC_GET_INSTANCE);
	p_revinfo(ifname);
	p_phylist(ifname);

	printf("  --- radio/BSS state ---\n");
	p_int(ifname, "isup", WLC_GET_UP);
	p_radio(ifname);
	p_int(ifname, "ap", WLC_GET_AP);
	p_int(ifname, "infra", WLC_GET_INFRA);
	p_int(ifname, "band", WLC_GET_BAND);
	p_int(ifname, "phytype", WLC_GET_PHYTYPE);
	p_bss(ifname);
	p_ssid(ifname);
	p_mac(ifname, "bssid", WLC_GET_BSSID);
	p_country(ifname);
	p_chanspec(ifname);
	p_int(ifname, "wsec", WLC_GET_WSEC);
	p_int(ifname, "monitor", WLC_GET_MONITOR);
	p_iovar_int(ifname, "mpc", "mpc");
	p_iovar_int(ifname, "nmode", "nmode");
	p_iovar_int(ifname, "vhtmode", "vhtmode");
	p_iovar_int(ifname, "txpwr_override", "txpwr_override");

	printf("  --- traffic ---\n");
	p_assoclist(ifname);
}

int main(int argc, char *argv[])
{
	char *ifname;

	if (argc < 2) {
		fprintf(stderr,
			"Usage: wldiag <ifname> [ioctl <cmd> | iovar <name> | bss 0|1]\n");
		return 1;
	}
	ifname = argv[1];
	ifname_for_bcmerror = ifname;

	if (argc == 4 && !strcmp(argv[2], "bss")) {
		printf("=== %s ===\n", ifname);
		return bss_set(ifname, 0, atoi(argv[3])) ? 1 : 0;
	}
	if (argc == 4 && !strcmp(argv[2], "ioctl")) {
		p_int(ifname, argv[3], atoi(argv[3]));
		return 0;
	}
	if (argc == 4 && !strcmp(argv[2], "iovar")) {
		p_iovar_int(ifname, argv[3], argv[3]);
		return 0;
	}

	dump(ifname);
	return 0;
}
