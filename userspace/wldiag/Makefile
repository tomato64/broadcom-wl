# wldiag - ask the driver what it thinks its state is.
#
# Written for this port. wlconf reports nothing about the result of what it
# did, so this covers the subset bring-up and diagnosis need: isup, bss[0],
# SSID, BSSID, chanspec, country, assoclist, the bcmerror behind any failure,
# and single ioctl/iovar queries by hand.
#
# FreshTomato's own `wl` is now shipped too (see the README - it is their
# prebuilt uclibc binary, run against musl through the libc.so.0 shim), and it
# is far more capable. This stays because it is ours: it builds from source
# with the rest of the tree, and it is the thing to reach for first when the
# question is whether the shadow ABI itself is behaving.
#
# Kept in the image rather than left behind as a test tool: it is the only way
# to interrogate this driver on the device, and the driver is a shadow-ABI port
# whose receive path has had far less scrutiny than its transmit path.

include ../common.mk

CFLAGS_ALL = $(BCM_CPPFLAGS) $(BCM_CFLAGS)

all: wldiag

wldiag: wldiag.o
	$(CC) $(CFLAGS_ALL) $(BCM_LDFLAGS) -o $@ $^ -lnvram -lshared

%.o: %.c
	$(CC) $(CFLAGS_ALL) -c -o $@ $<

clean:
	rm -f wldiag.o wldiag
