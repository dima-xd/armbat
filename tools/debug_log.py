#!/usr/bin/env python3
"""Turn an Armbat log into something readable.

A fault report names guest code the way the container sees it:

    fault: instruction fetch from unmapped guest address 0x00000000 (pc=0x00000000 ...)
    fault trace:
      #01 lr  libminecraftpe.so+0x1af8a44
      #02 stk libminecraftpe.so+0x155e54b

Reading that used to mean running llvm-nm, sorting it and searching for the
offset by hand. This script does that, and the rest of the archaeology:

  * resolves every `module+0xoffset` to the symbol that contains it,
  * names the import a faulting PLT call went through (`fault got: r12=...`),
  * disassembles the instruction that faulted,
  * prints a one-screen crash summary,
  * leaves the annotated log so it can still be read line by line.

Usage:
    python tools/debug_log.py log.txt --apk Minecraft.apk
    python tools/debug_log.py log.txt --apk Minecraft.apk --only-crash
    python tools/debug_log.py log.txt --so build/app-libs/libminecraftpe.so

The APK is the archive that was imported, on this machine. The sysroot and the
container's own guest images are indexed automatically.
"""

from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# module+0x1234, the form DescribeGuestAddress() prints.
MODULE_TOKEN = re.compile(r"([A-Za-z0-9_.+\-]+\.so)\+0x([0-9a-fA-F]+)")

# The one line that says a fault happened. Everything after it is detail.
FAULT_LINE = re.compile(
    r"^(instruction fetch from unmapped|read of \d+ bytes from unmapped|"
    r"write of \d+ bytes to unmapped|exclusive write of \d+ bytes to unmapped|"
    r"guest exception \d+|unhandled SVC|interpreter fallback)"
)

FRAME_LINE = re.compile(r"#(\d+)\s+(pc|lr|fp|stk)\s+(\S+)")

SHT_SYMTAB, SHT_STRTAB, SHT_RELA, SHT_DYNAMIC = 2, 3, 4, 6
SHT_REL, SHT_DYNSYM = 9, 11
PT_LOAD = 1
STT_OBJECT, STT_FUNC, STT_GNU_IFUNC = 1, 2, 10


# --------------------------------------------------------------------------
# ELF
# --------------------------------------------------------------------------


@dataclass
class Symbol:
    addr: int
    size: int
    name: str
    kind: int


@dataclass
class Image:
    """One shared object, indexed the way a guest address sees it.

    A guest address is resolved against the image's lowest PT_LOAD: the loader
    maps the file there, so `st_value - bias` is exactly the offset the
    container prints after `module+0x`.
    """

    path: Path
    name: str
    bias: int = 0
    symbols: list[Symbol] = field(default_factory=list)
    relocations: dict[int, str] = field(default_factory=dict)
    addresses: list[int] = field(default_factory=list)

    def finish(self) -> "Image":
        # One entry per address: a function beats an object, a sized symbol
        # beats an unsized alias.
        best: dict[int, Symbol] = {}
        for symbol in self.symbols:
            if symbol.addr == 0 or not symbol.name:
                continue
            previous = best.get(symbol.addr)
            if previous is None or Rank(symbol) < Rank(previous):
                best[symbol.addr] = symbol
        self.symbols = sorted(best.values(), key=lambda s: s.addr)
        self.addresses = [s.addr for s in self.symbols]
        return self

    def resolve(self, offset: int) -> tuple[str, int] | None:
        if not self.symbols:
            return None
        offset &= ~1  # Thumb functions carry the bit in st_value
        index = bisect.bisect_right(self.addresses, offset) - 1
        if index < 0:
            return None
        symbol = self.symbols[index]
        return symbol.name, offset - symbol.addr

    def relocation(self, offset: int) -> str | None:
        return self.relocations.get(offset & ~1)


def Rank(symbol: Symbol) -> tuple[int, int]:
    """Smaller is better when two symbols share an address."""
    named = 0 if symbol.kind in (STT_FUNC, STT_GNU_IFUNC) else 1
    unsized = 0 if symbol.size else 1
    return (named, unsized)


