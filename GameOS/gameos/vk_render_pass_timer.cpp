// vk_render_pass_timer.cpp -- see the header.
#include "vk_render_pass_timer.h"

#include "render_backend_select.h"

#ifdef MC2_VULKAN_ISLAND
#include <memory>
#include <vector>

#include "mc2fmt.h"
#include "vk_owned.h"
#include "vulkan_context.h"
#endif

namespace vk_render_pass_timer {

#ifdef MC2_VULKAN_ISLAND
namespace {

using gos_render_pass_timer::Pass_Count;

// Frames in flight the ring covers before a slot is reused, as the GL timer; a slot still unfinished then is dropped.
constexpr int kRing = 4;
// Brackets one frame may record (every island pass, several shadow cascades); more are ignored for that frame.
constexpr uint32_t kBracketsPerSlot = 64;

struct Bracket {
    int      pass = -1;
    uint32_t query = 0;      // first of the bracket's two queries
    bool     closed = false; // End stamped; an open bracket is never read
};

struct Slot {
    std::vector<Bracket> brackets;
    bool pending = false;    // the frame was closed and awaits its results
};

// The pool lives in the engine context's island slot, so the context releases it ahead of the device.
struct TimerState : mc2vk::IslandState {
    VkDevice device = VK_NULL_HANDLE;
    double   nsPerTick = 0.0;
    uint64_t validMask = ~0ull;
    mc2vk::OwnedQueryPool pool;   // kRing * kBracketsPerSlot * 2 TIMESTAMP queries
    Slot     ring[kRing];
    int      cur = 0;
    int      open[Pass_Count];    // bracket index in ring[cur] for an open pass, -1 none
    TimerState() { for (int& o : open) o = -1; }
};

bool s_unsupported = false;   // the device cannot time on the graphics queue: decided once, logged once

// Puts every query of the new pool in the reset state with one GPU submit, so a read of a bracket whose command buffer was
// recorded but never submitted finds an unavailable query (counted as not ready) instead of an uninitialized one.
bool resetAll(const mc2vk::VulkanContext& ctx, VkQueryPool qp, uint32_t count) {
    const VkDevice device = ctx.device();
    mc2vk::OwnedCommandPool cpool;
    mc2vk::OwnedFence fence;
    bool ok = false, submitted = false;
    do {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = ctx.graphicsFamily();
        if (!mc2vk::create(cpool, device, [&](VkCommandPool* h) { return vkCreateCommandPool(device, &pci, nullptr, h); })) break;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = cpool.get();
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device, &ai, &cb) != VK_SUCCESS) break;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!mc2vk::create(fence, device, [&](VkFence* h) { return vkCreateFence(device, &fci, nullptr, h); })) break;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) break;
        vkCmdResetQueryPool(cb, qp, 0, count);
        if (vkEndCommandBuffer(cb) != VK_SUCCESS) break;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        if (vkQueueSubmit(ctx.graphicsQueue(), 1, &si, fence.get()) != VK_SUCCESS) break;
        submitted = true;
        const VkFence fh = fence.get();
        ok = vkWaitForFences(device, 1, &fh, VK_TRUE, 5ull * 1000 * 1000 * 1000) == VK_SUCCESS;
    } while (false);
    // The fence and the pool are released on return: a submit whose fence did not signal must finish first.
    if (submitted && !ok) ctx.waitIdle("pass_timer_reset");
    return ok;
}

TimerState* state(bool create) {
    if (s_unsupported) return nullptr;
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) return nullptr;
    std::unique_ptr<mc2vk::IslandState>& slot = ctx.islandSlot(mc2vk::IslandSlot::PassTimer);
    if (slot || !create) return static_cast<TimerState*>(slot.get());

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(ctx.physicalDevice(), &props);
    uint32_t qfc = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice(), &qfc, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfc);
    if (qfc) vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice(), &qfc, qfs.data());
    const uint32_t fam = ctx.graphicsFamily();
    if (props.limits.timestampPeriod <= 0.0f || fam >= qfc || qfs[fam].timestampValidBits == 0) {
        s_unsupported = true;
        mc2::fmt::print_out("[RENDER_PASS_TIME v1] vk: the graphics queue has no timestamps; VK pass times stay empty\n");
        return nullptr;
    }
    auto t = std::make_unique<TimerState>();
    t->device = ctx.device();
    t->nsPerTick = props.limits.timestampPeriod;
    const uint32_t bits = qfs[fam].timestampValidBits;
    t->validMask = (bits >= 64) ? ~0ull : ((1ull << bits) - 1ull);
    VkQueryPoolCreateInfo qpci{};
    qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = kRing * kBracketsPerSlot * 2u;
    if (!mc2vk::create(t->pool, t->device, [&](auto* h) { return vkCreateQueryPool(t->device, &qpci, nullptr, h); })) {
        s_unsupported = true;
        mc2::fmt::print_out("[RENDER_PASS_TIME v1] vk: vkCreateQueryPool failed; VK pass times stay empty\n");
        return nullptr;
    }
    if (!resetAll(ctx, t->pool.get(), qpci.queryCount)) {
        s_unsupported = true;
        mc2::fmt::print_out("[RENDER_PASS_TIME v1] vk: the query pool reset submit failed; VK pass times stay empty\n");
        return nullptr;
    }
    mc2::fmt::print_out("[RENDER_PASS_TIME v1] vk: armed ring={:d} brackets={:d} ns_per_tick={:.3f}\n", mc2::fmt::as_i32(kRing),
                        mc2::fmt::as_u32(kBracketsPerSlot), t->nsPerTick);
    slot = std::move(t);
    return static_cast<TimerState*>(slot.get());
}

