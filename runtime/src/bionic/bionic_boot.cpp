#include "armbat/bionic_boot.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <vector>

#include <android/log.h>

#include "armbat/log.h"
#include <sys/random.h>

#include "armbat/a32_machine.h"
#include "armbat/guest_arena.h"
#include "armbat/guest_cpu.h"

#include "guest_boot.h"
#include "sysroot_linker.h"

// The bionic boot program's own handshake, mirrored from runtime/guest/boot.c.
namespace {
constexpr std::uint32_t kApiMagic = 0x41524254u;  // "ARBT"
constexpr std::uint32_t kApiVersion = 1u;
constexpr std::uint32_t kApiSize = 12u + 4u * 9u;

// Guest layout owned by the boot path. Both bases sit above the container's
// dlopen region (0x06000000) and below the kernel's mmap region (0x20000000),
// so neither the HLE images nor bionic's own allocations can collide with them.
constexpr std::uint32_t kLinkerBase = 0x0a000000;
constexpr std::uint32_t kBootBase = 0x0b000000;
constexpr std::uint32_t kScratchBase = 0x0c000000;
constexpr std::uint32_t kScratchSize = 0x00010000;
constexpr std::uint32_t kPageSize = 4096;

// The core is described once, in guest_cpu.h: the linker's AT_HWCAP, the
// getauxval HLE call and /proc/cpuinfo have to agree or a library picks a code
// path from one descriptor that the others deny.
constexpr std::uint32_t kHwcap = armbat::kGuestHwcap;

constexpr std::uint32_t AtNull = 0;
constexpr std::uint32_t AtPhdr = 3;
constexpr std::uint32_t AtPhnum = 5;
constexpr std::uint32_t AtPagesz = 6;
constexpr std::uint32_t AtBase = 7;
constexpr std::uint32_t AtFlags = 8;
constexpr std::uint32_t AtEntry = 9;
constexpr std::uint32_t AtUid = 11;
constexpr std::uint32_t AtEuid = 12;
constexpr std::uint32_t AtGid = 13;
constexpr std::uint32_t AtEgid = 14;
constexpr std::uint32_t AtPlatform = 15;
constexpr std::uint32_t AtHwcap = 16;
constexpr std::uint32_t AtClktck = 17;
constexpr std::uint32_t AtSecure = 23;
constexpr std::uint32_t AtRandom = 25;
constexpr std::uint32_t AtHwcap2 = 26;
constexpr std::uint32_t AtExecfn = 31;

constexpr std::uint32_t AlignDown(std::uint32_t value) { return value & ~(kPageSize - 1); }
constexpr std::uint32_t AlignUp(std::uint32_t value) {
    return (value + kPageSize - 1) & ~(kPageSize - 1);
}

}  // namespace

namespace armbat {
namespace {

void WriteU32(GuestArena& arena, std::uint32_t address, std::uint32_t value) {
    std::memcpy(arena.Ptr(address), &value, sizeof(value));
}

std::uint32_t ReadU32(GuestArena& arena, std::uint32_t address) {
    std::uint32_t value = 0;
    std::memcpy(&value, arena.Ptr(address), sizeof(value));
    return value;
}

}  // namespace

BionicBoot::BionicBoot(A32Machine& machine, GuestArena& arena)
    : machine_(machine), arena_(arena) {}

BionicBoot::~BionicBoot() = default;

void BionicBoot::OnReady(std::uint32_t api_address) {
    // Runs inside the SVC handler while the JIT is executing: only record. The
    // pointer is validated once control is back on the host.
    ready_api_ = api_address;
}

bool BionicBoot::ReadApi(std::uint32_t address, std::string* error) {
    if (!arena_.IsMapped(address, kApiSize)) {
        *error = "boot API pointer is outside the arena";
        return false;
    }
    const std::uint32_t magic = ReadU32(arena_, address);
    const std::uint32_t version = ReadU32(arena_, address + 4);
    if (magic != kApiMagic) {
        *error = "boot API has a wrong magic";
        return false;
    }
    if (version != kApiVersion) {
        *error = "boot API version " + std::to_string(version) + " is not supported";
        return false;
    }
    Api api;
    api.dlopen = ReadU32(arena_, address + 12);
    api.dlsym = ReadU32(arena_, address + 16);
    api.dlclose = ReadU32(arena_, address + 20);
    api.dlerror = ReadU32(arena_, address + 24);
    api.exit = ReadU32(arena_, address + 28);
    api.abort = ReadU32(arena_, address + 32);
    api.atexit = ReadU32(arena_, address + 36);
    api.malloc = ReadU32(arena_, address + 40);
    api.free = ReadU32(arena_, address + 44);
    if (api.dlopen == 0 || api.dlsym == 0) {
        *error = "boot API is missing the loader";
        return false;
    }
    api_ = api;
    return true;
}

bool BionicBoot::MapImage(const unsigned char* image, std::uint32_t size, std::uint32_t base,
                          std::uint32_t* entry, std::uint32_t* phdr, std::uint32_t* phnum,
                          std::string* error) {
    auto fail = [error](std::string message) {
        *error = std::move(message);
        return false;
    };
    if (image == nullptr || size < sizeof(Elf32_Ehdr)) {
        return fail("image is truncated");
    }
    const auto* eh = reinterpret_cast<const Elf32_Ehdr*>(image);
    if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        return fail("not an ELF image");
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_machine != EM_ARM ||
        eh->e_type != ET_DYN) {
        return fail("not a 32-bit ARM shared object");
    }
    if (eh->e_phoff + std::uint64_t(eh->e_phnum) * sizeof(Elf32_Phdr) > size || eh->e_phnum == 0) {
        return fail("program headers are out of range");
    }

