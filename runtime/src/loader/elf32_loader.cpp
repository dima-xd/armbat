#include "armbat/elf32_loader.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <elf.h>
#include <android/log.h>

#include "armbat/a32_machine.h"
#include "elf32_internal.h"

namespace armbat {
namespace {

constexpr std::uint32_t kPageSize = 4096;

std::uint32_t AlignDown(std::uint32_t value) {
    return value & ~(kPageSize - 1);
}

std::uint32_t AlignUp(std::uint32_t value) {
    return (value + kPageSize - 1) & ~(kPageSize - 1);
}

bool RangeOk(std::uint64_t offset, std::uint64_t length, std::size_t size) {
    return offset <= size && length <= size - offset;
}

#ifndef EF_ARM_ABI_FLOAT_HARD
#    define EF_ARM_ABI_FLOAT_HARD 0x400
#endif

// Android's packed relocation tables. Not produced by the NDK version these
// libraries were built with, but a newer .so would otherwise load "successfully"
// with no relocations applied and then jump through unrelocated pointers.
#ifndef DT_ANDROID_REL
#    define DT_ANDROID_REL 0x6000000f
#endif
#ifndef DT_ANDROID_RELA
#    define DT_ANDROID_RELA 0x60000010
#endif
#ifndef DT_RELR
#    define DT_RELR 36
#endif
#ifndef DT_ANDROID_RELR
#    define DT_ANDROID_RELR 0x6fffe000
#endif
#ifndef DT_GNU_HASH
#    define DT_GNU_HASH 0x6ffffef5
#endif

// elf_hash from the System V ABI: the hash the dynamic linker built DT_HASH
// with. It is not a good hash, but it is the one the table was filled by, so a
// lookup has to use exactly this.
std::uint32_t ElfHash(std::string_view name) {
    std::uint32_t hash = 0;
    for (const char character : name) {
        hash = (hash << 4) + static_cast<unsigned char>(character);
        const std::uint32_t high = hash & 0xf0000000u;
        if (high != 0) {
            hash ^= high >> 24;
        }
        hash &= ~high;
    }
    return hash;
}

}  // namespace

Elf32Loader::Elf32Loader(GuestArena& arena) : arena_(arena) {}

void Elf32Loader::SetImportResolver(ImportResolver resolver) {
    import_resolver_ = std::move(resolver);
}

std::optional<LoadedLibrary> Elf32Loader::Load(const void* image, std::size_t size, std::uint32_t base,
                                               std::string* error) {
    auto fail = [error](std::string message) -> std::optional<LoadedLibrary> {
        if (error) {
            *error = std::move(message);
        }
        return std::nullopt;
    };

    if (image == nullptr || size < sizeof(Elf32_Ehdr)) {
        return fail("ELF image is truncated");
    }

    const auto* eh = static_cast<const Elf32_Ehdr*>(image);
    if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        return fail("not an ELF file");
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS32) {
        return fail("not an ELF32 image");
    }
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        return fail("not little-endian");
    }
    if (eh->e_machine != EM_ARM) {
        return fail("not an ARM image");
    }
    if (eh->e_type != ET_DYN) {
        return fail("not a shared object (ET_DYN)");
    }
    if ((eh->e_flags & EF_ARM_ABI_FLOAT_HARD) != 0) {
        // The JIT and the HLE layer implement the softfp calling convention:
        // floating point arguments arrive in r0-r3. A hard-float image would pass
        // them in s0-s15 and every call would silently read garbage.
        return fail("hard-float ARM image (the emulator implements the ARM softfp ABI)");
    }
    if (eh->e_phentsize != sizeof(Elf32_Phdr)) {
        return fail("unexpected program header size");
    }
    if (eh->e_phnum == 0 || !RangeOk(eh->e_phoff, std::uint64_t(eh->e_phnum) * sizeof(Elf32_Phdr), size)) {
        return fail("program headers out of range");
    }
    if (base % kPageSize != 0) {
        return fail("load base must be page aligned");
    }

    const auto* image_bytes = static_cast<const std::uint8_t*>(image);
    const auto* phdrs = reinterpret_cast<const Elf32_Phdr*>(image_bytes + eh->e_phoff);

    std::vector<const Elf32_Phdr*> loads;
    for (std::size_t i = 0; i < eh->e_phnum; ++i) {
        const Elf32_Phdr& ph = phdrs[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) {
            continue;
        }
        if (!RangeOk(ph.p_offset, ph.p_filesz, size)) {
            return fail("loadable segment points outside the image");
        }
        if (ph.p_filesz > ph.p_memsz) {
            return fail("segment file size exceeds memory size");
        }
        const std::uint64_t end = std::uint64_t(base) + ph.p_vaddr + ph.p_memsz;
        if (base + ph.p_vaddr < base || end > arena_.Size()) {
            return fail("segment does not fit in the guest arena");
        }
        loads.push_back(&ph);
    }
    if (loads.empty()) {
        return fail("no loadable segments");
    }
    for (std::size_t i = 0; i < loads.size(); ++i) {
        for (std::size_t j = i + 1; j < loads.size(); ++j) {
            const std::uint64_t first_start = std::uint64_t(base) + loads[i]->p_vaddr;
            const std::uint64_t first_end = first_start + loads[i]->p_memsz;
            const std::uint64_t second_start = std::uint64_t(base) + loads[j]->p_vaddr;
            const std::uint64_t second_end = second_start + loads[j]->p_memsz;
            if (first_start < second_end && second_start < first_end) {
                return fail("overlapping loadable segments");
            }
        }
    }

    // Segments share their boundary page whenever p_align is at most the page
    // size, so mapping each segment separately would map that page twice. Merge
    // the page ranges first and commit every page exactly once.
    struct PageRange {
        std::uint32_t start;
        std::uint32_t end;
    };
    std::vector<PageRange> ranges;
    for (const Elf32_Phdr* ph : loads) {
        ranges.push_back({AlignDown(base + ph->p_vaddr),
                          AlignUp(base + ph->p_vaddr + ph->p_memsz)});
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const PageRange& left, const PageRange& right) { return left.start < right.start; });
    std::vector<PageRange> merged;
    for (const PageRange& range : ranges) {
        if (!merged.empty() && range.start <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, range.end);
        } else {
            merged.push_back(range);
        }
    }

    // Any failure past this point has to release the pages again: a retry would
    // otherwise load a second image on top of half-relocated memory.
    struct MappedGuard {
        GuestArena& arena;
        std::vector<PageRange> ranges;
        bool armed{true};
        ~MappedGuard() {
            if (!armed) {
                return;
            }
            for (const PageRange& range : ranges) {
                arena.Unmap(range.start, range.end - range.start);
            }
        }
    } guard{arena_, {}};

    // Pass 1: commit and zero every page. Zeroing first and copying later keeps
    // segments that share a page from erasing each other.
    for (const PageRange& range : merged) {
        if (!arena_.Map(range.start, range.end - range.start)) {
            return fail("cannot map guest segment");
        }
        guard.ranges.push_back(range);
        std::memset(arena_.Ptr(range.start), 0, range.end - range.start);
    }

    // Pass 2: file contents.
    for (const Elf32_Phdr* ph : loads) {
        if (ph->p_filesz != 0) {
            std::memcpy(arena_.Ptr(base + ph->p_vaddr), image_bytes + ph->p_offset, ph->p_filesz);
        }
    }

    const Elf32_Phdr* dyn_phdr = nullptr;
    for (std::size_t i = 0; i < eh->e_phnum; ++i) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dyn_phdr = &phdrs[i];
        }
    }    if (dyn_phdr == nullptr || !RangeOk(dyn_phdr->p_offset, dyn_phdr->p_filesz, size)) {
        return fail("no usable PT_DYNAMIC segment");
    }

    // The unwinder's table lives in its own segment on ARM, so it can be found
    // without the section header table (which stripped libraries usually drop).
    const Elf32_Phdr* exidx_phdr = nullptr;
    for (std::size_t i = 0; i < eh->e_phnum; ++i) {
        if (phdrs[i].p_type == PT_ARM_EXIDX) {
            exidx_phdr = &phdrs[i];
        }
    }

    detail::RelocationContext reloc{arena_};
    reloc.base = base;
    reloc.import_resolver = import_resolver_ ? &import_resolver_ : nullptr;

    LoadedLibrary lib;
    lib.base = base;

    const auto* dyn = reinterpret_cast<const Elf32_Dyn*>(image_bytes + dyn_phdr->p_offset);
    const std::size_t dyn_count = dyn_phdr->p_filesz / sizeof(Elf32_Dyn);
    std::uint32_t hash_table = 0;
    std::uint32_t gnu_hash_table = 0;
    std::uint32_t soname_offset = 0;
    std::vector<std::uint32_t> needed_offsets;

    for (std::size_t i = 0; i < dyn_count && dyn[i].d_tag != DT_NULL; ++i) {
        const std::uint32_t value = dyn[i].d_un.d_val;
        switch (dyn[i].d_tag) {
        case DT_STRTAB:
            reloc.strtab = base + value;
            break;
        case DT_SYMTAB:
            reloc.symtab = base + value;
            break;
        case DT_STRSZ:
            lib.strtab_size = value;
            break;
        case DT_SYMENT:
            reloc.syment = value;
            lib.syment = value;
            break;
        case DT_REL:
            reloc.rel = base + value;
            break;
        case DT_RELSZ:
            reloc.relsz = value;
            break;
        case DT_RELENT:
            reloc.relent = value;
            break;
        case DT_JMPREL:
            reloc.jmprel = base + value;
            break;
        case DT_PLTRELSZ:
            reloc.pltrelsz = value;
            break;
        case DT_PLTREL:
            reloc.pltrel = value;
            break;
        case DT_INIT:
            lib.entry = base + value;
            break;
        case DT_INIT_ARRAY:
            lib.init_array = base + value;
            break;
        case DT_INIT_ARRAYSZ:
            lib.init_array_size = value;
            break;
        case DT_HASH:
            hash_table = base + value;
            break;
        case DT_GNU_HASH:
            gnu_hash_table = base + value;
            break;
        case DT_SONAME:
            soname_offset = value;
            break;
        case DT_NEEDED:
            needed_offsets.push_back(value);
            break;
        case DT_RELA:
        case DT_RELASZ:
        case DT_RELR:
        case DT_ANDROID_REL:
        case DT_ANDROID_RELA:
        case DT_ANDROID_RELR:
            // These would need a second, different relocation walker. Refusing
            // to load is the only honest answer: applying nothing and reporting
            // success leaves the image full of unrelocated pointers.
            return fail("packed or RELA relocations are not supported (DT tag " +
                        std::to_string(dyn[i].d_tag) + ")");
        default:
            break;
        }
    }

    if (reloc.symtab != 0) {
        if (reloc.syment != sizeof(Elf32_Sym)) {
            return fail("unexpected symbol table entry size");
        }
        if (!arena_.IsMapped(reloc.symtab, sizeof(Elf32_Sym))) {
            return fail("DT_SYMTAB outside the loaded image");
        }
    }
    if (reloc.strtab != 0) {
        if (!arena_.IsMapped(reloc.strtab, 1)) {
            return fail("DT_STRTAB outside the loaded image");
        }
        if (lib.strtab_size == 0) {
            // Some linkers drop DT_STRSZ; the table still ends where the mapped
            // run that holds it ends.
            const std::uint64_t end = arena_.MappedEnd(reloc.strtab);
            if (end <= reloc.strtab) {
                return fail("cannot size the dynamic string table");
            }
            lib.strtab_size = static_cast<std::uint32_t>(end - reloc.strtab);
        }
    }

    // Publish the tables before anything reads them: the dependency and soname
    // lookups below go through ReadStrtabString, which works on LoadedLibrary.
    lib.symtab = reloc.symtab;
    lib.strtab = reloc.strtab;

    for (std::uint32_t offset : needed_offsets) {
        const std::optional<std::string> needed = ReadStrtabString(lib, offset);
        if (!needed) {
            return fail("DT_NEEDED outside the string table");
        }
        if (!virtual_libraries_.contains(*needed)) {
            return fail("missing dependency '" + *needed + "'");
        }
        lib.needed.push_back(*needed);
    }

    std::uint32_t applied = 0;
    if (!detail::ApplyRelocations(reloc, false, &applied, error)) {
        return std::nullopt;
    }
    if (!detail::ApplyRelocations(reloc, true, &applied, error)) {
        return std::nullopt;
    }
    lib.relocation_count = applied;

    lib.symtab = reloc.symtab;
    lib.strtab = reloc.strtab;

    // DT_HASH stores the symbol count directly. DT_GNU_HASH does not, so walk the
    // last bucket's chain to its terminator. The old "distance between .dynstr
    // and .dynsym" guess silently included .gnu.version entries, which then
    // looked like dozens of bogus exports.
    if (gnu_hash_table != 0 && arena_.IsMapped(gnu_hash_table, 16)) {
        const auto* header =
            reinterpret_cast<const std::uint32_t*>(arena_.Ptr(gnu_hash_table));
        const std::uint32_t buckets = header[0];
        const std::uint32_t symoffset = header[1];
        const std::uint32_t bloom_size = header[2];
        const std::uint64_t buckets_address =
            std::uint64_t(gnu_hash_table) + 16 + std::uint64_t(bloom_size) * 4;
        if (bloom_size <= 0x100000 && buckets <= 0x100000 &&
            buckets_address + std::uint64_t(buckets) * 4 <= arena_.Size() &&
            arena_.IsMapped(static_cast<std::uint32_t>(buckets_address), std::uint64_t(buckets) * 4)) {
            const auto* bucket = reinterpret_cast<const std::uint32_t*>(
                arena_.Ptr(static_cast<std::uint32_t>(buckets_address)));
            std::uint32_t last = 0;
            for (std::uint32_t i = 0; i < buckets; ++i) {
                last = std::max(last, bucket[i]);
            }
            if (last < symoffset) {
                lib.symbol_count = symoffset;
            } else {
                const std::uint64_t chain_address =
                    buckets_address + std::uint64_t(buckets) * 4;
                std::uint32_t index = last;
                for (;;) {
                    const std::uint64_t entry =
                        chain_address + std::uint64_t(index - symoffset) * 4;
                    if (entry + 4 > arena_.Size() ||
                        !arena_.IsMapped(static_cast<std::uint32_t>(entry), 4)) {
                        break;
                    }
                    std::uint32_t hash = 0;
                    std::memcpy(&hash, arena_.Ptr(static_cast<std::uint32_t>(entry)), sizeof(hash));
                    ++index;
                    if ((hash & 1u) != 0) {
                        break;
                    }
                    if (index > 0x200000) {
                        break;
                    }
                }
                lib.symbol_count = index;
            }
        }
    } else if (hash_table != 0 && arena_.IsMapped(hash_table, 8)) {
        lib.symbol_count = reinterpret_cast<const std::uint32_t*>(arena_.Ptr(hash_table))[1];
    } else if (reloc.strtab > reloc.symtab && reloc.syment != 0) {
        lib.symbol_count = (reloc.strtab - reloc.symtab) / reloc.syment;
    }

    // A usable SysV hash table turns an import lookup from a scan of every
    // symbol into a probe. It is only trusted when it and .dynsym are mapped in
    // full, so an index read from it can never point outside the image.
    if (hash_table != 0 && lib.symtab != 0 && lib.symbol_count != 0 &&
        lib.symbol_count <= 0x200000 && lib.syment == sizeof(Elf32_Sym) &&
        arena_.IsMapped(hash_table, 8) &&
        arena_.IsMapped(lib.symtab, std::uint64_t(lib.symbol_count) * lib.syment)) {
        const auto* header = reinterpret_cast<const std::uint32_t*>(arena_.Ptr(hash_table));
        const std::uint32_t bucket_count = header[0];
        const std::uint32_t chain_count = header[1];
        const std::uint64_t table_size =
            (2 + std::uint64_t(bucket_count) + chain_count) * sizeof(std::uint32_t);
        if (bucket_count != 0 && chain_count == lib.symbol_count &&
            arena_.IsMapped(hash_table, table_size)) {
            lib.hash_table = hash_table;
        }
    }

    if (soname_offset != 0) {
        const std::optional<std::string> soname = ReadStrtabString(lib, soname_offset);
        if (soname) {
            lib.soname = *soname;
        }
    }
    if (lib.soname.empty()) {
        lib.soname = "<unnamed>";
    }

    if (lib.init_array_size != 0 && !arena_.IsMapped(lib.init_array, lib.init_array_size)) {
        return fail("DT_INIT_ARRAY outside the loaded image");
    }

    lib.image_size = 0;
    for (const Elf32_Phdr* ph : loads) {
        lib.image_size = std::max(lib.image_size, ph->p_vaddr + ph->p_memsz);
    }

    if (exidx_phdr != nullptr && exidx_phdr->p_memsz >= 8 &&
        arena_.IsMapped(base + exidx_phdr->p_vaddr, exidx_phdr->p_memsz)) {
        lib.exidx = base + exidx_phdr->p_vaddr;
        lib.exidx_count = exidx_phdr->p_memsz / 8;
    }

    guard.armed = false;
    return lib;
}

