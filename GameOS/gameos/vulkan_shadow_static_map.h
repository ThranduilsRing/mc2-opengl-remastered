// vulkan_shadow_static_map.h -- the world-fixed static sun shadow map of the VK shadow island.
//
// WHAT: one D32 depth map over the whole map, holding the terrain and every registry building, rendered with the engine's static light matrix. Built once
// per map and again whenever the engine rebakes its static map (a building destroyed), never per frame. Terrain samples it at its static-shadow binding and
// the screen shadow samples it for object pixels, the two places GL samples its own static map.
// WHY: GL renders this map itself and the Vulkan terrain island used to read it back into a Vulkan image (a 64 MB depth readback per rebuild). Building
// it here keeps the buildings out of the cascades (GL keeps them out of its dynamic pass, so the cascades would put tent and hangar shadows on the apron
// where GL has none) and lets Vulkan run without the GL map.
//
// The island lends its depth-only render pass and its static-prop geometry; this object owns the image, its pipelines and the terrain mesh. The terrain
// comes from the CPU terrain field store, the buildings from the static-prop registry, the matrix from the engine's static shadow spec (all CPU data).
#ifndef MC2_VULKAN_SHADOW_STATIC_MAP_H
#define MC2_VULKAN_SHADOW_STATIC_MAP_H

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>

#include "vk_owned.h"
#include "vulkan_shadow_island_live.h"   // Mc2ShadowPacket
#include "shadow_static_building_runs.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace mc2vk {

// The island's static-prop geometry as the building draws need it: its VK copies of the shared VBO / IBO and the packets of each type.
struct StaticPropGeometryView {
    VkBuffer vbo = VK_NULL_HANDLE;
    VkBuffer ibo = VK_NULL_HANDLE;
    std::span<const Mc2ShadowPacket> packets;
    std::span<const uint32_t> typeFirstPacket;   // indexed by typeID
    std::span<const uint32_t> typePacketCount;
};

class ShadowStaticMap {
public:
    ShadowStaticMap() = default;
    ShadowStaticMap(const ShadowStaticMap&) = delete;
    ShadowStaticMap& operator=(const ShadowStaticMap&) = delete;

    // Builds the pipelines for `pass` (the island's depth-only pass: D32, cleared, left in DEPTH_STENCIL_READ_ONLY). False, with the reason logged, when a
    // shader module or an object cannot be made; the island then runs without a static map.
    bool init(VkDevice device, VmaAllocator allocator, VkRenderPass pass, std::string_view spvDir);

    // True when the engine has a static map generation this object has not built yet.
    bool stale() const;

    // Records the build of the engine's current generation into `cb` (outside any render pass). The caller submits `cb` and then calls markSubmitted().
    // False (logged) when an input is missing or malformed; the previous build, if any, stays the one consumers see.
    bool record(VkCommandBuffer cb, const StaticPropGeometryView& geometry);
    void markSubmitted() { if (pendingGeneration_ != 0) { builtGeneration_ = pendingGeneration_; pendingGeneration_ = 0; } }

    // The map of the last submitted build; null before the first one.
    VkImageView view() const { return builtGeneration_ != 0 ? view_.get() : VK_NULL_HANDLE; }
    uint64_t generation() const { return builtGeneration_; }
    // For the matrix diag's depth dump.
    VkImage image() const { return image_.get(); }
    int size() const { return size_; }

    // A map boundary: the next map's generation builds afresh, and nothing is handed out until it has.
    void invalidate() { builtGeneration_ = 0; pendingGeneration_ = 0; failedGeneration_ = 0; }

private:
    bool ensureTarget(int size, VkRenderPass pass);
    bool buildTerrainMesh();
    bool stageBuildings(const StaticPropGeometryView& geometry);
    bool ensureBuffer(OwnedBuffer& buffer, VkDeviceSize& capacity, VkDeviceSize bytes, VkBufferUsageFlags usage, std::string_view name);

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    bool inited_ = false;
    int size_ = 0;
    uint64_t builtGeneration_ = 0;
    uint64_t pendingGeneration_ = 0;
    uint64_t failedGeneration_ = 0;
    uint32_t terrainIndexCount_ = 0;
    uint64_t terrainHeightVersion_ = 0;

    std::vector<ShadowBuildingRun> buildingRuns_;   // runs of one type in the sorted instance buffer

    // Declared in creation order: released in reverse, so the framebuffer and view go before the image, the pipelines before their layout.
    OwnedImage image_;
    OwnedImageView view_;   // depth aspect: the attachment of the build and the view the consumers sample
    OwnedFramebuffer fb_;
    OwnedBuffer ubo_, terrainVbo_, terrainIbo_, instBuf_;
    VkDeviceSize terrainVboCap_ = 0, terrainIboCap_ = 0, instCap_ = 0;
    OwnedDescriptorSetLayout dsl_;
    OwnedDescriptorPool pool_;
    VkDescriptorSet set_ = VK_NULL_HANDLE;   // allocated from pool_, freed with it
    OwnedPipelineLayout layout_;
    OwnedShaderModule terrainVert_, propVert_, frag_;
    OwnedPipeline terrainPipe_, buildingPipe_;
};

}  // namespace mc2vk

#endif  // MC2_VULKAN_ISLAND
#endif  // MC2_VULKAN_SHADOW_STATIC_MAP_H