def ReadElf(path: Path) -> Image:
    data = path.read_bytes()
    if data[:4] != b"\x7fELF":
        raise ValueError(f"{path} is not an ELF file")
    is64 = data[4] == 2
    little = data[5] == 1
    endian = "<" if little else ">"

    if is64:
        (_, _, _, _, phoff, shoff, _, _, phentsize, phnum, shentsize, shnum,
         shstrndx) = struct.unpack_from(endian + "HHIQQQIHHHHHH", data, 16)
    else:
        (_, _, _, _, phoff, shoff, _, _, phentsize, phnum, shentsize, shnum,
         shstrndx) = struct.unpack_from(endian + "HHIIIIIHHHHHH", data, 16)

    bias = 0
    lowest = None
    for i in range(phnum):
        at = phoff + i * phentsize
        if is64:
            kind, _, _, vaddr = struct.unpack_from(endian + "IIQQ", data, at)
        else:
            kind, _, vaddr = struct.unpack_from(endian + "III", data, at)
        if kind == PT_LOAD and (lowest is None or vaddr < lowest):
            lowest = vaddr
    if lowest:
        bias = lowest & ~0xFFF  # the loader aligns the segment down

    sections = []
    for i in range(shnum):
        at = shoff + i * shentsize
        if is64:
            name, kind, flags, addr, offset, size, link, info, addralign, entsize = struct.unpack_from(
                endian + "IIQQQQIIQQ", data, at)
        else:
            name, kind, flags, addr, offset, size, link, info, addralign, entsize = struct.unpack_from(
                endian + "IIIIIIIIII", data, at)
        # Layout: name, type, addr, offset, size, link, info, align, entsize.
        sections.append((name, kind, addr, offset, size, link, info, addralign, entsize))

    def section_data(section) -> bytes:
        return data[section[3]:section[3] + section[4]]

    image = Image(path=path, name=path.name, bias=bias)

    symbols: list[Symbol] = []
    for name_off, kind, _, offset, size, link, _, _, entsize in sections:
        if kind not in (SHT_SYMTAB, SHT_DYNSYM) or link >= len(sections) or not size:
            continue
        strings = section_data(sections[link])
        count = size // entsize if entsize else 0
        for i in range(count):
            at = offset + i * entsize
            if is64:
                st_name, st_info, _, shndx, st_value, st_size = struct.unpack_from(
                    endian + "IBBHQQ", data, at)
            else:
                st_name, st_value, st_size, st_info, _, shndx = struct.unpack_from(
                    endian + "IIIBBH", data, at)
            symbol_kind = st_info & 0xF
            if symbol_kind not in (STT_FUNC, STT_OBJECT, STT_GNU_IFUNC, 0):
                continue
            if shndx == 0:  # undefined: an import, not an address here
                continue
            # Thumb functions carry the instruction-set bit in st_value, and the
            # container's module-relative offsets do not. Masking it everywhere
            # is safe: nothing the guest maps is odd-aligned.
            st_value &= ~1
            end = strings.find(b"\0", st_name)
            text = strings[st_name:end].decode("utf-8", "replace") if st_name else ""
            if not text:
                continue
            symbols.append(Symbol(st_value - bias, st_size, text, symbol_kind))
    image.symbols = symbols

    # Name of every GOT slot and every jump slot, keyed by its module-relative
    # offset. This is what answers "which import did this call go through".
    for name_off, kind, _, offset, size, link, _, _, entsize in sections:
        if kind not in (SHT_REL, SHT_RELA) or link >= len(sections) or not size:
            continue
        table = sections[link]
        if table[1] not in (SHT_SYMTAB, SHT_DYNSYM) or not table[8]:
            continue
        linked = sections[table[5]]
        strings = section_data(linked)
        count = size // entsize if entsize else 0
        for i in range(count):
            at = offset + i * entsize
            if is64:
                r_offset, r_info = struct.unpack_from(endian + "QQ", data, at)
                sym_index = r_info >> 32
                link_entsize = table[8]
                sym_at = table[3] + sym_index * link_entsize
                st_name = struct.unpack_from(endian + "I", data, sym_at)[0]
            else:
                r_offset, r_info = struct.unpack_from(endian + "II", data, at)
                sym_index = r_info >> 8
                sym_at = table[3] + sym_index * table[8]
                st_name = struct.unpack_from(endian + "I", data, sym_at)[0]
            end = strings.find(b"\0", st_name)
            if end <= st_name:
                continue
            text = strings[st_name:end].decode("utf-8", "replace")
            image.relocations[r_offset - bias] = text

    return image.finish()


