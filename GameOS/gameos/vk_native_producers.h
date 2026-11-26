// vk_native_producers.h -- native Vulkan compute producers (step 3.4P of the GL deletion plan).
//
// WHAT: the terrain-lighting compute and the water cull/pack + draw-args computes, as Slang compute pipelines (shaders/vulkan/*.comp.slang) recorded
// into the water island's command buffer on the single graphics queue. Their outputs (per-vertex lighting, packed thin records, the indirect draw
// arguments) stay in VK device memory and the water draw consumes them directly: no GL compute, no glGetBufferSubData readback, no re-upload.
// WHY: the VK water path used to read the GL-packed records back (count, then count x 48 B, a blocking sync every frame) and re-upload them. The GL
// producers are still the CPU consumers' source, so they keep running for the lighting; the water cull/pack GL dispatch is skipped while this is
// active (except on the parity frames, where both run and the outputs are compared).
// SYNC: one command buffer, barriers between the stages (vk_compute_pipeline.h cmdBarrier); the parity slot fence retires the per-slot buffers.
#ifndef MC2_VK_NATIVE_PRODUCERS_H
#define MC2_VK_NATIVE_PRODUCERS_H


#include <volk.h>

#include "gos_terrain_lighting.h"
#include "vk_compute_pipeline.h"
#include "vk_owned.h"
#include "vk_frame_ring.h"
#include "../../mclib/terrain_vertex_dirty.h"

#include <cstdint>

namespace mc2vk {

class VulkanContext;

// std140 mirror of LightingParams in shaders/vulkan/terrain_lighting.comp.slang.
struct VkTerrainLightingParams {
    int32_t counts[4];      // numVertices, numLights, vertexOffset, windowCount
    int32_t baseRgb[4];     // BaseVertexColor R, G, B, useFog
    float   weather[4];     // rainLightLevel, rendererSW, reserved, reserved
    float   fogHaze[4];     // fogStart, fogFull, minHazeDistance, maxClipDistance
    float   playArea[4];
    float   camDist[4];     // camera world position xyz, distanceFactor
};
static_assert(sizeof(VkTerrainLightingParams) == 96, "LightingParams std140 size (terrain_lighting.comp.slang)");

// std140 mirror of WaterCullParams in shaders/vulkan/water_cull_pack.comp.slang.
struct VkWaterCullParams {
    float   clip[16];       // the four columns of u_worldToClipGL: the raw 16 floats the GL path uploads with GL_FALSE
    float   misc0[4];       // waterElevation, frameCos, shoreExt, unused
    int32_t misc1[4];       // recipeCount, maxThinRecords, mapSide, biasLegacy
};
static_assert(sizeof(VkWaterCullParams) == 96, "WaterCullParams std140 size (water_cull_pack.comp.slang)");

// Mirror of DrawArgsParams in shaders/vulkan/water_draw_args.comp.slang.
struct VkDrawArgsParams { int32_t v[4]; };
static_assert(sizeof(VkDrawArgsParams) == 16, "DrawArgsParams std140 size (water_draw_args.comp.slang)");

// The per-frame water scalars the GL cull dispatch used (captured by WaterStream at the same point, so both read the same values).
struct WaterCullFrame {
    float    mvp[16];
    float    frameCos;
    float    waterElevation;
    float    shoreExt;
    uint32_t mapSide;
    int32_t  biasLegacy;
    int32_t  waterVertices = 6;
};

// Result of one GL-vs-VK comparison (parity frames only).
struct ProducerParity {
    uint32_t glCount = 0, vkVisible = 0, vkCompared = 0;   // thin records: GL count, VK header count, VK records read back
    uint32_t onlyGl = 0, onlyVk = 0, same = 0, differ = 0; // matched by recipe index
    uint32_t lightCompared = 0, lightMismatch = 0, fogMismatch = 0, lightMaxDelta = 0;
    bool     truncated = false;
    uint32_t firstDiffGl[12] = {}, firstDiffVk[12] = {};   // the first differing pair (diagnosis)
    bool     haveDiff = false;
};

class NativeProducers {
public:
    static constexpr uint32_t kSlots = kMaxFramesInFlight;
    static constexpr uint32_t kMaxThin = 131072;     // thin-record capacity per slot (6.3 MB); an overflow shows in the parity line
    static constexpr uint32_t kParityCap = 32768;    // records read back on a parity frame
    // VRAM ceiling for the thin-record capacity: 1 << 20 records x 48 B = 48 MB per slot (96 MB for both). It covers every quad of a
    // 1024-vertex map (1023^2 = 1,046,529), so a fully flooded engine-cap map is never truncated.
    static constexpr uint32_t kMaxThinCeiling = 1u << 20;

