#include "armbat/symbolizer.h"

#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <cxxabi.h>

namespace armbat {
namespace {

// A symbol table of a 25 MB game library is a few thousand entries; the cap is
// there for a pathological file full of aliases, not for the normal case.
constexpr std::size_t kMaxSymbols = 200000;

// Symbols without a size (libc's assembly helpers are like that) still belong
// to the code after them, but only for a short distance: further away the name
// would be a lie about a function the address is not in.
constexpr std::uint32_t kUnsizedReach = 0x4000;

struct Symbol {
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    std::uint32_t name_offset = 0;
    std::uint32_t name_length = 0;
    // Functions beat objects, and a defined symbol beats an undefined alias
    // that happens to share the address.
    int rank = 0;
};

struct Image {
    bool parsed = false;
    bool usable = false;
    std::string names;
    std::vector<Symbol> symbols;
};

std::mutex g_mutex;
std::map<std::string, Image> g_images;

unsigned SymbolRank(unsigned char info, std::uint16_t section) {
    const unsigned type = ELF32_ST_TYPE(info);
    const unsigned bind = ELF32_ST_BIND(info);
    int rank = 0;
    if (type == STT_FUNC || type == STT_GNU_IFUNC) {
        rank = 3;
    } else if (type == STT_OBJECT || type == STT_COMMON) {
        rank = 1;
    } else {
        rank = 2;
    }
    if (bind == STB_GLOBAL || bind == STB_WEAK) {
        rank += 1;
    }
    if (section == SHN_UNDEF) {
        rank = 0;
    }
    return static_cast<unsigned>(rank);
}

void AddTable(Image& image, const char* base, std::size_t file_size, const Elf32_Shdr* sections,
              std::size_t count, std::size_t index) {
    const Elf32_Shdr& table = sections[index];
    if (table.sh_link == 0 || table.sh_link >= count || table.sh_entsize != sizeof(Elf32_Sym)) {
        return;
    }
    const Elf32_Shdr& strings = sections[table.sh_link];
    if (table.sh_offset > file_size || table.sh_size > file_size - table.sh_offset ||
        strings.sh_offset > file_size || strings.sh_size > file_size - strings.sh_offset) {
        return;
    }
    const auto* symbols = reinterpret_cast<const Elf32_Sym*>(base + table.sh_offset);
    const std::size_t entries = table.sh_size / sizeof(Elf32_Sym);
    const char* names = base + strings.sh_offset;
    for (std::size_t i = 0; i < entries; ++i) {
        const Elf32_Sym& symbol = symbols[i];
        const unsigned type = ELF32_ST_TYPE(symbol.st_info);
        if (symbol.st_value == 0 || symbol.st_name == 0 || symbol.st_name >= strings.sh_size) {
            continue;
        }
        if (type != STT_FUNC && type != STT_OBJECT && type != STT_GNU_IFUNC &&
            type != STT_COMMON && type != STT_NOTYPE) {
            continue;
        }
        const char* name = names + symbol.st_name;
        const std::size_t remaining = strings.sh_size - symbol.st_name;
        const std::size_t length = ::strnlen(name, remaining);
        if (length == 0) {
            continue;
        }
        Symbol entry;
        entry.address = symbol.st_value & ~1u;  // Thumb functions carry the bit
        entry.size = symbol.st_size;
        entry.name_offset = static_cast<std::uint32_t>(image.names.size());
        entry.name_length = static_cast<std::uint32_t>(length);
        entry.rank = static_cast<int>(SymbolRank(symbol.st_info, symbol.st_shndx));
        if (entry.rank == 0) {
            continue;
        }
        image.names.append(name, length);
        image.names.push_back('\0');
        image.symbols.push_back(entry);
        if (image.symbols.size() >= kMaxSymbols) {
            return;
        }
    }
}

// One name per address: two names for the same stub are common (an alias and
// its implementation).
void Collapse(Image& image) {
    std::sort(image.symbols.begin(), image.symbols.end(),
              [](const Symbol& left, const Symbol& right) {
                  if (left.address != right.address) {
                      return left.address < right.address;
                  }
                  if (left.rank != right.rank) {
                      return left.rank > right.rank;
                  }
                  return left.size > right.size;
              });
    std::vector<Symbol> unique;
    unique.reserve(image.symbols.size());
    for (const Symbol& symbol : image.symbols) {
        if (!unique.empty() && unique.back().address == symbol.address) {
            continue;
        }
        unique.push_back(symbol);
    }
    image.symbols = std::move(unique);
}

bool ReadImage(const std::string& path, Image& image) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct stat status {};
    if (::fstat(fd, &status) != 0 || status.st_size < static_cast<off_t>(sizeof(Elf32_Ehdr)) ||
        status.st_size > static_cast<off_t>(512ll * 1024 * 1024)) {
        ::close(fd);
        return false;
    }
    const std::size_t size = static_cast<std::size_t>(status.st_size);
    void* mapping = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (mapping == MAP_FAILED) {
        return false;
    }
    const char* base = static_cast<const char*>(mapping);
    const auto* header = reinterpret_cast<const Elf32_Ehdr*>(base);
    bool ok = false;
    if (std::memcmp(header->e_ident, ELFMAG, SELFMAG) == 0 &&
        header->e_ident[EI_CLASS] == ELFCLASS32 && header->e_ident[EI_DATA] == ELFDATA2LSB &&
        header->e_shentsize == sizeof(Elf32_Shdr) && header->e_shoff != 0) {
        const std::size_t count = header->e_shnum;
        const std::size_t offset = header->e_shoff;
        if (offset <= size && count <= (size - offset) / sizeof(Elf32_Shdr)) {
            const auto* sections = reinterpret_cast<const Elf32_Shdr*>(base + offset);
            for (std::size_t index = 0; index < count; ++index) {
                if (sections[index].sh_type == SHT_SYMTAB ||
                    sections[index].sh_type == SHT_DYNSYM) {
                    AddTable(image, base, size, sections, count, index);
                    // Offsets are file offsets here, not load addresses: reject
                    // a section that points outside the mapping.
                }
            }
            ok = true;
        }
    }
    ::munmap(mapping, size);
    Collapse(image);
    image.usable = ok && !image.symbols.empty();
    return ok;
}

std::string Pretty(const char* name) {
    int status = 0;
    char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    if (status == 0 && demangled != nullptr) {
        std::string text(demangled);
        std::free(demangled);
        return text;
    }
    return name;
}

const Symbol* Find(const Image& image, std::uint32_t offset) {
    if (image.symbols.empty()) {
        return nullptr;
    }
    std::size_t low = 0;
    std::size_t high = image.symbols.size();
    while (low < high) {
        const std::size_t middle = (low + high) / 2;
        if (image.symbols[middle].address <= offset) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low == 0) {
        return nullptr;
    }
    const Symbol& symbol = image.symbols[low - 1];
    const std::uint32_t inside = offset - symbol.address;
    if (symbol.size != 0 && inside >= symbol.size) {
        return nullptr;
    }
    if (symbol.size == 0 && inside > kUnsizedReach) {
        return nullptr;
    }
    return &symbol;
}

}  // namespace

std::string SymbolizeModuleOffset(const std::string& host_path, std::uint32_t offset) {
    // An embedded image is registered by soname, not by a file it was read
    // from; there is nothing to parse and asking the filesystem would be wrong.
    if (host_path.empty() || host_path[0] != '/') {
        return {};
    }
    // Parsing happens under the lock: a fault report asks for a dozen addresses
    // in a row, and a half-built table would name nothing for the first of them.
    const std::lock_guard lock(g_mutex);
    Image& image = g_images[host_path];
    if (!image.parsed) {
        ReadImage(host_path, image);
        image.parsed = true;
    }
    if (!image.usable) {
        return {};
    }
    const Symbol* symbol = Find(image, offset & ~1u);
    if (symbol == nullptr) {
        return {};
    }
    const char* name = image.names.data() + symbol->name_offset;
    std::string text = Pretty(name);
    const std::uint32_t inside = (offset & ~1u) - symbol->address;
    if (inside != 0) {
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "+0x%x", inside);
        text += suffix;
    }
    return text;
}

void ForgetSymbols() {
    const std::lock_guard lock(g_mutex);
    g_images.clear();
}

}  // namespace armbat