// Reads every closed bracket of `s`; false (nothing published) while any is unfinished.
bool harvest(TimerState& t, Slot& s) {
    double ms[Pass_Count] = {};
    bool   has[Pass_Count] = {};
    for (const Bracket& b : s.brackets) {
        if (!b.closed) continue;
        uint64_t r[4] = {};   // (begin, available, end, available)
        const VkResult res = vkGetQueryPoolResults(t.device, t.pool.get(), b.query, 2, sizeof(r), r, 2 * sizeof(uint64_t),
                                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (res != VK_SUCCESS || r[1] == 0 || r[3] == 0) return false;
        ms[b.pass] += TicksToMs(r[0], r[2], t.validMask, t.nsPerTick);
        has[b.pass] = true;
    }
    gos_render_pass_timer::SubmitBackendFrame(ms, has);
    return true;
}

}  // namespace

bool Active() {
    return mc2::renderBackend() == mc2::RenderBackend::Vulkan && gos_render_pass_timer::Enabled();
}

void Begin(VkCommandBuffer cb, gos_render_pass_timer::Pass p) {
    if (cb == VK_NULL_HANDLE || p < 0 || p >= Pass_Count || !Active()) return;
    TimerState* t = state(true);
    if (!t || t->open[p] >= 0) return;
    Slot& s = t->ring[t->cur];
    if (s.brackets.size() >= kBracketsPerSlot) return;
    Bracket b;
    b.pass = p;
    b.query = (static_cast<uint32_t>(t->cur) * kBracketsPerSlot + static_cast<uint32_t>(s.brackets.size())) * 2u;
    vkCmdResetQueryPool(cb, t->pool.get(), b.query, 2);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, t->pool.get(), b.query);
    t->open[p] = static_cast<int>(s.brackets.size());
    s.brackets.push_back(b);
}

void End(VkCommandBuffer cb, gos_render_pass_timer::Pass p) {
    if (cb == VK_NULL_HANDLE || p < 0 || p >= Pass_Count || !Active()) return;
    TimerState* t = state(false);
    if (!t || t->open[p] < 0) return;
    Bracket& b = t->ring[t->cur].brackets[static_cast<size_t>(t->open[p])];
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, t->pool.get(), b.query + 1u);
    b.closed = true;
    t->open[p] = -1;
}

void FrameEnd() {
    if (!Active()) return;
    TimerState* t = state(false);
    if (!t) return;
    for (int& o : t->open) o = -1;   // a bracket left open this frame is never read
    Slot& closing = t->ring[t->cur];
    closing.pending = !closing.brackets.empty();
    t->cur = (t->cur + 1) % kRing;
    // Oldest first: the slot about to be reused must be read now or dropped; younger ones wait their turn.
    for (int k = 0; k < kRing; ++k) {
        Slot& s = t->ring[(t->cur + k) % kRing];
        if (!s.pending) continue;
        if (harvest(*t, s)) {
            s.pending = false;
            s.brackets.clear();
        } else if (k == 0) {
            gos_render_pass_timer::SubmitBackendDropped();
            s.pending = false;
            s.brackets.clear();
        } else {
            break;
        }
    }
}

#else   // no Vulkan islands in this build: the GL timer stays the only source

bool Active() { return false; }
void Begin(VkCommandBuffer, gos_render_pass_timer::Pass) {}
void End(VkCommandBuffer, gos_render_pass_timer::Pass) {}
void FrameEnd() {}

#endif

}  // namespace vk_render_pass_timer
