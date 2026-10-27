#include "armbat/handles.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace armbat {
namespace {

std::mutex g_mutex;
std::vector<void*> g_hosts;  // index 0 is never handed out: 0 means "none"
// A host pointer always maps to the same guest handle. EGL configs and the
// window are compared by identity by the guest, and a handle that changes
// between two eglChooseConfig calls would look like a different config.
std::unordered_map<void*, std::uint32_t> g_handles;

}  // namespace

std::uint32_t RegisterHostHandle(void* host) {
    if (host == nullptr) {
        return 0;
    }
    const std::lock_guard lock(g_mutex);
    const auto existing = g_handles.find(host);
    if (existing != g_handles.end()) {
        return existing->second;
    }
    g_hosts.push_back(host);
    const std::uint32_t handle = static_cast<std::uint32_t>(g_hosts.size());
    g_handles.emplace(host, handle);
    return handle;
}

void* LookupHostHandle(std::uint32_t guest) {
    if (guest == 0) {
        return nullptr;
    }
    const std::lock_guard lock(g_mutex);
    if (guest > g_hosts.size()) {
        return nullptr;
    }
    return g_hosts[guest - 1];
}

}  // namespace armbat
