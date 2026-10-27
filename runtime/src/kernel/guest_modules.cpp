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
    for (const GuestModule& module : Merged()) {
        if (address >= module.base && address < module.end) {
            return true;
        }
    }
    return false;
}

std::string DescribeGuestAddress(std::uint32_t address) {
    std::string path;
    std::uint32_t offset = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const GuestModule& module : Merged()) {
            if (address >= module.base && address < module.end) {
                path = module.path;
                offset = address - module.base;
                break;
            }
        }
    }
    if (path.empty()) {
        char plain[16];
        std::snprintf(plain, sizeof(plain), "0x%08x", address);
        return plain;
    }
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
