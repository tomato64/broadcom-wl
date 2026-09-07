/*
 * uClibc-ABI compatibility shim, so FreshTomato's prebuilt `wl` runs on musl.
 *
 * FreshTomato ships `wl` only as a binary - there is no source for it anywhere
 * in their tree - built against uClibc for arm. It is otherwise a perfect fit
 * for this port: same v7-A, same soft-float AAPCS, same 8-byte alignment as
 * our arm-tomato64-linux-musleabi toolchain, and it comes out of the same
 * src-rt-6.x.4708/wl/ tree as the wl_apsta.o blob we link the driver from, so
 * its ioctl version matches the driver by construction.
 *
 * It also asks for almost nothing. Of its 67 undefined symbols, musl already
 * provides 64. __register_frame_info and __deregister_frame_info are WEAK
 * undefined and resolve to 0 harmlessly. That leaves exactly one gap:
 * __uClibc_main, uClibc's equivalent of __libc_start_main.
 *
 * So the binary keeps its own code, and the ABI differences are bridged around
 * it:
 *
 *   /lib/ld-uClibc.so.0  ->  symlink to ld-musl-arm.so.1   (its INTERP)
 *   /lib/uclibc.so       ->  this file                     (its DT_NEEDED)
 *
 * The DT_NEEDED cannot stay `libc.so.0`. musl's loader refuses to open any
 * library whose name begins lib + one of "c." "pthread." "rt." "m." "dl."
 * "util." "xnet." - it resolves them to itself, so a file called
 * /lib/libc.so.0 is never even looked at and the binary dies with
 * "Error relocating: __uClibc_main: symbol not found". The name is therefore
 * changed to `uclibc.so`, which is exactly the same nine bytes, so the edit is
 * an overwrite inside .dynstr with nothing moved. rename-needed.py does it and
 * explains why it has to be section-aware.
 *
 * This shim's own DT_NEEDED libc.so pulls musl into the global scope, where
 * the other 64 symbols resolve.
 *
 * The argument list below is not guessed. Disassembling the binary's _start
 * (entry 0x8fd4) shows it verbatim:
 *
 *	mov  fp, #0
 *	mov  lr, #0
 *	pop  {r1}		@ r1 = argc
 *	mov  r2, sp		@ r2 = argv
 *	push {r2}		@ stack[2] = stack_end
 *	push {r0}		@ stack[1] = rtld_fini (r0 from the loader)
 *	ldr  ip, =0x00045c08	@ _fini
 *	push {ip}		@ stack[0] = app_fini
 *	ldr  r0, =0x0003e450	@ main
 *	ldr  r3, =0x00008c98	@ _init
 *	b    __uClibc_main
 *
 * Forwarding to musl's own __libc_start_main rather than open-coding the
 * startup is deliberate: that is the entry musl's crt1 would have used, so
 * __init_libc, the stack guard, DT_INIT and DT_INIT_ARRAY all happen exactly
 * as they do for a native musl binary, and nothing here has to track musl
 * internals.
 */

extern int __libc_start_main(int (*main)(int, char **, char **),
                             int argc, char **argv,
                             void (*init)(void), void (*fini)(void),
                             void (*ldso_fini)(void));

void __uClibc_main(int (*main)(int, char **, char **), int argc, char **argv,
                   void (*app_init)(void), void (*app_fini)(void),
                   void (*rtld_fini)(void), void *stack_end);

void __uClibc_main(int (*main)(int, char **, char **), int argc, char **argv,
                   void (*app_init)(void), void (*app_fini)(void),
                   void (*rtld_fini)(void), void *stack_end)
{
	(void)stack_end;	/* musl derives its own from the initial sp */

	__libc_start_main(main, argc, argv, app_init, app_fini, rtld_fini);

	__builtin_unreachable();
}
