// vulkan_shadow_bake.h -- the static sun shadow bake of the VK shadow island.
//
// WHAT: two products, both built once per map from CPU-complete inputs (the gameplay caster set, the terrain heights, the clamped sun, the water level):
//   - the visibility MASK: R8 with mips over the terrain grid in world XY, the soft static shadow (props by deterministic PCSS, terrain by a cone march,
//     long prop shadows faded by their blurred blocker distance) evaluated at the visible surface max(terrain, water). Receivers sample it bilinearly.
//   - the OBJECT map: a D16 sun-space depth map of the terrain and every static caster with the engine's static light matrix, for receivers that are not
//     the ground (the screen shadow resolve).
// The mask is baked in tiles of 512^2 texels; each tile rasterises the casters that can reach it into a transient light-space window and filters there.
// Each stage is timed on the GPU and logged as [SHADOW_BAKE]; the transients are released once the bake's submit has retired.
// WHY: trees rode the camera-fit cascades (blocky, sharp, stair-stepped long shadows); a bake pre-filters them once at a resolution the cascades never
// had, and the receivers pay one bilinear tap.
#ifndef MC2_VULKAN_SHADOW_BAKE_H
#define MC2_VULKAN_SHADOW_BAKE_H


#include <volk.h>

#include "shadow_bake_math.h"
#include "shadow_dirty_tiles.h"
#include "shadow_mask_asset.h"
#include "vk_compute_pipeline.h"
#include "vk_owned.h"
#include "vk_frame_ring.h"
#include "vulkan_shadow_island_live.h"   // Mc2ShadowPacket
#include "vulkan_shadow_draw_ring.h"

#include <array>
#include <cstdint>
#include <chrono>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct GpuStaticPropInstance;

namespace mc2vk {

// Per packet of the shared static-prop geometry: the alpha-ON array layer its colour pass samples and the UV sub-region (layer < 0: opaque).
struct CasterPacketAlpha {
    int32_t layer = -1;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
};

// What the island lends a bake: its copy of the static-prop geometry, the per-packet alpha table and the alpha-ON array's descriptor set.
struct ShadowBakeInputs {
    VkBuffer vbo = VK_NULL_HANDLE;
    VkBuffer ibo = VK_NULL_HANDLE;
    std::span<const Mc2ShadowPacket> packets;
    std::span<const uint32_t> typeFirstPacket;   // indexed by typeID
    std::span<const uint32_t> typePacketCount;
    std::span<const CasterPacketAlpha> packetAlpha;
    VkDescriptorSet alphaSet = VK_NULL_HANDLE;
    uint64_t alphaContentHash = 0;
    bool alphaReady = false;    // the alpha-ON array is bound and the table built
    bool alphaAbsent = false;   // the map has no alpha-ON group, so there is nothing to wait for
};

// LOCKSTEP with shaders/vulkan/include/shadow_bake_common.glsl BakeTile (std140).
struct ShadowBakeTileParams {
    float window[16];
    float region[4];
    int32_t regionInfo[4];
    float sun[4];
    float windowInfo[4];
    float heightInfo[4];
    float fade[4];
    float march[4];
};

class ShadowBake {
public:
    ShadowBake() = default;
    ShadowBake(const ShadowBake&) = delete;
    ShadowBake& operator=(const ShadowBake&) = delete;

    // `alphaDsl` is the island's set-1 layout (the alpha-ON array). False, logged, when a shader or object cannot be made; the island then runs without.
    bool init(VkDevice device, VmaAllocator allocator, VkPhysicalDevice phys, VkDescriptorSetLayout alphaDsl, std::string_view spvDir);
    bool prewarmProducts();
    // Load barrier: the static-prop type boxes (a CPU walk of the prop geometry) that the first bake would otherwise compute inside a gameplay frame.
    bool prewarmTypeBounds(const ShadowBakeInputs& in);

    // Called once per frame after the island opened the frame's recording. Retires a submitted bake (timings, transients) and records a new one into `cb`
    // when the inputs are ready and differ from the last bake's. True when it recorded (the caller counts a pass and submits).
    bool update(VkCommandBuffer cb, uint64_t epoch, const ShadowBakeInputs& in);
    void markSubmitted(uint64_t epoch);

    // A frame must be opened for the bake even when no caster pass runs: a bake is due, or a submitted one waits to retire.
    bool wantsFrame() const;
    bool hasProducts() const { return static_cast<bool>(maskImage_) || static_cast<bool>(objectImage_) || static_cast<bool>(windowImage_); }

