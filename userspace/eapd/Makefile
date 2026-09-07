# Broadcom's EAP dispatcher.
#
# nas does not read the driver's event path itself. eapd owns the raw socket
# bound to ETHER_TYPE_BRCM (0x886c) - the driver's own event encapsulation,
# which is how association events and EAPOL frames arrive - and relays them to
# nas over UDP on loopback. Start nas without eapd and it blocks in select()
# forever, having seen no association.
#
# Note the socket is on the LAN *bridge*, not on wl0/wl1: eapd_add_interface()
# resolves each radio to its bridge with get_ifname_by_wlmac() and binds one
# socket per bridge. That is why lan_ifnames must list the radios.
#
# Source is FreshTomato's router/eapd_arm/ - _arm for the same reason as
# wlconf_arm.

include ../common.mk

CFLAGS_ALL  = $(BCM_CPPFLAGS) -Isrc -I$(USERSPACE)/nas/src $(BCM_CFLAGS)

# FreshTomato's flags. EAPD_WKSP_AUTO_CONFIG lets eapd discover interfaces from
# lan_ifnames when given none; EAPDDUMP compiles in the SIGUSR1 socket dump.
CFLAGS_ALL += -DEAPD_WKSP_AUTO_CONFIG -DEAPDDUMP -Dlinux

# BCMDBG is the one addition to FreshTomato's set, and here it is not optional
# if eapd is ever to explain itself: without it the "eapd_dbg" nvram read in
# eapd_linux.c is not compiled at all, so eapd_msg_level can never be raised,
# and its initialiser falls to 0 rather than EAPD_ERROR_VAL. With the restored
# EAPD_ERROR/EAPD_INFO macros in src/eapd.h, that is what makes a failed start
# say which failure it was - upstream ships them defined to nothing, so eapd
# used to exit in silence. Errors print by default; eapd_dbg=3 adds the trace.
CFLAGS_ALL += -DBCMDBG

# eapd calls wl_wlif_is_psta() and get_ifname_by_wlmac(), which libshared now
# builds for bcm53xx. Both sides agree on wlif_utils.h's TCONFIG_BCMARM and
# TCONFIG_RTNPLUS conditionals without any flag here: package/rc/features
# exports both permanently, gen_tomato_config.h.sh writes them into
# tomato_config.h, and shutils.h pulls that in via shared.h before
# wlif_utils.h. Keep that include order.

# BCM_DCS / BCM_MEVENT / BCM_BSD are off, matching a build with none of
# CONFIG_BCMDCS, CONFIG_BCM_MEVENT or TCONFIG_BCMBSD set.
OBJS := src/eapd.o src/eapd_linux.o src/nas_eap.o src/wps_eap.o src/ses_eap.o

# Ours, not Broadcom's: turns a bare "Segmentation fault" into a pc and a fault
# address. The target has no gdb, strace or core dumps.
OBJS += crashinfo.o

all: eapd

eapd: $(OBJS)
	$(CC) $(CFLAGS_ALL) $(BCM_LDFLAGS) -o $@ $^ -lnvram -lshared

%.o: %.c
	$(CC) $(CFLAGS_ALL) -c -o $@ $<

crashinfo.o: $(USERSPACE)/crashinfo.c
	$(CC) $(CFLAGS_ALL) -c -o $@ $<

clean:
	rm -f $(OBJS) eapd
