/*
 * Report where a fatal signal happened.
 *
 * Written for this project. The target has no gdb, no strace and no core
 * dumps, so "Segmentation fault" is the entire diagnosis - which is the
 * failure mode this project has already lost hardware cycles to. A handler
 * costs nothing until something dies and then names the instruction.
 *
 * Linked into nas and eapd. It installs itself from a constructor, so it is
 * armed before main() and needs no cooperation from Broadcom's sources.
 *
 * The binaries are built -no-pie precisely so the addresses printed here are
 * absolute and map straight onto the symbol table:
 *
 *     arm-tomato64-linux-musleabi-addr2line -fpe ./nas 0x<pc>
 *     arm-tomato64-linux-musleabi-nm -C ./nas | sort   # then find <pc>
 *
 * Everything below writes with write(2) and formats by hand. printf() is not
 * async-signal-safe, and a handler that deadlocks inside stdio while
 * reporting a crash reports nothing.
 */

#define _GNU_SOURCE
#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

static void put(const char *s)
{
	ssize_t n = (ssize_t)strlen(s);
	ssize_t done = 0, w;

	while (done < n) {
		w = write(2, s + done, (size_t)(n - done));
		if (w <= 0)
			return;
		done += w;
	}
}

static void puthex(unsigned long v)
{
	static const char digits[] = "0123456789abcdef";
	char buf[11];
	int i;

	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < 8; i++)
		buf[2 + i] = digits[(v >> ((7 - i) * 4)) & 0xf];
	buf[10] = '\0';
	put(buf);
}

static void field(const char *name, unsigned long v)
{
	put(name);
	puthex(v);
	put("\n");
}

static void crash_handler(int sig, siginfo_t *si, void *uc)
{
	mcontext_t *m = &((ucontext_t *)uc)->uc_mcontext;
	const char *name;

	switch (sig) {
	case SIGSEGV: name = "SIGSEGV"; break;
	case SIGBUS:  name = "SIGBUS";  break;
	case SIGILL:  name = "SIGILL";  break;
	case SIGFPE:  name = "SIGFPE";  break;
	case SIGABRT: name = "SIGABRT"; break;
	default:      name = "signal";  break;
	}

	put("\n*** ");
	put(name);
	put(" ***\n");
	field("  fault addr ", (unsigned long)si->si_addr);
	field("  pc         ", m->arm_pc);
	field("  lr         ", m->arm_lr);
	field("  sp         ", m->arm_sp);
	field("  fp         ", m->arm_fp);
	field("  r0         ", m->arm_r0);
	field("  r1         ", m->arm_r1);
	put("  resolve with: addr2line -fpe <binary> <pc>  (built -no-pie)\n");

	/* Re-raise on the default handler so the exit status still says what
	 * killed us, and so anything watching the process is not misled into
	 * thinking it exited cleanly. */
	signal(sig, SIG_DFL);
	raise(sig);
}

__attribute__((constructor))
static void crashinfo_install(void)
{
	struct sigaction sa;
	int i;
	static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = crash_handler;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;
	sigemptyset(&sa.sa_mask);

	for (i = 0; i < (int)(sizeof(sigs) / sizeof(sigs[0])); i++)
		sigaction(sigs[i], &sa, NULL);
}
