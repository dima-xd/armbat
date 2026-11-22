#include "armbat/guest_modules.h"

#include <algorithm>
#include <cstdio>
#include <mutex>

#include "armbat/symbolizer.h"

namespace armbat {
namespace {

// Every guest mapping is recorded, not just the merged image: a loader maps the
// head of an image to read its headers, unmaps it again, and then puts the real
// segments somewhere else. Merging those two into one range would claim the
// whole span between them, and every address inside would be described against
// the wrong base.
struct GuestMapping {
    std::uint32_t base;
    std::uint32_t length;
    std::string path;
};

std::mutex g_mutex;
std::vector<GuestMapping> g_mappings;

std::string BaseName(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The live mappings folded into one range per image. Caller holds g_mutex.
std::vector<GuestModule> Merged() {
    std::vector<GuestModule> modules;
    for (const GuestMapping& mapping : g_mappings) {
        bool merged = false;
        for (GuestModule& module : modules) {
            if (module.path != mapping.path) {
                continue;
            }
            module.base = std::min(module.base, mapping.base);
            module.end = std::max(module.end, mapping.base + mapping.length);
            merged = true;
            break;
        }
        if (!merged) {
            modules.push_back(
                GuestModule{mapping.base, mapping.base + mapping.length, mapping.path});
        }
    }
    return modules;
}

}  // namespace

void RegisterGuestModuleMapping(std::uint32_t base, std::uint32_t length, const std::string& path) {
    if (base == 0 || length == 0 || path.empty() || path == "-") {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_mappings.push_back(GuestMapping{base, length, path});
}

void UnregisterGuestMappings(std::uint32_t address, std::uint32_t length) {
    if (address == 0 || length == 0) {
        return;
    }
    const std::uint32_t end = address + length;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<GuestMapping> kept;
    for (const GuestMapping& mapping : g_mappings) {
        const std::uint32_t mapping_end = mapping.base + mapping.length;
        if (mapping_end <= address || mapping.base >= end) {
            kept.push_back(mapping);
            continue;
        }
        if (mapping.base < address) {
            kept.push_back(GuestMapping{mapping.base, address - mapping.base, mapping.path});
        }
        if (mapping_end > end) {
            kept.push_back(GuestMapping{end, mapping_end - end, mapping.path});
        }
    }
    g_mappings = std::move(kept);
}

std::vector<GuestModule> GuestModules() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return Merged();
}

bool IsGuestModuleAddress(std::uint32_t address) {
    std::lock_guard<std::mutex> lock(g_mutex);
    // The live mappings, not the merged view: an image whose head was mapped,
    // unmapped and mapped again appears twice, and the merged range would claim
    // the span between the two -- including addresses that belong to other
    // images.
    for (const GuestMapping& mapping : g_mappings) {
        if (address >= mapping.base && address < mapping.base + mapping.length) {
            return true;
        }
    }
    return false;
}

// The image that covers `address`, as "libc.so+0x17b44", written into a buffer
// the caller owns. Nothing is allocated and the mapping lock is only taken when
// it is free: a crash handler cannot wait for a thread that died holding it, and
// cannot afford an allocation in a heap that may be the thing that faulted, so
// the lookup is skipped rather than waited for.
bool DescribeGuestAddressInline(std::uint32_t address, char* out, std::size_t size) {
    if (out == nullptr || size == 0) {
        return false;
    }
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return false;
    }
    const std::string* path = nullptr;
    for (const GuestMapping& mapping : g_mappings) {
        if (address >= mapping.base && address < mapping.base + mapping.length) {
            path = &mapping.path;
            break;
        }
    }
    if (path == nullptr) {
        return false;
    }
    // The image base is the lowest address it was mapped at, which is the rule
    // the merged view uses as well.
    std::uint32_t base = address;
    for (const GuestMapping& mapping : g_mappings) {
        if (mapping.path == *path && mapping.base < base) {
            base = mapping.base;
        }
    }
    const char* name = path->c_str();
    if (const std::size_t slash = path->find_last_of('/'); slash != std::string::npos) {
        name += slash + 1;
    }
    std::snprintf(out, size, "%s+0x%x", name, address - base);
    return true;
}

std::string DescribeGuestAddress(std::uint32_t address) {
    std::string path;
    std::uint32_t base = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // The mapping that actually covers the address names the image; the
        // offset is taken against the lowest address that image was mapped at,
        // which is what its symbol table is relative to. Describing against the
        // merged range instead attributes every address between two mappings of
        // one image to that image, and the numbers come out far past its size.
        for (const GuestMapping& mapping : g_mappings) {
            if (address < mapping.base || address >= mapping.base + mapping.length) {
                continue;
            }
            path = mapping.path;
            base = mapping.base;
            for (const GuestMapping& other : g_mappings) {
                if (other.path == path && other.base < base) {
                    base = other.base;
                }
            }
            break;
        }
    }
    if (path.empty()) {
        char plain[16];
        std::snprintf(plain, sizeof(plain), "0x%08x", address);
        return plain;
    }
    const std::uint32_t offset = address - base;
    char suffix[24];
    std::snprintf(suffix, sizeof(suffix), "+0x%x", offset);
    std::string text = BaseName(path) + suffix;
    // The symbol is looked up outside the mapping lock: the first fault of a
    // session parses the image's symbol table, and no other thread should wait
    // for that to describe its own address.
    const std::string symbol = SymbolizeModuleOffset(path, offset);
    if (!symbol.empty()) {
        text += " (";
        text += symbol;
        text += ')';
    }
    return text;
}

}  // namespace armbat
