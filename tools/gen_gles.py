#!/usr/bin/env python3
"""Generates the mechanical GLES HLE handlers.

The guest reaches the HLE registry through the shim libraries, so every GL
entry point needs two halves: a trampoline in `libGLESv2.so` (see
gen_platform_shims.py) and a handler registered here. Most handlers do the same
thing -- cast the guest's ARM32 arguments to the host's C types and call the
driver -- and writing them by hand only adds places to make a typo.

This generator emits those handlers. It is deliberately narrow:

  * only signatures made entirely of plain 32-bit scalars are generated. The
    argument registers hold the value itself, so the handler is a cast and a
    call, with nothing to marshal;
  * anything that takes a pointer, a 64-bit value (GLint64, GLuint64,
    GLintptr, GLsizeiptr) or an opaque handle (GLsync) is skipped. How those
    cross the boundary is decided by the size of the guest range, the host ABI
    width or the handle table, and guessing there is how a container corrupts
    memory. They stay in guest_gles.cpp, or in the "unimplemented" bucket that
    answers zero;
  * names already registered by hand in guest_gles.cpp are never re-emitted.
    The manual list is read from the source, so it cannot drift from the code;
  * a value type this script does not know is a hard error rather than a
    silent skip: the NDK adding a type must be an explicit decision.

The symbol list comes from the NDK headers, which is the same surface the
shims export, so the two halves cannot disagree about which names exist.

Usage:
  gen_gles.py --ndk <ndk> --out runtime/src/hle/generated/guest_gles_auto.inc
  gen_gles.py --ndk <ndk> --out ... --check
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

HEADERS = (
    "GLES2/gl2.h",
    "GLES3/gl3.h",
    "GLES3/gl31.h",
    "GLES3/gl32.h",
)

# A scalar the guest passes in a single register. The tuple is the host C type
# and how the handler reads the argument: "float" goes through FloatArg because
# ARM32 puts the bit pattern in the register, everything else is a plain cast.
SCALARS = {
    "GLenum": ("unsigned int", "arg"),
    "GLbitfield": ("unsigned int", "arg"),
    "GLuint": ("unsigned int", "arg"),
    "GLint": ("int", "arg"),
    "GLsizei": ("int", "arg"),
    "GLboolean": ("unsigned char", "arg"),
    "GLbyte": ("signed char", "arg"),
    "GLubyte": ("unsigned char", "arg"),
    "GLshort": ("short", "arg"),
    "GLushort": ("unsigned short", "arg"),
    "GLchar": ("char", "arg"),
    "GLfixed": ("int", "arg"),
    "GLclampx": ("int", "arg"),
    "GLfloat": ("float", "float"),
    "GLclampf": ("float", "float"),
}

# Types that are not scalars on the host and have no mechanical marshalling.
# They are recognised so the generator can skip them by rule instead of failing
# on an unknown token; each one needs a hand-written handler before it can work.
NON_SCALAR = {
    "GLintptr",
    "GLsizeiptr",
    "GLint64",
    "GLuint64",
    "GLsync",
    # Callback typedefs are pointers even though the name carries no '*'.
    "GLDEBUGPROC",
    "GLDEBUGPROCKHR",
    "void",
}

PROTOTYPE = re.compile(
    r"GL_APICALL\s+(.+?)\s+GL_APIENTRY\s+(gl\w+)\s*\((.*?)\)\s*;", re.S
)
MANUAL_CALL = re.compile(r'registry\.Add\("(gl\w+)"')


def read_declarations(include_root: Path) -> dict[str, tuple[str, str, list[str]]]:
    """Returns name -> (return type, name, parameter type names)."""
    functions: dict[str, tuple[str, str, list[str]]] = {}
    for relative in HEADERS:
        path = include_root / relative
        if not path.is_file():
            raise SystemExit(f"{path}: missing GLES header")
        text = path.read_text(errors="replace")
        for match in PROTOTYPE.finditer(text):
            result, name, args = match.group(1), match.group(2), match.group(3)
            parameters = []
            if args.strip() not in ("", "void"):
                for part in args.split(","):
                    parameters.append(parameter_type(part))
            functions[name] = (normalise(result), name, parameters)
    return functions


def normalise(text: str) -> str:
    return " ".join(text.split())


def parameter_type(parameter: str) -> str:
    """Strips `const` and the parameter name, keeping the '*' if any."""
    text = normalise(parameter).replace(" const", "").replace("const ", "")
    match = re.search(r"[A-Za-z_]\w*$", text)
    base = text[: match.start()].strip() if match else text
    return base.replace(" ", "")


def classify(result: str, parameters: list[str]) -> tuple[str, str]:
    """Returns (kind, reason); kind is "generate", "skip" or "error"."""
    if "*" in result:
        return "skip", "returns a pointer"
    if result != "void" and result not in SCALARS:
        if result in NON_SCALAR:
            return "skip", f"returns {result}"
        return "error", f"unknown return type {result}"
    for parameter in parameters:
        if "*" in parameter:
            return "skip", "takes a pointer"
        if parameter not in SCALARS:
            if parameter in NON_SCALAR:
                return "skip", f"takes {parameter}"
            return "error", f"unknown parameter type {parameter}"
    return "generate", ""


def emit_handler(name: str, result: str, parameters: list[str]) -> list[str]:
    host_parameters = ", ".join(SCALARS[p][0] for p in parameters)
    lines = [
        f"    registry.Add(\"{name}\", [](HleCall& call) {{",
        f"        using Fn = {SCALARS[result][0] if result != 'void' else 'void'} (*)({host_parameters});",
        f"        static const Fn function = Gl<Fn>(\"{name}\");",
        "        if (function == nullptr) {",
        f"            call.Return(0);" if result != "void" else "            return;",
        "        }",
    ]
    arguments = []
    for index, parameter in enumerate(parameters):
        host, kind = SCALARS[parameter]
        if kind == "float":
            arguments.append(f"FloatArg(call, {index})")
        else:
            arguments.append(f"static_cast<{host}>(call.Arg({index}))")
    call_text = f"function({', '.join(arguments)})"
    if result == "void":
        lines.append(f"        {call_text};")
    else:
        lines.append(f"        call.Return(static_cast<std::uint32_t>({call_text}));")
    lines.append("    });")
    return lines


def generate(ndk: Path, source: Path) -> str:
    include_root = ndk / "toolchains" / "llvm" / "prebuilt"
    hosts = [entry for entry in include_root.iterdir() if entry.is_dir()]
    if not hosts:
        raise SystemExit(f"{include_root}: no prebuilt toolchain")
    include_root = hosts[0] / "sysroot" / "usr" / "include"

    functions = read_declarations(include_root)
    manual = set(MANUAL_CALL.findall(source.read_text(errors="replace")))

    generated: list[tuple[str, str, list[str]]] = []
    skipped: list[tuple[str, str]] = []
    for name in sorted(functions):
        result, _, parameters = functions[name]
        if name in manual:
            continue
        kind, reason = classify(result, parameters)
        if kind == "error":
            raise SystemExit(f"{name}: {reason}")
        if kind == "skip":
            skipped.append((name, reason))
            continue
        generated.append((name, result, parameters))

    lines = [
        "// Generated by tools/gen_gles.py. Do not edit.",
        "//",
        f"// {len(generated)} mechanical entry points from the NDK GLES headers.",
        f"// {len(skipped)} were left out: pointers, 64-bit values and GLsync need",
        "// marshalling this generator must not guess at. They stay in",
        "// guest_gles.cpp or answer zero until a hand-written handler exists.",
        "",
        "void RegisterGlesGeneratedFunctions(HleRegistry& registry) {",
    ]
    for name, result, parameters in generated:
        lines += emit_handler(name, result, parameters)
    lines.append("}")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", required=True)
    parser.add_argument(
        "--out",
        default=os.path.join("runtime", "src", "hle", "generated", "guest_gles_auto.inc"),
    )
    parser.add_argument(
        "--source",
        default=os.path.join("runtime", "src", "hle", "guest_gles.cpp"),
        help="manual handlers; names found there are never generated",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="do not write; exit 1 when the file on disk is stale",
    )
    arguments = parser.parse_args()

    text = generate(Path(arguments.ndk), Path(arguments.source))
    target = Path(arguments.out)
    if arguments.check:
        if not target.is_file() or target.read_text(encoding="utf-8") != text:
            print(f"{target}: stale, re-run tools/gen_gles.py", file=sys.stderr)
            return 1
        return 0
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="utf-8", newline="\n")
    print(f"{target}: generated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