const char* Elf32Loader::StrtabCString(const LoadedLibrary& lib, std::uint32_t offset) const {
    if (lib.strtab == 0 || lib.strtab_size == 0 || offset >= lib.strtab_size) {
        return nullptr;
    }
    const std::uint64_t address = std::uint64_t(lib.strtab) + offset;
    const std::uint64_t table_end = std::uint64_t(lib.strtab) + lib.strtab_size;
    if (address >= arena_.Size() || address >= table_end) {
        return nullptr;
    }
    const std::uint64_t mapped_end = arena_.MappedEnd(static_cast<std::uint32_t>(address));
    if (mapped_end <= address) {
        return nullptr;
    }
    const std::uint64_t limit = std::min(mapped_end, table_end);
    if (limit <= address) {
        return nullptr;
    }
    const char* text = reinterpret_cast<const char*>(arena_.Ptr(static_cast<std::uint32_t>(address)));
    if (std::memchr(text, 0, static_cast<std::size_t>(limit - address)) == nullptr) {
        return nullptr;
    }
    return text;
}

std::optional<std::string> Elf32Loader::ReadStrtabString(const LoadedLibrary& lib,
                                                         std::uint32_t offset) const {
    const char* text = StrtabCString(lib, offset);
    if (text == nullptr) {
        return std::nullopt;
    }
    return std::string(text);
}