    const auto* phdrs = reinterpret_cast<const Elf32_Phdr*>(image + eh->e_phoff);
    bool any = false;
    for (std::uint16_t i = 0; i < eh->e_phnum; ++i) {
        const Elf32_Phdr& ph = phdrs[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) {
            continue;
        }
        any = true;
        if (ph.p_filesz > ph.p_memsz || ph.p_offset + std::uint64_t(ph.p_filesz) > size) {
            return fail("segment " + std::to_string(i) + " is out of range");
        }
        const std::uint64_t start = std::uint64_t(base) + AlignDown(ph.p_vaddr);
        const std::uint64_t end = std::uint64_t(base) + AlignUp(ph.p_vaddr + ph.p_memsz);
        if (end > arena_.Size() || start >= end) {
            return fail("segment " + std::to_string(i) + " does not fit the arena");
        }
        if (arena_.IsMapped(static_cast<std::uint32_t>(start),
                            static_cast<std::uint64_t>(end - start))) {
            return fail("segment " + std::to_string(i) + " overlaps an existing mapping");
        }
        if (!arena_.Map(static_cast<std::uint32_t>(start),
                        static_cast<std::uint64_t>(end - start))) {
            return fail("cannot map segment " + std::to_string(i));
        }
        auto* destination = reinterpret_cast<unsigned char*>(
            arena_.Ptr(static_cast<std::uint32_t>(start)));
        std::memcpy(destination + (ph.p_vaddr - AlignDown(ph.p_vaddr)), image + ph.p_offset,
                    ph.p_filesz);
    }
    if (!any) {
        return fail("image has no loadable segments");
    }

    *entry = base + eh->e_entry;
    *phdr = base + eh->e_phoff;
    *phnum = eh->e_phnum;
    return true;
}

