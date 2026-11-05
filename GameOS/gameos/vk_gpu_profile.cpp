// vk_gpu_profile.cpp -- see the header.
#include "vk_gpu_profile.h"
#include "battle_bench.h"
#include "mc2_diag_verbose.h"

#ifdef MC2_VULKAN_ISLAND
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <string_view>
#include <vector>

#include "Gates/gate.h"
#include "hitch_stats.h"
#include "vk_gpu_timestamps.h"
#include "vk_one_shot.h"
#include "vk_owned.h"
#include "vulkan_context.h"

#ifdef TRACY_ENABLE
#include <tracy/TracyVulkan.hpp>
#endif

namespace mc2vk::gpuprof {
namespace {

constexpr uint32_t kRing = 4;          // frames of timestamp slots; a slot is read two frames after it was recorded
constexpr uint32_t kMaxScopes = 96;    // scopes one frame may time; more are drawn untimed and counted
constexpr uint32_t kQueriesPerSlot = kMaxScopes * 2;
constexpr uint32_t kLogEvery = 120;    // frames per [VK_GPU_PASSES v1] line
constexpr uint32_t kMaxNames = 64;

struct Row {
    const char* name = nullptr;
    double   sumMs = 0.0;      // over the window
    uint32_t calls = 0;
    uint32_t frames = 0;       // frames in the window in which the pass ran
};

struct State {
    VulkanContext*  ctx = nullptr;
    VkDevice        device = VK_NULL_HANDLE;
    bool            labels = false;          // vkCmdBeginDebugUtilsLabelEXT is loaded
    // timestamps (MC2_VK_GPU_TIMESTAMPS)
    bool            timers = false;
    VkQueryPool     pool = VK_NULL_HANDLE;
    double          nsPerTick = 0.0;
    uint64_t        validMask = ~0ull;
    std::atomic<uint32_t> count[kRing]{};
    const char*     names[kRing][kMaxScopes] = {};
    uint64_t        benchFrames[kRing] = {};
    std::atomic<uint64_t> frame{0};
    // frameEnd's query-reset / Tracy-collect submit: one persistent pool, a command buffer per ring slot, each reused once its last submit completed
    // (a pool created and destroyed per frame cost ~3 ms while Tracy was connected).
    OwnedCommandPool endPool;
    VkCommandBuffer endCb[kRing] = {};
    GpuTicket       endTicket[kRing] = {};
    // window aggregation
    Row             rows[kMaxNames];
    uint32_t        rowCount = 0;
    double          spanSumMs = 0.0, sumSumMs = 0.0;
    uint32_t        windowFrames = 0, windowDropped = 0, windowOverflow = 0;
#ifdef TRACY_ENABLE
    TracyVkCtx      tracy = nullptr;
    std::mutex      srclocMutex;
    struct Loc { const char* name; uint32_t line; tracy::SourceLocationData data; };
    std::deque<Loc> locs;
#endif
};

State g;

#ifdef TRACY_ENABLE
const tracy::SourceLocationData* locFor(std::string_view nameSv, std::string_view fileSv, uint32_t line) {
    const char* name = nameSv.data();
    const char* file = fileSv.data();
    std::lock_guard<std::mutex> lock(g.srclocMutex);
    for (const State::Loc& l : g.locs)
        if (l.name == name && l.line == line) return &l.data;
    g.locs.push_back({name, line, tracy::SourceLocationData{name, name, file, line, 0}});
    return &g.locs.back().data;
}
static_assert(sizeof(tracy::VkCtxScope) <= 48, "Scope::tracyZone_ is too small for tracy::VkCtxScope");
static_assert(alignof(tracy::VkCtxScope) <= 16, "Scope::tracyZone_ alignment");
tracy::VkCtxScope* zone(unsigned char* p) { return std::launder(reinterpret_cast<tracy::VkCtxScope*>(p)); }
#endif

Row& rowFor(std::string_view nameSv) {
    const char* name = nameSv.data();
    for (uint32_t i = 0; i < g.rowCount; ++i)
        if (g.rows[i].name == name || std::strcmp(g.rows[i].name, name) == 0) return g.rows[i];
    if (g.rowCount < kMaxNames) { g.rows[g.rowCount].name = name; return g.rows[g.rowCount++]; }
    return g.rows[kMaxNames - 1];
}

void flushWindow(uint64_t frame) {
    if (g.windowFrames == 0) return;
    const double n = g.windowFrames;
    std::fprintf(stderr, "[VK_GPU_PASSES v1] frame=%llu n=%u span_ms=%.3f sum_ms=%.3f unattributed_ms=%.3f scopes=%u dropped=%u overflow=%u |",
                 static_cast<unsigned long long>(frame), g.windowFrames, g.spanSumMs / n, g.sumSumMs / n,
                 (g.spanSumMs - g.sumSumMs) / n, g.rowCount, g.windowDropped, g.windowOverflow);
    for (uint32_t i = 0; i < g.rowCount; ++i) {
        const Row& r = g.rows[i];
        std::fprintf(stderr, " %s=%.3f(x%.1f)", r.name, r.sumMs / n, r.frames ? static_cast<double>(r.calls) / r.frames : 0.0);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    for (uint32_t i = 0; i < g.rowCount; ++i) { g.rows[i].sumMs = 0; g.rows[i].calls = 0; g.rows[i].frames = 0; }
    g.spanSumMs = g.sumSumMs = 0;
    g.windowFrames = g.windowDropped = g.windowOverflow = 0;
}

// Reads one slot and folds it into the window. `used` is how many scopes the slot recorded (all are reset afterwards).
void readSlot(uint32_t slot, uint32_t& used) {
    const uint32_t recorded = g.count[slot].exchange(0);
    if (recorded > kMaxScopes) g.windowOverflow += recorded - kMaxScopes;
    used = std::min<uint32_t>(recorded, kMaxScopes);
    if (used == 0) return;
    double perName[kMaxNames] = {};
    uint32_t callsOf[kMaxNames] = {};
    const char* nameOf[kMaxNames] = {};
    uint32_t nNames = 0;
    uint64_t minBegin = ~0ull, maxEnd = 0;
    uint64_t base = 0;
    bool haveBase = false;
    double sumMs = 0.0;
    bool any = false;
    for (uint32_t i = 0; i < used; ++i) {
        uint64_t r[4] = {};   // (begin, avail, end, avail)
        const uint32_t q = (slot * kMaxScopes + i) * 2;
        const VkResult res = vkGetQueryPoolResults(g.device, g.pool, q, 2, sizeof(r), r, 2 * sizeof(uint64_t),
                                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if ((res != VK_SUCCESS && res != VK_NOT_READY) || r[1] == 0 || r[3] == 0) { ++g.windowDropped; continue; }
        const uint64_t b = r[0] & g.validMask, e = r[2] & g.validMask;
        if (!haveBase) { base = b; haveBase = true; }
        const uint64_t rb = (b - base) & g.validMask, re = (e - base) & g.validMask;
        minBegin = std::min(minBegin, rb);
        maxEnd = std::max(maxEnd, re);
        const double ms = static_cast<double>((e - b) & g.validMask) * g.nsPerTick / 1.0e6;
        sumMs += ms;
        any = true;
        const char* nm = g.names[slot][i];
        uint32_t k = 0;
        while (k < nNames && nameOf[k] != nm) ++k;
        if (k == nNames && nNames < kMaxNames) { nameOf[nNames] = nm; ++nNames; }
        if (k < nNames) { perName[k] += ms; ++callsOf[k]; }
    }
    if (!any) return;
    mc2_hitch2::noteGpuFrame(static_cast<double>(maxEnd - minBegin) * g.nsPerTick / 1.0e6, sumMs);
    if (mc2::battlebench::output) {
        // One record, one fputs: the sim worker writes tick records to the same stream, so a record built in pieces would interleave.
        char line[4096];
        int len = std::snprintf(line, sizeof line, "{\"type\":\"gpu\",\"frame\":%llu,\"ms\":%.6f,\"recorded\":%u,\"valid\":%u,\"passes\":{",
            static_cast<unsigned long long>(g.benchFrames[slot]),
            static_cast<double>(maxEnd - minBegin) * g.nsPerTick / 1.0e6, recorded,
            [&] { uint32_t n = 0; for (uint32_t k = 0; k < nNames; ++k) n += callsOf[k]; return n; }());
        for (uint32_t k = 0; k < nNames && len > 0 && len < static_cast<int>(sizeof line) - 96; ++k)
            len += std::snprintf(line + len, sizeof line - len, "%s\"%s\":%.6f", k ? "," : "", nameOf[k], perName[k]);
        if (len > 0 && len < static_cast<int>(sizeof line) - 4) {
            std::snprintf(line + len, sizeof line - len, "}}\n");
            std::fputs(line, mc2::battlebench::output);
        }
    }
    for (uint32_t k = 0; k < nNames; ++k) {
        Row& row = rowFor(std::string_view(nameOf[k]));
        row.sumMs += perName[k];
        row.calls += callsOf[k];
        ++row.frames;
    }
    g.spanSumMs += static_cast<double>(maxEnd - minBegin) * g.nsPerTick / 1.0e6;
    g.sumSumMs += sumMs;
    ++g.windowFrames;
}

}  // namespace

bool active() {
    return g.labels || g.timers
#ifdef TRACY_ENABLE
           || g.tracy
#endif
        ;
}

void init(VulkanContext& ctx) {
    if (g.device == ctx.device() || !ctx.valid()) return;
    g.ctx = &ctx;
    g.device = ctx.device();
    g.labels = vkCmdBeginDebugUtilsLabelEXT != nullptr && vkCmdEndDebugUtilsLabelEXT != nullptr && debugNamesEnabled();

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(ctx.physicalDevice(), &props);
    uint32_t qfc = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice(), &qfc, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfc);
    if (qfc) vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice(), &qfc, qfs.data());
    const uint32_t fam = ctx.graphicsFamily();
    const bool tsOk = props.limits.timestampPeriod > 0.0f && fam < qfc && qfs[fam].timestampValidBits != 0;