void Elf32Loader::AddVirtualLibrary(std::string soname) {
    virtual_libraries_.insert(std::move(soname));
}

void AddSystemLibraries(Elf32Loader& loader) {
    for (const char* soname : {"libc.so", "libm.so", "libdl.so", "liblog.so",
                               "libandroid.so", "libstdc++.so", "libc++_shared.so", "libz.so",
                               "libGLESv2.so", "libEGL.so", "libOpenSLES.so"}) {
        loader.AddHostLibrary(soname);
    }
}

bool ReadImageIdentity(const void* image, std::size_t size, ImageIdentity* out) {
    if (out == nullptr) {
        return false;
    }
    out->soname.clear();
    out->needed.clear();
    if (image == nullptr || size < sizeof(Elf32_Ehdr)) {
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(image);
    const auto* eh = reinterpret_cast<const Elf32_Ehdr*>(bytes);
    if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_phentsize != sizeof(Elf32_Phdr) ||
        !RangeOk(eh->e_phoff, std::uint64_t(eh->e_phnum) * sizeof(Elf32_Phdr), size)) {
        return false;
    }

    const auto* phdrs = reinterpret_cast<const Elf32_Phdr*>(bytes + eh->e_phoff);
    const Elf32_Phdr* dynamic = nullptr;
    for (std::size_t index = 0; index < eh->e_phnum; ++index) {
        if (phdrs[index].p_type == PT_DYNAMIC) {
            dynamic = &phdrs[index];
        }
    }
    if (dynamic == nullptr || !RangeOk(dynamic->p_offset, dynamic->p_filesz, size)) {
        return false;
    }

    // DT_STRTAB holds a virtual address; in a file that has not been loaded yet
    // that address is only meaningful after translating it through PT_LOAD.
    struct FileRange {
        const char* data = nullptr;
        std::uint64_t available = 0;
    };
    auto file_range = [&](std::uint32_t address) -> FileRange {
        for (std::size_t index = 0; index < eh->e_phnum; ++index) {
            const Elf32_Phdr& ph = phdrs[index];
            if (ph.p_type != PT_LOAD || address < ph.p_vaddr) {
                continue;
            }
            const std::uint32_t delta = address - ph.p_vaddr;
            if (delta >= ph.p_filesz) {
                continue;
            }
            const std::uint64_t offset = std::uint64_t(ph.p_offset) + delta;
            if (!RangeOk(offset, 1, size)) {
                return {};
            }
            return {reinterpret_cast<const char*>(bytes + offset), ph.p_filesz - delta};
        }
        return {};
    };

    std::uint32_t strtab = 0;
    std::uint32_t strtab_size = 0;
    std::uint32_t soname_offset = 0;
    std::vector<std::uint32_t> needed_offsets;
    const auto* dyn = reinterpret_cast<const Elf32_Dyn*>(bytes + dynamic->p_offset);
    const std::size_t count = dynamic->p_filesz / sizeof(Elf32_Dyn);
    for (std::size_t index = 0; index < count && dyn[index].d_tag != DT_NULL; ++index) {
        switch (dyn[index].d_tag) {
        case DT_STRTAB:
            strtab = dyn[index].d_un.d_val;
            break;
        case DT_STRSZ:
            strtab_size = dyn[index].d_un.d_val;
            break;
        case DT_SONAME:
            soname_offset = dyn[index].d_un.d_val;
            break;
        case DT_NEEDED:
            needed_offsets.push_back(dyn[index].d_un.d_val);
            break;
        default:
            break;
        }
    }
    if (strtab == 0) {
        return true;  // an image without a string table has nothing to declare
    }
    const FileRange strings = file_range(strtab);
    if (strings.data == nullptr) {
        return false;
    }
    const std::uint64_t table_size =
        strtab_size != 0 ? std::min<std::uint64_t>(strtab_size, strings.available)
                         : strings.available;
    if (table_size == 0) {
        return false;
    }
    // A string is only accepted when its NUL terminator is inside the table; the
    // caller used to get a pointer into whatever followed the string table.
    auto string_at = [&](std::uint32_t offset) -> std::optional<std::string> {
        if (offset >= table_size) {
            return std::nullopt;
        }
        const char* start = strings.data + offset;
        const void* nul = std::memchr(start, 0, static_cast<std::size_t>(table_size - offset));
        if (nul == nullptr) {
            return std::nullopt;
        }
        return std::string(start, static_cast<const char*>(nul));
    };

    if (soname_offset != 0) {
        const std::optional<std::string> soname = string_at(soname_offset);
        if (!soname) {
            return false;
        }
        out->soname = *soname;
    }
    for (const std::uint32_t offset : needed_offsets) {
        const std::optional<std::string> needed = string_at(offset);
        if (!needed) {
            return false;
        }
        out->needed.push_back(*needed);
    }
    return true;
}

