#include "armbat/hle.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <android/log.h>

#include "armbat/log.h"
#include <fcntl.h>
#include <unistd.h>

#include "armbat/a32_machine.h"
#include "armbat/guest_arena.h"
#include "hle_format.h"

namespace armbat {
namespace {

constexpr std::uint32_t kStubSize = 8;
constexpr std::uint32_t kPageSize = 4096;
constexpr std::uint32_t kSvcEncoding = 0xEF000000u;  // SVC #imm24
constexpr std::uint32_t kBxLr = 0xE12FFF1Eu;

// SVC immediate used by the platform shim libraries, whose trampolines pass the
// address of their own name in r12 instead of a registration index.
constexpr std::uint32_t kDispatchByNameSvc = 0xFFFEu;

// HLE data cells live above the stub page: stubs need a few pages at most, and
// keeping them clear of it means a growing function table never collides with
// an already handed-out variable address.
constexpr std::uint32_t kDataBaseOffset = 0x200000;
// The data area runs from 0x01000000 to the heap at 0x02000000, i.e. sixteen
// megabytes. It used to be the strip between the stub page and the trap page,
// which is one megabyte -- and an application that maps a whole asset through
// AAsset_getBuffer hands over half a megabyte at a time, so the strip ran dry
// during asset loading and every later allocation answered null. What lives in
// here is what the guest is told it owns: strings, array copies, asset buffers.
constexpr std::uint32_t kDataRegionSize = 0x1200000;

// The trap page sits one megabyte above the stub page and ends the stub region;
// the data area begins above it.
constexpr std::uint32_t kStubRegionLimit = 0x100000;

// Guest wchar_t is 4 bytes on ARM32. %ls and %lc hand over UTF-32.
void AppendUtf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

// A guest pointer that is not mapped is a bug in the guest. Ignoring it makes the
// symptom appear far away from the cause, so name the import and the arguments.
// The first few are logged loudly; a loop would otherwise flood logd.
void ReportGuestMemoryFault(const char* function, std::uint32_t first, std::uint32_t second,
                            std::uint32_t length) {
    static std::atomic<unsigned> reported{0};
    const unsigned count = reported.fetch_add(1, std::memory_order_relaxed);
    if (count >= 64) {
        return;
    }
    ARMBAT_LOG_ERROR("Armbat",
                        "guest memory fault in %s(0x%08x, 0x%08x, 0x%08x)%s", function, first,
                        second, length,
                        count + 1 == 64 ? " (further faults suppressed)" : "");
}

}  // namespace

std::uint32_t HleCall::Arg(std::size_t index) const {
    if (index < 4) {
        return machine_.Reg(index);
    }
    const std::uint32_t sp = machine_.Reg(13);
    const std::uint32_t slot = sp + static_cast<std::uint32_t>(4 * (index - 4));
    std::uint32_t value = 0;
    if (!arena_.IsMapped(slot, sizeof(value))) {
        return 0;
    }
    std::memcpy(&value, arena_.Ptr(slot), sizeof(value));
    return value;
}

void HleCall::Return(std::uint32_t value) {
    machine_.SetReg(0, value);
}

std::uint32_t HleCall::ErrnoCell() const {
    // Slot 2 of the thread pointer block, as bionic lays it out.
    return machine_.TlsBase() + 8;
}

void HleCall::ReturnPair(std::uint32_t first, std::uint32_t second) {
    machine_.SetReg(0, first);
    machine_.SetReg(1, second);
}

void HleCall::ReturnQuad(std::uint32_t r0, std::uint32_t r1, std::uint32_t r2,
                         std::uint32_t r3) {
    machine_.SetReg(0, r0);
    machine_.SetReg(1, r1);
    machine_.SetReg(2, r2);
    machine_.SetReg(3, r3);
}

bool HleCall::CopyIn(std::uint32_t address, void* destination, std::size_t length) const {
    if (length != 0 && !arena_.IsMapped(address, length)) {
        return false;
    }
    if (length != 0) {
        std::memcpy(destination, arena_.Ptr(address), length);
    }
    return true;
}

bool HleCall::CopyOut(std::uint32_t address, const void* source, std::size_t length) {
    if (length != 0 && !arena_.IsMapped(address, length)) {
        return false;
    }
    if (length != 0) {
        std::memcpy(arena_.Ptr(address), source, length);
    }
    return true;
}

bool HleCall::CopyWithin(std::uint32_t destination, std::uint32_t source, std::size_t length) {
    if (length == 0) {
        return true;
    }
    if (!arena_.IsMapped(destination, length) || !arena_.IsMapped(source, length)) {
        return false;
    }
    std::memmove(arena_.Ptr(destination), arena_.Ptr(source), length);
    return true;
}

bool HleCall::CompareWithin(std::uint32_t left, std::uint32_t right, std::size_t length,
                            int* result) {
    if (length == 0) {
        *result = 0;
        return true;
    }
    if (!arena_.IsMapped(left, length) || !arena_.IsMapped(right, length)) {
        return false;
    }
    *result = std::memcmp(arena_.Ptr(left), arena_.Ptr(right), length);
    return true;
}

bool HleCall::Fill(std::uint32_t address, std::uint8_t value, std::size_t length) {
    if (length != 0 && !arena_.IsMapped(address, length)) {
        return false;
    }
    if (length != 0) {
        std::memset(arena_.Ptr(address), value, length);
    }
    return true;
}

bool HleCall::Zero(std::uint32_t address, std::size_t length) {
    return Fill(address, 0, length);
}

bool HleCall::ReadString(std::uint32_t address, std::size_t max_length, bool require_terminator,
                         std::string& out) const {
    out.clear();
    out.reserve(std::min<std::size_t>(max_length, 256));

    // Ask about one contiguous mapping at a time and let memchr scan inside it.
    // Checking every byte separately took a shared lock and a range lookup per
    // character, which made strlen the most expensive import in the runtime.
    std::uint64_t cursor = address;
    std::size_t left = max_length;
    while (left != 0) {
        if (cursor > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        const std::uint64_t run_end = arena_.MappedEnd(static_cast<std::uint32_t>(cursor));
        if (run_end == 0 || run_end <= cursor) {
            return false;
        }
        const std::size_t take =
            static_cast<std::size_t>(std::min<std::uint64_t>(run_end - cursor, left));
        const char* chunk = reinterpret_cast<const char*>(arena_.Ptr(static_cast<std::uint32_t>(cursor)));
        if (const void* terminator = std::memchr(chunk, '\0', take)) {
            out.append(chunk, static_cast<const char*>(terminator) - chunk);
            return true;
        }
        out.append(chunk, take);
        cursor += take;
        left -= take;
    }
    return !require_terminator;
}

bool HleCall::ReadCString(std::uint32_t address, std::string& out, std::size_t max_length) const {
    return ReadString(address, max_length, true, out);
}

bool HleCall::ReadCStringWindow(std::uint32_t address, std::size_t max_length,
                                std::string& out) const {
    return ReadString(address, max_length, false, out);
}

bool HleCall::CStringLength(std::uint32_t address, std::uint64_t& length) const {
    length = 0;
    std::uint64_t cursor = address;
    while (cursor <= std::numeric_limits<std::uint32_t>::max()) {
        const std::uint64_t run_end = arena_.MappedEnd(static_cast<std::uint32_t>(cursor));
        if (run_end == 0 || run_end <= cursor) {
            return false;
        }
        const std::size_t take = static_cast<std::size_t>(run_end - cursor);
        const char* chunk = reinterpret_cast<const char*>(arena_.Ptr(static_cast<std::uint32_t>(cursor)));
        const void* terminator = std::memchr(chunk, '\0', take);
        if (terminator != nullptr) {
            length += static_cast<const char*>(terminator) - chunk;
            return true;
        }
        length += take;
        cursor += take;
    }
    return false;
}

std::byte* HleCall::Mapped(std::uint32_t address, std::size_t length) {
    if (length != 0 && !arena_.IsMapped(address, length)) {
        return nullptr;
    }
    return reinterpret_cast<std::byte*>(arena_.Ptr(address));
}

HleRegistry::HleRegistry(A32Machine& machine, GuestArena& arena, std::uint32_t stub_base)
    : machine_(machine), arena_(arena), stub_base_(stub_base) {}

void HleRegistry::Add(std::string name, HleFunction function) {
    const auto existing = by_name_.find(name);
    if (existing != by_name_.end()) {
        functions_[existing->second] = std::move(function);
        names_[existing->second] = std::move(name);
        return;
    }
    by_name_.emplace(name, static_cast<std::uint32_t>(functions_.size()));
    names_.push_back(std::move(name));
    functions_.push_back(std::move(function));
}

std::uint32_t HleRegistry::Resolve(std::string_view name) const {
    const auto data = data_by_name_.find(std::string(name));
    if (data != data_by_name_.end()) {
        return data->second;
    }
    const auto it = by_name_.find(std::string(name));
    if (it == by_name_.end()) {
        return 0;
    }
    return stub_base_ + it->second * kStubSize;
}

std::uint32_t HleRegistry::AllocateData(std::uint32_t size) {
    if (size == 0) {
        size = 4;
    }
    size = (size + 15u) & ~15u;
    const std::lock_guard lock(data_mutex_);

    // Released blocks are reused first. The strings the JNI bridge hands to the
    // guest are short-lived and numerous, and a bump allocator alone runs the
    // few hundred kilobytes between the stubs and the trap page dry during
    // startup.
    std::size_t best = data_free_.size();
    for (std::size_t i = 0; i < data_free_.size(); ++i) {
        if (data_free_[i].size >= size &&
            (best == data_free_.size() || data_free_[i].size < data_free_[best].size)) {
            best = i;
        }
    }
    if (best != data_free_.size()) {
        const std::uint32_t address = data_free_[best].address;
        if (data_free_[best].size > size) {
            data_free_[best].address += size;
            data_free_[best].size -= size;
        } else {
            data_free_.erase(data_free_.begin() + static_cast<std::ptrdiff_t>(best));
        }
        data_live_[address] = size;
        return address;
    }

    if (data_cursor_ == 0) {
        data_cursor_ = stub_base_ + kDataBaseOffset;
    }
    const std::uint32_t address = data_cursor_;
    const std::uint32_t end = address + size;
    // The data area cannot grow past the heap, and silently handing out zero
    // turns into a null string in the guest.
    if (end > stub_base_ + kDataRegionSize) {
        ARMBAT_LOG_ERROR("Armbat",
                            "hle data area is exhausted (%u bytes carved, asked %u)",
                            data_cursor_ - stub_base_ - kDataBaseOffset, size);
        return 0;
    }

    for (std::uint32_t page = address & ~(kPageSize - 1); page < end; page += kPageSize) {
        if (!arena_.IsMapped(page, kPageSize) && !arena_.Map(page, kPageSize)) {
            return 0;
        }
    }
    data_cursor_ = end;
    data_live_[address] = size;
    return address;
}

void HleRegistry::ReleaseData(std::uint32_t address) {
    if (address == 0) {
        return;
    }
    const std::lock_guard lock(data_mutex_);
    const auto live = data_live_.find(address);
    if (live == data_live_.end()) {
        return;
    }
    const std::uint32_t size = live->second;
    data_live_.erase(live);

    std::size_t at = 0;
    while (at < data_free_.size() && data_free_[at].address < address) {
        ++at;
    }
    data_free_.insert(data_free_.begin() + static_cast<std::ptrdiff_t>(at), DataBlock{address, size});
    if (at + 1 < data_free_.size() &&
        data_free_[at].address + data_free_[at].size == data_free_[at + 1].address) {
        data_free_[at].size += data_free_[at + 1].size;
        data_free_.erase(data_free_.begin() + static_cast<std::ptrdiff_t>(at + 1));
    }
    if (at > 0 && data_free_[at - 1].address + data_free_[at - 1].size == data_free_[at].address) {
        data_free_[at - 1].size += data_free_[at].size;
        data_free_.erase(data_free_.begin() + static_cast<std::ptrdiff_t>(at));
    }
}

std::uint32_t HleRegistry::AddData(std::string name, std::uint32_t size) {
    const std::uint32_t address = AllocateData(size);
    if (address != 0) {
        data_by_name_[std::move(name)] = address;
    }
    return address;
}

bool HleRegistry::Prepare(std::string* error) {
    if (functions_.empty()) {
        return true;
    }

    const std::uint32_t bytes = static_cast<std::uint32_t>(functions_.size()) * kStubSize;
    // The stubs grow up from stub_base_ and must stop short of the trap page.
    // If the table ever outgrows that gap, the two would overlap silently, so
    // refuse to start instead.
    if (bytes > kStubRegionLimit) {
        if (error) {
            *error = "too many HLE imports: the stub table would overwrite the data area";
        }
        return false;
    }
    const std::uint32_t pages = (bytes + kPageSize - 1) / kPageSize;
    if (!arena_.IsMapped(stub_base_, pages * kPageSize) &&
        !arena_.Map(stub_base_, pages * kPageSize)) {
        if (error) *error = "cannot map the HLE stub page";
        return false;
    }

    for (std::uint32_t i = 0; i < functions_.size(); ++i) {
        const std::uint32_t code[2] = {kSvcEncoding | (i + 1), kBxLr};
        std::memcpy(arena_.Ptr(stub_base_ + i * kStubSize), code, sizeof(code));
    }

    // Per-function counters for the graphics census, sized once the table is
    // known. A function registered after this point is simply not counted.
    if (counts_size_ < functions_.size()) {
        counts_ = std::make_unique<std::atomic<std::uint32_t>[]>(functions_.size());
        for (std::size_t i = 0; i < functions_.size(); ++i) {
            counts_[i].store(0, std::memory_order_relaxed);
        }
        counts_size_ = functions_.size();
    }

    if (!handler_installed_) {
        machine_.AddSvcHandler([this](std::uint32_t svc, A32Machine& machine) {
            return Dispatch(svc, machine);
        });
        handler_installed_ = true;
    }
    return true;
}

void HleRegistry::Record(std::uint32_t index, std::uint32_t pc, bool jni) const {
    static thread_local const std::int32_t thread = ::gettid();
    const std::uint64_t slot = recent_index_.fetch_add(1, std::memory_order_relaxed);
    RecentCall& entry = recent_[slot % kRecentCount];
    entry.thread = thread;
    entry.pc = pc;
    entry.index = index;
    entry.jni = jni;
}

void HleRegistry::RecordJniCall(std::uint32_t index, std::uint32_t pc) {
    Record(index, pc, true);
}

std::string HleRegistry::DescribeRecent(const RecentCall& entry) const {
    char text[128];
    if (entry.jni) {
        std::snprintf(text, sizeof(text), "jni#%u@0x%08x", entry.index, entry.pc);
    } else if (entry.index < names_.size()) {
        std::snprintf(text, sizeof(text), "%s@0x%08x", names_[entry.index].c_str(), entry.pc);
    } else {
        std::snprintf(text, sizeof(text), "#%u@0x%08x", entry.index, entry.pc);
    }
    return text;
}

std::vector<std::string> HleRegistry::RecentFunctions(std::int32_t thread) const {
    const std::uint64_t total = recent_index_.load(std::memory_order_relaxed);
    const std::uint64_t count = std::min<std::uint64_t>(total, kRecentCount);
    const std::uint64_t start = total - count;
    std::vector<std::string> out;
    for (std::uint64_t i = start; i < total; ++i) {
        const RecentCall& entry = recent_[i % kRecentCount];
        if (entry.thread != thread) {
            continue;
        }
        out.push_back(DescribeRecent(entry));
    }
    return out;
}

std::vector<std::string> HleRegistry::RecentFunctions() const {
    const std::uint64_t total = recent_index_.load(std::memory_order_relaxed);
    const std::uint64_t count = std::min<std::uint64_t>(total, kRecentCount);
    const std::uint64_t start = total - count;
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = start; i < total; ++i) {
        out.push_back(DescribeRecent(recent_[i % kRecentCount]));
    }
    return out;
}

std::vector<std::string> HleRegistry::ThreadTails(std::size_t per_thread) const {
    const std::uint64_t total = recent_index_.load(std::memory_order_relaxed);
    const std::uint64_t count = std::min<std::uint64_t>(total, kRecentCount);
    struct Tail {
        std::string text;
        std::size_t count = 0;
    };
    std::vector<std::pair<std::int32_t, Tail>> tails;
    for (std::uint64_t i = count; i > 0; --i) {
        const RecentCall& entry = recent_[(total - i) % kRecentCount];
        Tail* tail = nullptr;
        for (auto& candidate : tails) {
            if (candidate.first == entry.thread) {
                tail = &candidate.second;
                break;
            }
        }
        if (tail == nullptr) {
            tails.push_back({entry.thread, Tail{}});
            tail = &tails.back().second;
        }
        if (tail->count >= per_thread) {
            continue;
        }
        ++tail->count;
        tail->text += " " + DescribeRecent(entry);
    }
    std::vector<std::string> out;
    out.reserve(tails.size());
    for (auto& entry : tails) {
        out.push_back(std::to_string(entry.first) + ":" + entry.second.text);
    }
    return out;
}

namespace {

std::atomic<HleRegistry*> g_diagnostics_registry{nullptr};

}  // namespace

void SetDiagnosticRegistry(HleRegistry* registry) {
    g_diagnostics_registry.store(registry, std::memory_order_release);
}

std::string GraphicsCallSummary() {
    HleRegistry* const registry = g_diagnostics_registry.load(std::memory_order_acquire);
    return registry != nullptr ? registry->CallSummary() : std::string();
}

std::vector<std::string> RecentCallsOfThread(std::int32_t thread, std::size_t count) {
    HleRegistry* const registry = g_diagnostics_registry.load(std::memory_order_acquire);
    if (registry == nullptr) {
        return {};
    }
    std::vector<std::string> calls = registry->RecentFunctions(thread);
    if (calls.size() > count) {
        calls.erase(calls.begin(), calls.end() - static_cast<std::ptrdiff_t>(count));
    }
    return calls;
}

bool LogAllowed(const char* category, unsigned limit) {    static std::mutex mutex;
    static std::unordered_map<std::string, unsigned> counts;
    const std::lock_guard lock(mutex);
    const unsigned count = ++counts[category];
    return count <= limit;
}

bool HleRegistry::Dispatch(std::uint32_t svc, A32Machine& machine) {
    // A platform shim library exports one trampoline per API symbol: it puts the
    // address of its own name into r12 and traps here. That keeps the shim
    // independent of registration order, which is what lets the same prebuilt
    // library serve every guest.
    if (svc == kDispatchByNameSvc) {
        const std::uint32_t name_address = machine.Reg(12);
        const std::size_t slot =
            (static_cast<std::size_t>(name_address) * 2654435761u >> 8) & (kNameCacheSlots - 1);
        const std::uint64_t cached = name_cache_[slot].load(std::memory_order_acquire);
        if (static_cast<std::uint32_t>(cached >> 32) == name_address) {
            const std::uint32_t value = static_cast<std::uint32_t>(cached);
            if (value != 0) {
                if (value == 0xFFFFFFFFu) {
                    // A name the container does not implement: the shim gets a
                    // zero answer, and the fact is already in the log.
                    machine.SetReg(0, 0);
                    return true;
                }
                return DispatchIndex(value - 1, machine);
            }
        }

        HleCall call(machine, arena_);
        std::string name;
        if (!call.ReadCString(name_address, name, 256) || name.empty()) {
            ARMBAT_LOG_ERROR("Armbat",
                                "hle: unreadable name pointer 0x%08x", name_address);
            machine.SetReg(0, 0);
            return true;
        }
        std::uint32_t packed = 0;
        {
            const std::lock_guard lock(name_cache_mutex_);
            const auto it = by_name_.find(name);
            if (it == by_name_.end()) {
                // The library exports the whole Android API, the container
                // implements a subset. An unimplemented call returns zero instead
                // of faulting, and names itself once so the gap is visible in the
                // log.
                if (LogAllowed(("hle-missing:" + name).c_str(), 1)) {
                    ARMBAT_LOG_WARN("Armbat", "hle: unimplemented %s",
                                        name.c_str());
                }
                packed = (static_cast<std::uint64_t>(name_address) << 32) | 0xFFFFFFFFu;
                name_cache_[slot].store(packed, std::memory_order_release);
                machine.SetReg(0, 0);
                return true;
            }
            packed = (static_cast<std::uint64_t>(name_address) << 32) |
                     static_cast<std::uint64_t>(it->second + 1);
            name_cache_[slot].store(packed, std::memory_order_release);
        }
        return DispatchIndex(static_cast<std::uint32_t>(packed) - 1, machine);
    }

    const std::uint32_t index = svc - 1;
    if (index >= functions_.size()) {
        return false;
    }
    return DispatchIndex(index, machine);
}

bool HleRegistry::DispatchIndex(std::uint32_t index, A32Machine& machine) {
    HleCall call(machine, arena_);
    last_index_.store(index, std::memory_order_relaxed);
    for (std::size_t i = 0; i < 4; ++i) {
        last_args_[i].store(call.Arg(i), std::memory_order_relaxed);
    }
    Record(index, machine.Reg(14), false);
    machine.SetHleCall(names_[index].c_str());
    CensusCall(index);
    functions_[index](call);
    machine.SetHleCall(nullptr);
    return true;
}

// The graphics entry points are forwarded silently, so the log cannot say how
// far rendering got. The first call of every gl*/egl* name is reported, and
// buffer swaps are counted once per five seconds. The count is a relaxed
// increment of a per-function atomic: this runs on every draw.
void HleRegistry::CensusCall(std::uint32_t index) const {
    if (counts_ == nullptr || index >= counts_size_) {
        return;
    }
    const std::string& name = names_[index];
    if (name.rfind("gl", 0) != 0 && name.rfind("egl", 0) != 0) {
        return;
    }
    const std::uint32_t count = counts_[index].fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1) {
        ARMBAT_LOG_INFO("Armbat", "gfx: first %s", name.c_str());
        return;
    }
    if (name == "eglSwapBuffers" && (count <= 3 || count % 60 == 0)) {
        ARMBAT_LOG_INFO("Armbat", "gfx: eglSwapBuffers %u", count);
    }
}