bool BionicBoot::BuildStack(std::uint32_t top, const std::string& exec_path, std::uint32_t phdr,
                            std::uint32_t phnum, std::uint32_t entry, std::uint32_t* sp,
                            std::string* error) {
    auto fail = [error](std::string message) {
        *error = std::move(message);
        return false;
    };
    if (top <= 0x1000 || !arena_.IsMapped(top - 0x1000, 0x1000)) {
        return fail("the guest stack is not mapped");
    }

    std::uint32_t cursor = top;
    auto push_string = [&](const std::string& text) {
        const std::uint32_t size = static_cast<std::uint32_t>(text.size()) + 1;
        if (cursor < 0x1000 + size) {
            return 0u;
        }
        cursor -= size;
        std::memcpy(arena_.Ptr(cursor), text.c_str(), size);
        return cursor;
    };

    const std::uint32_t exec = push_string(exec_path);
    if (exec == 0) {
        return fail("the initial stack ran out of room");
    }
    const char* environment[] = {
        "ANDROID_ROOT=/system",
        "ANDROID_DATA=/data",
        "ANDROID_ASSETS=/system/app",
        "LD_LIBRARY_PATH=/system/lib:/armbat/app-libs",
        "PATH=/system/bin",
    };
    std::vector<std::uint32_t> env_addresses;
    for (const char* item : environment) {
        const std::uint32_t address = push_string(item);
        if (address == 0) {
            return fail("the initial stack ran out of room");
        }
        env_addresses.push_back(address);
    }
    const std::uint32_t platform = push_string("armv7l");
    if (platform == 0) {
        return fail("the initial stack ran out of room");
    }

    cursor -= 16;
    cursor &= ~15u;
    const std::uint32_t random = cursor;
    if (::getrandom(arena_.Ptr(random), 16, 0) != 16) {
        // Deterministic bytes would make every run share stack canaries.
        for (std::uint32_t i = 0; i < 16; ++i) {
            *reinterpret_cast<unsigned char*>(arena_.Ptr(random + i)) =
                static_cast<unsigned char>(0x5a + i * 7);
        }
    }

    const std::uint32_t argc = 1;
    const std::uint32_t envc = static_cast<std::uint32_t>(env_addresses.size());
    const std::uint32_t auxc = 17;
    const std::uint32_t block_size = 4 + 4 * (argc + 1) + 4 * (envc + 1) + 8 * (auxc + 1);
    if (cursor < block_size + 0x100) {
        return fail("the initial stack ran out of room");
    }
    std::uint32_t block = (cursor - block_size) & ~7u;

    std::uint32_t p = block;
    auto emit = [&](std::uint32_t value) {
        WriteU32(arena_, p, value);
        p += 4;
    };
    emit(argc);
    emit(exec);
    emit(0);
    for (std::uint32_t address : env_addresses) {
        emit(address);
    }
    emit(0);

    auto emit_pair = [&](std::uint32_t key, std::uint32_t value) {
        WriteU32(arena_, p, key);
        WriteU32(arena_, p + 4, value);
        p += 8;
    };
    emit_pair(AtPhdr, phdr);
    emit_pair(AtPhnum, phnum);
    emit_pair(AtPagesz, kPageSize);
    emit_pair(AtBase, linker_base_);
    emit_pair(AtFlags, 0);
    emit_pair(AtEntry, entry);
    emit_pair(AtUid, 10000);
    emit_pair(AtEuid, 10000);
    emit_pair(AtGid, 10000);
    emit_pair(AtEgid, 10000);
    emit_pair(AtPlatform, platform);
    emit_pair(AtHwcap, kHwcap);
    emit_pair(AtClktck, 100);
    emit_pair(AtSecure, 0);
    emit_pair(AtRandom, random);
    emit_pair(AtHwcap2, 0);
    emit_pair(AtExecfn, exec);
    emit_pair(AtNull, 0);

    *sp = block;
    return true;
}

bool BionicBoot::Boot(std::string* error) {
    if (booted_) {
        return true;
    }
    std::string local;
    auto fail = [&](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };

    linker_base_ = kLinkerBase;
    boot_base_ = kBootBase;
    std::uint32_t linker_entry = 0;
    std::uint32_t linker_phdr = 0;
    std::uint32_t linker_phnum = 0;
    if (!MapImage(sysroot_linker::kLinkerImage, sysroot_linker::kLinkerSize, linker_base_,
                  &linker_entry, &linker_phdr, &linker_phnum, &local)) {
        return fail("linker: " + local);
    }
    std::uint32_t boot_entry = 0;
    std::uint32_t boot_phdr = 0;
    std::uint32_t boot_phnum = 0;
    if (!MapImage(guest_boot::kBootImage, guest_boot::kBootSize, boot_base_, &boot_entry,
                  &boot_phdr, &boot_phnum, &local)) {
        return fail("boot program: " + local);
    }
    if (!arena_.Map(kScratchBase, kScratchSize)) {
        return fail("cannot map the boot scratch page");
    }
    scratch_ = kScratchBase;
    std::memset(arena_.Ptr(scratch_), 0, kScratchSize);

    std::uint32_t sp = 0;
    if (!BuildStack(machine_.Sp(), "/system/bin/armbat_boot", boot_phdr, boot_phnum, boot_entry, &sp,
                    &local)) {
        return fail("initial stack: " + local);
    }

    ARMBAT_LOG_INFO("Armbat",
                        "bionic: linker=0x%08x entry=0x%08x boot=0x%08x entry=0x%08x sp=0x%08x "
                        "hwcap=0x%08x",
                        linker_base_, linker_entry, boot_base_, boot_entry, sp, kHwcap);

    machine_.SetSp(sp);
    api_ = {};
    ready_api_ = 0;
    machine_.ClearError();
    boot_error_.clear();
    machine_.CallFunction(linker_entry, {});

    if (!machine_.LastError().empty()) {
        boot_error_ = machine_.LastError();
        return fail("bionic boot faulted: " + boot_error_);
    }
    if (ready_api_ == 0) {
        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "returned without the ready handshake: halt=%s pc=0x%08x r7=0x%08x r0=0x%08x "
                      "lr=0x%08x sp=0x%08x",
                      machine_.LastHaltReasonName().c_str(), machine_.Reg(15), machine_.Reg(7),
                      machine_.Reg(0), machine_.Reg(14), machine_.Reg(13));
        // The linker aborts are silent: bionic's abort() prints through a raw
        // write that a faulting boot never reaches. The guest stack still has
        // the return addresses of the frames that led there.
        const std::uint32_t stack_pointer = machine_.Reg(13);
        for (std::uint32_t offset = 0; offset < 0x100; offset += 16) {
            if (!arena_.IsMapped(stack_pointer + offset, 16)) {
                break;
            }
            std::uint32_t words[4];
            std::memcpy(words, arena_.Ptr(stack_pointer + offset), sizeof(words));
            ARMBAT_LOG_INFO("Armbat",
                                "boot stack +%02x: %08x %08x %08x %08x", offset, words[0],
                                words[1], words[2], words[3]);
        }
        return fail(std::string("bionic boot ") + detail);
    }
    if (!ReadApi(ready_api_, &local)) {
        return fail("bionic boot API: " + local);
    }
    booted_ = true;
    ARMBAT_LOG_INFO("Armbat",
                        "bionic: libc initialized, dlopen=0x%08x dlsym=0x%08x malloc=0x%08x",
                        api_.dlopen, api_.dlsym, api_.malloc);
    return true;
}