    // The products of the last submitted bake; null / 0 before the first.
    bool ready() const { return maskReady_; }
    // A bake is recorded and not yet retired, or the cache read is still running (the load-time drain keeps stepping while either holds).
    bool inFlight() const { return recorded_ || (cacheLoad_.valid() && cacheLoad_.wait_for(std::chrono::seconds(0)) != std::future_status::ready); }
    VkImageView maskView() const { return maskReady_ ? maskView_.get() : VK_NULL_HANDLE; }
    VkImageView objectView() const { return objectReady_ ? objectView_.get() : VK_NULL_HANDLE; }
    const float* objectMatrix() const { return objectMatrix_.data(); }
    uint64_t objectGeneration() const { return objectReady_ ? objectGeneration_ : 0; }
    void countLocalTileRebakes(uint32_t count) { localTileRebakes_ += count; }
    const mc2shadow::MaskLayout& layout() const { return layout_; }

    // A map boundary (the caller has idled the device): every product and transient goes; the next map bakes afresh.
    void invalidate();

private:
    struct TileWork {
        mc2shadow::Window window;
        float casterTop = 0.0f;
        uint32_t firstRun = 0, runCount = 0;
    };
    struct InstRun { uint32_t typeID, firstInstance, count; };
    struct CasterView {
        const std::vector<GpuStaticPropInstance>* leaves;
        const std::vector<mc2shadow::LightBox>* boxes;
        const std::vector<float>* tops;
        const std::vector<uint32_t>* live;   // index into leaves of each box
    };
    struct RegionJob { uint32_t tile; int regionTexels; int originX, originY; };

    struct BakeKeys { uint64_t mask = 0, object = 0; };
    std::optional<BakeKeys> inputsReady(const ShadowBakeInputs& in);
    bool ensureProducts(int maskTexels);
    bool ensureTransients();
    bool computeTypeBounds(const ShadowBakeInputs& in);
    bool recordColdTile(VkCommandBuffer cb, const ShadowBakeInputs& in);
    bool readHeightGrid();
    uint32_t coldNextTile_ = 0;
    bool coldPending_ = false;
    bool recordBake(VkCommandBuffer cb, const ShadowBakeInputs& in);
    bool recordObjectMap(VkCommandBuffer cb, const ShadowBakeInputs& in, uint32_t queryBase);
    void drawRuns(VkCommandBuffer cb, const ShadowBakeInputs& in, std::span<const InstRun> runs, VkPipeline opaque, VkPipeline alpha);
    ShadowDrawRing drawRing_;   // the static-prop caster draws of the frame's regions as indirect records
    void retire(uint64_t epoch);
    uint64_t contentKey(const ShadowBakeInputs& in, uint64_t casterHash);
    bool ensureEncoder();
    void encodeLevel(VkCommandBuffer cb, VkDescriptorSet set, uint32_t slice, uint32_t side);
    void recordTileEncode(VkCommandBuffer cb, int originX, int originY, uint32_t tile, uint32_t sliceBase);
    void planRegion(mc2shadow::Rect r, int originX, int originY, int regionTexels, float casterTop, float tanT, const CasterView& cv,
                    std::vector<InstRun>& runs, std::vector<GpuStaticPropInstance>& inst, TileWork& tw, ShadowBakeTileParams& p) const;
    void recordRegion(VkCommandBuffer cb, const ShadowBakeInputs& in, const RegionJob& job, const TileWork& tw, std::span<const InstRun> runs,
                      VkDescriptorSet computeSet, VkDescriptorSet windowSet, uint32_t paramOff, uint32_t casterOff, uint32_t passBase, uint32_t sliceBase,
                      int queryBase);
    std::optional<mc2shadow::Rect> leavesFootprint(const std::vector<GpuStaticPropInstance>& leaves) const;
    mc2shadow::Rect grownForDirty(mc2shadow::Rect r) const;
    bool updateIncremental(VkCommandBuffer cb, uint64_t epoch, const ShadowBakeInputs& in);
    bool ensureIncremental();
    void writeIncrementalSets(uint32_t par);
    void retireIncremental(uint64_t epoch);
    void releaseIncremental();
    void rebuildLive();
    void appendLive(const GpuStaticPropInstance& leaf);
    void applyLiveEvent(const std::vector<GpuStaticPropInstance>& before, const std::vector<GpuStaticPropInstance>& after);
    static bool diagOn();
    bool recordDiagReadback(VkCommandBuffer cb, uint64_t epoch);
    void compareChains(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) const;
    void recordTopLevels(VkCommandBuffer cb);
    void recordTopLevelsMipsOnly(VkCommandBuffer cb);
    void recordMaskToRead(VkCommandBuffer cb, bool readback);
    bool recordWarmUpload(VkCommandBuffer cb, const mc2shadow::ShadowMaskAsset& asset);
    void storeReadback();
    std::vector<VkBufferImageCopy> chainRegions() const;
    void releaseTransients();

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout alphaDsl_ = VK_NULL_HANDLE;
    bool inited_ = false;
    bool timed_ = false;
    double timestampPeriodNs_ = 1.0;