std::string HleRegistry::CallSummary() const {
    static const char* kInteresting[] = {
        "glCreateProgram", "glCreateShader", "glShaderSource", "glCompileShader",
        "glAttachShader", "glLinkProgram", "glUseProgram", "glDeleteProgram",
        "glDeleteShader", "glGetProgramiv", "glGetShaderiv", "glGetActiveUniform",
        "glGetUniformLocation", "glDrawElements", "glDrawArrays", "eglSwapBuffers"};
    std::string out;
    for (const char* name : kInteresting) {
        const auto it = by_name_.find(name);
        if (it == by_name_.end() || counts_ == nullptr || it->second >= counts_size_) {
            continue;
        }
        const std::uint32_t count = counts_[it->second].load(std::memory_order_relaxed);
        if (count == 0) {
            continue;
        }
        if (!out.empty()) {
            out += ' ';
        }
        out += name;
        out += '=';
        out += std::to_string(count);
    }
    return out;
}

std::string HleRegistry::LastFunctionName() const {
    const std::uint32_t index = last_index_.load(std::memory_order_relaxed);
    if (index >= names_.size()) {
        return {};
    }
    return names_[index];
}

std::array<std::uint32_t, 4> HleRegistry::LastFunctionArgs() const {
    std::array<std::uint32_t, 4> args{};
    for (std::size_t i = 0; i < args.size(); ++i) {
        args[i] = last_args_[i].load(std::memory_order_relaxed);
    }
    return args;
}