# --------------------------------------------------------------------------
# Indexing and caching
# --------------------------------------------------------------------------


# Bump when the parsed layout changes: a stale index resolves to the wrong
# symbol, which is worse than no index at all.
CACHE_VERSION = 2


def CacheDirectory() -> Path:
    return ROOT / "build" / "symbols"


def IndexKey(path: Path) -> str:
    stat = path.stat()
    digest = f"v{CACHE_VERSION}|{path}|{stat.st_size}|{stat.st_mtime_ns}"
    return hashlib.sha1(digest.encode()).hexdigest()[:16]


def LoadImage(path: Path, cache: bool) -> Image:
    cache_file = CacheDirectory() / (IndexKey(path) + ".json")
    if cache and cache_file.exists():
        try:
            payload = json.loads(cache_file.read_text(encoding="utf-8"))
            image = Image(
                path=path,
                name=path.name,
                bias=payload["bias"],
                symbols=[Symbol(*item) for item in payload["symbols"]],
                relocations={int(k): v for k, v in payload["relocations"].items()},
            )
            return image.finish()
        except (OSError, ValueError, KeyError):
            pass
    image = ReadElf(path)
    if cache:
        try:
            cache_file.parent.mkdir(parents=True, exist_ok=True)
            cache_file.write_text(
                json.dumps({
                    "path": str(path),
                    "bias": image.bias,
                    "symbols": [[s.addr, s.size, s.name, s.kind] for s in image.symbols],
                    "relocations": {str(k): v for k, v in image.relocations.items()},
                }),
                encoding="utf-8",
            )
        except OSError:
            pass
    return image


def ExtractApk(apk: Path) -> list[Path]:
    """Unpack the 32-bit libraries of an archive once and keep them."""
    target = CacheDirectory() / "src" / IndexKey(apk)
    target.mkdir(parents=True, exist_ok=True)
    extracted = []
    with zipfile.ZipFile(apk) as archive:
        names = [n for n in archive.namelist() if n.endswith(".so") and n.startswith("lib/")]
        # armeabi-v7a is the only ABI the container runs.
        preferred = [n for n in names if n.startswith("lib/armeabi-v7a/")]
        for name in preferred or names:
            destination = target / Path(name).name
            if destination.exists() and destination.stat().st_size == archive.getinfo(name).file_size:
                extracted.append(destination)
                continue
            with archive.open(name) as source, open(destination, "wb") as sink:
                shutil.copyfileobj(source, sink)
            extracted.append(destination)
    return extracted


def FindLlvmTool(name: str) -> Path | None:
    for variable in ("ANDROID_NDK_ROOT", "ANDROID_NDK_HOME", "ANDROID_NDK"):
        root = os.environ.get(variable)
        if root:
            found = next(Path(root).glob(f"toolchains/llvm/prebuilt/*/bin/{name}*"), None)
            if found:
                return found
    for base in ("G:/Tools/Dev/AndroidSDK/ndk", os.path.expanduser("~/AppData/Local/Android/Sdk/ndk")):
        directory = Path(base)
        if not directory.is_dir():
            continue
        for version in sorted(directory.iterdir(), reverse=True):
            found = next(version.glob(f"toolchains/llvm/prebuilt/*/bin/{name}*"), None)
            if found:
                return found
    found = shutil.which(name)
    return Path(found) if found else None