std::uint32_t Elf32Loader::Sym(const LoadedLibrary& lib, std::string_view name) const {
    if (lib.symtab == 0 || lib.strtab == 0 || lib.syment != sizeof(Elf32_Sym)) {
        return 0;
    }
    // An image that carries its own index is asked through it: a relocating game
    // library has around fifty thousand imports, and comparing each of them
    // against every one of its seventy-odd thousand symbols is what made linking
    // take seconds. The index covers exactly the symbols a scan would accept, so
    // there is no fallback when it misses.
    if (lib.hash_table != 0) {
        return SymThroughHash(lib, name);
    }
    for (std::uint32_t i = 0; i < lib.symbol_count; ++i) {
        const std::uint64_t entry_address =
            std::uint64_t(lib.symtab) + std::uint64_t(i) * lib.syment;
        if (entry_address + sizeof(Elf32_Sym) > arena_.Size() ||
            !arena_.IsMapped(static_cast<std::uint32_t>(entry_address), sizeof(Elf32_Sym))) {
            break;
        }
        const auto* sym = reinterpret_cast<const Elf32_Sym*>(
            arena_.Ptr(static_cast<std::uint32_t>(entry_address)));
        if (sym->st_shndx == SHN_UNDEF || sym->st_name == 0) {
            continue;
        }
        // A local definition is not an export, even when the name matches.
        if (ELF32_ST_BIND(sym->st_info) == STB_LOCAL) {
            continue;
        }
        const char* candidate = StrtabCString(lib, sym->st_name);
        if (candidate == nullptr) {
            continue;
        }
        if (name == candidate) {
            return sym->st_shndx == SHN_ABS ? sym->st_value : lib.base + sym->st_value;
        }
    }
    return 0;
}

