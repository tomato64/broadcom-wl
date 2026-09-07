/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Broadcom's sources do `#include <stdarg.h>`, which worked in 2.6.36 because
 * kernel builds still picked up the compiler's copy. Since 5.15 the kernel
 * provides its own and -nostdinc means the compiler's is unreachable.
 *
 * Interposed here rather than patched into vendor/shared, so the vendored
 * sources stay byte-identical to FreshTomato's and remain easy to re-sync.
 */
#ifndef WL_BCM_OVERRIDE_STDARG_H
#define WL_BCM_OVERRIDE_STDARG_H
#include <linux/stdarg.h>
#endif
