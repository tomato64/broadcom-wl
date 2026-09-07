#!/usr/bin/env python3
"""Rename one DT_NEEDED entry of an ELF, in place, without moving anything.

Why this exists
---------------
FreshTomato's prebuilt `wl` asks for `libc.so.0`, and musl's dynamic linker
will never load a file under that name. ldso/dynlink.c, load_library():

	/* Catch and block attempts to reload the implementation itself */
	if (name[0]=='l' && name[1]=='i' && name[2]=='b') {
		static const char reserved[] = "c.pthread.rt.m.dl.util.xnet.";
		...
		if (*rp) ... is_self = 1;
	}

`libc.so.0` matches the reserved entry "c." on `name+3`, so musl resolves it to
*itself* and our shim is never opened - the binary then dies with
"Error relocating: __uClibc_main: symbol not found". Any name outside that set
works; `uclibc.so` does not even begin with "lib", so it cannot match.

Why a nine-byte poke rather than patchelf
-----------------------------------------
The replacement name is chosen to be exactly as long as the original, so the
edit is a literal overwrite of 9 bytes inside .dynstr. Nothing moves: no new
PT_LOAD, no relocated .dynsym/.hash, no rewritten program headers. For a
proprietary binary nobody can rebuild, that is worth more than the convenience
of a general tool. patchelf does the job too, and does it correctly - it just
does considerably more of it.

Why it must be section-aware
----------------------------
`libc.so.0` occurs twice in the file. The other one is the tail of
`/lib/ld-uClibc.so.0` in .interp, and that string is load-bearing and correct:
the interpreter is reached through a /lib/ld-uClibc.so.0 -> ld-musl-arm.so.1
symlink. A naive search-and-replace would corrupt it. So the offset is resolved
through the section header table, and only the .dynstr copy is touched.
"""

import struct
import sys


def sections(data):
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        sys.exit("not a 32-bit little-endian ELF")
    (e_shoff,) = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    out = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name, _typ, _flags, _addr, sh_off, sh_size = struct.unpack_from("<6I", data, off)
        out.append((name, sh_off, sh_size))
    shstr_off = out[e_shstrndx][1]

    def name_of(idx):
        s = shstr_off + idx
        return data[s:data.index(b"\0", s)].decode()

    return {name_of(n): (o, s) for (n, o, s) in out}


def main():
    if len(sys.argv) != 5:
        sys.exit("usage: rename-needed.py <in> <out> <old-soname> <new-soname>")
    src, dst, old, new = sys.argv[1:]
    old_b, new_b = old.encode(), new.encode()

    if len(new_b) != len(old_b):
        sys.exit("names must be the same length (%r is %d, %r is %d) - the point "
                 "of this tool is that nothing moves"
                 % (old, len(old_b), new, len(new_b)))

    data = bytearray(open(src, "rb").read())
    secs = sections(data)
    if ".dynstr" not in secs:
        sys.exit("no .dynstr")
    off, size = secs[".dynstr"]
    blob = bytes(data[off:off + size])

    # NUL on both sides: a whole string, not a suffix another entry shares.
    hits = []
    start = 0
    while True:
        i = blob.find(old_b, start)
        if i < 0:
            break
        start = i + 1
        if (i == 0 or blob[i - 1] == 0) and blob[i + len(old_b)] == 0:
            hits.append(i)

    if len(hits) != 1:
        sys.exit("expected exactly one whole-string %r in .dynstr, found %d"
                 % (old, len(hits)))

    at = off + hits[0]
    data[at:at + len(new_b)] = new_b
    open(dst, "wb").write(data)
    print("rename-needed: %s -> %s at .dynstr+%d (file offset 0x%x)"
          % (old, new, hits[0], at))


if __name__ == "__main__":
    main()