std::uint32_t Elf32Loader::SymThroughHash(const LoadedLibrary& lib,
                                          std::string_view name) const {
    const std::uint32_t table = lib.hash_table;
    // Everything below was validated when the table was accepted, so the reads
    // are bounds-checked by construction: nbucket, nchain, the buckets and the
    // chains all sit inside mapped memory.
    const auto* header = reinterpret_cast<const std::uint32_t*>(arena_.Ptr(table));
    const std::uint32_t bucket_count = header[0];
    const std::uint32_t chain_count = header[1];
    const std::uint32_t* buckets = header + 2;
    const std::uint32_t* chains = buckets + bucket_count;

    std::uint32_t index = buckets[ElfHash(name) % bucket_count];
    // A chain is finite: it ends at 0, and every step must move to a valid
    // symbol. The cycle guard is there for a corrupt table, not for real ones.
    for (std::uint32_t step = 0; index != 0 && index < chain_count && step < chain_count; ++step) {
        const auto* symbol = reinterpret_cast<const Elf32_Sym*>(
            arena_.Ptr(static_cast<std::uint32_t>(lib.symtab) + index * lib.syment));
        if (symbol->st_shndx != SHN_UNDEF && symbol->st_name != 0 &&
            ELF32_ST_BIND(symbol->st_info) != STB_LOCAL) {
            const char* candidate = StrtabCString(lib, symbol->st_name);
            if (candidate != nullptr && name == candidate) {
                return symbol->st_shndx == SHN_ABS ? symbol->st_value
                                                   : lib.base + symbol->st_value;
            }
        }
        index = chains[index];
    }
    return 0;
}