std::uint32_t BionicBoot::Call(std::uint32_t function, std::initializer_list<std::uint32_t> args) {
    if (function == 0) {
        return 0;
    }
    machine_.ClearError();
    return machine_.CallNested(function, args);
}

std::uint32_t BionicBoot::WriteString(const std::string& text) {
    if (scratch_ == 0 || text.size() + 1 > kScratchSize) {
        return 0;
    }
    std::memcpy(arena_.Ptr(scratch_), text.c_str(), text.size() + 1);
    return scratch_;
}

std::uint32_t BionicBoot::Dlopen(const std::string& guest_path, int flags) {
    const std::uint32_t address = WriteString(guest_path);
    if (address == 0) {
        return 0;
    }
    return Call(api_.dlopen, {address, static_cast<std::uint32_t>(flags)});
}

std::uint32_t BionicBoot::Dlsym(std::uint32_t handle, const std::string& name) {
    const std::uint32_t address = WriteString(name);
    if (address == 0) {
        return 0;
    }
    return Call(api_.dlsym, {handle, address});
}

std::uint32_t BionicBoot::Malloc(std::uint32_t size) { return Call(api_.malloc, {size}); }

void BionicBoot::Free(std::uint32_t address) { Call(api_.free, {address}); }

void BionicBoot::SetTargetSdk(std::uint32_t target_sdk) {
    if (target_sdk == 0) {
        return;
    }
    // libdl exports the setter as a platform symbol; it forwards to a global the
    // linker reads on every load, so setting it once covers every library the
    // archive has and every library it dlopens later.
    const std::uint32_t libdl = Dlopen("libdl.so", 2 /*RTLD_NOW*/);
    if (libdl == 0) {
        ARMBAT_LOG_WARN("Armbat", "bionic: no libdl for the target sdk");
        return;
    }
    const std::uint32_t setter = Dlsym(libdl, "android_set_application_target_sdk_version");
    if (setter == 0) {
        ARMBAT_LOG_WARN("Armbat",
                            "bionic: libdl has no android_set_application_target_sdk_version");
        return;
    }
    Call(setter, {target_sdk});
    ARMBAT_LOG_INFO("Armbat", "bionic: target sdk %u",
                        static_cast<unsigned>(target_sdk));
}

std::string BionicBoot::LastError() const {
    if (api_.dlerror == 0) {
        return {};
    }
    // dlerror clears itself when read, so a missing message is not an error.
    auto* self = const_cast<BionicBoot*>(this);
    const std::uint32_t message = self->Call(api_.dlerror, {});
    if (message == 0 || !arena_.IsMapped(message, 1)) {
        return {};
    }
    std::string text;
    for (std::uint32_t offset = 0; offset < 512; ++offset) {
        if (!arena_.IsMapped(message + offset, 1)) {
            break;
        }
        const char ch = static_cast<char>(*arena_.Ptr(message + offset));
        if (ch == '\0') {
            break;
        }
        text.push_back(ch);
    }
    return text;
}

std::string BionicBoot::Describe() const {
    if (!booted_) {
        return "bionic: not booted";
    }
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer),
                  "bionic: linker=0x%08x boot=0x%08x dlopen=0x%08x dlsym=0x%08x", linker_base_,
                  boot_base_, api_.dlopen, api_.dlsym);
    return buffer;
}

}  // namespace armbat
