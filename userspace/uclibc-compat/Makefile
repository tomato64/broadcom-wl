# uclibc.so - the uClibc-ABI shim that lets FreshTomato's prebuilt `wl` run
# against musl - and the one-string edit to `wl` that makes it ask for the shim
# by a name musl is willing to load. See uclibc_compat.c and rename-needed.py.
#
# The SONAME is the point. `wl` shipped with DT_NEEDED libc.so.0, and musl's
# loader refuses to open ANY libc.* / libpthread.* / librt.* / libm.* / libdl.*
# / libutil.* / libxnet.* - it resolves them to itself. So both sides move to
# `uclibc.so`, which is the same nine bytes as `libc.so.0` and matches nothing
# musl reserves.

include ../common.mk

CFLAGS_ALL = $(BCM_CPPFLAGS) $(BCM_CFLAGS)

SONAME  = uclibc.so
VENDOR_WL = $(VENDOR)/wl/arm-uclibc/wl

all: $(SONAME) wl

# -fPIC and -shared only; no -no-pie from BCM_CFLAGS, which is meant for the
# executables. Filtered rather than dropped so a future addition to BCM_CFLAGS
# still reaches here.
$(SONAME): uclibc_compat.c
	$(CC) $(filter-out -no-pie,$(CFLAGS_ALL)) -fPIC -shared \
	  -Wl,-soname,$(SONAME) $(filter-out -no-pie,$(LDFLAGS)) -o $@ $<

# Nine bytes of .dynstr and nothing else - the binary is otherwise installed
# exactly as FreshTomato ships it. Depends on the Makefile so that changing
# SONAME here cannot leave a stale `wl` pointing at the old name.
wl: $(VENDOR_WL) rename-needed.py Makefile
	./rename-needed.py $(VENDOR_WL) $@ libc.so.0 $(SONAME)
	chmod 0755 $@

clean:
	rm -f $(SONAME) wl
