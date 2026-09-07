# vendor/ — third-party material

Committed deliberately so the repo builds standalone, with no FreshTomato
checkout required.

> **This repo must not gain a public remote.** `wl_apsta.o` is Broadcom's
> closed driver and its `.modinfo` declares `license=Proprietary`. Keeping it
> in a local repo is fine; publishing it is redistribution.

Provenance: FreshTomato `freshtomato-arm`, `release/src-rt-6.x.4708/`.

| path | source | why it's here |
|---|---|---|
| `wl/linux/wl_apsta.o` | `wl/linux/` | **the blob.** SMP build — what we link. 4.8 MB, ARMv7-A, Thumb-2, soft-float EABI, 8-byte align |
| `wl/up/wl_apsta.o` | `wl/up/` | uniprocessor build. Not used (the R7000's BCM4709 is dual-core A9); kept only so FreshTomato is never needed again |
| `wl/config/` | `wl/config/` | `wltunable_*.h` build-tuning headers |
| `shared/` | `shared/` | Broadcom's Tier-1 sources — 48 `.c` providing the 176 `si_*`/`osl_*`/`bcm_*`/`pktq_*` symbols the blob imports. Replaces the weak stubs when the port gets real |
| `include/` | `include/` | Broadcom headers, 100 `.h`. `linux_osl.h:413-414` is the `PKTDATA`/`PKTLEN` evidence |
| `linux-2.6.36/config_base` | `linux/linux-2.6.36/` | **the ABI reference.** 2.6.36.4, ARM, `SMP=y`, `NET_NS=y`, `NF_CONNTRACK=y`, `NET_SCHED=y`, `NET_CLS_ACT=y`, `IPV6=y` — all of which move `sk_buff`/`net_device` fields |
| `linux-2.6.36/include/`, `linux-2.6.36/arch/arm/include/` | same | 2.6.36 kernel headers, for deriving the struct offsets the blob has baked in. 26 MB of the 475 MB tree — only what the offsets probe needs |

## What is NOT here

`wl/sysdeps/` (25 MB of per-device CLM regulatory data) is **not needed**. The
blob defines `clm_data`, `clm_limits`, `clm_country_channels` and friends itself
— CLM is self-contained inside it, and imports nothing.

## Why config_base matters

Struct layout is config-dependent, so "2.6.36 offsets" is meaningless without
the config. FreshTomato ships a *working* combination of this config and this
blob, so this config is by definition ABI-compatible with it. That makes it the
authoritative reference for the shadow layer.

## Toolchain the blob was built with

`.comment` says **GCC 4.5.3 (Buildroot 2012.02)**. So `__GNUC__ == 4`, which is
why the 2.6.36 headers want `linux/compiler-gcc4.h` — relevant when compiling
them with a modern GCC. See `shadow/README.md`.
