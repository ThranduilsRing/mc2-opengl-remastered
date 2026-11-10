// vk_gpu_profile.h -- one GPU scope per render pass / compute dispatch group on the shared graphics queue.
//
// A mc2vk::gpuprof::Scope brackets the commands recorded on `cb` between its construction and end() (or its destructor) and
// feeds three independent sinks, each off unless its source is present:
//   1. Tracy GPU zone (TracyVkZone semantics) on the engine's single Tracy Vulkan context, one per queue, collected once per
//      frame by frameEnd(). Only recorded while a Tracy profiler is connected; absent from the build when TRACY_ENABLE is off.
//   2. VK_EXT_debug_utils command-buffer label with the same name, for RGP / RenderDoc / Nsight. Only when the instance
//      extension was enabled (vkCmdBeginDebugUtilsLabelEXT is loaded).
//   3. MC2_VK_GPU_TIMESTAMPS=1: a TIMESTAMP pair in a per-frame ring; frameEnd() reads finished frames back (never waiting)
//      and logs "[VK_GPU_PASSES v1]" with the mean ms of EVERY named pass plus the frame's GPU span and the unattributed rest.
// With all three off a Scope is one predictable branch and touches no Vulkan call.
//
// Names are string literals (stored by pointer) and follow the CPU zones: VKIsl.<Island>[.<Part>].
// A Scope may sit inside a render pass: it never resets a query (the frame-end command buffer does, after the host read).
#pragma once

#include <volk.h>

#include <cstdint>
#include <string_view>

namespace mc2vk {
class VulkanContext;

namespace gpuprof {

// Born once the shared device exists (the end of VulkanContext::init). Idempotent.
void init(VulkanContext& ctx);
// Before the device goes: frees the Tracy context and the timestamp pool.
void shutdown(VulkanContext& ctx);
// Main thread, once per frame (just before the swap): Tracy collect, timestamp read-back + reset, the periodic log line.
void frameEnd();
// Present tail (same thread as frameEnd, before its vkEndCommandBuffer, outside a render pass): records this frame's read-back reset and
// Tracy collect into cb so frameEnd() needs no submit of its own. Call frameEndSubmitted() once cb's submit succeeded; frameEnd() still
// runs every frame and re-issues the reset if the submit was refused. False when nothing was recorded.
bool recordFrameEnd(VkCommandBuffer cb);
void frameEndSubmitted();

// True when any sink would record (cheap; lets a caller skip building a name).
bool active();

// Rolling per-pass mean for the ImGui overlay (main thread). emaMs is an exponential average over frames the pass ran; calls is the
// per-frame call count of the last frame it ran. Returns the number of rows written; spanMs is the EMA of the frame's GPU span.
struct PassStat { const char* name; float emaMs; float calls; };
uint32_t snapshot(PassStat* out, uint32_t maxRows, float* spanMs);

class Scope {
public:
    // `name` and `file` must be string literals (their data pointers are kept).
    Scope(VkCommandBuffer cb, std::string_view name, std::string_view file, uint32_t line);
    ~Scope() { end(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    // Closes the scope early (after vkCmdEndRenderPass / the last dispatch); a second call and the destructor do nothing.
    void end();

private:
    VkCommandBuffer cb_ = VK_NULL_HANDLE;
    int32_t         tsIndex_ = -1;     // timestamp pair index inside the frame slot, -1 = not timed
    uint32_t        tsSlot_ = 0;
    bool            label_ = false;
    bool            tracy_ = false;
    alignas(16) unsigned char tracyZone_[48];   // tracy::VkCtxScope, built in place by the .cpp
};

}  // namespace gpuprof
}  // namespace mc2vk

#define MC2_VK_GPU_CAT2(a, b) a##b
#define MC2_VK_GPU_CAT(a, b) MC2_VK_GPU_CAT2(a, b)
// MC2_VK_GPU_SCOPE(gs, cb, "VKIsl.Terrain");  ...record...  gs.end();   (or let the enclosing scope end it)
#define MC2_VK_GPU_SCOPE(var, cb, name) ::mc2vk::gpuprof::Scope var((cb), (name), __FILE__, static_cast<uint32_t>(__LINE__))
