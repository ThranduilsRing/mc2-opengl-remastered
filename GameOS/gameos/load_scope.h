#pragma once
// Mission-load sub-phase timers. MC2_LOAD_SCOPE("name") adds its wall time to a named accumulator while a mission
// load is in progress (between mission_phase_begin and mission_phase_report); outside a load it costs one atomic
// read. Times are inclusive (a scope nested in another is counted in both). One [LOAD_SCOPES v1] line per load.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace mc2::load {
struct Entry {
    const char* name = nullptr;
    std::atomic<int64_t> ns{0};
    std::atomic<uint32_t> calls{0};
};
inline constexpr int kMaxEntries = 128;
inline Entry g_entries[kMaxEntries];
inline std::atomic<int> g_entryCount{0};
inline std::atomic<bool> g_active{false};
inline std::mutex g_entryMutex;

inline Entry* entry(const char* name) {
    std::scoped_lock lock(g_entryMutex);
    const int n = g_entryCount.load();
    for (int i = 0; i < n; ++i)
        if (!std::strcmp(g_entries[i].name, name)) return &g_entries[i];
    if (n >= kMaxEntries) return nullptr;
    g_entries[n].name = name;
    g_entryCount.store(n + 1);
    return &g_entries[n];
}
inline void begin() {
    const int n = g_entryCount.load();
    for (int i = 0; i < n; ++i) { g_entries[i].ns.store(0); g_entries[i].calls.store(0); }
    g_active.store(true);
}
// "name=ms(xcalls)" for every scope that ran, largest first; ends the load window.
inline std::string report() {
    g_active.store(false);
    const int n = g_entryCount.load();
    int order[kMaxEntries];
    int used = 0;
    for (int i = 0; i < n; ++i)
        if (g_entries[i].calls.load()) order[used++] = i;
    std::sort(order, order + used, [](int a, int b) { return g_entries[a].ns.load() > g_entries[b].ns.load(); });
    std::string out;
    for (int k = 0; k < used; ++k) {
        const Entry& e = g_entries[order[k]];
        char item[160];
        std::snprintf(item, sizeof(item), " %s=%.1f(x%u)", e.name, static_cast<double>(e.ns.load()) / 1e6, e.calls.load());
        out += item;
    }
    return out;
}
class Scope {
    Entry* entry_;
    std::chrono::steady_clock::time_point start_;
public:
    explicit Scope(Entry* e) : entry_(e && g_active.load(std::memory_order_relaxed) ? e : nullptr) {
        if (entry_) start_ = std::chrono::steady_clock::now();
    }
    ~Scope() {
        if (!entry_) return;
        entry_->ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count());
        entry_->calls.fetch_add(1);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
}

#define MC2_LOAD_SCOPE_CAT2(a, b) a##b
#define MC2_LOAD_SCOPE_CAT(a, b) MC2_LOAD_SCOPE_CAT2(a, b)
#define MC2_LOAD_SCOPE(name) \
    static mc2::load::Entry* MC2_LOAD_SCOPE_CAT(mc2LoadEntry_, __LINE__) = mc2::load::entry(name); \
    mc2::load::Scope MC2_LOAD_SCOPE_CAT(mc2LoadScope_, __LINE__)(MC2_LOAD_SCOPE_CAT(mc2LoadEntry_, __LINE__))
