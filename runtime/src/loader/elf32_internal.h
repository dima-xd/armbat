#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include <elf.h>

#include "armbat/elf32_loader.h"
#include "armbat/guest_arena.h"

namespace armbat::detail {

// Everything relocation processing needs, gathered while walking PT_DYNAMIC.
struct RelocationContext {
    GuestArena& arena;
    std::uint32_t base = 0;
    std::uint32_t symtab = 0;   // guest address
    std::uint32_t strtab = 0;   // guest address
    std::uint32_t syment = sizeof(Elf32_Sym);
    std::uint32_t rel = 0;      // guest address of DT_REL
    std::uint32_t relsz = 0;
    std::uint32_t relent = sizeof(Elf32_Rel);
    std::uint32_t jmprel = 0;   // guest address of DT_JMPREL
    std::uint32_t pltrelsz = 0;
    std::uint32_t pltrel = DT_REL;
    const std::function<std::uint32_t(std::string_view)>* import_resolver = nullptr;
};

// Applies one relocation table. Returns false and fills `error` on an
// unsupported type or an unresolved import.
bool ApplyRelocations(const RelocationContext& ctx, bool is_plt, std::uint32_t* applied,
                      std::string* error);

const char* SymbolName(const RelocationContext& ctx, std::uint32_t index);

}  // namespace armbat::detail