namespace detail {

void StoreErrno(HleCall& call, std::uint32_t cell, std::int32_t value) {
    if (cell != 0) {
        call.CopyOut(cell, &value, sizeof(value));
    }
}

// Renders a guest printf-style format. Arguments arrive in r0-r3 and then on the
// stack, so they are read as words instead of a host va_list. Each conversion is
// handed to the host snprintf with the value already extracted, which keeps
// width, precision and flags correct without reimplementing padding.
std::string RenderFormat(const ArgumentReader& arguments, const std::string& format) {
    std::string out;

    const auto next32 = [&]() { return arguments.next32(); };
    const auto next64 = [&]() -> std::uint64_t { return arguments.next64(); };
    const auto is_digit = [](char value) {
        return std::isdigit(static_cast<unsigned char>(value)) != 0;
    };

    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') {
            out.push_back(format[i]);
            continue;
        }
        if (i + 1 >= format.size()) {
            out.push_back('%');
            break;
        }
        if (format[i + 1] == '%') {
            out.push_back('%');
            ++i;
            continue;
        }

        const std::size_t start = i;
        ++i;

        std::string spec = "%";
        bool left_aligned = false;
        while (i < format.size() && std::strchr("-+ #0", format[i]) != nullptr) {
            left_aligned = left_aligned || format[i] == '-';
            spec.push_back(format[i]);
            ++i;
        }

