#pragma once
// vk_render_pass_timer -- the Vulkan source of the per-pass GPU times that gos_render_pass_timer publishes.
//
// WHAT: under the VK backend each island brackets its own command buffer with Begin/End for the coarse pass it draws; the
// timer writes a TIMESTAMP pair per bracket into a query pool slot for the frame, reads each slot back at FrameEnd once the
// GPU has finished it (never waiting), sums the pairs per pass and hands the per-pass ms to gos_render_pass_timer, whose
// LastMs / HasSample / [RENDER_PASS_TIME v1] line / dev-shell framegraph then read VK values unchanged.
// WHY: the GL GL_TIME_ELAPSED scopes wrap GL calls the VK islands replaced, so under VK every pass read ~0 ms; GL deletion
// step 2 needs a VK source for the editor Frame Inspector and the dev-shell framegraph.
// Zero cost when gos_render_pass_timer::Enabled() is false: no pool is created and every entry returns at once.

#include "gos_render_pass_timer.h"

// The Vulkan handle type, declared as vulkan_core.h declares it, so GL-side includers need no Vulkan headers.
typedef struct VkCommandBuffer_T* VkCommandBuffer;

#include <cstdint>

namespace vk_render_pass_timer {

// Milliseconds between two TIMESTAMP values of a counter that keeps only the bits in `validMask`, so an end stamp
// that wrapped past the counter's top still yields the forward distance.
inline double TicksToMs(uint64_t begin, uint64_t end, uint64_t validMask, double nsPerTick) {
    const uint64_t ticks = ((end & validMask) - (begin & validMask)) & validMask;
    return static_cast<double>(ticks) * nsPerTick / 1.0e6;
}

// True when the VK backend drew this session and the pass timer collects: the GL timer then skips its own queries.
bool Active();

// Bracket the work an island records on `cb` for pass `p`; Begin goes outside a render pass. A pass bracketed more than
// once in a frame reports the sum of its brackets. No-ops in a build without Vulkan islands.
void Begin(VkCommandBuffer cb, gos_render_pass_timer::Pass p);
void End(VkCommandBuffer cb, gos_render_pass_timer::Pass p);
// The island decided not to submit the command buffer that holds the pass's latest bracket: that bracket's queries keep
// an older frame's results, so it must never be read (it would publish a stale time as current).
void Cancel(gos_render_pass_timer::Pass p);

// Once per frame from gos_render_pass_timer::FrameEnd: closes the frame's slot, harvests every finished older slot into
// gos_render_pass_timer::SubmitBackendFrame, and counts a slot still unfinished when it comes round again as dropped.
void FrameEnd();

}  // namespace vk_render_pass_timer
