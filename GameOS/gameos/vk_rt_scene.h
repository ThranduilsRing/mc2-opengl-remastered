// vk_rt_scene.h -- the persistent ray-visible scene: one TLAS over every STATIC object of the loaded map (displaced terrain + registry static props).
//
// WHAT: a registry of scene records keyed by stable object IDs, one BLAS per static-prop type (shared by every instance of that type) plus one terrain
// BLAS, and one TLAS whose instances are the whole registry. Nothing here renders or binds the TLAS to a pass yet (RT foundation only).
// KEYS: the TLAS instance custom index of a record is its object ID, the same encoding the raster object-ID target writes
// (RenderWorld::objectIdRawForStaticPropRecipe(GpuStaticPropRegistry::getRecipeIndexForType(typeID)) = 0x100000 | recipeIndex; the raster writer lands with
// claude/vk-guides-ids). Terrain is 0 (the raster "background" id). The mech range kMechIdFirst..kMechIdLast is reserved for the movers (part 2).
// MEMBERSHIP: the full registry, never the camera frustum: off-screen geometry is in the TLAS so a ray leaving the view still hits it.
// FOG OF WAR: terrain and static props are map geometry that the raster draws under the fog too (the fog darkens them, it does not hide them), so every one
// is ray-visible. Movers (part 2) are gameplay-hidden outside sensor range and must enter with an instance mask a consumer can exclude.
// GEOMETRY: props trace their raster mesh (the colour-prop VBO; tree sway (flags bit 2) is not applied), one BLAS geometry per non-empty packet; a packet
// the raster alpha-tests (vegetation, fences) is a NON-OPAQUE geometry that a ray query must alpha-test (MATERIALS below), every other geometry is OPAQUE. Terrain traces the terrain_tier1.vert surface: the 4x visual-bake grid (hiresNearHeight's interior path) when the bake is loaded, else the
// coarse 128-unit grid, split per quad by the shared terrain_diagonal.h rule (as the raster patches), cut into the kTerrainTileCoarse world tiles (one BLAS each). The per-patch visualMode 0 (GL keeps
// a few patches undisplaced) is not mirrored: the traced surface is displaced everywhere the bake exists. TLAS space is MC2 world (x, y, z up).
// BUILD FLAGS: every static BLAS is PREFER_FAST_TRACE | ALLOW_COMPACTION (no ALLOW_UPDATE) and is compacted when that saves >= 10%; the TLAS is rebuilt
// (never refit) when membership changes.
// LIFETIME: builds are blocking one-shots on map load and when the registry generation or the terrain key changes (a stall then; moving them to the frame's
// own command buffer is a part-2 follow-up). Replacements are built into new owners and published only when the whole set succeeded; the replaced TLAS,
// then the replaced BLASes, retire through VulkanContext::retire. A failed build keeps the last valid snapshot (never a partial membership) and stops
// retrying until the inputs change. No ray query on the device: nothing is created.
// MATERIALS (part 3, docs/rt-scene-contract.md): one MaterialRow per BLAS geometry in one storage buffer published with the TLAS. A hit's row is
// instanceShaderBindingTableRecordOffset + geometry index (the custom index stays the object ID). Row 0 is the movers' row, the prop rows follow type by
// type (packet order), then one row per terrain tile (row = terrainRowBase + tile ID).
#ifndef MC2_VK_RT_SCENE_H
#define MC2_VK_RT_SCENE_H

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mc2vk::rt::scene {

enum class Kind : uint8_t { Terrain = 0, StaticProp = 1, Mover = 2 };

inline constexpr uint32_t kTerrainObjectId = 0;
inline constexpr uint32_t kMechIdFirst = 0x10000;   // the movers: kMechIdFirst + scene-table slot
inline constexpr uint32_t kMechIdLast = 0x3FFFF;
inline constexpr uint32_t kCustomIndexMask = 0xFFFFFF;   // VkAccelerationStructureInstanceKHR::instanceCustomIndex is 24 bits
inline constexpr uint8_t kMaskStatic = 0x01;              // instance mask bit of terrain + static props
inline constexpr uint8_t kMaskMover = 0x02;               // skinned movers (mechs, vehicles): gameplay-hidden in the fog, so a consumer can exclude them

// MOVERS (part 2): one updatable BLAS per actor (a mech or a vehicle), built in the frame command buffer straight from the GPU skin output
// (MechSkinPass, no readback), refit every frame it is drawn and rebuilt on a topology / LOD change or after kRefitsBeforeRebuild refits. Its TLAS
// instance custom index is kMechIdFirst + a bounded scene-table slot (never the 32-bit handle; the table keeps the key). An actor not drawn this frame
// (off screen) keeps its last pose and stays in the TLAS; it leaves when its RenderWorld handle dies (mechs) or after kMoverUnseenFrames unseen frames
// (vehicles, keyed by shape address), and on map unload. A left slot is reused only after every fresh slot was used (FIFO), so a stale index ages out.
inline constexpr uint32_t kMoverSlots = kMechIdLast - kMechIdFirst + 1;
constexpr std::optional<uint32_t> moverCustomIndex(uint32_t slot) {
    if (slot >= kMoverSlots) return std::nullopt;
    return kMechIdFirst + slot;
}
inline constexpr uint64_t kMoverUnseenFrames = 600;
// A mech's key is its RenderWorld object handle (bit 31 clear); a vehicle's carries bit 31 (shape address based, no liveness query).
constexpr bool moverIsHandleKey(uint32_t key) { return (key & 0x80000000u) == 0; }
constexpr bool moverLeaves(bool handleKey, bool alive, uint64_t unseenFrames) { return handleKey ? !alive : unseenFrames >= kMoverUnseenFrames; }
// The skin output is MC2 Stuff space (x, y = -mc2 x, mc2 z (up), mc2 y order as GpuStaticPropInstance): the mover instance transform into MC2 world
// (the static-prop axis swap with an identity model), and the same map for one point.
void moverTransform(float out[12]);
void stuffToWorld(const float in[3], float out[3]);

// A record's identity: kind, object ID and the occurrence of that ID (props of one type share the type's object ID; the ordinal is the instance's rank among
// them in registry order, so the key of a given placed prop is stable while the registry is).
struct Key {
    Kind kind = Kind::Terrain;
    uint32_t objectId = 0;
    uint32_t ordinal = 0;
    friend constexpr auto operator<=>(const Key&, const Key&) = default;
};

struct Record {
    Key key;
    uint32_t typeId = 0;          // static props: the registry typeID (the BLAS slot); terrain: 0
    float transform[12] = {};     // row-major 3x4, object -> TLAS space
};

// The 24-bit instance custom index holds a bounded ID only: a static prop's per-type object ID (0x100000 | recipe) or terrain's 0. A 32-bit generational
// handle (RenderCore/Handle.h) never goes in it: movers (part 2) store a bounded scene-table slot in the mech range and keep handle + generation in the table.
constexpr bool customIndexFits(uint32_t objectId) { return objectId <= kCustomIndexMask; }
constexpr uint32_t customIndexOf(const Key& k) { return k.objectId & kCustomIndexMask; }
constexpr bool isMechId(uint32_t id) { return id >= kMechIdFirst && id <= kMechIdLast; }

// GpuStaticPropInstance::modelMatrix (row-major, render space: x = -mc2 x, y = mc2 z (up), z = mc2 y) -> the 3x4 into MC2 world.
void propTransform(const float model[16], float out[12]);
void identityTransform(float out[12]);

// One placed static prop as the registry hands it out.
struct PropPlacement {
    uint32_t typeId = 0;
    uint32_t objectId = 0;
    const float* model = nullptr;   // 16 floats
};

// Records for `terrainChunks` terrain chunks (ordinal = chunk index) and `props`, sorted by key, prop ordinals assigned per object ID in input order. Props
// whose object ID is 0 (no recipe; it would alias terrain) or does not fit the custom index are dropped and counted in `dropped`.
std::vector<Record> buildRecords(std::span<const PropPlacement> props, uint32_t terrainChunks, uint32_t* dropped = nullptr);

// The terrain surface the raster draws (see the header comment).
struct TerrainSource {
    int32_t mapSide = 0;
    float halfMap = 0.0f;
    const float* heights = nullptr;   // mapSide^2
    const float* fine = nullptr;      // visualSide^2, null = coarse grid
    int32_t visualSide = 0;
    float displaceFar = 1.0f;
    uint64_t heightVersion = 0;
    uint64_t visualVersion = 0;
};

bool terrainUsable(const TerrainSource& t);
bool terrainUsesFine(const TerrainSource& t);
// Vertices per side of the traced grid.
uint32_t terrainGridSide(const TerrainSource& t);
// Height of grid vertex (gx, gy) (fine units when terrainUsesFine, else coarse) and its world x / y.
float terrainGridHeight(const TerrainSource& t, int32_t gx, int32_t gy);
float terrainGridX(const TerrainSource& t, int32_t gx);
float terrainGridY(const TerrainSource& t, int32_t gy);
// TILE CONTRACT (p35 section 6): the terrain is cut into stable world-space tiles of kTerrainTileCoarse coarse intervals (2048 world units), one BLAS each,
// anchored at the map's north-west corner (grid 0, 0). Tile (tx, ty) has ID ty * tilesPerSide + tx; the count and IDs depend only on mapSide (never the
// camera, never whether the visual bake is loaded); a loaded bake only makes each tile 4x denser. The last row / column holds the remainder.
inline constexpr uint32_t kTerrainTileCoarse = 16;
inline constexpr float kTerrainTileWorld = static_cast<float>(kTerrainTileCoarse) * 128.0f;
uint32_t terrainTilesPerSide(const TerrainSource& t);
// Grid cells per tile side: kTerrainTileCoarse, x4 when terrainUsesFine.
uint32_t terrainTileCells(const TerrainSource& t);
inline uint32_t terrainTileId(const TerrainSource& t, uint32_t tx, uint32_t ty) { return ty * terrainTilesPerSide(t) + tx; }
// Vertex positions (xyz floats, TLAS space) and uint32 indices of tile (tx, ty); empty when out of range. Each quad's diagonal comes from
// mc2terrain::quadSplitsTrBl on the raw bake corners (the raster rule), TL-BR without a bake.
void buildTerrainTile(const TerrainSource& t, uint32_t tx, uint32_t ty, std::vector<float>& positions, std::vector<uint32_t>& indices);
// What a rebuild of the terrain BLAS keys on.
uint64_t terrainKey(const TerrainSource& t);

// One packet of the shared static-prop geometry (mirrors Mc2ShadowPacket).
struct PacketRange {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t baseVertex = 0;
};
// Appends the indices of `packets` (whole triangles only) to `out`, each rebased by its packet's baseVertex, so one index list addresses the shared VBO from
// its base. Indices >= vertexCount, ranges past the IBO and partial triangles are dropped. Returns the number of triangles appended.
uint32_t appendRebasedIndices(std::span<const uint32_t> ibo, std::span<const PacketRange> packets, uint32_t vertexCount, std::vector<uint32_t>& out);

// ---- materials (part 3) ----
inline constexpr uint32_t kMatAlphaOn = 1;    // albedoLayer indexes the colour-prop island's alpha-ON array (else alpha-OFF)
inline constexpr uint32_t kMatCutout = 2;     // the geometry is NON-OPAQUE: commit a candidate only when its albedo alpha >= alphaCut
inline constexpr uint32_t kMatTerrain = 4;    // materialId / packet = the terrain tile ID
inline constexpr uint32_t kMatMover = 8;
inline constexpr uint32_t kMatProp = 16;
inline constexpr float kAlphaCut = 0.5f;      // the raster's mip-0 cut (color_prop_tier1.frag)

// One row per BLAS geometry; std430 twin in shaders/vulkan/rt_scene_probe.comp.
struct MaterialRow {
    int32_t albedoLayer = -1;    // layer in the array kMatAlphaOn selects; -1 = untextured
    uint32_t flags = 0;
    float alphaCut = kAlphaCut;
    int32_t materialId = -1;     // props: the raster PropMaterialGpu index (packetMaterialIds); terrain: the tile ID; -1 none
    float uvScaleX = 1.0f, uvScaleY = 1.0f;   // the raster's per-packet crop: uv = fract(uv) * scale
    uint32_t firstIndex = 0;     // props: first of this geometry's rebased indices in the published uv-index buffer
    uint32_t packet = 0;         // props: the global packet; terrain: the tile ID
};
static_assert(sizeof(MaterialRow) == 32);

inline constexpr uint32_t kMoverRow = 0;
inline constexpr uint32_t kPropRowBase = 1;
constexpr uint32_t terrainRowBase(uint32_t propRows) { return kPropRowBase + propRows; }

// What the colour-prop island routes for one packet.
struct PacketMaterial {
    int32_t layerOff = -1;       // alpha-OFF array layer (alpha class 0 types)
    int32_t layerOn = -1;        // alpha-ON array layer (alpha class 1 types)
    float uvX = 1.0f, uvY = 1.0f;
    int alphaTest = 0;           // mc2_static_prop_packet_alpha_test
    int32_t materialId = -1;
};
// The raster cuts an ON packet always and an OFF packet when it is alpha-tested and textured (untextured OFF samples vec4(1): never cut).
constexpr bool packetIsCutout(const PacketMaterial& p) { return p.layerOn >= 0 || (p.alphaTest != 0 && p.layerOff >= 0); }
constexpr MaterialRow propMaterialRow(const PacketMaterial& p, uint32_t firstIndex, uint32_t packet) {
    MaterialRow r;
    const bool on = p.layerOn >= 0;
    r.albedoLayer = on ? p.layerOn : p.layerOff;
    r.flags = kMatProp | (on ? kMatAlphaOn : 0u) | (packetIsCutout(p) ? kMatCutout : 0u);
    r.materialId = p.materialId;
    r.uvScaleX = p.uvX;
    r.uvScaleY = p.uvY;
    r.firstIndex = firstIndex;
    r.packet = packet;
    return r;
}
constexpr MaterialRow terrainMaterialRow(uint32_t tile) {
    MaterialRow r;
    r.flags = kMatTerrain;
    r.materialId = static_cast<int32_t>(tile);
    r.packet = tile;
    return r;
}
constexpr MaterialRow moverMaterialRow() {
    MaterialRow r;
    r.flags = kMatMover;
    return r;
}

}  // namespace mc2vk::rt::scene