        // Width and precision written as '*' are arguments. They have to be
        // folded into the spec text: the host snprintf has no vararg for the star
        // and would take the next conversion's value instead.
        long long width = 0;
        if (i < format.size() && format[i] == '*') {
            width = static_cast<std::int32_t>(next32());
            ++i;
        } else {
            while (i < format.size() && is_digit(format[i])) {
                width = std::min<long long>(width * 10 + (format[i] - '0'), 1 << 20);
                ++i;
            }
        }
        if (width < 0) {
            if (!left_aligned) {
                spec.insert(1, "-");  // a negative width is a '-' flag plus a width
            }
            width = -width;
        }
        if (width != 0) {
            spec += std::to_string(width);
        }

        bool has_precision = false;
        long long precision = 0;
        if (i < format.size() && format[i] == '.') {
            ++i;
            has_precision = true;
            if (i < format.size() && format[i] == '*') {
                precision = static_cast<std::int32_t>(next32());
                if (precision < 0) {
                    has_precision = false;  // a negative precision counts as absent
                }
                ++i;
            } else {
                precision = 0;
                while (i < format.size() && is_digit(format[i])) {
                    precision = std::min<long long>(precision * 10 + (format[i] - '0'), 1 << 20);
                    ++i;
                }
            }
        }
        if (has_precision) {
            spec.push_back('.');
            spec += std::to_string(precision);
        }

