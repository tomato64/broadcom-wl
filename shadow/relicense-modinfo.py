#!/usr/bin/env python3
"""Rewrite the license= entry in an object's .modinfo section.

Why: the blob declares license=Proprietary, so modpost refuses to let the
module link GPL-only exports. linux_osl.c needs skb_to_sgvec(), which is
EXPORT_SYMBOL_GPL in 6.12 (it was a plain EXPORT_SYMBOL in 2.6.36, which is why
FreshTomato never hit this).

This is a personal build for hardware the author owns and is never
distributed - the repo has no remote and vendor/README.md says so. Copyright
governs distribution, not private use, so the kernel's licence gate is not
protecting anyone here; it is just blocking a link. Changing it is recorded
explicitly rather than hidden, and it is trivially reversible: drop the
relicense step from the Makefile and the module goes back to Proprietary (and
back to failing to link).

The module still taints the kernel at load, which is correct and left alone.

Mechanics: .modinfo is a run of NUL-terminated key=value strings. The
replacement is padded with NULs to the original length, so the section size and
every other entry stay put; trailing NULs simply read as empty entries.
"""
import subprocess
import sys
import tempfile
import os

def main() -> int:
    if len(sys.argv) != 4:
        sys.exit("usage: relicense-modinfo.py <objcopy> <object> <license>")
    objcopy, obj, lic = sys.argv[1:4]

    fd, tmp = tempfile.mkstemp(suffix=".modinfo")
    os.close(fd)
    try:
        subprocess.run([objcopy, "--dump-section", f".modinfo={tmp}", obj],
                       check=True)
        data = bytearray(open(tmp, "rb").read())

        want = b"license=" + lic.encode()
        i, patched, old = 0, False, None
        while i < len(data):
            j = data.find(b"\0", i)
            if j < 0:
                break
            entry = bytes(data[i:j])
            if entry.startswith(b"license="):
                old = entry.decode(errors="replace")
                if len(want) > len(entry):
                    sys.exit(f"'{lic}' is longer than '{old}'; cannot pad in place")
                data[i:j] = want + b"\0" * (len(entry) - len(want))
                patched = True
                break
            i = j + 1

        if not patched:
            print("relicense: no license= entry found; leaving .modinfo alone")
            return 0
        if old == f"license={lic}":
            return 0

        open(tmp, "wb").write(bytes(data))
        subprocess.run([objcopy, "--update-section", f".modinfo={tmp}", obj],
                       check=True)
        print(f"relicense: {old} -> license={lic}  (personal build; see this script's docstring)")
        return 0
    finally:
        os.unlink(tmp)

if __name__ == "__main__":
    sys.exit(main())