#ifndef MC2_RT_PURE_ONLY

struct VkCommandBuffer_T;   // VkCommandBuffer, without pulling volk into every includer

namespace mc2vk {
class VulkanContext;
}

namespace mc2vk::rt::scene {

// The terrain island's frame feed: rebuilds the terrain BLAS (and the TLAS) when the surface changed. Main thread.
void noteTerrain(const TerrainSource& t);
// The colour-prop island once its map geometry is resident (`propVbo` = its VBO's device address, 0 when the VBO lacks BDA usage): builds the missing type
// BLASes and rebuilds the TLAS when the registry generation moved. Main thread.
void updateProps(uint64_t propVbo);
// Map unload (the prop geometry is going away): retires every prop BLAS, the records and the TLAS, and every mover.
void invalidateProps();

// One draw run of one mover this frame, already skinned on the GPU: positions R32G32B32_SFLOAT at `vertices` (device address, `vertexStride` apart),
// uint32 indices at `indices` (the node range's own vertex numbering). `anchor` is the actor's root in MC2 world (the self-test aims at it).
struct MoverGeometry {
    uint32_t key = 0;
    uint64_t vertices = 0;
    uint32_t vertexStride = 0;
    uint32_t vertexCount = 0;
    uint64_t indices = 0;
    uint32_t triangleCount = 0;
    float anchor[3] = {};
};
// The mech island, in its frame command buffer `cb` (outside a render pass, after the skin dispatch), with its frame slot `slot` (the slot's previous
// submit has retired): refits / builds every drawn mover's BLAS and rebuilds the frame TLAS (static instances + every live mover). Main thread.
void recordMovers(VkCommandBuffer_T* cb, uint32_t slot, std::span<const MoverGeometry> geos);

}  // namespace mc2vk::rt::scene

#endif  // MC2_RT_PURE_ONLY
#endif  // MC2_VK_RT_SCENE_H
