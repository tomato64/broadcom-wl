# Broadcom's WPA authenticator.
#
# The 4-way handshake ends in WLC_SET_KEY - the driver's own ioctl, not
# nl80211 - which is why hostapd is no help here. nas needs eapd running to
# hear anything at all; see ../eapd/Makefile.
#
# Source is FreshTomato's router/nas_arm/ - _arm for the same reason as
# wlconf_arm.

include ../common.mk

CFLAGS_ALL  = $(BCM_CPPFLAGS) -Isrc -I$(USERSPACE)/eapd/src $(BCM_CFLAGS)

# FreshTomato's flag set, minus NAS_GTK_PER_STA which it leaves commented out:
#   NAS_WKSP_BUILD_NAS_AUTH   the authenticator - the AP half
#   NAS_WKSP_BUILD_NAS_SUPPL  the supplicant - needed for WDS links, and for
#                             the WPA role negotiation even in AP mode
#   NAS_RADIUS                WPA-Enterprise; harmless when only PSK is used
#   NAS_WKSP_ON_DEMAND        take interfaces from eapd's events as they
#                             appear rather than enumerating once at startup,
#                             which is why nas takes no arguments
#   BCMSUPPL                  build the supplicant state machine in wpa.c
#   NAS_IPV6                  Tomato64 sets TCONFIG_IPV6
CFLAGS_ALL += -DNAS_WKSP_BUILD_NAS_AUTH -DNAS_WKSP_BUILD_NAS_SUPPL \
	      -DNAS_RADIUS -DNAS_WKSP_ON_DEMAND -DBCMSUPPL -DNAS_IPV6

# BCMDBG is the one addition to FreshTomato's flags, and it follows this port's
# rule that instrumentation is compiled in and switched at runtime. Both macros
# it enables are already runtime-gated - dbg() on nas->debug, which get_wsec()
# reads from wl<N>_nas_dbg, and NASDBG() on debug_nwksp, which nvram nas_dbg
# sets - so a default run prints exactly what it printed without the flag. It
# is safe for the ioctl ABI: the only two BCMDBG sites in the headers nas
# includes are additive and move no field of any struct handed to the driver.
CFLAGS_ALL += -DBCMDBG

# get_wsec() and friends come from libshared, which builds wlif_utils.o for
# bcm53xx. Anything here that includes wlif_utils.h must agree with libshared
# about TCONFIG_BCMARM, because wsec_info_t inserts a field mid-struct under
# it. No flag is needed for that: package/rc/features exports TCONFIG_BCMARM
# and TCONFIG_RTNPLUS permanently, gen_tomato_config.h.sh writes them into
# tomato_config.h, and nas.h includes shutils.h - hence shared.h, hence
# tomato_config.h - before wlif_utils.h. Keep that include order.

NASOBJS := src/mppe.o src/wpa.o src/nas.o src/nas_linux.o src/nas_wl.o \
	   src/nas_wksp.o src/nas_radius.o src/nas_wksp_radius.o

# UTLOBJS in FreshTomato's Makefile, from $(SRCBASE)/shared - which is
# ../../vendor/shared here, already present for the kernel module. Compiled in
# rather than linked from libshared, which does not export them.
UTLOBJS := bcmwpa.o bcmutils.o

# Ours, not Broadcom's: turns a bare "Segmentation fault" into a pc and a fault
# address. The target has no gdb, strace or core dumps.
UTLOBJS += crashinfo.o

CRYPTO := $(CURDIR)/../libbcmcrypto

all: nas

nas: $(NASOBJS) $(UTLOBJS)
	$(CC) $(CFLAGS_ALL) $(BCM_LDFLAGS) -o $@ $^ \
		-L$(CRYPTO) -lbcmcrypto -lnvram -lshared

%.o: %.c
	$(CC) $(CFLAGS_ALL) -c -o $@ $<

bcmwpa.o: $(VENDOR)/shared/bcmwpa.c
	$(CC) $(CFLAGS_ALL) -c -o $@ $<

bcmutils.o: $(VENDOR)/shared/bcmutils.c
	$(CC) $(CFLAGS_ALL) -c -o $@ $<

crashinfo.o: $(USERSPACE)/crashinfo.c
	$(CC) $(CFLAGS_ALL) -c -o $@ $<

clean:
	rm -f $(NASOBJS) $(UTLOBJS) nas