def Demangle(names: set[str]) -> dict[str, str]:
    mangled = sorted({n for n in names if n.startswith("_Z")})
    if not mangled:
        return {}
    tool = FindLlvmTool("llvm-cxxfilt")
    if tool is None:
        return {}
    try:
        result = subprocess.run(
            [str(tool)], input="\n".join(mangled), capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=120,
        )
    except (OSError, subprocess.SubprocessError):
        return {}
    lines = result.stdout.splitlines()
    if len(lines) != len(mangled):
        return {}
    return dict(zip(mangled, lines))


# --------------------------------------------------------------------------
# Analysis
# --------------------------------------------------------------------------


@dataclass
class Frame:
    slot: int
    kind: str
    text: str


class Report:
    def __init__(self, images: dict[str, Image]) -> None:
        self.images = images
        self.demangled: dict[str, str] = {}

    # ---- symbol lookup ----------------------------------------------------

    def symbol(self, module: str, offset: int) -> tuple[str, int] | None:
        image = self.images.get(module)
        if image is None:
            return None
        found = image.resolve(offset)
        if found is None:
            return None
        return self.demangled.get(found[0], found[0]), found[1]

    def collect(self, text: str) -> set[str]:
        """Every name this log will display, so it can be demangled in one call."""
        names: set[str] = set()
        for line in text.splitlines():
            for module, offset in MODULE_TOKEN.findall(line):
                found = self.symbol(module, int(offset, 16))
                if found:
                    names.add(found[0])
            if "fault got:" in line:
                match = MODULE_TOKEN.search(line)
                image = self.images.get(match.group(1)) if match else None
                name = image.relocation(int(match.group(2), 16)) if image else None
                if name:
                    names.add(name)
        return names

    def annotate(self, line: str) -> str:
        def replace(match: re.Match) -> str:
            module, offset = match.group(1), int(match.group(2), 16)
            resolved = self.symbol(module, offset)
            if resolved is None:
                return match.group(0)
            name, delta = resolved
            return f"{module}+0x{offset:x} ({name}+0x{delta:x})"

        return MODULE_TOKEN.sub(replace, line)


LOGCAT_PREFIX = re.compile(r"^\d\d-\d\d\s+[\d:.]+\s+\d+\s+\d+\s+[A-Z]\s+[^:]*:\s*")


def StripLogcat(line: str) -> str:
    """Drop the timestamp, pid and tag so a summary line reads as a sentence."""
    return LOGCAT_PREFIX.sub("", line).strip()


def ParseReport(lines: list[str], report: Report, images: dict[str, Image]) -> str:
    """One screen that answers: where, how, and who called it."""
    fault = next((StripLogcat(line) for line in lines if FAULT_LINE.search(line)), None)
    if fault is None:
        fault = next((StripLogcat(line) for line in lines if "unmapped guest address" in line), None)
    if fault is None:
        return ""

    frames: list[Frame] = []
    for line in lines:
        match = FRAME_LINE.search(line)
        if match:
            frames.append(Frame(int(match.group(1)), match.group(2), match.group(3)))

    got = next((StripLogcat(line) for line in lines if "fault got:" in line), None)
    calls = next((StripLogcat(line) for line in lines if "fault calls:" in line), None)
    thread = next((StripLogcat(line) for line in lines if "fault thread:" in line), None)

    out = ["", "=" * 78, "crash", "=" * 78]
    reason = fault[len("fault: "):] if fault.startswith("fault: ") else fault
    out.append(f"reason:  {report.annotate(reason)}")
    if thread:
        out.append("thread:  " + report.annotate(thread.replace("fault thread:", "").strip()))
    if frames:
        out.append("chain:")
        for frame in frames[:12]:
            where = report.annotate(frame.text)
            out.append(f"  #{frame.slot:02d} {frame.kind:<3} {where}")
    if got:
        out.append("import:  " + AnnotateImport(got, report, images))
    instruction = Disassemble(frames, lines, report, images)
    if instruction:
        out.append("code:")
        for line in instruction:
            out.append("  " + line)
    if calls:
        out.append("calls:   " + calls.split("fault calls:", 1)[-1].strip())

    notes = NotesFor(fault, frames, report)
    if notes:
        out.append("notes:")
        for note in notes:
            out.append("  - " + note)
    out.append("=" * 78)
    return "\n".join(out)


