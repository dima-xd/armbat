#include "elf32_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

// bionic's <elf.h> ships only a subset of the ARM relocation constants.
#ifndef R_ARM_TARGET1
#    define R_ARM_TARGET1 38
#endif
#ifndef R_ARM_V4BX
#    define R_ARM_V4BX 40
#endif
#ifndef R_ARM_PREL31
#    define R_ARM_PREL31 42
#endif
#ifndef R_ARM_ABS32_NOI
#    define R_ARM_ABS32_NOI 55
#endif
#ifndef R_ARM_TLS_DTPMOD32
#    define R_ARM_TLS_DTPMOD32 17
#endif
#ifndef R_ARM_TLS_DTPOFF32
#    define R_ARM_TLS_DTPOFF32 18
#endif
#ifndef R_ARM_TLS_TPOFF32
#    define R_ARM_TLS_TPOFF32 19
#endif
#ifndef R_ARM_IRELATIVE
#    define R_ARM_IRELATIVE 160
#endif

namespace armbat::detail {
namespace {

std::string Hex(std::uint32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08x", value);
    return buffer;
}

// A relocation may point at packed data, so the store goes through memcpy:
// casting an arbitrary guest address to std::uint32_t* is undefined behaviour on
// the host even when the guest address itself is valid.
bool Write32(const RelocationContext& ctx, std::uint32_t address, std::uint32_t value,
             std::string* error) {
    if (!ctx.arena.IsMapped(address, sizeof(std::uint32_t))) {
        if (error) *error = "relocation target " + Hex(address) + " outside the loaded image";
        return false;
    }
    std::memcpy(ctx.arena.Ptr(address), &value, sizeof(value));
    return true;
}

const Elf32_Sym* SymAt(const RelocationContext& ctx, std::uint32_t index) {
    return reinterpret_cast<const Elf32_Sym*>(ctx.arena.Ptr(ctx.symtab + index * ctx.syment));
}

// The name is only handed out when the whole string is inside one mapped run:
// a truncated or hostile string table must not turn into a read past the arena.
const char* NameAt(const RelocationContext& ctx, std::uint32_t sym_index) {
    if (ctx.strtab == 0 || ctx.symtab == 0 || ctx.syment != sizeof(Elf32_Sym)) {
        return "";
    }
    if (!ctx.arena.IsMapped(ctx.symtab + sym_index * ctx.syment, sizeof(Elf32_Sym))) {
        return "";
    }
    const Elf32_Sym* sym = SymAt(ctx, sym_index);
    if (sym->st_name == 0) {
        return "";
    }
    const std::uint64_t address = std::uint64_t(ctx.strtab) + sym->st_name;
    if (address >= ctx.arena.Size()) {
        return "";
    }
    const std::uint64_t end = ctx.arena.MappedEnd(static_cast<std::uint32_t>(address));
    if (end == 0 || end <= address) {
        return "";
    }
    const char* text = reinterpret_cast<const char*>(ctx.arena.Ptr(static_cast<std::uint32_t>(address)));
    if (std::memchr(text, 0, static_cast<std::size_t>(end - address)) == nullptr) {
        return "";
    }
    return text;
}

}  // namespace

const char* SymbolName(const RelocationContext& ctx, std::uint32_t index) {
    return NameAt(ctx, index);
}

bool ApplyRelocations(const RelocationContext& ctx, bool is_plt, std::uint32_t* applied,
                      std::string* error) {
    const std::uint32_t table = is_plt ? ctx.jmprel : ctx.rel;
    const std::uint32_t table_size = is_plt ? ctx.pltrelsz : ctx.relsz;
    if (table == 0 || table_size == 0) {
        return true;
    }
    if (is_plt && ctx.pltrel != DT_REL) {
        if (error) *error = "DT_PLTREL must be DT_REL for ARM";
        return false;
    }

    if (ctx.symtab != 0 && ctx.syment != sizeof(Elf32_Sym)) {
        if (error) *error = "unexpected symbol table entry size";
        return false;
    }

    const std::uint32_t entry_size = is_plt ? sizeof(Elf32_Rel) : ctx.relent;
    if (entry_size != sizeof(Elf32_Rel)) {
        if (error) *error = "unexpected relocation entry size";
        return false;
    }
    if (!ctx.arena.IsMapped(table, table_size)) {
        if (error) *error = "relocation table outside the loaded image";
        return false;
    }

    const auto fail = [error](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };

    const auto* rels = reinterpret_cast<const Elf32_Rel*>(ctx.arena.Ptr(table));
    const std::uint32_t count = table_size / entry_size;

    for (std::uint32_t i = 0; i < count; ++i) {
        const Elf32_Rel& rel = rels[i];
        const std::uint32_t type = ELF32_R_TYPE(rel.r_info);
        const std::uint32_t sym_index = ELF32_R_SYM(rel.r_info);

        if (type == R_ARM_NONE) {
            continue;
        }

        // r_offset is relative to the load base; adding it in 32 bits could wrap
        // around the address space and land on a mapped page by accident.
        const std::uint64_t where_wide = std::uint64_t(ctx.base) + rel.r_offset;
        if (where_wide >= ctx.arena.Size()) {
            return fail("relocation target " + Hex(rel.r_offset) + " outside the guest arena");
        }
        const auto where_address = static_cast<std::uint32_t>(where_wide);
        if (!ctx.arena.IsMapped(where_address, sizeof(std::uint32_t))) {
            return fail("relocation target " + Hex(where_address) + " outside the loaded image");
        }
        std::uint32_t addend = 0;
        std::memcpy(&addend, ctx.arena.Ptr(where_address), sizeof(addend));

        std::uint32_t symbol_value = 0;
        std::string_view symbol_name;
        bool weak = false;
        if (sym_index != 0) {
            if (ctx.symtab == 0 ||
                !ctx.arena.IsMapped(ctx.symtab + sym_index * ctx.syment, sizeof(Elf32_Sym))) {
                return fail("relocation symbol " + std::to_string(sym_index) +
                            " outside the symbol table");
            }
            const Elf32_Sym* sym = SymAt(ctx, sym_index);
            weak = ELF32_ST_BIND(sym->st_info) == STB_WEAK;
            if (sym->st_shndx == SHN_UNDEF) {
                symbol_name = NameAt(ctx, sym_index);
                if (ctx.import_resolver != nullptr && *ctx.import_resolver) {
                    symbol_value = (*ctx.import_resolver)(symbol_name);
                }
                if (symbol_value == 0) {
                    // A weak undefined symbol is defined as zero by the ABI; the
                    // library is expected to test the address before using it.
                    if (!weak) {
                        return fail("unresolved import '" + std::string(symbol_name) + "'");
                    }
                }
            } else {
                symbol_value =
                    sym->st_shndx == SHN_ABS ? sym->st_value : ctx.base + sym->st_value;
            }
        }

        std::uint32_t value = 0;
        switch (type) {
        case R_ARM_RELATIVE:
            value = ctx.base + addend;
            break;
        case R_ARM_ABS32:
        case R_ARM_ABS32_NOI:
        case R_ARM_TARGET1:
            value = symbol_value + addend;
            break;
        case R_ARM_REL32:
            value = symbol_value + addend - where_address;
            break;
        case R_ARM_GLOB_DAT:
        case R_ARM_JUMP_SLOT:
            value = symbol_value;
            break;
        case R_ARM_PREL31: {
            // The addend is a signed 31-bit value and bit 31 of the word belongs
            // to the unwind table's compact encoding, not to the offset.
            const auto signed_addend = static_cast<std::int32_t>(addend << 1) >> 1;
            const std::uint32_t offset =
                symbol_value + static_cast<std::uint32_t>(signed_addend) - where_address;
            value = (addend & 0x80000000u) | (offset & 0x7FFFFFFFu);
            break;
        }
        case R_ARM_TLS_DTPMOD32:
            // There is one module (the guest process) in this address space.
            value = 1;
            break;
        case R_ARM_TLS_DTPOFF32:
            value = symbol_value + addend;
            break;
        case R_ARM_TLS_TPOFF32:
            return fail("TLS model is not supported (relocation for '" +
                        std::string(symbol_name) + "')");
        case R_ARM_IRELATIVE:
            return fail("ifunc relocations are not supported (resolver for '" +
                        std::string(symbol_name) + "')");
        case R_ARM_V4BX:
            break;
        default:
            return fail("unsupported relocation type " + std::to_string(type) + " at " +
                        Hex(rel.r_offset));
        }

        if (type != R_ARM_V4BX && !Write32(ctx, where_address, value, error)) {
            return false;
        }
        if (applied != nullptr) {
            ++*applied;
        }
    }

    return true;
}

}  // namespace armbat::detail