        int longs = 0;
        bool intmax = false;
        while (i < format.size()) {
            const char modifier = format[i];
            if (modifier == 'l') {
                ++longs;
                ++i;
                continue;
            }
            if (modifier == 'j' || modifier == 'q') {
                intmax = true;  // ARM32 intmax_t is 64-bit, unlike the host's long
                ++i;
                continue;
            }
            if (modifier == 'z' || modifier == 't') {
                ++i;  // 32-bit in the guest ABI: the host sees a plain int
                continue;
            }
            if (modifier == 'h') {
                ++i;
                continue;
            }
            if (modifier == 'L') {
                ++i;  // guest long double is 8 bytes, so it is rendered as a double
                continue;
            }
            break;
        }
        if (i >= format.size()) {
            out.append(format, start, std::string::npos);
            break;
        }

        const char conversion = format[i];
        const bool wide = longs >= 1;  // %ls and %lc
        const bool sixty_four = intmax || longs >= 2;
        if (sixty_four && conversion != 's' && conversion != 'c') {
            spec += "ll";
        }
        spec += conversion;

        // Render this one conversion into a buffer sized from snprintf's own
        // answer. The fixed 1024-byte buffer used to clip a long %s silently and
        // report the clipped length as the result.
        const auto emit = [&out](const char* spec_text, auto... values) {
            char stack[256];
            const int needed = std::snprintf(stack, sizeof(stack), spec_text, values...);
            if (needed <= 0) {
                return;
            }
            if (static_cast<std::size_t>(needed) < sizeof(stack)) {
                out.append(stack, static_cast<std::size_t>(needed));
                return;
            }
            std::string heap(static_cast<std::size_t>(needed) + 1, '\0');
            std::snprintf(heap.data(), heap.size(), spec_text, values...);
            out.append(heap.data(), static_cast<std::size_t>(needed));
        };

        switch (conversion) {
        case 'd':
        case 'i':
            if (sixty_four) {
                emit(spec.c_str(), static_cast<long long>(static_cast<std::int64_t>(next64())));
            } else {
                emit(spec.c_str(), static_cast<int>(static_cast<std::int32_t>(next32())));
            }
            break;
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            if (sixty_four) {
                emit(spec.c_str(), static_cast<unsigned long long>(next64()));
            } else {
                emit(spec.c_str(), static_cast<unsigned>(next32()));
            }
            break;
        case 'c':
            if (wide) {
                std::string utf8;
                AppendUtf8(utf8, next32());
                spec.back() = 's';
                emit(spec.c_str(), utf8.c_str());
            } else {
                emit(spec.c_str(), static_cast<int>(static_cast<std::int32_t>(next32())));
            }
            break;
        case 's': {
            std::string text;
            const std::uint32_t address = next32();
            const bool ok = wide ? arguments.read_wide(address, text)
                                 : arguments.read_string(address, text);
            if (!ok) {
                text = "<fault>";
            }
            emit(spec.c_str(), text.c_str());
            break;
        }
        case 'p':
            emit(spec.c_str(),
                 reinterpret_cast<void*>(static_cast<std::uintptr_t>(next32())));
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            const std::uint64_t bits = next64();
            double value = 0.0;
            std::memcpy(&value, &bits, sizeof(value));
            emit(spec.c_str(), value);
            break;
        }
        case 'n':
            next32();  // %n is not implemented; still consume its pointer
            break;
        default:
            out.append(format, start, i - start + 1);
            continue;
        }
    }

    return out;
}

ArgumentReader MakeCallReader(HleCall& call, std::size_t first_argument) {
    // Arguments arrive in r0-r3 and then on the stack; 64-bit values are 8-byte
    // aligned relative to the start of the argument list.
    auto index = std::make_shared<std::size_t>(first_argument);
    ArgumentReader reader;
    reader.next32 = [&call, index]() { return call.Arg((*index)++); };
    reader.next64 = [&call, index]() -> std::uint64_t {
        if ((*index & 1u) != 0) {
            ++*index;
        }
        const std::uint64_t low = call.Arg((*index)++);
        const std::uint64_t high = call.Arg((*index)++);
        return low | (high << 32);
    };
    reader.read_string = [&call](std::uint32_t address, std::string& out) {
        return call.ReadCString(address, out, 1u << 20);
    };
    reader.read_wide = [&call](std::uint32_t address, std::string& out) {
        out.clear();
        for (std::size_t i = 0; i < (1u << 18); ++i) {
            const std::uint64_t at = static_cast<std::uint64_t>(address) + i * 4;
            if (at + sizeof(std::uint32_t) > (1ull << 32)) {
                return false;
            }
            std::uint32_t code = 0;
            if (!call.CopyIn(static_cast<std::uint32_t>(at), &code, sizeof(code))) {
                return false;
            }
            if (code == 0) {
                return true;
            }
            AppendUtf8(out, code);
        }
        return false;
    };
    return reader;
}