std::vector<std::pair<std::string, std::uint32_t>> Elf32Loader::Exports(
    const LoadedLibrary& lib) const {
    std::vector<std::pair<std::string, std::uint32_t>> exports;
    if (lib.symtab == 0 || lib.strtab == 0) {
        return exports;
    }
    for (std::uint32_t i = 0; i < lib.symbol_count; ++i) {
        const std::uint64_t entry_address =
            std::uint64_t(lib.symtab) + std::uint64_t(i) * lib.syment;
        if (entry_address + sizeof(Elf32_Sym) > arena_.Size() ||
            !arena_.IsMapped(static_cast<std::uint32_t>(entry_address), sizeof(Elf32_Sym))) {
            break;
        }
        const auto* sym = reinterpret_cast<const Elf32_Sym*>(
            arena_.Ptr(static_cast<std::uint32_t>(entry_address)));
        if (sym->st_shndx == SHN_UNDEF || sym->st_name == 0) {
            continue;
        }
        // Only code: data exports cannot be a native method, and STT_NOTYPE
        // would drag in every symbol the linker kept.
        if (ELF32_ST_TYPE(sym->st_info) != STT_FUNC) {
            continue;
        }
        const std::uint32_t binding = ELF32_ST_BIND(sym->st_info);
        if (binding != STB_GLOBAL && binding != STB_WEAK) {
            continue;
        }
        const char* name = StrtabCString(lib, sym->st_name);
        if (name == nullptr) {
            continue;
        }
        const std::uint32_t address = sym->st_shndx == SHN_ABS ? sym->st_value : lib.base + sym->st_value;
        exports.emplace_back(name, address);
    }
    return exports;
}

