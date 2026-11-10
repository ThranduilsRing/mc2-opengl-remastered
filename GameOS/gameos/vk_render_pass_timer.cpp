// vk_render_pass_timer.cpp -- see the header; the ring rules are in vk_render_pass_timer_ring.h.
#include "vk_render_pass_timer.h"

#include "render_backend_select.h"

#include <memory>
#include <mutex>
#include <vector>

#include "mc2fmt.h"
#include "vk_one_shot.h"
#include "vk_owned.h"
#include "vk_render_pass_timer_ring.h"
#include "vulkan_context.h"

namespace vk_render_pass_timer {

namespace {

using gos_render_pass_timer::Pass_Count;

// Frames in flight the ring covers before a slot is reused, as the GL timer; a slot still unfinished then is dropped.
constexpr int kRing = 4;
// Brackets one frame may record (every island pass, several shadow cascades); more are ignored for that frame and logged.
constexpr uint32_t kBracketsPerSlot = 64;

using TimerRing = Ring<Pass_Count, kRing>;

// The pool lives in the engine context's island slot, so the context releases it ahead of the device.
struct TimerState : mc2vk::IslandState {
    VkDevice device = VK_NULL_HANDLE;
    double   nsPerTick = 0.0;
    uint64_t validMask = ~0ull;
    mc2vk::OwnedQueryPool pool;   // kRing * kBracketsPerSlot * 2 TIMESTAMP queries
    TimerRing ring{kBracketsPerSlot};
    bool     errorLogged = false;
    bool     overflowLogged = false;
};

// The device for which timing was found impossible; a recreated device (a new handle) is asked again. Not an owner.
VkDevice s_unsupportedDevice = VK_NULL_HANDLE;

// Puts every query of the new pool in the reset state with one GPU submit, so a read of a query no submitted command
// buffer has written yet is "not ready" rather than uninitialized.
bool resetAll(const mc2vk::VulkanContext& ctx, VkQueryPool qp, uint32_t count) {
    return mc2vk::submitOneShot(ctx, [&](VkCommandBuffer cb) { vkCmdResetQueryPool(cb, qp, 0, count); }, "pass_timer_reset");
}

TimerState* state(bool create) {
    mc2vk::VulkanContext& ctx = mc2vk::engineVulkanContextNoWait();   // Begin/End also run on a recording worker (see VulkanContext::submitRecorded)
    if (!ctx.valid() || ctx.device() == s_unsupportedDevice) return nullptr;
    std::unique_ptr<mc2vk::IslandState>& slot = ctx.islandSlot(mc2vk::IslandSlot::PassTimer);
    if (slot || !create) return static_cast<TimerState*>(slot.get());

    auto unsupported = [&](const char* why) -> TimerState* {
        s_unsupportedDevice = ctx.device();
        mc2::fmt::print_out("[RENDER_PASS_TIME v1] vk: {}; VK pass times stay empty\n", mc2::fmt::safe(why));
        return nullptr;
    };
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(ctx.physicalDevice(), &props);
    uint32_t qfc = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice(), &qfc, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfc);
    if (qfc) vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice(), &qfc, qfs.data());
    const uint32_t fam = ctx.graphicsFamily();
    if (props.limits.timestampPeriod <= 0.0f || fam >= qfc || qfs[fam].timestampValidBits == 0)
        return unsupported("the graphics queue has no timestamps");
    auto t = std::make_unique<TimerState>();
    t->device = ctx.device();
    t->nsPerTick = props.limits.timestampPeriod;
    const uint32_t bits = qfs[fam].timestampValidBits;
    t->validMask = (bits >= 64) ? ~0ull : ((1ull << bits) - 1ull);
    VkQueryPoolCreateInfo qpci{};
    qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = kRing * kBracketsPerSlot * 2u;
    if (!mc2vk::create(t->pool, t->device, [&](auto* h) { return vkCreateQueryPool(t->device, &qpci, nullptr, h); }))
        return unsupported("vkCreateQueryPool failed");
    if (!resetAll(ctx, t->pool.get(), qpci.queryCount)) return unsupported("the query pool reset submit failed");
    mc2::fmt::print_out("[RENDER_PASS_TIME v1] vk: armed ring={:d} brackets={:d} ns_per_tick={:.3f}\n", mc2::fmt::as_i32(kRing),
                        mc2::fmt::as_u32(kBracketsPerSlot), t->nsPerTick);
    slot = std::move(t);
    return static_cast<TimerState*>(slot.get());
}

}  // namespace

