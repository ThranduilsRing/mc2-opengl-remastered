#pragma once
// Inclusive compatibility counters; exclusive accounting over begin-to-report.
// Boundary-crossing scopes are discarded and remain in the unscoped residual.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace mc2::load {
enum class Kind { io, decode, cook, upload, gpu_wait, sim, pump, other, count };
inline constexpr const char* kindNames[] = {"io", "decode", "cook", "upload", "gpu_wait", "sim", "pump", "other"};
struct Rule { std::string_view name; Kind kind; bool prefix = false; };
inline constexpr Rule rules[] = {
    {"tree.clone.", Kind::sim, true}, {"tree.register.", Kind::sim, true},
    {"io.", Kind::io, true}, {"decode.", Kind::decode, true}, {"cook.", Kind::cook, true},
    {"upload.", Kind::upload, true}, {"gpuwait.", Kind::gpu_wait, true}, {"sim.", Kind::sim, true},
    {"pump.", Kind::pump, true}, {"TextureSource.Open", Kind::io}, {"TextureSource.Read", Kind::io},
    {"TextureSource.Cooked.Read", Kind::io}, {"txm.hashInput", Kind::io},
    {"TerrainColorMap::init colorMap read", Kind::io}, {"TerrainColorMap::init normalMap read", Kind::io},
    {"TextureSource.Cooked.Parse", Kind::decode}, {"TextureSource.CreateCpuTexture", Kind::decode},
    {"TextureSource.Cooked.Decode", Kind::decode}, {"TerrainColorMap::init colorMap decode", Kind::decode},
    {"TerrainColorMap::init normalMap decode", Kind::decode}, {"txm.createTransition", Kind::cook},
    {"finalize.vegetation", Kind::cook}, {"finalize.forestOcclusion", Kind::cook},
    {"init.initAblCoreLibs", Kind::sim}, {"init.initMissionScript", Kind::sim},
    {"init.initWarriorsHeader", Kind::sim}, {"init.loadWarriors", Kind::sim},
    {"init.loadBrainParamsPhase", Kind::sim}, {"init.loadBrainRuntime", Kind::sim},
    {"init.loadBrainDispatch", Kind::sim}, {"init.initPartsHeader", Kind::sim}, {"init.initPartsLoad", Kind::sim},
    {"init.initSpawnActors", Kind::sim}
};
inline constexpr Kind kind(std::string_view name) {
    for (const auto& r : rules)
        if (r.prefix ? name.substr(0, r.name.size()) == r.name : name == r.name) return r.kind;
    return Kind::other;
}
inline int64_t clockNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Entry {
    std::string_view name;
    int64_t ns = 0, exclusive[2]{};
    uint32_t calls = 0, exclusiveCalls[2]{};
};
inline constexpr int kMaxEntries = 256;
inline Entry g_entries[kMaxEntries];
inline int g_entryCount = 0;
inline uint32_t g_capacityDrops = 0;
inline std::atomic<bool> g_active{false};
inline std::mutex g_entryMutex;
inline uint64_t g_generation = 0;
inline std::thread::id g_owner;
inline int64_t g_start = 0, g_kinds[2][8]{}, g_bytes[8]{};
inline uint32_t g_open = 0;
inline Entry* entry(std::string_view name) {
    std::scoped_lock lock(g_entryMutex);
    for (int i = 0; i < g_entryCount; ++i)
        if (g_entries[i].name == name) return &g_entries[i];
    if (g_entryCount == kMaxEntries) { ++g_capacityDrops; return nullptr; }
    auto* e = &g_entries[g_entryCount++]; e->name = name; return e;
}
inline void begin(int64_t now = -1) {
    std::scoped_lock lock(g_entryMutex);
    g_active.store(false); ++g_generation; g_owner = std::this_thread::get_id(); g_open = 0;
    for (int i = 0; i < g_entryCount; ++i) {
        auto name = g_entries[i].name; g_entries[i] = {}; g_entries[i].name = name;
    }
    for (auto& thread : g_kinds) for (auto& value : thread) value = 0;
    for (auto& value : g_bytes) value = 0;
    g_start = now < 0 ? clockNs() : now; g_active.store(true);
}
inline void bytes(Kind k, uint64_t n) {
    if (!g_active.load(std::memory_order_relaxed)) return;
    std::scoped_lock lock(g_entryMutex);
    if (g_active.load() && k != Kind::count) g_bytes[static_cast<int>(k)] += static_cast<int64_t>(n);
}
class Scope;
inline thread_local Scope* g_parent = nullptr;
class Scope {
    Entry* entry_ = nullptr;
    Scope* parent_ = nullptr;
    uint64_t generation_ = 0;
    int64_t start_ = 0, children_ = 0;
    int worker_ = 0;
public:
    explicit Scope(Entry* e, int64_t now = -1) {
        if (!e || !g_active.load(std::memory_order_relaxed)) return;
        std::scoped_lock lock(g_entryMutex);
        if (!g_active.load()) return;
        entry_ = e; generation_ = g_generation; worker_ = std::this_thread::get_id() != g_owner;
        start_ = now < 0 ? clockNs() : now; parent_ = g_parent; g_parent = this; ++g_open;
    }
    void end(int64_t now) {
        if (!entry_) return;
        std::scoped_lock lock(g_entryMutex);
        g_parent = parent_;
        if (generation_ == g_generation && g_active.load()) {
            const int64_t elapsed = (std::max)(int64_t{0}, now - start_);
            const int64_t exclusive = (std::max)(int64_t{0}, elapsed - children_);
            entry_->ns += elapsed; ++entry_->calls;
            entry_->exclusive[worker_] += exclusive; ++entry_->exclusiveCalls[worker_];
            g_kinds[worker_][static_cast<int>(kind(entry_->name))] += exclusive; --g_open;
            if (parent_ && parent_->generation_ == generation_) parent_->children_ += elapsed;
        }
        entry_ = nullptr;
    }
    ~Scope() { if (entry_) end(clockNs()); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
struct Results {
    std::string scopes, split, top;
    int64_t total = 0, kinds[2][8]{};
};
inline Results report(int64_t now = -1) {
    std::scoped_lock lock(g_entryMutex);
    g_active.store(false);
    Results r; r.total = (now < 0 ? clockNs() : now) - g_start;
    for (int t = 0; t < 2; ++t) for (int k = 0; k < 8; ++k) r.kinds[t][k] = g_kinds[t][k];
    int64_t scoped = 0; for (int k = 0; k < 8; ++k) scoped += r.kinds[0][k];
    r.kinds[0][7] += r.total - scoped;
    char item[256];
    std::snprintf(item, sizeof(item), " total=%.3f", r.total / 1e6); r.split = item;
    for (int t = 0; t < 2; ++t) for (int k = 0; k < 8; ++k) {
        std::snprintf(item, sizeof(item), " %s%s=%.3f", t ? "worker_" : "", kindNames[k], r.kinds[t][k] / 1e6);
        r.split += item;
    }
    for (const auto k : {Kind::io, Kind::decode, Kind::upload}) {
        std::snprintf(item, sizeof(item), " bytes_%s=%lld", kindNames[static_cast<int>(k)], static_cast<long long>(g_bytes[static_cast<int>(k)]));
        r.split += item;
    }
    std::snprintf(item, sizeof(item), " bytes_coverage=unavailable scoped_ms=%.3f residual_ms=%.3f open_scopes=%u capacity_drops=%u",
        scoped / 1e6, (r.total - scoped) / 1e6, g_open, g_capacityDrops); r.split += item;
    int order[kMaxEntries], used = 0;
    for (int i = 0; i < g_entryCount; ++i) if (g_entries[i].calls) order[used++] = i;
    std::sort(order, order + used, [](int a, int b) { return g_entries[a].ns > g_entries[b].ns; });
    for (int i = 0; i < used; ++i) {
        const auto& e = g_entries[order[i]];
        std::snprintf(item, sizeof(item), " %.*s=%.1f(x%u)", static_cast<int>(e.name.size()), e.name.data(), e.ns / 1e6, e.calls);
        r.scopes += item;
    }
    for (int t = 0; t < 2; ++t) {
        std::sort(order, order + used, [t](int a, int b) { return g_entries[a].exclusive[t] > g_entries[b].exclusive[t]; });
        for (int i = 0; i < (std::min)(12, used); ++i) {
            const auto& e = g_entries[order[i]]; if (!e.exclusiveCalls[t]) continue;
            std::snprintf(item, sizeof(item), " %s%.*s=%.1f(x%u)", t ? "worker_" : "", static_cast<int>(e.name.size()), e.name.data(), e.exclusive[t] / 1e6, e.exclusiveCalls[t]);
            r.top += item;
        }
    }
    return r;
}
}
#define MC2_LOAD_SCOPE_CAT2(a, b) a##b
#define MC2_LOAD_SCOPE_CAT(a, b) MC2_LOAD_SCOPE_CAT2(a, b)
#define MC2_LOAD_SCOPE(name) \
    static mc2::load::Entry* MC2_LOAD_SCOPE_CAT(mc2LoadEntry_, __LINE__) = mc2::load::entry(name); \
    mc2::load::Scope MC2_LOAD_SCOPE_CAT(mc2LoadScope_, __LINE__)(MC2_LOAD_SCOPE_CAT(mc2LoadEntry_, __LINE__))