// Arguments of a guest va_list. In the ARM AAPCS a va_list is passed by value as
// the address of the argument block: the prologue spilled r0-r3 there and the
// caller's stack arguments follow, so reading words in order and aligning 64-bit
// values is the whole algorithm.
ArgumentReader MakeVaListReader(HleCall& call, std::uint32_t va_list) {
    auto cursor = std::make_shared<std::uint32_t>(va_list);

    ArgumentReader reader;
    reader.next32 = [&call, cursor]() {
        std::uint32_t value = 0;
        call.CopyIn(*cursor, &value, sizeof(value));
        *cursor += 4;
        return value;
    };
    reader.next64 = [&call, cursor]() -> std::uint64_t {
        *cursor = (*cursor + 7u) & ~7u;  // doubles sit on an 8-byte boundary
        std::uint64_t value = 0;
        call.CopyIn(*cursor, &value, sizeof(value));
        *cursor += 8;
        return value;
    };
    reader.read_string = [&call](std::uint32_t address, std::string& out) {
        return call.ReadCString(address, out, 1u << 20);
    };
    reader.read_wide = [&call](std::uint32_t address, std::string& out) {
        out.clear();
        for (std::size_t i = 0; i < (1u << 18); ++i) {
            const std::uint64_t at = static_cast<std::uint64_t>(address) + i * 4;
            if (at + sizeof(std::uint32_t) > (1ull << 32)) {
                return false;
            }
            std::uint32_t code = 0;
            if (!call.CopyIn(static_cast<std::uint32_t>(at), &code, sizeof(code))) {
                return false;
            }
            if (code == 0) {
                return true;
            }
            AppendUtf8(out, code);
        }
        return false;
    };
    return reader;
}

}  // namespace detail

namespace {
std::uint32_t g_last_abort = 0;
}

void NoteGuestAbort(std::uint32_t return_address) { g_last_abort = return_address; }
std::uint32_t LastGuestAbort() { return g_last_abort; }

void RegisterExtraFunctions(HleRegistry& registry);