def AnnotateImport(got: str, report: Report, images: dict[str, Image]) -> str:
    """`fault got: r12=lib+0x26d4cd0 target=0x0 ...` -> the import behind it."""
    match = MODULE_TOKEN.search(got)
    target = re.search(r"target=(\S+)", got)
    if match is None:
        return report.annotate(got)
    module, offset = match.group(1), int(match.group(2), 16)
    image = images.get(module)
    name = image.relocation(offset) if image else None
    if name is None:
        return report.annotate(got)
    demangled = report.demangled.get(name, name)
    if target is not None and int(target.group(1), 16) == 0:
        return (f"r12={module}+0x{offset:x} is the slot of `{demangled}`, "
                f"and it was loaded as NULL")
    return f"r12={module}+0x{offset:x} is the slot of `{demangled}`"


def Disassemble(frames: list[Frame], lines: list[str], report: Report,
                images: dict[str, Image]) -> list[str]:
    """The instruction that faulted, or the call that jumped into nothing."""
    tool = FindLlvmTool("llvm-objdump")
    if tool is None or not frames:
        return []
    cpsr = next((int(m.group(1), 16) for line in lines
                 for m in [re.search(r"cpsr=0x([0-9a-fA-F]+)", line)] if m), None)
    thumb = (cpsr & 0x20) != 0 if cpsr is not None else True

    # Prefer the faulting pc; when it is outside every image the fault was a
    # jump, and the caller's `blx` is the instruction worth seeing.
    chosen = None
    for frame in frames[:4]:
        match = MODULE_TOKEN.search(frame.text)
        if match and match.group(1) in images:
            chosen = (match.group(1), int(match.group(2), 16), frame.kind)
            break
    if chosen is None:
        return []
    module, offset, kind = chosen
    image = images[module]
    start = max(0, offset - 12) if kind in ("lr", "stk") else max(0, offset - 8)
    stop = offset + 8
    triple = "thumbv7-linux-androideabi" if thumb else "armv7-linux-androideabi"
    try:
        result = subprocess.run(
            [str(tool), "-d", f"--triple={triple}", "--no-show-raw-insn",
             f"--start-address=0x{start:x}", f"--stop-address=0x{stop:x}", str(image.path)],
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120,
        )
    except (OSError, subprocess.SubprocessError):
        return []
    out = []
    marked = False
    for line in result.stdout.splitlines():
        stripped = line.strip()
        if not stripped or stripped.endswith(":"):
            continue
        # `1af670a:  blx  r0`
        match = re.match(r"([0-9a-f]+):\s+(.*)", stripped)
        if not match:
            continue
        at = int(match.group(1), 16)
        # A return address is the instruction *after* the call, so the call is
        # the first instruction that ends exactly where the frame starts.
        marker = "  "
        if not marked:
            if kind == "pc" and at == offset:
                marker, marked = ">>", True
            elif kind in ("lr", "stk") and at < offset <= at + 4:
                marker, marked = ">>", True
        out.append(f"{marker} +0x{at:x}: {match.group(2)}")
    return out


