#pragma once

#include <cstdint>
#include <string>

namespace armbat {

// Names an offset inside a loaded image, e.g.
// "DBStorage::syncAndSuspendStorage+0x0", so a fault report reads like the
// symbol a person would look up by hand.
//
// The image is read from its host path and parsed once; the symbols stay in
// memory for the rest of the session. An empty string means the file is not an
// ELF32 image, has no symbol table, or the offset lies outside every symbol --
// the caller then prints the bare `module+0xoffset` it had before.
std::string SymbolizeModuleOffset(const std::string& host_path, std::uint32_t offset);

// Drops the parsed images; a new session may load different builds of the same
// soname, and the old symbol table would name the wrong functions.
void ForgetSymbols();

}  // namespace armbat
