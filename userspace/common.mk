# Shared settings for the broadcom-wl userspace tools.
#
# Built inside Buildroot, so CC/CFLAGS/LDFLAGS arrive from
# TARGET_CONFIGURE_OPTS and the -L for staging is already in LDFLAGS. The
# defaults below only exist so a tool can still be built by hand against a
# finished output/ tree while debugging.

BR2_EXTERNAL_TOMATO64_PATH ?= /home/lance/tomato64/tomato64
T64_OUT                    ?= /home/lance/tomato64/src/buildroot/output
CROSS_COMPILE              ?= $(T64_OUT)/host/bin/arm-tomato64-linux-musleabi-

# NOT `CC ?=`. make gives CC and AR built-in default values ("cc", "ar"), so
# ?= never fires and the tools silently build for the host - which fails at
# the link with "skipping incompatible libnvram.so" rather than anywhere
# useful. Only override when the value is make's own default; a CC passed by
# Buildroot or on the command line must win.
ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif
ifeq ($(origin AR),default)
AR := $(CROSS_COMPILE)ar
endif

# Under Buildroot, CPPFLAGS/LDFLAGS already point at the sysroot. Standing in
# for them out of tree matters for one header in particular: libshared's
# shared.h includes tomato_config.h, which does not exist in the source tree at
# all - libshared generates it and installs it to staging - so a build without
# this fails immediately and confusingly.
BCM_SYSROOT ?= $(T64_OUT)/host/arm-tomato64-linux-musleabi/sysroot
ifeq ($(origin CPPFLAGS),undefined)
CPPFLAGS := -I$(BCM_SYSROOT)/usr/include
endif
ifeq ($(origin LDFLAGS),undefined)
LDFLAGS := -L$(BCM_SYSROOT)/usr/lib
endif

# The one thing this repo needs from tomato64. libshared installs only
# libshared.so and tomato_config.h into staging, so its headers - shared.h,
# shutils.h, wlutils.h, wlif_utils.h and the Broadcom trees beside them - can
# only be reached in its source directory. Buildroot's package passes
# BR2_EXTERNAL_TOMATO64_PATH down for exactly this.
LIBSHARED := $(BR2_EXTERNAL_TOMATO64_PATH)/package/libshared/shared
VENDOR    := $(CURDIR)/../../vendor
USERSPACE := $(CURDIR)/..

# -std=gnu11, not the compiler default: GCC 15 defaults to C23, where `bool` is
# a keyword and Broadcom's typedefs.h typedefs it.
#
# Deliberately NOT shadow/wlflags. Those are the *driver's* flags and include
# -DBCMDRIVER, which sends the Broadcom headers down their kernel branch -
# generated/autoconf.h, dma_addr_t, a conflicting bool typedef. FreshTomato
# builds these tools with no driver feature flags at all; the ioctl structs are
# the stable ABI and do not depend on them.
BCM_CPPFLAGS  = -std=gnu11
BCM_CPPFLAGS += -I$(LIBSHARED) -I$(LIBSHARED)/include
BCM_CPPFLAGS += -I$(LIBSHARED)/common/include
BCM_CPPFLAGS += -I$(LIBSHARED)/bcmwifi/include
# Broadcom headers libshared does not carry. Everything libshared DOES carry is
# taken from libshared, not from vendor/: its wlioctl.h, bcmwifi and proto/
# trees are byte-identical to FreshTomato's src-rt-6.x.4708 copies, and one
# source for a shared ABI is worth more than a second identical one. vendor/
# stays first-class for the kernel module, which needs FreshTomato's exact set.
BCM_CPPFLAGS += -I$(VENDOR)/include
BCM_CPPFLAGS += $(CPPFLAGS)

BCM_CFLAGS  = -O2 -Wall
# -no-pie so an address in a crash report maps directly onto the symbol table:
# addr2line -fpe <binary> <pc>. See crashinfo.c - the target has no gdb, no
# strace and no core dumps, so without this a crash is one word.
BCM_CFLAGS += -no-pie
BCM_CFLAGS += $(CFLAGS)

BCM_LDFLAGS  = -no-pie
BCM_LDFLAGS += $(LDFLAGS)