void RegisterStandardFunctions(HleRegistry& registry) {
    // --- errno and C++ runtime bookkeeping ----------------------------------
    registry.Add("__errno", [](HleCall& call) { call.Return(call.ErrnoCell()); });
    registry.Add("__cxa_atexit", [](HleCall& call) { call.Return(0); });
    registry.Add("__cxa_finalize", [](HleCall& call) { call.Return(0); });
    registry.Add("atexit", [](HleCall& call) { call.Return(0); });

    // Function-local statics. Guest threads can reach the guard concurrently, so
    // the claim is a compare-exchange: exactly one thread runs the initializer.
    // The guard byte doubles as "done", which means a waiter that loses the race
    // may see the object slightly early; a double-initialised static is worse.
    registry.Add("__cxa_guard_acquire", [](HleCall& call) {
        auto* guard = reinterpret_cast<std::uint8_t*>(call.Mapped(call.Arg(0), 1));
        if (guard == nullptr) {
            call.Return(0);
            return;
        }
        std::uint8_t expected = 0;
        const bool claimed = __atomic_compare_exchange_n(guard, &expected, 1, false,
                                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        call.Return(claimed ? 1u : 0u);
    });
    registry.Add("__cxa_guard_release", [](HleCall& call) {
        if (auto* guard = reinterpret_cast<std::uint8_t*>(call.Mapped(call.Arg(0), 1))) {
            __atomic_store_n(guard, 1, __ATOMIC_RELEASE);
        }
        call.Return(0);
    });
    registry.Add("__cxa_guard_abort", [](HleCall& call) { call.Return(0); });

    registry.Add("atoi", [](HleCall& call) {
        std::string text;
        if (!call.ReadCString(call.Arg(0), text, 64)) {
            call.Return(0);
            return;
        }
        call.Return(static_cast<std::uint32_t>(std::strtol(text.c_str(), nullptr, 10)));
    });

    registry.Add("snprintf", [](HleCall& call) {
        const std::uint32_t destination = call.Arg(0);
        const std::uint32_t capacity = call.Arg(1);
        std::string format;
        if (!call.ReadCString(call.Arg(2), format, 4096)) {
            call.Return(0);
            return;
        }

        const std::string rendered =
            detail::RenderFormat(detail::MakeCallReader(call, 3), format);
        const std::size_t written =
            capacity == 0 ? 0 : std::min<std::size_t>(rendered.size(), capacity - 1);
        if (written != 0) {
            call.CopyOut(destination, rendered.data(), written);
        }
        if (capacity != 0) {
            const char terminator = '\0';
            call.CopyOut(destination + static_cast<std::uint32_t>(written), &terminator, 1);
        }
        call.Return(static_cast<std::uint32_t>(rendered.size()));
    });

    // The va_list variant: the guest passes a pointer to the AAPCS argument block
    // instead of registers, so the reader walks that memory.
    registry.Add("vsnprintf", [](HleCall& call) {
        const std::uint32_t destination = call.Arg(0);
        const std::uint32_t capacity = call.Arg(1);
        std::string format;
        if (!call.ReadCString(call.Arg(2), format, 4096)) {
            call.Return(0);
            return;
        }

        const std::string rendered =
            detail::RenderFormat(detail::MakeVaListReader(call, call.Arg(3)), format);
        const std::size_t written =
            capacity == 0 ? 0 : std::min<std::size_t>(rendered.size(), capacity - 1);
        if (written != 0) {
            call.CopyOut(destination, rendered.data(), written);
        }
        if (capacity != 0) {
            const char terminator = '\0';
            call.CopyOut(destination + static_cast<std::uint32_t>(written), &terminator, 1);
        }
        call.Return(static_cast<std::uint32_t>(rendered.size()));
    });

    // --- libc wrappers that use errno ---------------------------------------
    registry.Add("open", [](HleCall& call) {
        std::string path;
        if (!call.ReadCString(call.Arg(0), path, 4096)) {
            detail::StoreErrno(call, call.ErrnoCell(), EFAULT);
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        const bool traced = path.find("ShaderCache") != std::string::npos;
        if (traced && LogAllowed("shadercache-open", 40)) {
            ARMBAT_LOG_INFO("Armbat", "file: open %s flags=0x%x",
                                path.c_str(), call.Arg(1));
        }
        if (path.empty() || path[0] != '/') {
            // The guest has no working directory: every path it resolves is
            // absolute. A relative one is simply not found, not unreadable.
            detail::StoreErrno(call, call.ErrnoCell(), ENOENT);
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        // Guest ARM32 and host ARM64 use the same values for O_DIRECTORY,
        // O_NOFOLLOW, O_DIRECT and O_LARGEFILE (the arm64 UAPI header keeps the
        // legacy ARM encoding), so the flags pass through unchanged. Masking
        // 0x8000 here stripped O_NOFOLLOW, not O_LARGEFILE.
        const int flags = static_cast<int>(call.Arg(1));
        const int fd = ::open(path.c_str(), flags, static_cast<int>(call.Arg(2)));
        if (fd < 0) {
            const int error = errno;
            if (traced && LogAllowed("shadercache-open-fail", 20)) {
                ARMBAT_LOG_INFO("Armbat", "file: open %s failed errno=%d",
                                    path.c_str(), error);
            }
            detail::StoreErrno(call, call.ErrnoCell(), error);
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        call.Return(static_cast<std::uint32_t>(fd));
    });

    registry.Add("close", [](HleCall& call) {
        if (::close(static_cast<int>(call.Arg(0))) != 0) {
            const int error = errno;
            detail::StoreErrno(call, call.ErrnoCell(), error);
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        call.Return(0);
    });

    registry.Add("read", [](HleCall& call) {
        const int fd = static_cast<int>(call.Arg(0));
        const std::uint32_t count = std::min<std::uint32_t>(call.Arg(2), INT32_MAX);
        std::byte* buffer = call.Mapped(call.Arg(1), count);
        if (buffer == nullptr && count != 0) {
            detail::StoreErrno(call, call.ErrnoCell(), EFAULT);
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        const ssize_t result = ::read(fd, buffer, count);
        if (result < 0) {
            const int error = errno;
            detail::StoreErrno(call, call.ErrnoCell(), error);
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        call.Return(static_cast<std::uint32_t>(result));
    });

    registry.Add("write", [](HleCall& call) {
        const int fd = static_cast<int>(call.Arg(0));
        const std::uint32_t count = std::min<std::uint32_t>(call.Arg(2), INT32_MAX);
        const std::byte* buffer = call.Mapped(call.Arg(1), count);
        if (buffer == nullptr && count != 0) {
            detail::StoreErrno(call, call.ErrnoCell(), EFAULT);
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        const ssize_t result = ::write(fd, buffer, count);
        if (result < 0) {
            const int error = errno;
            detail::StoreErrno(call, call.ErrnoCell(), error);
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        call.Return(static_cast<std::uint32_t>(result));
    });

    registry.Add("lseek", [](HleCall& call) {
        const off_t result = ::lseek(static_cast<int>(call.Arg(0)),
                                     static_cast<off_t>(static_cast<std::int32_t>(call.Arg(1))),
                                     static_cast<int>(call.Arg(2)));
        if (result < 0) {
            const int error = errno;
            detail::StoreErrno(call, call.ErrnoCell(), error);
            call.Return(static_cast<std::uint32_t>(-1));
            return;
        }
        call.Return(static_cast<std::uint32_t>(result));
    });

    registry.Add("memcpy", [](HleCall& call) {
        const std::uint32_t destination = call.Arg(0);
        if (!call.CopyWithin(destination, call.Arg(1), call.Arg(2))) {
            ReportGuestMemoryFault("memcpy", call.Arg(0), call.Arg(1), call.Arg(2));
        }
        call.Return(destination);
    });

    registry.Add("memmove", [](HleCall& call) {
        const std::uint32_t destination = call.Arg(0);
        if (!call.CopyWithin(destination, call.Arg(1), call.Arg(2))) {
            ReportGuestMemoryFault("memmove", call.Arg(0), call.Arg(1), call.Arg(2));
        }
        call.Return(destination);
    });

    registry.Add("memset", [](HleCall& call) {
        const std::uint32_t destination = call.Arg(0);
        if (!call.Fill(destination, static_cast<std::uint8_t>(call.Arg(1)), call.Arg(2))) {
            ReportGuestMemoryFault("memset", call.Arg(0), call.Arg(1), call.Arg(2));
        }
        call.Return(destination);
    });

    registry.Add("memcmp", [](HleCall& call) {
        int result = 0;
        if (!call.CompareWithin(call.Arg(0), call.Arg(1), call.Arg(2), &result)) {
            ReportGuestMemoryFault("memcmp", call.Arg(0), call.Arg(1), call.Arg(2));
            call.Return(0);
            return;
        }
        call.Return(static_cast<std::uint32_t>(result));
    });

    registry.Add("strlen", [](HleCall& call) {
        std::uint64_t length = 0;
        if (!call.CStringLength(call.Arg(0), length)) {
            ReportGuestMemoryFault("strlen", call.Arg(0), 0, 0);
            call.Return(0);
            return;
        }
        call.Return(static_cast<std::uint32_t>(length));
    });

    registry.Add("strcmp", [](HleCall& call) {
        std::string left;
        std::string right;
        const bool ok = call.ReadCString(call.Arg(0), left, 1u << 20) &&
                        call.ReadCString(call.Arg(1), right, 1u << 20);
        if (!ok) {
            ReportGuestMemoryFault("strcmp", call.Arg(0), call.Arg(1), 0);
            call.Return(0);
            return;
        }
        const int result = left.compare(right);
        call.Return(static_cast<std::uint32_t>(result < 0 ? -1 : (result > 0 ? 1 : 0)));
    });

    registry.Add("strncmp", [](HleCall& call) {
        // strncmp may look at no more than `limit` bytes, so read exactly that
        // window: a buffer without a terminator inside it used to be read to
        // 1 MiB (and a fault there was reported as "equal").
        const std::uint32_t limit = call.Arg(2);
        std::string left;
        std::string right;
        const bool left_ok = call.ReadCStringWindow(call.Arg(0), limit, left);
        const bool right_ok = call.ReadCStringWindow(call.Arg(1), limit, right);
        if (!left_ok || !right_ok) {
            ReportGuestMemoryFault("strncmp", call.Arg(0), call.Arg(1), limit);
            call.Return(0);
            return;
        }
        const int result = left.compare(right);
        call.Return(static_cast<std::uint32_t>(result < 0 ? -1 : (result > 0 ? 1 : 0)));
    });

    registry.Add("abs", [](HleCall& call) {
        const auto value = static_cast<std::int32_t>(call.Arg(0));
        call.Return(value == std::numeric_limits<std::int32_t>::min()
                        ? static_cast<std::uint32_t>(value)
                        : static_cast<std::uint32_t>(value < 0 ? -value : value));
    });

    // ARM EABI helpers. __aeabi_memset takes (dest, length, byte) вЂ” not the C
    // (dest, byte, length) order.
    for (const char* name : {"__aeabi_memcpy", "__aeabi_memcpy4", "__aeabi_memcpy8"}) {
        registry.Add(name, [name](HleCall& call) {
            const std::uint32_t destination = call.Arg(0);
            if (!call.CopyWithin(destination, call.Arg(1), call.Arg(2))) {
                ReportGuestMemoryFault(name, call.Arg(0), call.Arg(1), call.Arg(2));
            }
            call.Return(destination);
        });
    }

    for (const char* name : {"__aeabi_memmove", "__aeabi_memmove4", "__aeabi_memmove8"}) {
        registry.Add(name, [name](HleCall& call) {
            const std::uint32_t destination = call.Arg(0);
            if (!call.CopyWithin(destination, call.Arg(1), call.Arg(2))) {
                ReportGuestMemoryFault(name, call.Arg(0), call.Arg(1), call.Arg(2));
            }
            call.Return(destination);
        });
    }

    for (const char* name : {"__aeabi_memclr", "__aeabi_memclr4", "__aeabi_memclr8"}) {
        registry.Add(name, [name](HleCall& call) {
            if (!call.Zero(call.Arg(0), call.Arg(1))) {
                ReportGuestMemoryFault(name, call.Arg(0), 0, call.Arg(1));
            }
            call.Return(call.Arg(0));
        });
    }

    for (const char* name : {"__aeabi_memset", "__aeabi_memset4", "__aeabi_memset8"}) {
        registry.Add(name, [name](HleCall& call) {
            const std::uint32_t destination = call.Arg(0);
            if (!call.Fill(destination, static_cast<std::uint8_t>(call.Arg(2)), call.Arg(1))) {
                ReportGuestMemoryFault(name, call.Arg(0), call.Arg(2), call.Arg(1));
            }
            call.Return(destination);
        });
    }

    // C++ runtime leftovers that compiled ARMv7 images reference but that only
    // make sense against a real unwinder. Unwinding cannot cross an HLE frame:
    // the guest frames between the archive and the host do not exist in the
    // DWARF tables, so the personality routines report "no handler" instead of
    // walking into the host.
    registry.Add("__cxa_pure_virtual", [](HleCall& call) { call.Machine().Stop(); });
    registry.Add("__cxa_deleted_virtual", [](HleCall& call) { call.Machine().Stop(); });
    registry.Add("__cxa_begin_cleanup", [](HleCall& call) { call.Return(1); });
    registry.Add("__cxa_end_cleanup", [](HleCall&) {});
    registry.Add("__cxa_call_unexpected", [](HleCall& call) { call.Return(0); });
    registry.Add("__cxa_type_match", [](HleCall& call) { call.Return(0); });
    registry.Add("__gnu_Unwind_Find_exidx", [](HleCall& call) { call.Return(0); });
    registry.Add("_Unwind_Complete", [](HleCall&) {});
    registry.Add("_Unwind_VRS_Get", [](HleCall& call) { call.Return(0); });
    registry.Add("_Unwind_VRS_Set", [](HleCall& call) { call.Return(0); });
    registry.Add("__stack_chk_fail", [](HleCall& call) { call.Machine().Stop(); });
    for (const char* name : {"_Unwind_Resume", "_Unwind_Backtrace"}) {
        registry.Add(name, [](HleCall& call) { call.Machine().Stop(); });
    }

    registry.Add("__aeabi_idiv", [](HleCall& call) {
        const auto numerator = static_cast<std::int32_t>(call.Arg(0));
        const auto denominator = static_cast<std::int32_t>(call.Arg(1));
        if (denominator == 0) {
            call.Return(0);
            return;
        }
        if (numerator == std::numeric_limits<std::int32_t>::min() && denominator == -1) {
            call.Return(static_cast<std::uint32_t>(numerator));
            return;
        }
        call.Return(static_cast<std::uint32_t>(numerator / denominator));
    });

    registry.Add("__aeabi_uidiv", [](HleCall& call) {
        const std::uint32_t numerator = call.Arg(0);
        const std::uint32_t denominator = call.Arg(1);
        call.Return(denominator == 0 ? 0u : numerator / denominator);
    });

    registry.Add("__aeabi_idivmod", [](HleCall& call) {
        const auto numerator = static_cast<std::int32_t>(call.Arg(0));
        const auto denominator = static_cast<std::int32_t>(call.Arg(1));
        if (denominator == 0) {
            call.ReturnPair(0, 0);
            return;
        }
        if (numerator == std::numeric_limits<std::int32_t>::min() && denominator == -1) {
            call.ReturnPair(static_cast<std::uint32_t>(numerator), 0);
            return;
        }
        call.ReturnPair(static_cast<std::uint32_t>(numerator / denominator),
                        static_cast<std::uint32_t>(numerator % denominator));
    });

    registry.Add("__aeabi_uidivmod", [](HleCall& call) {
        const std::uint32_t numerator = call.Arg(0);
        const std::uint32_t denominator = call.Arg(1);
        if (denominator == 0) {
            call.ReturnPair(0, 0);
            return;
        }
        call.ReturnPair(numerator / denominator, numerator % denominator);
    });

    registry.Add("__aeabi_ldivmod", [](HleCall& call) {
        const auto numerator = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(call.Arg(0)) |
            (static_cast<std::uint64_t>(call.Arg(1)) << 32));
        const auto denominator = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(call.Arg(2)) |
            (static_cast<std::uint64_t>(call.Arg(3)) << 32));
        if (denominator == 0) {
            call.ReturnQuad(0, 0, 0, 0);
            return;
        }
        if (numerator == std::numeric_limits<std::int64_t>::min() && denominator == -1) {
            call.ReturnQuad(0, 0x80000000u, 0, 0);
            return;
        }
        const auto quotient = static_cast<std::uint64_t>(numerator / denominator);
        const auto remainder = static_cast<std::uint64_t>(numerator % denominator);
        call.ReturnQuad(static_cast<std::uint32_t>(quotient & 0xFFFFFFFFu),
                        static_cast<std::uint32_t>(quotient >> 32),
                        static_cast<std::uint32_t>(remainder & 0xFFFFFFFFu),
                        static_cast<std::uint32_t>(remainder >> 32));
    });

    registry.Add("__aeabi_uldivmod", [](HleCall& call) {
        const auto numerator = static_cast<std::uint64_t>(call.Arg(0)) |
                               (static_cast<std::uint64_t>(call.Arg(1)) << 32);
        const auto denominator = static_cast<std::uint64_t>(call.Arg(2)) |
                                 (static_cast<std::uint64_t>(call.Arg(3)) << 32);
        if (denominator == 0) {
            call.ReturnQuad(0, 0, 0, 0);
            return;
        }
        const std::uint64_t quotient = numerator / denominator;
        const std::uint64_t remainder = numerator % denominator;
        call.ReturnQuad(static_cast<std::uint32_t>(quotient & 0xFFFFFFFFu),
                        static_cast<std::uint32_t>(quotient >> 32),
                        static_cast<std::uint32_t>(remainder & 0xFFFFFFFFu),
                        static_cast<std::uint32_t>(remainder >> 32));
    });

    RegisterExtraFunctions(registry);
}

}  // namespace armbat