    // Builds the pipelines and the per-slot buffers. `recipes` is the island's device-local recipe buffer (64 B x recipeCount, mission-static).
    // `waterEligibleBound` is an upper bound on the quads the cull can ever emit (the recipes the shader's water-eligibility predicate admits,
    // with headroom); the thin capacity is sized from it: max(kMaxThin, bound), at most recipeCount and kMaxThinCeiling. 0 = unknown (kMaxThin).
    // False (logged) on any failure; nothing is half-built then.
    bool init(const VulkanContext& ctx, uint32_t numVertices, uint32_t recipeCount, VkBuffer recipes, uint32_t waterEligibleBound = 0);
    bool prewarmPipelines(const VulkanContext& ctx);
    bool ready() const noexcept { return ready_; }

    VkBuffer thin(uint32_t par) const noexcept { return thin_[par % kSlots].get(); }
    VkBuffer drawArgs(uint32_t par) const noexcept { return drawArgs_[par % kSlots].get(); }
    uint32_t maxThin() const noexcept { return maxThin_; }
    // Visible quads from the last completed slot.
    uint32_t lastVisible() const noexcept { return lastVisible_; }
    uint32_t eligibleBound() const noexcept { return eligibleBound_; }

    // Stages this frame's inputs into slot `par` (the slot must be retired) and records lighting -> cull/pack -> draw-args with the barriers, ending
    // with the hand-off to the vertex stage and the indirect draw. With `parity`, also copies the outputs to the readback buffers. False = nothing recorded.
    bool record(VkCommandBuffer cb, uint32_t par, const gos_terrain_lighting::VkProducerSnapshot& light, const WaterCullFrame& water, bool parity);

    // After a parity frame's work has finished on the GPU: compares against the GL producers' output. `glThin` is the GL-packed records (48 B each).
    ProducerParity compare(uint32_t par, const void* glThin, uint32_t glThinCount, const gos_terrain_lighting::GpuTerrainLightingOutput* glLight,
                           uint32_t lightFirst, uint32_t lightCount);

private:
    // One-shot startup check of the lighting compute: a synthetic vertex/light set (4096 vertices, 8 lights, every branch) is run on the GPU and compared with a
    // C++ transcription of the quad.cpp math. Logs [VK_PRODUCER_SELFTEST v1]. Returns the mismatch count (0 = exact).
    uint32_t selfTestLighting();
    bool makeDevice(OwnedBuffer& buf, VkDeviceSize size, VkBufferUsageFlags usage, const char* name, int i0 = -1);
    void writeAll();
    // Points slot `par`'s frame-ring bindings (params) at `buffer`.
    void syncInputs(uint32_t par, VkBuffer buffer, VkDeviceSize range);

    bool ready_ = false;
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    const VulkanContext* ctx_ = nullptr;
    uint32_t numVertices_ = 0, recipeCount_ = 0, maxThin_ = 0;
    uint32_t eligibleBound_ = 0;
    uint32_t lastVisible_ = 0, healthFrames_ = 0;
    bool healthPending_[kSlots] = {};   // slot `par`'s healthHdr_ holds a completed frame's header (read when the slot is recorded again, after its host wait)
    VkBuffer recipes_ = VK_NULL_HANDLE;
    bool lightOutCleared_ = false;

    // Owners are released in reverse order of declaration; the buffers outlive nothing that uses them (the island idles the device before teardown).
    ComputePipeline lightP_, cullP_, argsP_;
    ComputeSets lightSets_, cullSets_, argsSets_;
    OwnedBuffer lightOut_;                       // numVertices x 8 B, device-local, written by the lighting compute, read by the cull
    OwnedBuffer lightIn_[kSlots];                // resident absolute vertex inputs; written only after this slot retires
    mc2::terrain::VertexUploadRanges vertexUploads_[kSlots];
    // Lights and the two parameter blocks are frame-ring spans bound with dynamic offsets; ringBound_[par] = the
    // ring buffer slot `par`'s bindings currently name (the slot's set is rewritten only after the island's slot fence retired its last use).
    VkBuffer ringBound_[kSlots] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    OwnedBuffer argsUbo_[kSlots];   // map-static thin capacity and vertices per record
    OwnedBuffer header_[kSlots], drawArgs_[kSlots], thin_[kSlots];   // device-local
    OwnedBuffer healthHdr_[kSlots];   // host-visible copy of header_[par] (visible count) for the [WATER] health line
    // Parity readback (host-visible), made on first use.
    OwnedBuffer parityHdr_, parityThin_, parityLight_;
    uint32_t parityLightFirst_ = 0, parityLightCount_ = 0;
};

}  // namespace mc2vk

#endif  // MC2_VK_NATIVE_PRODUCERS_H