bool Elf32Loader::RunInitializers(A32Machine& machine, const LoadedLibrary& lib, std::string* error) {
    // A constructor may call dlopen, which appends to the loader's container.
    // Copy everything this function needs before the first guest instruction runs.
    const std::uint32_t entry = lib.entry;
    const std::uint32_t init_array = lib.init_array;
    const std::uint32_t count = lib.init_array_size / sizeof(std::uint32_t);
    last_initializer_count_ = count;

    // bionic passes (argc, argv, envp) to initializers. Constructors that take no
    // arguments ignore the registers, but code written for __libc_init reads them.
    const std::initializer_list<std::uint32_t> arguments{0, 0, 0};
    // dlopen itself runs inside an HLE call, i.e. while the guest is executing.
    // dynarmic refuses to re-enter a running Jit, so a load triggered from guest
    // code has to go through the nested (second Jit) path.
    const bool nested = machine.Executing();
    const auto invoke = [&machine, &arguments, nested](std::uint32_t fn) {
        machine.ClearError();
        if (nested) {
            machine.CallNested(fn, arguments);
        } else {
            machine.CallFunction(fn, arguments);
        }
    };

    if (entry != 0) {
        if (!arena_.IsMapped(entry, 4)) {
            if (error) *error = "DT_INIT outside the loaded image";
            return false;
        }
        last_initializer_ = entry;
        last_initializer_index_ = 0;
        // LastError is sticky by design: a fault must not be overwritten by the
        // next entry. Everything that runs several entries has to clear it.
        invoke(entry);
        if (!machine.LastError().empty()) {
            if (error) *error = machine.LastError();
            return false;
        }
    }

    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t slot = init_array + i * sizeof(std::uint32_t);
        if (!arena_.IsMapped(slot, 4)) {
            if (error) *error = "DT_INIT_ARRAY entry outside the loaded image";
            return false;
        }
        std::uint32_t fn = 0;
        std::memcpy(&fn, arena_.Ptr(slot), sizeof(fn));
        if (fn == 0 || fn == 0xFFFFFFFFu) {
            continue;
        }
        if (!arena_.IsMapped(fn, 4)) {
            if (error) *error = "initializer points outside the loaded image";
            return false;
        }
        last_initializer_ = fn;
        last_initializer_index_ = i + 1;
        invoke(fn);
        if (!machine.LastError().empty()) {
            if (error) *error = machine.LastError();
            return false;
        }
    }
    return true;
}

