# Broadcom's radio configurator.
#
# This driver is not cfg80211/mac80211, so iw and iwconfig do not apply -
# wlconf is how a wl radio is brought into service, driven by wl<N>_* NVRAM
# variables through the driver's private ioctls.
#
# Source is FreshTomato's router/wlconf_arm/ - the _arm variant, which
# common.mak selects when neither CONFIG_BCM7 nor CONFIG_BCM714 is set, i.e.
# the base 6.x SDK. That matches the blob (6.37.14.126, src-rt-6.x.4708); the
# _arm_7 and _arm_714 variants would give a mismatched ioctl ABI.
#
# Tomato64's libshared/include/wlioctl.h is byte-identical to FreshTomato's
# src-rt-6.x.4708 copy, so the ioctl ABI already lines up.

include ../common.mk

CFLAGS_ALL = $(BCM_CPPFLAGS) -Isrc $(BCM_CFLAGS)

OBJS := src/wlconf.o src/bcmwifi_channels.o

all: wlconf

wlconf: $(OBJS)
	$(CC) $(CFLAGS_ALL) $(BCM_LDFLAGS) -o $@ $^ -lnvram -lshared

%.o: %.c
	$(CC) $(CFLAGS_ALL) -c -o $@ $<

clean:
	rm -f $(OBJS) wlconf
