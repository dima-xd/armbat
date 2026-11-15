#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace armbat {

// One loaded image in the guest address space, as reported by the guest's own
// loader through mmap. Used to turn a bare guest address into
// "libc.so+0x17b44" in diagnostics: a container that prints bare addresses
// makes every guest fault look like a mystery.
struct GuestModule {
    std::uint32_t base = 0;
    std::uint32_t end = 0;
    std::string path;
};

void RegisterGuestModuleMapping(std::uint32_t base, std::uint32_t length, const std::string& path);

// Drops the mappings the range covers, so an image whose header was mapped and
// unmapped again is described against the segments that are actually loaded.
void UnregisterGuestMappings(std::uint32_t address, std::uint32_t length);

std::vector<GuestModule> GuestModules();

// "libc.so+0x17b44", or "0x20033b44" when no module covers the address.
std::string DescribeGuestAddress(std::uint32_t address);

// The same description written into the caller's buffer, without allocating and
// without waiting for the mapping lock. For a crash handler, which has neither
// the time nor a heap to spare, and which must not hang if the fault happened
// while the mapping table was being updated.
bool DescribeGuestAddressInline(std::uint32_t address, char* out, std::size_t size);

// The image covering the address, or null when the address is not inside one.
bool IsGuestModuleAddress(std::uint32_t address);

}  // namespace armbat
