#!/usr/bin/env python3
"""Builds the APK fixture the runtime tests read.

The point is not to look like a real application, but to exercise the parts of a
real APK the container touches first: a manifest that is only bytes to us, a dex
that is never opened, native libraries under lib/armeabi-v7a, and libraries for
other ABIs that must be ignored. One entry is stored, the other deflated, so both
zip methods go through the reader.
"""

import argparse
import zipfile


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True, help="arm32 .so to package")
    parser.add_argument("--extra-library", help="second arm32 .so, dependent on the first")
    parser.add_argument("--output", required=True, help="APK to write")
    parser.add_argument(
        "--abi",
        default="armeabi-v7a",
        choices=["armeabi-v7a", "armeabi"],
        help="directory the libraries are packaged under",
    )
    args = parser.parse_args()

    with open(args.library, "rb") as source:
        library = source.read()

    extra = None
    if args.extra_library:
        with open(args.extra_library, "rb") as source:
            extra = source.read()

    with zipfile.ZipFile(args.output, "w") as apk:
        # Binary XML would be pointless here: the runtime never parses it yet, it
        # only has to hand it to whoever asks.
        apk.writestr("AndroidManifest.xml", b"\x03\x00\x08\x00" + b"\x00" * 60,
                     compress_type=zipfile.ZIP_STORED)
        apk.writestr("classes.dex", b"dex\n035\x00" + b"\x00" * 120,
                     compress_type=zipfile.ZIP_STORED)
        apk.writestr(f"lib/{args.abi}/libguesttest.so", library,
                     compress_type=zipfile.ZIP_DEFLATED)
        if extra is not None:
            apk.writestr(f"lib/{args.abi}/libguesttest_dep.so", extra,
                         compress_type=zipfile.ZIP_DEFLATED)
        # The container must pick the arm32 library and leave this one alone.
        apk.writestr("lib/arm64-v8a/libguesttest.so", b"\x7fELF" + b"\x02" * 64,
                     compress_type=zipfile.ZIP_STORED)
        # An empty v7a directory must not beat a populated legacy one.
        if args.abi == "armeabi":
            apk.writestr("lib/armeabi-v7a/", b"", compress_type=zipfile.ZIP_STORED)
        apk.writestr("res/raw/notes.txt", b"armenian bat\n",
                     compress_type=zipfile.ZIP_DEFLATED)


if __name__ == "__main__":
    main()