// Recorded islands bracket their passes from worker threads: the ring is one shared structure.
std::mutex s_ringMutex;

bool Active() {
    return gos_render_pass_timer::Enabled();
}

void Arm() {
    // The query pool is born at the top of the frame on the render thread, outside every lock: its reset submit (submitOneShot) takes submitMutex_, which
    // a recording resolve holds. Begin never creates it (a first Begin under that lock self-deadlocked); a timer switched on mid-frame starts next frame.
    if (Active()) (void)state(true);
}

void Begin(VkCommandBuffer cb, gos_render_pass_timer::Pass p) {
    if (cb == VK_NULL_HANDLE || !Active()) return;
    TimerState* t = state(false);
    if (!t) return;
    int64_t q;
    { std::lock_guard<std::mutex> lock(s_ringMutex); q = t->ring.begin(p); }
    if (q < 0) return;
    // The pair is reset on the same command buffer, so a reused slot never carries an older frame's stamps into this read.
    vkCmdResetQueryPool(cb, t->pool.get(), static_cast<uint32_t>(q), 2);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, t->pool.get(), static_cast<uint32_t>(q));
}

void End(VkCommandBuffer cb, gos_render_pass_timer::Pass p) {
    if (cb == VK_NULL_HANDLE || !Active()) return;
    TimerState* t = state(false);
    if (!t) return;
    int64_t q;
    { std::lock_guard<std::mutex> lock(s_ringMutex); q = t->ring.end(p); }
    if (q >= 0) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, t->pool.get(), static_cast<uint32_t>(q));
}

void Cancel(gos_render_pass_timer::Pass p) {
    if (!Active()) return;
    if (TimerState* t = state(false)) { std::lock_guard<std::mutex> lock(s_ringMutex); t->ring.cancel(p); }
}

void FrameEnd() {
    if (!Active()) return;
    mc2vk::getOrCreateEngineVulkanContext().resolveRecorded();   // reserved recordings bracket into this frame's slot before it closes
    TimerState* t = state(false);
    if (!t) return;
    std::lock_guard<std::mutex> lock(s_ringMutex);
    VkResult hardError = VK_SUCCESS;
    auto read = [&](const TimerRing::Bracket& b, double* ms) -> ReadResult {
        uint64_t r[4] = {};   // (begin, available, end, available)
        const VkResult res = vkGetQueryPoolResults(t->device, t->pool.get(), b.query, 2, sizeof(r), r, 2 * sizeof(uint64_t),
                                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (res != VK_SUCCESS && res != VK_NOT_READY) { hardError = res; return ReadResult::Error; }
        if (r[1] == 0 || r[3] == 0) return ReadResult::NotReady;
        *ms = TicksToMs(r[0], r[2], t->validMask, t->nsPerTick);
        return ReadResult::Ready;
    };
    auto publish = [](const double* ms, const bool* has) { gos_render_pass_timer::SubmitBackendFrame(ms, has); };
    const auto c = t->ring.frameEnd(read, publish);
    for (unsigned i = 0; i < c.dropped; ++i) gos_render_pass_timer::SubmitBackendDropped();
    if (c.errors && !t->errorLogged) {
        t->errorLogged = true;
        mc2::fmt::print_err("[RENDER_PASS_TIME v1] vk: vkGetQueryPoolResults failed ({:d}); those frames are discarded\n",
                            mc2::fmt::as_i32(static_cast<int>(hardError)));
    }
    if (t->ring.takeOverflow() && !t->overflowLogged) {
        t->overflowLogged = true;
        mc2::fmt::print_err("[RENDER_PASS_TIME v1] vk: a frame recorded more than {:d} brackets; the extra ones were not timed\n",
                            mc2::fmt::as_u32(kBracketsPerSlot));
    }
}


}  // namespace vk_render_pass_timer