    if (mc2vkts::timestampsEnabled() && tsOk) {
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
        vkGetPhysicalDeviceProperties2(ctx.physicalDevice(), &identity);
        std::fprintf(stderr, "[BENCH_DEVICE v1] device=\"%s\" vendor=%u device_id=%u driver_name=\"%s\" driver_info=\"%s\" driver_version=%u api_version=%u\n",
            props.deviceName, props.vendorID, props.deviceID, driver.driverName, driver.driverInfo, props.driverVersion, props.apiVersion);
        g.nsPerTick = props.limits.timestampPeriod;
        const uint32_t bits = qfs[fam].timestampValidBits;
        g.validMask = (bits >= 64) ? ~0ull : ((1ull << bits) - 1ull);
        VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = kRing * kQueriesPerSlot;
        if (vkCreateQueryPool(g.device, &qpci, nullptr, &g.pool) == VK_SUCCESS) {
            // Every query starts reset, so one no submitted command buffer ever wrote reads "not available".
            const VkQueryPool pool = g.pool;
            const uint32_t n = qpci.queryCount;
            g.timers = submitOneShot(ctx, [&](VkCommandBuffer cb) { vkCmdResetQueryPool(cb, pool, 0, n); }, "gpuprof_reset");
            if (!g.timers) { vkDestroyQueryPool(g.device, g.pool, nullptr); g.pool = VK_NULL_HANDLE; }
        }
        std::fprintf(stderr, "[VK_GPU_PASSES v1] timestamps %s ns_per_tick=%.3f scopes_per_frame=%u\n", g.timers ? "armed" : "unavailable",
                     g.nsPerTick, kMaxScopes);
    }

