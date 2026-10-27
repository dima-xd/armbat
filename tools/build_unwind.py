#!/usr/bin/env python3
"""Export the ARM EHABI unwinder entry points from the NDK's libunwind.a.

bionic keeps its unwinder inside libc.so; the container has to ship its own, so a
guest image that calls `_Unwind_*` can import them. The NDK archive is built with
hidden visibility, and a shared object linked from it as-is keeps every entry
point local. Flipping those symbols to default visibility and linking the whole
archive lets the linker export them; ARM/Thumb interworking stays its job.
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

EXPORT = re.compile(r"^(_Unwind|__gnu_unwind|__aeabi_unwind)")


def run(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(" ".join(str(c) for c in cmd) + "\n" + result.stderr)
        raise SystemExit(1)
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--clang", required=True)
    parser.add_argument("--objcopy", required=True)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--archive", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--target", default="armv7a-linux-androideabi21")
    args = parser.parse_args()

    archive = pathlib.Path(args.archive)
    if not archive.is_file():
        raise SystemExit("missing archive: %s" % archive)

    names = sorted({
        parts[-1]
        for parts in (line.split() for line in
                      run([args.nm, "--defined-only", "--extern-only", str(archive)]).splitlines())
        if len(parts) >= 3 and EXPORT.match(parts[-1])
    })
    if not names:
        raise SystemExit("archive defines no unwind entry points")

    work = pathlib.Path(tempfile.mkdtemp(prefix="armbat-unwind-"))
    public = work / "libunwind-public.a"
    symbols = work / "symbols.txt"
    symbols.write_text("\n".join(names) + "\n", encoding="ascii")

    run([args.objcopy, "--set-symbols-visibility=%s=default" % symbols,
         str(archive), str(public)])
    run([args.clang,
         "--target=" + args.target,
         "-march=armv7-a",
         "-mfpu=neon",
         "-mfloat-abi=softfp",
         "-fPIC",
         "-shared",
         "-nostdlib",
         "-Wl,--whole-archive", str(public), "-Wl,--no-whole-archive",
         "-Wl,-soname,libarmbat_unwind.so",
         "-o", str(args.out)])

    exported = set(run([args.nm, "-D", "--defined-only", str(args.out)]).split())
    missing = [name for name in names if name not in exported]
    if missing:
        raise SystemExit("unwinder did not export: %s" % ", ".join(missing))
    print("unwinder: %d entry points" % len(names))


if __name__ == "__main__":
    main()