def NotesFor(fault: str, frames: list[Frame], report: Report) -> list[str]:
    """The two or three conclusions a person reaches by hand today."""
    notes = []
    address = re.search(r"unmapped guest address (0x[0-9a-fA-F]+)", fault)
    if address and int(address.group(1), 16) < 0x1000:
        notes.append("the guest dereferenced a null pointer"
                     + (f" plus 0x{int(address.group(1), 16):x}"
                        if int(address.group(1), 16) else ""))
    if "instruction fetch" in fault:
        notes.append("a call went through a slot that held nothing: an unresolved "
                     "import, or a method missing from the object's vtable")
    if "unhandled SVC" in fault:
        notes.append("the guest used a trap the container does not answer; the "
                     "runtime prints each SVC number once")
    if "interpreter fallback" in fault:
        notes.append("the JIT met an instruction it does not implement (NEON and "
                     "ARMv8 instructions are the usual ones)")
    symbols = []
    for frame in frames:
        match = MODULE_TOKEN.search(frame.text)
        if match:
            found = report.symbol(match.group(1), int(match.group(2), 16))
            if found:
                symbols.append(found[0])
    named = symbols
    if any("abort" in n or "assert" in n.lower() or "throw" in n.lower() for n in named):
        notes.append("the chain reaches an abort/assert path: the crash is a "
                     "reported failure, not corruption")
    return notes


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------


def DefaultImages() -> list[Path]:
    found: list[Path] = []
    sysroot = ROOT / "runtime" / "sysroot" / "armeabi-v7a" / "lib"
    if sysroot.is_dir():
        found.extend(sorted(sysroot.glob("*.so")))
    guest = ROOT / "build" / "arm64" / "guest"
    if guest.is_dir():
        found.extend(sorted(guest.glob("*.so")))
    return found


def ReadLog(path: Path) -> str:
    """A log written by `adb logcat -d > log.txt` in PowerShell is UTF-16.

    Reading it as UTF-8 leaves NUL between every byte and silently hides every
    line: the tool then reports "no fault" on a log full of faults.
    """
    data = path.read_bytes()
    if data.startswith(b"\xff\xfe") or data.startswith(b"\xfe\xff"):
        return data.decode("utf-16", errors="replace")
    if data.startswith(b"\xef\xbb\xbf"):
        return data[3:].decode("utf-8", errors="replace")
    # UTF-16LE without a BOM still has NUL in every other byte.
    if len(data) > 4 and data[1::2].count(0) > len(data) // 4:
        return data.decode("utf-16-le", errors="replace")
    return data.decode("utf-8", errors="replace")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Decode an Armbat log.")
    parser.add_argument("log", nargs="?", help="log file, stdin when omitted")
    parser.add_argument("--apk", action="append", default=[], help="imported archive to index")
    parser.add_argument("--so", action="append", default=[], help="library to index")
    parser.add_argument("--no-sysroot", action="store_true", help="skip the bundled sysroot")
    parser.add_argument("--no-cache", action="store_true", help="re-read ELF files every time")
    parser.add_argument("--only-crash", action="store_true", help="print the summary alone")
    parser.add_argument("--list", action="store_true", help="show the indexed images and stop")
    arguments = parser.parse_args(argv)

    sources: list[Path] = []
    if not arguments.no_sysroot:
        sources.extend(DefaultImages())
    for apk in arguments.apk:
        path = Path(apk)
        if not path.is_file():
            print(f"no such archive: {path}", file=sys.stderr)
            return 2
        sources.extend(ExtractApk(path))
    sources.extend(Path(item) for item in arguments.so)

    images: dict[str, Image] = {}
    for path in sources:
        if path.name in images:
            continue  # the sysroot owns a name; the built stubs are fallbacks
        try:
            images[path.name] = LoadImage(path, cache=not arguments.no_cache)
        except (OSError, ValueError, struct.error) as error:
            print(f"skip {path}: {error}", file=sys.stderr)

    if arguments.list:
        for name, image in sorted(images.items()):
            print(f"{name:<28} {len(image.symbols):>7} symbols  "
                  f"{len(image.relocations):>5} relocations  {image.path}")
        return 0

    if arguments.log:
        text = ReadLog(Path(arguments.log))
    else:
        text = sys.stdin.read()
    lines = text.splitlines()

    report = Report(images)
    report.demangled = Demangle(report.collect(text))

    if not arguments.only_crash:
        for line in lines:
            print(report.annotate(line))

    summary = ParseReport(lines, report, images)
    if summary:
        print(summary)
    elif not arguments.only_crash:
        print("\nno fault in this log", file=sys.stderr)
    return 0


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    raise SystemExit(main(sys.argv[1:]))