    bool tracyCtx = false;
#ifdef TRACY_ENABLE
    if (tsOk) {
        OwnedCommandPool pool;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        // The Tracy context begins / ends / submits this command buffer itself and idles the queue, so it must be unrecorded.
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = fam;
        const VkDevice dev = g.device;
        if (create(pool, dev, [&](VkCommandPool* h) { return vkCreateCommandPool(dev, &pci, nullptr, h); })) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = pool.get();
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(dev, &ai, &cb) == VK_SUCCESS) {
                g.tracy = TracyVkContext(ctx.physicalDevice(), dev, ctx.graphicsQueue(), cb);
                if (g.tracy) TracyVkContextName(g.tracy, "VkGraphics", 10);
            }
        }
        tracyCtx = g.tracy != nullptr;
    }
#endif
    if (mc2DiagVerbose()) {   // startup summary is an MC2_DIAG_VERBOSE line (a default build stays quiet)
        std::fprintf(stderr, "[VK_GPU_PROFILE v1] debug_labels=%d timestamps=%d tracy_ctx=%d\n", g.labels ? 1 : 0, g.timers ? 1 : 0, tracyCtx ? 1 : 0);
        std::fflush(stderr);
    }
}

void shutdown(VulkanContext& ctx) {
    if (g.device == VK_NULL_HANDLE) return;
    ctx.waitIdle("gpuprof_shutdown");
#ifdef TRACY_ENABLE
    if (g.tracy) { tracy::DestroyVkContext(g.tracy); g.tracy = nullptr; }
#endif
    if (g.pool) { vkDestroyQueryPool(g.device, g.pool, nullptr); g.pool = VK_NULL_HANDLE; }
    g.endPool.reset();
    for (uint32_t i = 0; i < kRing; ++i) { g.endCb[i] = VK_NULL_HANDLE; g.endTicket[i] = {}; }
    g.timers = false;
    g.labels = false;
    g.device = VK_NULL_HANDLE;
    g.ctx = nullptr;
}