std::vector<std::string> ReadDynamicFunctionNames(const void* image, std::size_t size) {
    std::vector<std::string> names;
    if (image == nullptr || size < sizeof(Elf32_Ehdr)) {
        return names;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(image);
    const auto* eh = reinterpret_cast<const Elf32_Ehdr*>(bytes);
    if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_shentsize != sizeof(Elf32_Shdr) ||
        !RangeOk(eh->e_shoff, std::uint64_t(eh->e_shnum) * sizeof(Elf32_Shdr), size)) {
        return names;
    }
    const auto* sections = reinterpret_cast<const Elf32_Shdr*>(bytes + eh->e_shoff);
    for (std::size_t index = 0; index < eh->e_shnum; ++index) {
        const Elf32_Shdr& section = sections[index];
        if (section.sh_type != SHT_DYNSYM || section.sh_link >= eh->e_shnum) {
            continue;
        }
        const Elf32_Shdr& strings_section = sections[section.sh_link];
        if (section.sh_entsize != sizeof(Elf32_Sym) ||
            !RangeOk(section.sh_offset, section.sh_size, size) ||
            !RangeOk(strings_section.sh_offset, strings_section.sh_size, size)) {
            continue;
        }
        const char* strings = reinterpret_cast<const char*>(bytes + strings_section.sh_offset);
        const auto* symbols = reinterpret_cast<const Elf32_Sym*>(bytes + section.sh_offset);
        const std::size_t count = section.sh_size / sizeof(Elf32_Sym);
        for (std::size_t symbol_index = 0; symbol_index < count; ++symbol_index) {
            const Elf32_Sym& symbol = symbols[symbol_index];
            if (symbol.st_shndx == SHN_UNDEF || symbol.st_name == 0 ||
                symbol.st_name >= strings_section.sh_size) {
                continue;
            }
            const unsigned bind = ELF32_ST_BIND(symbol.st_info);
            if (bind != STB_GLOBAL && bind != STB_WEAK) {
                continue;
            }
            if (ELF32_ST_TYPE(symbol.st_info) != STT_FUNC) {
                continue;
            }
            const char* name = strings + symbol.st_name;
            if (std::memchr(name, 0, strings_section.sh_size - symbol.st_name) == nullptr) {
                continue;
            }
            names.emplace_back(name);
        }
    }
    return names;
}

}  // namespace armbat