    // The bake state: a recorded bake is pending until its submit retires (its descriptors and transients are not touched before that).
    uint64_t bakedKey_ = 0, objectKey_ = 0, recordedKey_ = 0, recordedObjectKey_ = 0;
    bool recorded_ = false, submitted_ = false, recordedMask_ = false;
    uint64_t submittedEpoch_ = 0, recordedEpoch_ = 0;
    bool maskReady_ = false, objectReady_ = false;
    uint64_t objectGeneration_ = 0, recordedObjectGeneration_ = 0;
    std::array<float,16> objectMatrix_{}, recordedObjectMatrix_{};
    uint32_t waitFrames_ = 0;
    bool loggedWait_ = false, loggedHeightDrift_ = false;
    uint64_t bakedHeightVersion_ = 0, bakedSetVersion_ = 0;

    // This bake's inputs (CPU).
    mc2shadow::MaskLayout layout_;
    mc2shadow::LightBasis basis_;
    mc2shadow::Vec3 sun_;
    float waterZ_ = 0.0f, terrainTop_ = 0.0f, terrainMin_ = 0.0f, halfMap_ = 0.0f, fadeMax_ = 0.0f, casterTop_ = 0.0f;
    int mapSide_ = 0, heightSide_ = 0;
    float heightSpacing_ = 0.0f;
    std::vector<float> heights_;
    uint64_t readHeightVersion_ = ~0ull;
    std::vector<GpuStaticPropInstance> leaves_;
    struct TypeBounds { float mn[3], mx[3]; bool valid; };
    std::vector<TypeBounds> typeBounds_;
    std::vector<TileWork> tiles_;
    std::vector<InstRun> runs_;
    std::vector<InstRun> objectRuns_;
    uint32_t binnedInstances_ = 0;
    double cpuMs_ = 0.0;
    VkDeviceSize transientBytes_ = 0;

    // Declared in creation order (released in reverse).
    OwnedDescriptorSetLayout casterDsl_;
    OwnedPipelineLayout casterLayout_;
    OwnedShaderModule propVert_, terrainVert_, opaqueFrag_, alphaFrag_;
    OwnedPipeline windowOpaque_, windowAlpha_, objectOpaque_, objectAlpha_, objectTerrain_;
    OwnedDescriptorPool casterPool_;
    VkDescriptorSet windowCasterSet_ = VK_NULL_HANDLE, objectCasterSet_ = VK_NULL_HANDLE;   // freed with casterPool_
    ComputePipeline pcss_, march_, resolve_, bc4_;
    OwnedDescriptorPool bc4Pool_;
    VkDescriptorSet bc4TileSet_ = VK_NULL_HANDLE, bc4TopSet_ = VK_NULL_HANDLE;   // freed with bc4Pool_
    std::string cacheDir_;   // <user data>/cache/shadow; empty = no disk cache
    struct CacheLoad {
        std::optional<mc2shadow::ShadowMaskAsset> asset;
        double readMs = 0.0;
    };
    // Only owned bytes cross the worker boundary; no renderer or registry state is read there.
    std::future<CacheLoad> cacheLoad_;
    uint64_t cacheLoadKey_ = 0, cacheLoadInputKey_ = 0;   // inputKey: the mask key the content key was computed for
    uint64_t contentKey_ = 0, shaderContentHash_ = 0;
    uint64_t geomHash_ = 0; bool geomHashValid_ = false;   // mission-static prop geometry hash (key part 2), filled at prewarm
    std::optional<uint64_t> geometryHash(const ShadowBakeInputs& in);
    std::array<uint64_t, 6> keyParts_{};   // heights, casters, geometry, alpha cut-outs, scalars, versions
    bool warm_ = false, readbackPending_ = false;
    double warmReadMs_ = 0.0;
    VkDeviceSize tileLevelOff_[8] = {};