void frameEnd() {
    if (g.device == VK_NULL_HANDLE) return;
    bool tracyLive = false;
#ifdef TRACY_ENABLE
    tracyLive = g.tracy && tracy::GetProfiler().IsConnected();
#endif
    if (!g.timers && !tracyLive) return;

    const uint64_t f = g.frame.load(std::memory_order_relaxed);
    uint32_t used = 0;
    const uint32_t rslot = static_cast<uint32_t>((f + kRing - 2) % kRing);   // recorded two frames ago
    if (g.timers && f >= 2) {
        readSlot(rslot, used);
        if (g.windowFrames >= kLogEvery) flushWindow(f);
    }
    if (used || tracyLive) {
        const uint32_t cslot = static_cast<uint32_t>(f % kRing);
        if (!g.endPool) {
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pci.queueFamilyIndex = g.ctx->graphicsFamily();
            const VkDevice dev = g.device;
            if (create(g.endPool, dev, [&](VkCommandPool* h) { return vkCreateCommandPool(dev, &pci, nullptr, h); })) {
                VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                ai.commandPool = g.endPool.get();
                ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                ai.commandBufferCount = kRing;
                if (vkAllocateCommandBuffers(dev, &ai, g.endCb) != VK_SUCCESS) { g.endPool.reset(); for (VkCommandBuffer& c : g.endCb) c = VK_NULL_HANDLE; }
            }
        }
        VkCommandBuffer cb = g.endCb[cslot];
        // The slot's previous submit is kRing frames old (at most 2 frames are in flight), so this wait returns at once; a slot that never completes
        // skips this frame's submit.
        if (cb != VK_NULL_HANDLE && g.ctx->wait(g.endTicket[cslot], 100'000'000ull, "gpuprof_end_cb")) {
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkResetCommandBuffer(cb, 0) == VK_SUCCESS && vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS) {
                if (used) vkCmdResetQueryPool(cb, g.pool, rslot * kQueriesPerSlot, used * 2);
#ifdef TRACY_ENABLE
                if (tracyLive) TracyVkCollect(g.tracy, cb);
#endif
                if (vkEndCommandBuffer(cb) == VK_SUCCESS) {
                    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                    si.commandBufferCount = 1;
                    si.pCommandBuffers = &cb;
                    uint64_t value = 0;
                    if (g.ctx->submit(si, VK_NULL_HANDLE, &value) == VK_SUCCESS) g.endTicket[cslot] = GpuTicket{value, 0};
                }
            }
        } else if (used && cb != VK_NULL_HANDLE) {
            // The slot's end command buffer did not retire in time: the read-out queries still must be reset or the slot reads stale next lap.
            const VkQueryPool pool = g.pool;
            const uint32_t first = rslot * kQueriesPerSlot, count = used * 2;
            submitOneShot(*g.ctx, [&](VkCommandBuffer one) { vkCmdResetQueryPool(one, pool, first, count); }, "gpuprof_reset_late");
        }
    }
    g.frame.fetch_add(1, std::memory_order_relaxed);
}

Scope::Scope(VkCommandBuffer cb, std::string_view nameSv, [[maybe_unused]] std::string_view file, [[maybe_unused]] uint32_t line) {
    infra::breadcrumb("pass", nameSv, 0, cb);
    const char* name = nameSv.data();
    if (cb == VK_NULL_HANDLE || g.device == VK_NULL_HANDLE) return;
    cb_ = cb;
    if (g.labels) {
        VkDebugUtilsLabelEXT l{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        l.pLabelName = name;
        l.color[0] = 0.35f; l.color[1] = 0.65f; l.color[2] = 0.95f; l.color[3] = 1.0f;
        vkCmdBeginDebugUtilsLabelEXT(cb, &l);
        label_ = true;
    }
    if (g.timers) {
        const uint64_t f = g.frame.load(std::memory_order_relaxed);
        const uint32_t slot = static_cast<uint32_t>(f % kRing);
        const uint32_t idx = g.count[slot].fetch_add(1, std::memory_order_relaxed);
        if (idx == 0) g.benchFrames[slot] = mc2::battlebench::frame;
        if (idx < kMaxScopes) {
            g.names[slot][idx] = name;
            tsSlot_ = slot;
            tsIndex_ = static_cast<int32_t>(idx);
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.pool, (slot * kMaxScopes + idx) * 2);
        }
    }
#ifdef TRACY_ENABLE
    if (g.tracy && tracy::GetProfiler().IsConnected()) {
        ::new (static_cast<void*>(tracyZone_)) tracy::VkCtxScope(g.tracy, locFor(name, file, line), cb, true);
        tracy_ = true;
    }
#endif
}

void Scope::end() {
    if (cb_ == VK_NULL_HANDLE) return;
#ifdef TRACY_ENABLE
    if (tracy_) { zone(tracyZone_)->~VkCtxScope(); tracy_ = false; }
#endif
    if (tsIndex_ >= 0) {
        vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.pool, (tsSlot_ * kMaxScopes + static_cast<uint32_t>(tsIndex_)) * 2 + 1);
        tsIndex_ = -1;
    }
    if (label_) { vkCmdEndDebugUtilsLabelEXT(cb_); label_ = false; }
    cb_ = VK_NULL_HANDLE;
}

}  // namespace mc2vk::gpuprof
#endif  // MC2_VULKAN_ISLAND
