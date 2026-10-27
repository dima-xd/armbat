#include "armbat/selftest.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>

#include <unistd.h>

#include "armbat/a32_machine.h"
#include "armbat/detail/report.h"
#include "armbat/guest_apk.h"
#include "armbat/dynamic_loader.h"
#include "armbat/elf32_loader.h"
#include "armbat/guest_arena.h"
#include "armbat/guest_heap.h"
#include "armbat/guest_kernel.h"
#include "armbat/guest_libc.h"
#include "armbat/hle.h"
#include "armbat/guest_pthread.h"
#include "armbat/guest_stdio.h"
#include "guest_lib.h"
#include "guest_lib_cpp.h"
#include "guest_lib_imports.h"
#include "guest_lib_missing.h"
#include "guest_lib_needed.h"
#include "guest_lib_pthread.h"
#include "guest_lib_cppstdlib.h"
#include "guest_libcxx.h"
#include "guest_apk.h"
#include "guest_apk_armeabi.h"
#include "guest_lib_real.h"
#include "guest_lib_stdio.h"
#include "guest_lib_syscalls.h"

namespace armbat {
namespace {

// Clear of the HLE data area the registry below carves out of
// 0x01000000-0x02000000: the images this test loads must not land in it.
constexpr std::uint32_t kLibBase = 0x05000000;
constexpr std::uint32_t kTrapAddr = 0x00F00000;
constexpr std::uint32_t kStubAddr = 0x00E00000;
constexpr std::uint32_t kDataAddr = 0x00500000;
constexpr std::uint32_t kHeapBase = 0x02000000;
constexpr std::uint32_t kHeapSize = 0x02000000;
constexpr std::uint32_t kDlopenBase = 0x04000000;
constexpr std::uint32_t kArenaSize = 0x08000000;
constexpr std::uint32_t kStackAddr = 0x00800000;
constexpr std::uint32_t kStackSize = 0x00040000;

// SVC #0xFFFFFF: returning guest code lands here and halts the JIT. SVC #0 is
// the Linux syscall trap and must stay free for guest libc.
constexpr std::uint32_t kHaltInstruction = 0xEFFFFFFFu;

}  // namespace

SuiteReport RunLoaderSelftest() {
    detail::Reporter report;
    const std::string header = "armbat ELF32 loader\n";

    auto finish = [&]() {
        SuiteReport result;
        result.text = report.Text(header);
        result.failures = report.Failures();
        return result;
    };

    GuestArena arena(kArenaSize);
    if (!arena.Ok()) {
        report.Fail("arena.reserve", arena.Error() ? arena.Error() : "unknown");
        return finish();
    }

    if (!arena.Map(kTrapAddr, 0x1000) || !arena.Map(kStackAddr, kStackSize) ||
        !arena.Map(kDataAddr, 0x1000)) {
        report.Fail("arena.map", "cannot map trap page, stack or data page");
        return finish();
    }
    std::memcpy(arena.Ptr(kTrapAddr), &kHaltInstruction, sizeof(kHaltInstruction));

    A32Machine machine(arena);
    machine.SetSp(kStackAddr + kStackSize - 64);
    machine.SetReturnTrap(kTrapAddr);

    GuestHeap heap(arena, kHeapBase, kHeapSize);
    std::string heap_error;
    if (!heap.Init(&heap_error)) {
        report.Fail("heap.init", heap_error);
        return finish();
    }

    HleRegistry hle(machine, arena, kStubAddr);
    RegisterStandardFunctions(hle);
    heap.RegisterFunctions(hle);

    GuestStdio stdio(hle);
    std::string stdio_error;
    if (!stdio.Install(&stdio_error)) {
        report.Fail("stdio.install", stdio_error);
        return finish();
    }

    hle.Add("__armbat_probe_missing", [](HleCall& call) { call.Return(0); });

    GuestPthread pthread(hle);
    std::uint32_t thread_pointer = 0;
    std::string pthread_error;
    GuestThreadConfig thread_config;
    thread_config.stack_base = 0x00840000;  // between the main stack and the HLE page
    thread_config.stack_size = 512 * 1024;
    thread_config.stack_count = 11;
    thread_config.main_machine = &machine;
    if (!pthread.Install(thread_config, &thread_pointer, &pthread_error)) {
        report.Fail("pthread.install", pthread_error);
        return finish();
    }
    machine.SetTlsBase(thread_pointer);

    GuestLibc libc(hle, heap);
    std::string libc_error;
    if (!libc.Install(&libc_error)) {
        report.Fail("libc.install", libc_error);
        return finish();
    }
    // bionic keeps errno in the TLS block; HleCall::ErrnoCell() derives it from
    // the calling thread's pointer, so __errno() and guest TLS access agree.
    Elf32Loader loader(arena);
    AddSystemLibraries(loader);
    DynamicLoader dynamic(machine, arena, loader, kDlopenBase);
    dynamic.RegisterFunctions(hle);
    loader.SetImportResolver([&hle, &dynamic](std::string_view name) {
        if (const std::uint32_t address = dynamic.Resolve(name); address != 0) {
            return address;
        }
        return hle.Resolve(name);
    });

    std::string hle_error;
    if (!hle.Prepare(&hle_error)) {
        report.Fail("hle.prepare", hle_error);
        return finish();
    }
    report.Pass("hle.prepare", detail::Format("%zu host functions, %u MiB guest heap",
                                              hle.Count(), kHeapSize >> 20));

    GuestKernel kernel(machine, arena,
                       KernelConfig{0x05000000, 0x07000000, 0x07000000, 0x08000000});
    std::string kernel_error;
    if (!kernel.Prepare(&kernel_error)) {
        report.Fail("kernel.prepare", kernel_error);
        return finish();
    }

    {
        const std::uint32_t first = heap.Allocate(100);
        const std::uint32_t second = heap.Allocate(200);
        report.Check("heap.alloc",
                     first != 0 && second != 0 && first % 8 == 0 && second % 8 == 0 &&
                         first != second,
                     detail::Format("0x%08x and 0x%08x, 8-byte aligned", first, second));

        heap.Release(first);
        const std::uint32_t reused = heap.Allocate(90);
        report.Check("heap.reuse", reused == first,
                     detail::Format("90 bytes reused the 100-byte hole at 0x%08x", reused));
        heap.Release(reused);
        heap.Release(second);
        report.Check("heap.release", heap.InUse() == 0,
                     detail::Format("in use after release: %u bytes", heap.InUse()));
    }
    {
        const std::uint32_t low = heap.Allocate(1000);
        const std::uint32_t middle = heap.Allocate(1000);
        const std::uint32_t high = heap.Allocate(1000);
        heap.Release(middle);
        heap.Release(high);
        const std::uint32_t merged = heap.Allocate(1900);
        report.Check("heap.coalesce", merged == middle,
                     detail::Format("1900 bytes fit the merged 2000-byte hole at 0x%08x "
                                    "(middle 0x%08x)",
                                    merged, middle));
        heap.Release(low);
        heap.Release(merged);
        report.Check("heap.coalesce_empty", heap.InUse() == 0,
                     detail::Format("in use after release: %u bytes", heap.InUse()));
    }
    {
        // Random allocate/free with content verification: catches broken
        // boundary tags, double reuse of a block, and missed coalescing.
        struct Slot {
            std::uint32_t address = 0;
            std::uint32_t size = 0;
            std::uint32_t seed = 0;
        };
        std::vector<Slot> slots(96);
        std::uint32_t random = 0x12345678u;
        const auto next = [&random]() {
            random = random * 1664525u + 1013904223u;
            return random;
        };

        bool ok = true;
        std::string detail = "4000 operations";
        for (int step = 0; step < 4000 && ok; ++step) {
            Slot& slot = slots[next() % slots.size()];
            if (slot.address != 0) {
                for (std::uint32_t offset = 0; offset + 4 <= slot.size; offset += 4) {
                    std::uint32_t value = 0;
                    std::memcpy(&value, arena.Ptr(slot.address + offset), sizeof(value));
                    if (value != slot.seed + offset) {
                        ok = false;
                        detail = detail::Format("slot corrupted at step %d, offset %u", step,
                                                offset);
                        break;
                    }
                }
                heap.Release(slot.address);
                slot = Slot{};
            } else {
                const std::uint32_t size = 8 + next() % 300;
                const std::uint32_t address = heap.Allocate(size);
                if (address == 0) {
                    continue;
                }
                if (address % 8 != 0) {
                    ok = false;
                    detail = detail::Format("unaligned allocation 0x%08x at step %d", address,
                                            step);
                    break;
                }
                for (const Slot& other : slots) {
                    if (other.address == 0) {
                        continue;
                    }
                    const bool overlaps = address < other.address + other.size &&
                                          other.address < address + size;
                    if (overlaps) {
                        ok = false;
                        detail = detail::Format("0x%08x overlaps 0x%08x at step %d", address,
                                                other.address, step);
                        break;
                    }
                }
                if (!ok) {
                    break;
                }
                slot = Slot{address, size, next()};
                for (std::uint32_t offset = 0; offset + 4 <= size; offset += 4) {
                    const std::uint32_t value = slot.seed + offset;
                    std::memcpy(arena.Ptr(address + offset), &value, sizeof(value));
                }
            }
        }
        for (const Slot& slot : slots) {
            if (slot.address != 0) {
                heap.Release(slot.address);
            }
        }
        if (ok && heap.InUse() != 0) {
            ok = false;
            detail = detail::Format("leak: %u bytes still in use", heap.InUse());
        }
        report.Check("heap.stress", ok, detail);
    }

    std::string error;
    auto lib = loader.Load(guest_lib::kGuestLibImage, guest_lib::kGuestLibSize, kLibBase, &error);
    if (!lib) {
        report.Fail("elf.load", error);
        return finish();
    }
    report.Pass("elf.load", detail::Format("%s, %u bytes, %u relocations at 0x%08x",
                                           lib->soname.c_str(), lib->image_size,
                                           lib->relocation_count, lib->base));

    if (!loader.RunInitializers(machine, *lib, &error)) {
        report.Fail("elf.init_array", error);
    } else {
        report.Pass("elf.init_array", "constructors ran");
    }

    const auto call_in = [&](const LoadedLibrary& library, const char* name,
                             std::initializer_list<std::uint32_t> args) -> std::uint32_t {
        const std::uint32_t address = loader.Sym(library, name);
        if (address == 0) {
            report.Fail(std::string("symbol.") + name, "not exported");
            return 0;
        }
        const std::uint32_t value = machine.CallFunction(address, args);
        if (!machine.LastError().empty()) {
            report.Fail(std::string("call.") + name, machine.LastError());
        }
        return value;
    };
    const auto call = [&](const char* name, std::initializer_list<std::uint32_t> args) {
        return call_in(*lib, name, args);
    };

    const std::uint32_t add = call("lib_add", {2, 3});
    report.Check("call.add", add == 5, detail::Format("lib_add(2,3)=%u", add));

    const std::uint32_t mul = call("lib_mul", {6, 7});
    report.Check("call.mul", mul == 42, detail::Format("lib_mul(6,7)=%u", mul));

    const std::uint32_t fib = call("lib_fib", {20});
    report.Check("call.fib", fib == 6765, detail::Format("lib_fib(20)=%u", fib));

    const std::uint32_t data = call("lib_data_sum", {4});
    report.Check("call.data", data == 10, detail::Format("lib_data_sum(4)=%u", data));

    const std::uint32_t indirect_add = call("lib_call_table", {3, 4, 0});
    report.Check("call.reloc_add", indirect_add == 7,
                 detail::Format("lib_call_table(3,4,0)=%u (RELATIVE pointer)", indirect_add));

    const std::uint32_t indirect_mul = call("lib_call_table", {3, 4, 1});
    report.Check("call.reloc_mul", indirect_mul == 12,
                 detail::Format("lib_call_table(3,4,1)=%u", indirect_mul));

    const std::uint32_t ctor = call("lib_ctor_value", {});
    report.Check("call.ctor", ctor == 42, detail::Format("lib_ctor_value()=%u (constructor)", ctor));

    const std::uint32_t name_len = call("lib_name_len", {1});
    report.Check("call.rodata", name_len == 3, detail::Format("lib_name_len(1)=%u", name_len));

    // --- Library with libc imports, resolved to HLE stubs --------------------
    const std::uint32_t text_address = kDataAddr + 0x100;
    const std::uint32_t other_address = kDataAddr + 0x200;
    const char kText[] = "armbat";
    const char kOther[] = "armbaT";
    std::memcpy(arena.Ptr(text_address), kText, sizeof(kText));
    std::memcpy(arena.Ptr(other_address), kOther, sizeof(kOther));

    std::string import_error;
    auto imports = loader.Load(guest_lib_imports::kImportsLibImage,
                               guest_lib_imports::kImportsLibSize, kLibBase + 0x00400000,
                               &import_error);
    if (!imports) {
        report.Fail("imports.load", import_error);
        return finish();
    }
    report.Pass("imports.load", detail::Format("%s, %u bytes, %u relocations (PLT imports)",
                                               imports->soname.c_str(), imports->image_size,
                                               imports->relocation_count));
    if (!loader.RunInitializers(machine, *imports, &import_error)) {
        report.Fail("imports.init", import_error);
    }

    const std::uint32_t imported_len = call_in(*imports, "lib_strlen", {text_address});
    report.Check("imports.strlen", imported_len == 6,
                 detail::Format("lib_strlen(\"%s\")=%u", kText, imported_len));

    const std::uint32_t checksum = call_in(*imports, "lib_checksum", {text_address});
    report.Check("imports.memcpy", checksum == 631,
                 detail::Format("lib_checksum(\"%s\")=%u", kText, checksum));

    const std::uint32_t fill = call_in(*imports, "lib_fill", {0x7F, 4});
    const std::uint32_t buffer = call_in(*imports, "lib_buffer", {});
    std::uint8_t filled[4] = {};
    const bool have_fill = fill == 0x7F + 0x7F && arena.IsMapped(buffer, sizeof(filled));
    if (have_fill) {
        std::memcpy(filled, arena.Ptr(buffer), sizeof(filled));
    }
    report.Check("imports.memset", have_fill && filled[0] == 0x7F && filled[3] == 0x7F,
                 detail::Format("lib_fill byte=%u, buffer[0]=%u", fill,
                                static_cast<unsigned>(filled[0])));

    const std::uint32_t same = call_in(*imports, "lib_strcmp", {text_address, text_address});
    const std::uint32_t different = call_in(*imports, "lib_strcmp", {text_address, other_address});
    report.Check("imports.strcmp", same == 0 && different != 0,
                 detail::Format("same=%u, \"%s\" vs \"%s\"=%d", same, kText, kOther,
                                static_cast<std::int32_t>(different)));

    const std::uint32_t quotient = call_in(*imports, "lib_div", {1000, 7});
    report.Check("imports.idiv", quotient == 142,
                 detail::Format("lib_div(1000,7)=%u", quotient));

    const std::uint32_t negative = call_in(*imports, "lib_div",
                                           {static_cast<std::uint32_t>(-1000), 7});
    report.Check("imports.idiv_negative", static_cast<std::int32_t>(negative) == -142,
                 detail::Format("lib_div(-1000,7)=%d", static_cast<std::int32_t>(negative)));

    const std::uint32_t unsigned_quotient = call_in(*imports, "lib_udiv", {1000, 7});
    report.Check("imports.uidiv", unsigned_quotient == 142,
                 detail::Format("lib_udiv(1000,7)=%u", unsigned_quotient));

    const std::uint32_t remainder = call_in(*imports, "lib_mod", {1000, 7});
    report.Check("imports.idivmod", remainder == 6,
                 detail::Format("lib_mod(1000,7)=%u", remainder));

    const std::int64_t wide = 12345678901234LL;
    const std::uint32_t wide_low = call_in(*imports, "lib_ldiv",
                                           {static_cast<std::uint32_t>(wide & 0xFFFFFFFF),
                                            static_cast<std::uint32_t>(wide >> 32), 7, 0});
    const std::uint32_t wide_high = machine.Reg(1);
    const std::int64_t wide_quotient = static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(wide_high) << 32) | wide_low);
    report.Check("imports.ldivmod", wide_quotient == wide / 7,
                 detail::Format("lib_ldiv(%lld,7)=%lld", static_cast<long long>(wide),
                                static_cast<long long>(wide_quotient)));

    const std::uint32_t allocated = call_in(*imports, "lib_alloc_sum", {100});
    report.Check("imports.malloc", allocated == 14850,
                 detail::Format("lib_alloc_sum(100)=%u", allocated));

    const std::uint32_t calloc_dirty = call_in(*imports, "lib_calloc_probe", {64});
    report.Check("imports.calloc", calloc_dirty == 0,
                 detail::Format("lib_calloc_probe(64)=%u non-zero words", calloc_dirty));

    const std::uint32_t grown = call_in(*imports, "lib_realloc_sum", {10, 4});
    report.Check("imports.realloc", grown == 72,
                 detail::Format("lib_realloc_sum(10,4)=%u (old contents preserved)", grown));

    // --- Raw syscalls (SVC #0, r7 = number) ----------------------------------
    std::string temp_path;
    {
        std::vector<std::string> candidates;
        const char* environment = std::getenv("TMPDIR");
        if (environment != nullptr && environment[0] != '\0') {
            candidates.emplace_back(environment);
        }
        candidates.emplace_back("/data/local/tmp");

        for (const std::string& directory : candidates) {
            const std::string candidate = directory + "/armbat-syscall-test.bin";
            std::FILE* probe = std::fopen(candidate.c_str(), "wb");
            if (probe != nullptr) {
                std::fclose(probe);
                std::remove(candidate.c_str());
                temp_path = candidate;
                break;
            }
        }
    }
    if (temp_path.empty()) {
        report.Fail("kernel.tempdir", "no writable directory for the file test");
        return finish();
    }

    const std::uint32_t syscall_path = kDataAddr + 0x300;
    const std::uint32_t syscall_text = kDataAddr + 0x400;
    const std::uint32_t syscall_buffer = kDataAddr + 0x500;
    const std::uint32_t missing_path = kDataAddr + 0x600;
    const char kPayload[] = "hello syscall";
    const char kMissing[] = "/nonexistent/armbat-test";
    std::memcpy(arena.Ptr(syscall_path), temp_path.c_str(), temp_path.size() + 1);
    std::memcpy(arena.Ptr(syscall_text), kPayload, sizeof(kPayload));
    std::memcpy(arena.Ptr(missing_path), kMissing, sizeof(kMissing));

    std::string syscall_error;
    auto syscalls = loader.Load(guest_lib_syscalls::kSyscallsLibImage,
                                guest_lib_syscalls::kSyscallsLibSize, kLibBase + 0x00600000,
                                &syscall_error);
    if (!syscalls) {
        report.Fail("kernel.load", syscall_error);
        return finish();
    }
    report.Pass("kernel.load", detail::Format("%s, %u bytes", syscalls->soname.c_str(),
                                              syscalls->image_size));

    const auto file_roundtrip = call_in(*syscalls, "lib_file_roundtrip",
                                        {syscall_path, syscall_text, sizeof(kPayload) - 1,
                                         syscall_buffer});
    report.Check("kernel.file", file_roundtrip == sizeof(kPayload) - 1,
                 detail::Format("wrote and read back %u bytes (%s)", file_roundtrip,
                                temp_path.c_str()));

    std::string guest_text(sizeof(kPayload) - 1, '\0');
    std::memcpy(guest_text.data(), arena.Ptr(syscall_buffer), guest_text.size());
    report.Check("kernel.file_content", guest_text == kPayload,
                 detail::Format("guest buffer holds \"%s\"", guest_text.c_str()));

    std::FILE* written = std::fopen(temp_path.c_str(), "rb");
    std::string host_text;
    if (written != nullptr) {
        char chunk[64];
        const std::size_t count = std::fread(chunk, 1, sizeof(chunk), written);
        host_text.assign(chunk, count);
        std::fclose(written);
    }
    report.Check("kernel.host_content", host_text == kPayload,
                 detail::Format("host sees \"%s\" in %s", host_text.c_str(), temp_path.c_str()));

    const std::uint32_t at_result = call_in(*syscalls, "lib_file_roundtrip_at",
                                            {syscall_path, syscall_text, sizeof(kPayload) - 1,
                                             syscall_buffer});
    report.Check("kernel.openat", at_result == sizeof(kPayload) - 1,
                 detail::Format("openat roundtrip: %u bytes", at_result));

    const std::uint32_t stat_size = call_in(*syscalls, "lib_fstat_size", {syscall_path});
    report.Check("kernel.fstat", stat_size == sizeof(kPayload) - 1,
                 detail::Format("st_size=%u (32-bit struct stat)", stat_size));

    const std::uint32_t missing = call_in(*syscalls, "lib_open_missing", {missing_path});
    report.Check("kernel.enoent", static_cast<std::int32_t>(missing) == -2,
                 detail::Format("open of a missing path returned %d", static_cast<std::int32_t>(missing)));

    const std::uint32_t bad_fd = call_in(*syscalls, "lib_read_bad_fd", {});
    report.Check("kernel.ebadf", static_cast<std::int32_t>(bad_fd) == -9,
                 detail::Format("read(-1) returned %d", static_cast<std::int32_t>(bad_fd)));

    const std::uint32_t guest_pid = call_in(*syscalls, "lib_getpid", {});
    report.Check("kernel.pid", guest_pid == static_cast<std::uint32_t>(::getpid()),
                 detail::Format("getpid=%u", guest_pid));

    const std::uint32_t guest_tid = call_in(*syscalls, "lib_gettid", {});
    report.Check("kernel.tid", guest_tid == static_cast<std::uint32_t>(::gettid()),
                 detail::Format("gettid=%u", guest_tid));

    const std::uint32_t exit_result = call_in(*syscalls, "lib_exit_early", {7});
    report.Check("kernel.exit_group", exit_result == 0,
                 detail::Format("exit_group stopped the run before returning (%u)", exit_result));

    report.Pass("kernel.calls", detail::Format("%llu syscalls, last number %llu",
                                               static_cast<unsigned long long>(kernel.Calls()),
                                               static_cast<unsigned long long>(kernel.LastNumber())));

    // --- Guest memory, time and randomness -----------------------------------
    const std::uint32_t anon_sum = call_in(*syscalls, "lib_mmap_anon", {});
    report.Check("kernel.mmap_anon", anon_sum == 14672896,
                 detail::Format("8 KiB anonymous mapping summed to %u", anon_sum));

    const std::uint32_t file_sum =
        call_in(*syscalls, "lib_mmap_file", {syscall_path, sizeof(kPayload) - 1});
    std::uint32_t expected_sum = 0;
    for (std::size_t i = 0; i + 1 < sizeof(kPayload); ++i) {
        expected_sum += static_cast<std::uint8_t>(kPayload[i]);
    }
    report.Check("kernel.mmap_file", file_sum == expected_sum,
                 detail::Format("file-backed mapping of \"%s\" summed to %u", kPayload, file_sum));

    const std::uint32_t brk_value = call_in(*syscalls, "lib_brk_probe", {});
    report.Check("kernel.brk", brk_value == 42,
                 detail::Format("wrote and read the byte at the new break: %u", brk_value));

    const std::uint32_t slept = call_in(*syscalls, "lib_sleep_measure", {});
    report.Check("kernel.clock", slept >= 5 && slept < 500,
                 detail::Format("CLOCK_MONOTONIC advanced %u ms across a 10 ms sleep", slept));

    const std::uint32_t random_nonzero =
        call_in(*syscalls, "lib_random_fill", {syscall_buffer, 16});
    report.Check("kernel.getrandom", random_nonzero > 0,
                 detail::Format("%u of 16 random bytes were non-zero", random_nonzero));

    const std::uint32_t churn = call_in(*syscalls, "lib_mmap_churn", {200});
    report.Check("kernel.mmap_churn", churn == 19900,
                 detail::Format("200 map/unmap rounds summed to %u", churn));

    const std::uint32_t protect = call_in(*syscalls, "lib_mprotect_probe", {});
    report.Check("kernel.mprotect", protect == 0, detail::Format("mprotect returned %d",
                                                                 static_cast<std::int32_t>(protect)));

    std::remove(temp_path.c_str());

    // --- DT_NEEDED against a host-provided libc ------------------------------
    std::string needed_error;
    auto needed = loader.Load(guest_lib_needed::kNeededLibImage,
                              guest_lib_needed::kNeededLibSize, kLibBase + 0x00800000,
                              &needed_error);
    if (!needed) {
        report.Fail("needed.load", needed_error);
    } else {
        std::string dependencies;
        for (const std::string& name : needed->needed) {
            dependencies += name;
            dependencies += ' ';
        }
        report.Pass("needed.load", detail::Format("%s depends on %s", needed->soname.c_str(),
                                                  dependencies.c_str()));

        const std::uint32_t needed_len = call_in(*needed, "lib_needed_strlen", {text_address});
        report.Check("needed.strlen", needed_len == 6,
                     detail::Format("lib_needed_strlen(\"%s\")=%u (stub would return 0)",
                                    kText, needed_len));

        const std::uint32_t needed_sum =
            call_in(*needed, "lib_needed_checksum", {text_address});
        report.Check("needed.hle_binding", needed_sum == 631,
                     detail::Format("lib_needed_checksum(\"%s\")=%u", kText, needed_sum));
    }
    {
        Elf32Loader strict(arena);  // no host-provided libraries registered
        std::string strict_error;
        const auto rejected = strict.Load(guest_lib_needed::kNeededLibImage,
                                          guest_lib_needed::kNeededLibSize,
                                          kLibBase + 0x00A00000, &strict_error);
        const bool named = strict_error.find("libc.so") != std::string::npos;
        report.Check("needed.reject_unknown", !rejected && named,
                     rejected ? "unknown dependency was accepted" : strict_error);
    }

    // --- dlopen/dlsym from guest code ----------------------------------------
    {
        const std::string directory = temp_path.substr(0, temp_path.find_last_of('/'));
        const std::string dlopen_path = directory + "/armbat-dlopen.so";
        std::FILE* output = std::fopen(dlopen_path.c_str(), "wb");
        bool written = false;
        if (output != nullptr) {
            written = std::fwrite(guest_lib_needed::kNeededLibImage, 1,
                                  guest_lib_needed::kNeededLibSize,
                                  output) == guest_lib_needed::kNeededLibSize;
            std::fclose(output);
        }
        report.Check("dlopen.image", written,
                     detail::Format("wrote %s", dlopen_path.c_str()));

        const std::uint32_t dlopen_path_address = kDataAddr + 0x700;
        std::memcpy(arena.Ptr(dlopen_path_address), dlopen_path.c_str(),
                    dlopen_path.size() + 1);

        const std::uint32_t opened = call_in(*imports, "lib_dlopen_strlen",
                                             {dlopen_path_address, text_address});
        report.Check("dlopen.call", opened == 6,
                     detail::Format("dlsym'd lib_needed_strlen(\"%s\")=%u", kText, opened));

        const std::uint32_t missing_symbol =
            call_in(*imports, "lib_dlsym_missing", {dlopen_path_address});
        report.Check("dlsym.missing", missing_symbol == 1,
                     detail::Format("dlsym of an absent symbol returned %u", missing_symbol));

        report.Pass("dlopen.modules", detail::Format("%zu module(s) opened at runtime",
                                                     dynamic.Count()));
        std::remove(dlopen_path.c_str());
    }

    // --- A normally linked NDK library ---------------------------------------
    {
        std::string real_error;
        auto real = loader.Load(guest_lib_real::kRealLibImage, guest_lib_real::kRealLibSize,
                                kLibBase + 0x00C00000, &real_error);
        if (!real) {
            report.Fail("real.load", real_error);
        } else {
            std::string dependencies;
            for (const std::string& name : real->needed) {
                dependencies += name;
                dependencies += ' ';
            }
            report.Pass("real.load", detail::Format("%s, %u bytes, %u relocations, needs %s",
                                                    real->soname.c_str(), real->image_size,
                                                    real->relocation_count,
                                                    dependencies.c_str()));

            const std::uint32_t real_len = call_in(*real, "real_strlen", {text_address});
            report.Check("real.strlen", real_len == 6,
                         detail::Format("real_strlen(\"%s\")=%u", kText, real_len));

            const std::uint32_t real_sum = call_in(*real, "real_alloc_sum", {10});
            report.Check("real.malloc", real_sum == 225,
                         detail::Format("real_alloc_sum(10)=%u", real_sum));

            const std::uint32_t format_len =
                call_in(*real, "real_format", {42, syscall_buffer, 64});
            std::string formatted(format_len < 64 ? format_len : 0, '\0');
            if (!formatted.empty()) {
                std::memcpy(formatted.data(), arena.Ptr(syscall_buffer), formatted.size());
            }
            report.Check("real.snprintf", formatted == "value=42",
                         detail::Format("real_format(42)=%u, \"%s\"", format_len,
                                        formatted.c_str()));

            const std::uint32_t error_number =
                call_in(*real, "real_errno_probe", {missing_path});
            report.Check("real.errno", error_number == 2,
                         detail::Format("open(\"%s\") left errno=%u", kMissing, error_number));

            const std::uint32_t atoi_address = kDataAddr + 0x800;
            const char kNumber[] = "12345";
            std::memcpy(arena.Ptr(atoi_address), kNumber, sizeof(kNumber));
            const std::uint32_t parsed = call_in(*real, "real_atoi", {atoi_address});
            report.Check("real.atoi", parsed == 12345,
                         detail::Format("real_atoi(\"%s\")=%u", kNumber, parsed));

            const std::uint32_t letters_address = kDataAddr + 0x840;
            const char kLetters[] = "ab";
            std::memcpy(arena.Ptr(letters_address), kLetters, sizeof(kLetters));
            const std::int64_t wide_value = 1234567890123LL;
            const std::uint32_t many_len = call_in(
                *real, "real_format_many",
                {syscall_buffer, 64, 7, letters_address,
                 static_cast<std::uint32_t>(wide_value & 0xFFFFFFFF),
                 static_cast<std::uint32_t>(wide_value >> 32), 0xBEEF});
            std::string many(many_len < 64 ? many_len : 0, '\0');
            if (!many.empty()) {
                std::memcpy(many.data(), arena.Ptr(syscall_buffer), many.size());
            }
            const std::string expected_many = "[0007|ab|1234567890123|0x0000beef|Z|%]";
            report.Check("real.format_many", many == expected_many,
                         detail::Format("got \"%s\"", many.c_str()));
        }
    }

    // --- A C++ library: static constructor, guards, operator new -------------
    {
        std::string cpp_error;
        auto cpp = loader.Load(guest_lib_cpp::kCppLibImage, guest_lib_cpp::kCppLibSize,
                               kLibBase + 0x00D00000, &cpp_error);
        if (!cpp) {
            report.Fail("cpp.load", cpp_error);
        } else {
            if (!loader.RunInitializers(machine, *cpp, &cpp_error)) {
                report.Fail("cpp.init_array", cpp_error);
            }
            report.Pass("cpp.load", detail::Format("%s, %u bytes, %u relocations",
                                                   cpp->soname.c_str(), cpp->image_size,
                                                   cpp->relocation_count));

            const std::uint32_t ctor_value = call_in(*cpp, "cpp_ctor_value", {});
            report.Check("cpp.ctor", ctor_value == 71,
                         detail::Format("cpp_ctor_value()=%u (value*10 + volatile seed)",
                                        ctor_value));

            const std::uint32_t first_static = call_in(*cpp, "cpp_local_static", {});
            const std::uint32_t second_static = call_in(*cpp, "cpp_local_static", {});
            report.Check("cpp.guard", first_static == 43 && second_static == 43,
                         detail::Format("local static returned %u then %u",
                                        first_static, second_static));

            const std::uint32_t new_sum = call_in(*cpp, "cpp_new_array", {10});
            report.Check("cpp.new_array", new_sum == 90,
                         detail::Format("cpp_new_array(10)=%u", new_sum));

            const std::uint32_t area = call_in(*cpp, "cpp_virtual_area", {6});
            report.Check("cpp.virtual", area == 36,
                         detail::Format("cpp_virtual_area(6)=%u", area));
        }
    }

    // --- C stdio through the handle table ------------------------------------
    {
        std::string stdio_load_error;
        auto stdio_lib = loader.Load(guest_lib_stdio::kStdioLibImage,
                                     guest_lib_stdio::kStdioLibSize,
                                     kLibBase + 0x00F00000, &stdio_load_error);
        if (!stdio_lib) {
            report.Fail("stdio.load", stdio_load_error);
        } else {
            report.Pass("stdio.load", detail::Format("%s, %u bytes, %u relocations",
                                                     stdio_lib->soname.c_str(),
                                                     stdio_lib->image_size,
                                                     stdio_lib->relocation_count));

            const std::uint32_t guest_file_size = call_in(*stdio_lib, "lib_stdio_file_size", {});
            report.Check("stdio.file_size", guest_file_size == 84,
                         detail::Format("guest sizeof(FILE)=%u (host decodes __sF with this)",
                                        guest_file_size));

            const std::uint32_t path_address = kDataAddr + 0x900;
            std::memcpy(arena.Ptr(path_address), temp_path.c_str(), temp_path.size() + 1);

            const std::uint32_t written = call_in(*stdio_lib, "lib_stdio_roundtrip", {path_address});
            report.Check("stdio.roundtrip", written == 18,
                         detail::Format("fwrite+fprintf+fgets+fseek produced %u bytes in %s",
                                        written, temp_path.c_str()));

            std::FILE* check = std::fopen(temp_path.c_str(), "rb");
            std::string contents;
            if (check != nullptr) {
                char chunk[64];
                const std::size_t count = std::fread(chunk, 1, sizeof(chunk), check);
                contents.assign(chunk, count);
                std::fclose(check);
            }
            report.Check("stdio.file_content", contents == "line one\nvalue=42\n",
                         detail::Format("host reads %zu bytes: \"%s\"", contents.size(),
                                        contents.c_str()));

            const std::uint32_t at_end = call_in(*stdio_lib, "lib_stdio_eof", {path_address});
            report.Check("stdio.eof", at_end == 1,
                         detail::Format("feof after reading past the end = %u", at_end));

            const std::uint32_t missing = call_in(*stdio_lib, "lib_stdio_missing", {});
            report.Check("stdio.errno", missing == ENOENT,
                         detail::Format("fopen of a missing file left errno=%u", missing));

            const std::uint32_t error_length = call_in(*stdio_lib, "lib_stdio_strerror_len", {});
            const std::string host_error = std::strerror(ENOENT);
            report.Check("stdio.strerror", error_length == host_error.size(),
                         detail::Format("strerror(ENOENT) is %u characters", error_length));

            // Take whatever sink the embedder installed (logcat in the app) so the
            // guest's printf still reaches it while we are capturing.
            StdioWriteSink forward = SetStdioSink(nullptr);
            std::string captured;
            SetStdioSink([&captured, forward](int fd, std::string_view text) {
                if (fd == 1) {
                    captured.append(text);
                }
                if (forward) {
                    forward(fd, text);
                }
            });

            const std::string label = "stdio";
            const std::uint32_t label_address = kDataAddr + 0x960;
            std::memcpy(arena.Ptr(label_address), label.c_str(), label.size() + 1);

            char expected[64];
            const int expected_length =
                std::snprintf(expected, sizeof(expected), "guest printf %s %d %08x %.1f\n",
                              label.c_str(), -7, 0xbeefu, 3.5);
            const std::uint32_t printed =
                call_in(*stdio_lib, "lib_stdio_printf", {label_address});
            report.Check("stdio.printf",
                         printed == static_cast<std::uint32_t>(expected_length) &&
                             captured == expected,
                         detail::Format("printf rendered %u characters: \"%s\"", printed,
                                        captured.c_str()));

            captured.clear();
            const std::uint32_t formatted =
                call_in(*stdio_lib, "lib_stdio_fprintf", {label_address});
            report.Check("stdio.fprintf_stdout",
                         formatted == 20 && captured == "guest fprintf stdio\n",
                         detail::Format("fprintf(stdout, ...) rendered %u characters: \"%s\"",
                                        formatted, captured.c_str()));

            // va_list path: the guest builds an AAPCS argument block and the host
            // walks it, which is how libc++ calls vsnprintf from its formatters.
            const std::uint32_t vbuffer = kDataAddr + 0xA00;
            const std::uint32_t vformat_text = kDataAddr + 0xA80;
            const std::uint32_t vargument = kDataAddr + 0xAC0;
            const char vformat[] = "%d|%s|%08x";
            const char vtext[] = "ok";
            std::memcpy(arena.Ptr(vformat_text), vformat, sizeof(vformat));
            std::memcpy(arena.Ptr(vargument), vtext, sizeof(vtext));

            captured.clear();
            const std::uint32_t vwritten = call_in(*stdio_lib, "lib_stdio_vformat",
                                                   {vbuffer, 64, vformat_text, 42,
                                                    vargument, 0xbeefu});
            std::string vrendered;
            if (vwritten < 64) {
                const char* start = reinterpret_cast<const char*>(arena.Ptr(vbuffer));
                vrendered.assign(start, start + vwritten);
            }
            report.Check("stdio.vsnprintf",
                         vwritten == 14 && vrendered == "42|ok|0000beef",
                         detail::Format("vsnprintf rendered %u characters: \"%s\"", vwritten,
                                        vrendered.c_str()));

            captured.clear();
            const char vf_format[] = "%s %d\n";
            std::memcpy(arena.Ptr(vformat_text), vf_format, sizeof(vf_format));
            const std::uint32_t vf_written = call_in(*stdio_lib, "lib_stdio_vfprintf_stdout",
                                                     {vformat_text, vargument, 7});
            report.Check("stdio.vfprintf",
                         vf_written == 5 && captured == "ok 7\n",
                         detail::Format("vfprintf rendered %u characters: \"%s\"", vf_written,
                                        captured.c_str()));

            const std::uint32_t double_written =
                call_in(*stdio_lib, "lib_stdio_vformat_double", {vbuffer, 64});
            std::string double_text;
            if (double_written < 64) {
                const char* start = reinterpret_cast<const char*>(arena.Ptr(vbuffer));
                double_text.assign(start, start + double_written);
            }
            report.Check("stdio.va_double",
                         double_text == "1.25|9",
                         detail::Format("64-bit vararg through a va_list: \"%s\"",
                                        double_text.c_str()));

            captured.clear();
            const std::uint32_t puts_result =
                call_in(*stdio_lib, "lib_stdio_puts", {label_address});
            report.Check("stdio.puts",
                         puts_result == 0 && captured == "stdio\ntail\n!",
                         detail::Format("puts+fputs+fputc produced \"%s\"", captured.c_str()));

            SetStdioSink(std::move(forward));
        }
    }

    // --- Thread pointer and pthread bookkeeping -----------------------------
    {
        std::string pthread_load_error;
        auto pthread_lib = loader.Load(guest_lib_pthread::kPthreadLibImage,
                                       guest_lib_pthread::kPthreadLibSize,
                                       kLibBase + 0x00E00000, &pthread_load_error);
        if (!pthread_lib) {
            report.Fail("pthread.load", pthread_load_error);
        } else {
            report.Pass("pthread.load", detail::Format("%s, %u bytes, %u relocations",
                                                       pthread_lib->soname.c_str(),
                                                       pthread_lib->image_size,
                                                       pthread_lib->relocation_count));

            const std::uint32_t tls_read = call_in(*pthread_lib, "lib_tls_read", {});
            report.Check("pthread.tls_read", tls_read == thread_pointer,
                         detail::Format("mrc p15, 0, .., c13, c0, 3 returned 0x%08x", tls_read));

            // TPIDRURW is writable; TPIDRURO must follow it, like the kernel does
            // on a context switch.
            const std::uint32_t moved = 0x12345000u;
            const std::uint32_t after_write =
                call_in(*pthread_lib, "lib_tls_write", {moved});
            report.Check("pthread.tls_write",
                         after_write == moved && machine.TlsBase() == moved,
                         detail::Format("wrote 0x%08x, read back 0x%08x", moved, after_write));

            call_in(*pthread_lib, "lib_tls_write", {thread_pointer});
            report.Check("pthread.tls_restore", machine.TlsBase() == thread_pointer,
                         detail::Format("thread pointer restored to 0x%08x",
                                        machine.TlsBase()));

            const std::uint32_t self = call_in(*pthread_lib, "lib_pthread_self", {});
            report.Check("pthread.self", self == thread_pointer,
                         detail::Format("pthread_self()=0x%08x", self));

            const std::uint32_t slots = call_in(*pthread_lib, "lib_tls_slots", {});
            report.Check("pthread.tls_slots", slots == 1,
                         detail::Format("TLS self and thread id slots point at the block: %u",
                                        slots));

            const std::uint32_t mutex = call_in(*pthread_lib, "lib_pthread_mutex", {});
            report.Check("pthread.mutex", mutex == 0,
                         detail::Format("init/lock/unlock/destroy returned %d",
                                        static_cast<int>(mutex)));

            const std::uint32_t cond = call_in(*pthread_lib, "lib_pthread_cond", {});
            report.Check("pthread.cond", cond == 0,
                         detail::Format("signal/broadcast/destroy returned %d",
                                        static_cast<int>(cond)));

            const std::uint32_t key = call_in(*pthread_lib, "lib_pthread_key", {});
            report.Check("pthread.key", key == 0,
                         detail::Format("key create/set/get/delete returned %d",
                                        static_cast<int>(key)));

            const std::uint32_t tls_errno = call_in(*pthread_lib, "lib_pthread_errno", {});
            report.Check("pthread.errno", tls_errno == ENOENT,
                         detail::Format("errno in the TLS block after a failed open = %u",
                                        tls_errno));

            const std::uint32_t once_runs = call_in(*pthread_lib, "lib_pthread_once", {});
            report.Check("pthread.once", once_runs == 1,
                         detail::Format("initializer ran %u time(s) across two pthread_once calls",
                                        once_runs));

            // --- real threads ------------------------------------------------
            const std::uint32_t counter = call_in(*pthread_lib, "lib_pthread_counter", {4, 250});
            report.Check("threads.counter", counter == 1000,
                         detail::Format("4 threads x 250 mutexed increments = %u", counter));

            const std::uint32_t joined = call_in(*pthread_lib, "lib_pthread_join_value", {4});
            report.Check("threads.join_value", joined == 34,
                         detail::Format("join returned %u, expected 4+7+10+13", joined));

            const std::uint32_t tls_sum = call_in(*pthread_lib, "lib_pthread_tls", {3});
            report.Check("threads.tls", tls_sum == 6,
                         detail::Format("per-thread key values 1+2+3 = %u", tls_sum));

            const std::uint32_t turn = call_in(*pthread_lib, "lib_pthread_pingpong", {50});
            report.Check("threads.pingpong", turn == 0,
                         detail::Format("50 condvar hand-offs ended on turn %u", turn));

            const std::uint32_t stress = call_in(*pthread_lib, "lib_pthread_counter", {8, 500});
            report.Check("threads.stress", stress == 4000,
                         detail::Format("8 threads x 500 increments under one mutex = %u", stress));

            const std::uint32_t exit_value = call_in(*pthread_lib, "lib_pthread_exit", {21});
            report.Check("threads.exit", exit_value == 42,
                         detail::Format("pthread_exit(21) came back through join as %u",
                                        exit_value));

            const std::uint32_t detached = call_in(*pthread_lib, "lib_pthread_detach", {});
            report.Check("threads.detach", detached == 0,
                         detail::Format("detached thread signalled and returned %d",
                                        static_cast<int>(detached)));
        }
    }

    // --- libc++ diagnostics -------------------------------------------------
    //
    // Set ARMBAT_LIBCXX_TRACE=1 to load libc++ on its own and report where an
    // initializer stops when its budget runs out: the halt reason, the program
    // counter and the last import that was dispatched.
    if (std::getenv("ARMBAT_LIBCXX_TRACE") != nullptr) {
        static const std::vector<std::uint8_t> image(guest_libcxx::kLibcxxImage,
                                                     guest_libcxx::kLibcxxImage +
                                                         guest_libcxx::kLibcxxSize);
        const std::uint32_t base = 0x03000000;
        std::string error;
        auto probe = loader.Load(image.data(), image.size(), base, &error);
        if (!probe) {
            report.Fail("trace.load", error);
        } else {
            machine.SetTickBudget(20'000'000);
            const bool ran = loader.RunInitializers(machine, *probe, &error);
            const std::uint32_t pc = machine.Reg(15);
            machine.SetTickBudget(1ull << 60);
            report.Pass("trace.stop",
                        detail::Format("ran=%d halt=%s pc=0x%08x last=%s", ran ? 1 : 0,
                                       machine.LastHaltReasonName().c_str(), pc,
                                       hle.LastFunctionName().c_str()));
            char words[96] = {};
            int cursor = 0;
            for (int index = -4; index < 4 && cursor < 90; ++index) {
                std::uint32_t value = 0;
                const std::uint32_t at = pc + static_cast<std::uint32_t>(index * 4);
                if (arena.IsMapped(at, 4)) {
                    std::memcpy(&value, arena.Ptr(at), 4);
                }
                cursor += std::snprintf(words + cursor, sizeof(words) - cursor, "%08x ", value);
            }
            report.Pass("trace.code", detail::Format("words around pc: %s", words));
        }
    }

    // --- The real C++ standard library ---------------------------------------
    // The NDK's arm32 libc++_shared.so is written out and loaded the way an app
    // loads it. A second library, compiled by the ordinary C++ driver, then links
    // against it and uses std::string, std::vector and exceptions. The libc++ code
    // itself runs under the JIT; only its libc imports come back through HLE.
    {
        const std::string directory = temp_path.substr(0, temp_path.find_last_of('/'));
        const std::string libcxx_path = directory + "/armbat-libc++_shared.so";
        const std::string cppstd_path = directory + "/armbat-cppstdlib.so";

        const auto write_image = [](const std::string& path, const unsigned char* data,
                                    std::size_t size) {
            std::FILE* output = std::fopen(path.c_str(), "wb");
            if (output == nullptr) {
                return false;
            }
            const bool written = std::fwrite(data, 1, size, output) == size;
            std::fclose(output);
            return written;
        };

        const bool images_written =
            write_image(libcxx_path, guest_libcxx::kLibcxxImage, guest_libcxx::kLibcxxSize) &&
            write_image(cppstd_path, guest_lib_cppstdlib::kCppStdLibImage,
                        guest_lib_cppstdlib::kCppStdLibSize);

        // Guest cycles are the only thing that bounds guest code. A budget that
        // is generous for the work at hand turns a runaway loop into a failed
        // check instead of a hung suite.
        const auto guarded = [&](std::uint64_t budget, auto&& body) {
            machine.SetTickBudget(budget);
            const auto result = body();
            const bool finished = machine.LastHaltReasonName() == "svc";
            const std::string halt = machine.LastHaltReasonName();
            machine.SetTickBudget(1ull << 60);
            return std::pair{result, finished ? std::string() : halt};
        };

        std::string libcxx_error;
        const auto [libcxx, libcxx_halt] =
            images_written ? guarded(100'000'000, [&] { return dynamic.Open(libcxx_path, &libcxx_error); })
                           : std::pair{0u, std::string()};

        if (!images_written) {
            report.Fail("cppstd.image", "could not write the images next to the temp file");
        } else if (libcxx == 0) {
            report.Fail("cppstd.libcxx", libcxx_halt.empty() ? libcxx_error
                                                             : "initializers did not return (" +
                                                                   libcxx_halt + ")");
        } else {
            report.Pass("cppstd.libcxx",
                        detail::Format("libc++_shared.so at 0x%08x, %zu bytes", libcxx,
                                       static_cast<std::size_t>(guest_libcxx::kLibcxxSize)));

            // A library that names it in DT_NEEDED must see the live image rather
            // than load a second copy.
            loader.AddVirtualLibrary("libc++_shared.so");

            std::string cppstd_error;
            std::uint32_t cppstd = 0;
            const auto [opened, cppstd_halt] =
                guarded(100'000'000, [&] {
                    cppstd = dynamic.Open(cppstd_path, &cppstd_error);
                    return cppstd;
                });
            (void)opened;
            if (cppstd == 0) {
                report.Fail("cppstd.load", cppstd_halt.empty() ? cppstd_error
                                                               : "initializers did not return (" +
                                                                     cppstd_halt + ")");
            } else {
                report.Pass("cppstd.load",
                            detail::Format("linked against libc++ at 0x%08x", cppstd));

                const auto call_module = [&](const char* name,
                                             std::initializer_list<std::uint32_t> args) {
                    const std::uint32_t address = dynamic.Symbol(cppstd, name);
                    if (address == 0) {
                        report.Fail(std::string("symbol.") + name, "not exported");
                        return 0u;
                    }
                    std::uint32_t value = 0;
                    const auto [result, halt] = guarded(200'000'000, [&] {
                        value = machine.CallFunction(address, args);
                        return value;
                    });
                    if (!machine.LastError().empty()) {
                        report.Fail(std::string("call.") + name, machine.LastError());
                    } else if (!halt.empty()) {
                        report.Fail(std::string("call.") + name,
                                    detail::Format("did not return (%s) at pc=0x%08x, last=%s",
                                                   halt.c_str(), machine.Reg(15),
                                                   hle.LastFunctionName().c_str()));
                    }
                    return result;
                };

                const std::uint32_t string_ops = call_module("cppstd_string_ops", {});
                report.Check("cppstd.string", string_ops == 12006,
                             detail::Format("\"hello\"+\" world\"+\"!\" = %u chars, find=6",
                                            string_ops / 1000));

                const std::uint32_t vector_sum = call_module("cppstd_vector_sum", {5});
                report.Check("cppstd.vector", vector_sum == 30,
                             detail::Format("sum of 0,3,6,9,12 after sort = %u", vector_sum));

                const std::uint32_t lookup = call_module("cppstd_map_lookup", {1});
                report.Check("cppstd.find", lookup == 1,
                             detail::Format("std::string::find('b') = %u", lookup));

                const std::uint32_t atomic_sum = call_module("cppstd_atomic", {1000});
                report.Check("cppstd.atomic", atomic_sum == 1000,
                             detail::Format("%u ldrex/strex increments", atomic_sum));

                const std::uint32_t quotient = call_module("cppstd_exception", {4});
                report.Check("cppstd.throw_clear", quotient == 25,
                             detail::Format("100/4 through try/catch = %u", quotient));

                const std::uint32_t caught = call_module("cppstd_exception", {0});
                report.Check("cppstd.throw_caught",
                             caught == static_cast<std::uint32_t>(-1016),
                             detail::Format("catch got \"division by zero\" (%d chars)",
                                            -static_cast<std::int32_t>(caught) - 1000));
            }
        }

        std::remove(libcxx_path.c_str());
        std::remove(cppstd_path.c_str());
    }

    // --- An APK, read the way the container will read it ---------------------
    {
        const std::string directory = temp_path.substr(0, temp_path.find_last_of('/'));
        const std::string apk_path = directory + "/armbat-fixture.apk";
        const std::string lib_path = directory + "/armbat-apk-lib.so";

        std::FILE* apk_file = std::fopen(apk_path.c_str(), "wb");
        bool written = false;
        if (apk_file != nullptr) {
            written = std::fwrite(guest_apk_fixture::kTestApkImage, 1,
                                  guest_apk_fixture::kTestApkSize,
                                  apk_file) == guest_apk_fixture::kTestApkSize;
            std::fclose(apk_file);
        }

        GuestApk apk;
        std::string apk_error;
        const bool opened = written && apk.Open(apk_path, &apk_error);
        report.Check("apk.open", opened,
                     opened ? detail::Format("%zu entries in %s", apk.Entries().size(),
                                             apk_path.c_str())
                            : (written ? apk_error : "could not write the fixture"));

        if (opened) {
            const std::vector<std::string> native = apk.NativeLibraries();
            const bool has_both = native.size() == 2 &&
                                  native[0] == "libguesttest.so" &&
                                  native[1] == "libguesttest_dep.so";
            report.Check("apk.armeabi_v7a", has_both,
                         detail::Format("%zu arm32 libraries: %s", native.size(),
                                        native.size() == 2
                                            ? (native[0] + ", " + native[1]).c_str()
                                            : ""));

            // The arm64 entry must not be mistaken for a guest library.
            const auto* arm64 = apk.Find("lib/arm64-v8a/libguesttest.so");
            report.Check("apk.abi_filter", arm64 != nullptr && native.size() == 2,
                         arm64 != nullptr ? "arm64 entry present but not selected"
                                          : "arm64 entry missing from the fixture");

            std::string extract_error;
            const bool extracted =
                apk.Extract("lib/armeabi-v7a/libguesttest.so", lib_path, &extract_error);
            report.Check("apk.extract", extracted,
                         extracted ? detail::Format("extracted to %s", lib_path.c_str())
                                   : extract_error);

            if (extracted) {
                std::string apk_load_error;
                std::vector<std::uint8_t> image;
                {
                    std::ifstream file(lib_path, std::ios::binary);
                    image.assign(std::istreambuf_iterator<char>(file),
                                 std::istreambuf_iterator<char>());
                }
                auto from_apk =
                    loader.Load(image.data(), image.size(), 0x03100000, &apk_load_error);
                if (!from_apk) {
                    report.Fail("apk.load", apk_load_error);
                } else {
                    const std::uint32_t sum = call_in(*from_apk, "lib_add", {2, 3});
                    report.Check("apk.call", sum == 5,
                                 detail::Format("lib_add(2,3) from the extracted entry = %u",
                                                sum));
                }
            }

            // An archive from before the v7a ABI existed: the libraries are in
            // lib/armeabi and must load all the same. The empty lib/armeabi-v7a/
            // entry in the fixture is what Android packages when an application
            // has no v7a code, and it must not win over the real directory.
            {
                const std::string legacy_path = directory + "/armbat-fixture-armeabi.apk";
                std::FILE* legacy_file = std::fopen(legacy_path.c_str(), "wb");
                bool legacy_written = false;
                if (legacy_file != nullptr) {
                    legacy_written =
                        std::fwrite(guest_apk_armeabi::kTestApkImage, 1,
                                    guest_apk_armeabi::kTestApkSize,
                                    legacy_file) == guest_apk_armeabi::kTestApkSize;
                    std::fclose(legacy_file);
                }
                GuestApk legacy;
                std::string legacy_error;
                const bool legacy_opened = legacy_written && legacy.Open(legacy_path, &legacy_error);
                const std::vector<std::string> legacy_native =
                    legacy_opened ? legacy.NativeLibraries() : std::vector<std::string>();
                const bool legacy_selected =
                    legacy_opened && legacy.AbiDirectory() == "lib/armeabi/" &&
                    legacy_native.size() == 2 && legacy_native[0] == "libguesttest.so" &&
                    legacy_native[1] == "libguesttest_dep.so";
                report.Check("apk.armeabi", legacy_selected,
                             legacy_opened ? detail::Format("abi %s, %zu arm32 libraries",
                                                            legacy.AbiDirectory().c_str(),
                                                            legacy_native.size())
                                           : (legacy_written ? legacy_error
                                                             : "could not write the fixture"));
                if (legacy_selected) {
                    std::vector<GuestApk::ExtractedNative> legacy_files;
                    std::string legacy_extract_error;
                    const bool legacy_extracted = legacy.ExtractNativeLibraries(
                        directory + "/legacy", &legacy_files, &legacy_extract_error);
                    report.Check("apk.armeabi_extract",
                                 legacy_extracted && legacy_files.size() == 2,
                                 legacy_extracted
                                     ? detail::Format("%zu libraries extracted from lib/armeabi",
                                                      legacy_files.size())
                                     : legacy_extract_error);
                }
                std::remove(legacy_path.c_str());
            }

            // The whole set, in dependency order: the second library imports a
            // function of the first, so it cannot be mapped earlier.
            std::vector<GuestApk::LoadedNative> natives;
            std::string natives_error;
            const bool bulk =
                apk.LoadNativeLibraries(dynamic, loader, directory, &natives, &natives_error);
            report.Check("apk.natives", bulk && natives.size() == 2,
                         bulk ? detail::Format("%zu libraries loaded from the APK",
                                               natives.size())
                              : natives_error);

            if (bulk) {
                std::uint32_t handle = 0;
                for (const GuestApk::LoadedNative& item : natives) {
                    if (item.soname == "libguesttest_dep.so") {
                        handle = item.base;
                    }
                }
                const std::uint32_t doubled_address =
                    handle != 0 ? dynamic.Symbol(handle, "lib_dep_double") : 0;
                const std::uint32_t doubled =
                    doubled_address != 0 ? machine.CallFunction(doubled_address, {5}) : 0;
                report.Check("apk.dependency", doubled == 10,
                             detail::Format("lib_dep_double(5) through libguesttest.so = %u",
                                            doubled));
            }

            for (const std::string& name : native) {
                std::remove((directory + "/" + name).c_str());
            }
        }

        std::remove(apk_path.c_str());
        std::remove(lib_path.c_str());
    }

    // --- Negative cases -----------------------------------------------------
    {
        std::string load_error;
        const auto truncated = loader.Load(guest_lib::kGuestLibImage, 16, kLibBase, &load_error);
        report.Check("elf.reject_truncated", !truncated && !load_error.empty(),
                     truncated ? "truncated image was accepted" : load_error);
    }
    {
        std::vector<std::uint8_t> corrupted(guest_lib::kGuestLibImage,
                                            guest_lib::kGuestLibImage + guest_lib::kGuestLibSize);
        // e_machine at offset 18, little-endian.
        corrupted[18] = 0xB7;
        corrupted[19] = 0x00;
        std::string load_error;
        const auto wrong = loader.Load(corrupted.data(), corrupted.size(), kLibBase + 0x00100000, &load_error);
        report.Check("elf.reject_machine", !wrong && !load_error.empty(),
                     wrong ? "wrong machine was accepted" : load_error);
    }
    {
        std::string load_error;
        const auto missing = loader.Load(guest_lib_missing::kMissingLibImage,
                                         guest_lib_missing::kMissingLibSize,
                                         kLibBase + 0x00200000, &load_error);
        const bool named = load_error.find("missing_external") != std::string::npos;
        report.Check("elf.reject_import", !missing && named,
                     missing ? "unresolved import was accepted" : load_error);
    }

    return finish();
}

}  // namespace armbat