    // The incremental path (P5): dirty sub-tiles, the live casters they bin, and per-parity buffers and sets.
    mc2shadow::DirtyQueue dirty_;
    std::vector<GpuStaticPropInstance> liveLeaves_;
    std::vector<mc2shadow::LightBox> liveBoxes_;
    std::vector<float> liveTops_;
    std::vector<uint32_t> liveIdx_;
    bool liveStale_ = true;
    uint32_t liveRebuilds_ = 0;
    double cpuStage_[3] = {0, 0, 0};   // cold bake CPU ms: resolve, content key, tile planning
    bool incActive_ = false, heightsMoved_ = false, eventsSinceDrain_ = false, objectDirty_ = false;
    uint64_t objectMapRecords_ = 0, dirtyBatches_ = 0, localTileRebakes_ = 0, statsEpoch_ = 0;
    uint64_t lastIncEpoch_ = 0, incRecordedEpoch_ = ~0ull, heightsGen_ = 1, incHeightsGen_[mc2vk::kMaxFramesInFlight] = {0, 0};
    uint32_t incSubTiles_ = 0, incBatches_ = 0, eventsLogged_ = 0;
    double incMaxGpuMs_ = 0.0, incSumGpuMs_ = 0.0;
    uint32_t incTimed_ = 0;
    uint32_t incInstanceCap_ = 0;
    bool incQueryUsed_[mc2vk::kMaxFramesInFlight] = {false, false};
    OwnedBuffer incParams_, incCasterUbo_, incPass_, incInstances_, incHeights_[mc2vk::kMaxFramesInFlight];
    OwnedQueryPool incQueries_;
    VkDescriptorSet incComputeSet_[mc2vk::kMaxFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE}, incWindowSet_[mc2vk::kMaxFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    // MC2_SHADOW_DIAG: the dirty-updated mask read back and compared with a fresh bake of the same state.
    bool diagRequest_ = false, diagPending_ = false, diagForceFull_ = false, diagSwept_ = false;
    uint64_t diagEpoch_ = 0;
    OwnedBuffer diagBuf_;
    std::vector<uint8_t> diagInc_;
    std::vector<VkDeviceSize> topLevelOff_;
    OwnedDescriptorPool computePool_;
    VkDescriptorSet computeSet_ = VK_NULL_HANDLE;   // allocated from computePool_ with pcss_'s layout (the three layouts are identical)
    OwnedSampler depthRawSampler_, depthCompareSampler_;
    OwnedQueryPool queries_;
    uint32_t queryCount_ = 0;

    // Products.
    OwnedImage maskImage_;
    OwnedImageView maskView_;
    int maskTexels_ = 0;
    uint32_t maskMips_ = 0;
    bool maskInitialized_ = false;
    OwnedImage topR8_;   // mask level 7 (4 texels a tile) and its mips, R8: the source of the levels below the tile grid
    OwnedImageView topR8View_;
    uint32_t topSide_ = 0, topMips_ = 0;
    bool topInitialized_ = false;
    OwnedImage objectImage_;
    OwnedImageView objectView_;
    bool objectInitialized_ = false;
    OwnedBuffer terrainVbo_, terrainIbo_;
    uint32_t terrainIndexCount_ = 0;
    uint64_t objectHeightVersion_ = ~0ull;
    OwnedBuffer objectInstances_, objectUbo_;

    // Transients (one bake).
    OwnedImage windowImage_;
    OwnedImageView windowView_;
    OwnedImage scratch_[4];   // props, terrain, blur, combined
    OwnedImageView scratchView_[4];
    OwnedBuffer staging_, tileParams_, passParams_, casterUbo_, instances_, heightBuf_;
    OwnedImage tileR8_;   // one tile's levels 512..4, R8, before BC4
    OwnedImageView tileR8View_;
    bool tileGeneral_ = false;   // tileR8_ moved to GENERAL once; copies, blits and sampling then share that layout (no per-level image barriers)
    OwnedBuffer blockStaging_, bc4Params_, transfer_;   // encoder output, per-level parameters, the read-back / upload of the whole chain
    // Last member: destruction joins the fenced readback worker before Vulkan
    // resources are released. invalidate() also joins at the map boundary.
    std::future<void> cacheStore_;
};

}  // namespace mc2vk

#endif  // MC2_VULKAN_SHADOW_BAKE_H
