// vk_gpu_timestamps.h — VK-DRAW-PERF-1 GPU timestamp instrumentation (header-only).
//
// A tiny, self-contained GPU-timestamp bracket for the island render passes. Gated
// behind MC2_VK_GPU_TIMESTAMPS (default-OFF, verification-only). Uses
// vkCmdWriteTimestamp (VK 1.1 core, no device
// feature required — only the queue-family timestampValidBits + limits.timestampPeriod
// gate it, so it is MoltenVK-clean). No pipeline-statistics query (that WOULD need a
// device feature the shared engine context does not enable).
//
// Usage (per island, once fenced per frame so a single pool is race-free):
//   GpuTimer t; t.init(device, phys, gfxFamily, N);   // in island init
//   ... per frame, on the recording command buffer:
//   t.reset(cb);                          // BEFORE vkCmdBeginRenderPass
//   t.write(cb, 0);                       // stamp 0 (pass start)
//   vkCmdBeginRenderPass(...); ... binds ...
//   t.write(cb, 1);                       // stamp 1 (draws about to start)
//   ... draw loop ...
//   vkCmdEndRenderPass(cb);
//   t.write(cb, 2);                       // stamp 2 (all GPU work done)
//   ... vkQueueSubmit ...
//   t.resolve();                          // after submit; folds frames recorded >= 2 frames ago, never blocks
//   // then t.avgTotalUs(), t.avgGapUs(1) etc. for the breakdown.
#pragma once

#include <volk.h>
#include <vector>
#include <cstdlib>
#include <cstdint>
#include "Gates/gate.h"

namespace mc2vkts {

inline bool timestampsEnabled() {
    static int s = -1;
    if (s < 0) s = (mc2::gate::get<"MC2_VK_GPU_TIMESTAMPS">()) ? 1 : 0;
    return s == 1;
}

struct GpuTimer {
    // H8: a ring of pools. resolve() reads a pool kLag frames after it was recorded, without VK_QUERY_RESULT_WAIT_BIT, so it never
    // blocks the CPU on the GPU (the old WAIT_BIT read forced a deferred-batch flush and a stall on the frame just submitted).
    static constexpr uint32_t kRing = 4;
    static constexpr uint64_t kLag  = 2;
    VkDevice    device   = VK_NULL_HANDLE;
    VkQueryPool pools[kRing] = {};
    uint64_t    seqOf[kRing] = {};        // 0 = not recorded / already folded; else the frame sequence that recorded it
    uint64_t    seq      = 0;             // frames recorded so far
    uint32_t    cur      = 0;             // pool the current frame records into
    float       periodNs = 0.0f;      // props.limits.timestampPeriod (ns per tick)
    uint32_t    count    = 0;         // number of timestamps in the pool
    bool        supported = false;

    // Exponential-moving-average of each inter-stamp gap, in microseconds.
    // gapAvgUs[i] = avg( stamp[i] - stamp[i-1] ) for i in [1..count-1].
    double   gapAvgUs[8] = {0};
    double   totalAvgUs  = 0.0;
    uint64_t samples     = 0;

    bool init(VkDevice dev, VkPhysicalDevice phys, uint32_t queueFamily, uint32_t nQueries) {
        if (!timestampsEnabled()) { supported = false; return false; }
        device = dev;
        count  = (nQueries > 8) ? 8 : nQueries;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(phys, &props);
        periodNs = props.limits.timestampPeriod;
        uint32_t qfc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfc, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qfc);
        if (qfc) vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfc, qfs.data());
        if (periodNs <= 0.0f || queueFamily >= qfc || qfs[queueFamily].timestampValidBits == 0) {
            supported = false; return false;
        }
        VkQueryPoolCreateInfo qpci{};
        qpci.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = count;
        for (uint32_t i = 0; i < kRing; ++i) {
            if (vkCreateQueryPool(device, &qpci, nullptr, &pools[i]) != VK_SUCCESS) {
                destroy(); return false;
            }
        }
        supported = true;
        return true;
    }

    // Recorded islands: claim() takes the frame's ring pool on the thread that owns the timer (main) and returns it (null when unsupported); the recording
    // thread then only issues cmdReset/cmdWrite on that pool. reset(cb)/write(cb, idx) remain the claim-and-record-in-one-place form.
    VkQueryPool claim() {
        if (!supported) return VK_NULL_HANDLE;
        cur = static_cast<uint32_t>((seq) % kRing);
        seqOf[cur] = ++seq;
        return pools[cur];
    }
    static void cmdReset(VkCommandBuffer cb, VkQueryPool pool, uint32_t n) { if (pool) vkCmdResetQueryPool(cb, pool, 0, n); }
    static void cmdWrite(VkCommandBuffer cb, VkQueryPool pool, uint32_t n, uint32_t idx) {
        if (pool && idx < n) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, idx);
    }

    // Take the next ring pool for this frame and reset it — MUST be called OUTSIDE any render pass (before Begin).
    void reset(VkCommandBuffer cb) {
        if (!supported) return;
        cur = static_cast<uint32_t>((seq) % kRing);
        seqOf[cur] = ++seq;
        vkCmdResetQueryPool(cb, pools[cur], 0, count);
    }

    // All stamps use BOTTOM_OF_PIPE so each gap = execution time of the work recorded
    // between the two stamps (completion-to-completion), which is what we want for a
    // per-segment GPU cost breakdown.
    void write(VkCommandBuffer cb, uint32_t idx) {
        if (supported && idx < count)
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pools[cur], idx);
    }

    // Fold every ring pool recorded at least kLag frames ago that the GPU has finished (never waits; VK_NOT_READY just retries
    // next frame). Call once per frame after the submit.
    void resolve() {
        if (!supported) return;
        for (uint32_t k = 0; k < kRing; ++k) {
            const uint32_t i = static_cast<uint32_t>((seq + k) % kRing);   // oldest recorded pool first
            if (seqOf[i] == 0 || seqOf[i] + kLag > seq) continue;
            uint64_t ts[8] = {0};
            const VkResult r = vkGetQueryPoolResults(
                device, pools[i], 0, count, sizeof(uint64_t) * count, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            if (r != VK_SUCCESS) continue;   // VK_NOT_READY: leave it for the next call
            seqOf[i] = 0;
            fold(ts);
        }
    }

    void fold(const uint64_t (&ts)[8]) {
        ++samples;
        const double a = 0.05;   // EMA weight for the newest sample
        for (uint32_t i = 1; i < count; ++i) {
            double us = static_cast<double>((ts[i] - ts[i - 1])) * static_cast<double>(periodNs) / 1000.0;
            gapAvgUs[i] = (samples == 1) ? us : (gapAvgUs[i] * (1.0 - a) + us * a);
        }
        double tot = static_cast<double>((ts[count - 1] - ts[0])) * static_cast<double>(periodNs) / 1000.0;
        totalAvgUs = (samples == 1) ? tot : (totalAvgUs * (1.0 - a) + tot * a);
    }

    double avgTotalUs()          const { return totalAvgUs; }
    double avgGapUs(uint32_t i)  const { return (i < count) ? gapAvgUs[i] : 0.0; }

    void destroy() {
        for (auto& p : pools) if (p) { vkDestroyQueryPool(device, p, nullptr); p = VK_NULL_HANDLE; }
        supported = false;
    }
};

} // namespace mc2vkts
