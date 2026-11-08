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
// GEOMETRY: props trace their raster mesh (the colour-prop VBO; tree sway (flags bit 2) is not applied and every packet is opaque until part 3 adds
// cutouts). Terrain traces the terrain_tier1.vert surface: the 4x visual-bake grid (hiresNearHeight's interior path) when the bake is loaded, else the
// coarse 128-unit grid, split per quad by the shared terrain_diagonal.h rule (as the raster patches), cut into the kTerrainTileCoarse world tiles (one BLAS each). The per-patch visualMode 0 (GL keeps
// a few patches undisplaced) is not mirrored: the traced surface is displaced everywhere the bake exists. TLAS space is MC2 world (x, y, z up).
// BUILD FLAGS: every static BLAS is PREFER_FAST_TRACE | ALLOW_COMPACTION (no ALLOW_UPDATE) and is compacted when that saves >= 10%; the TLAS is rebuilt
// (never refit) when membership changes.
// LIFETIME: builds are blocking one-shots on map load and when the registry generation or the terrain key changes (a stall then; moving them to the frame's
// own command buffer is a part-2 follow-up). Replacements are built into new owners and published only when the whole set succeeded; the replaced TLAS,
// then the replaced BLASes, retire through VulkanContext::retire. A failed build keeps the last valid snapshot (never a partial membership) and stops
// retrying until the inputs change. No ray query on the device: nothing is created.
#ifndef MC2_VK_RT_SCENE_H
#define MC2_VK_RT_SCENE_H

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mc2vk::rt::scene {

enum class Kind : uint8_t { Terrain = 0, StaticProp = 1 };

inline constexpr uint32_t kTerrainObjectId = 0;
inline constexpr uint32_t kMechIdFirst = 0x10000;   // reserved for movers (part 2)
inline constexpr uint32_t kMechIdLast = 0x3FFFF;
inline constexpr uint32_t kCustomIndexMask = 0xFFFFFF;   // VkAccelerationStructureInstanceKHR::instanceCustomIndex is 24 bits
inline constexpr uint8_t kMaskStatic = 0x01;              // instance mask bit of terrain + static props (movers get their own bits)

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

}  // namespace mc2vk::rt::scene

#ifndef MC2_RT_PURE_ONLY

namespace mc2vk {
class VulkanContext;
}

namespace mc2vk::rt::scene {

// The terrain island's frame feed: rebuilds the terrain BLAS (and the TLAS) when the surface changed. Main thread.
void noteTerrain(const TerrainSource& t);
// The colour-prop island once its map geometry is resident (`propVbo` = its VBO's device address, 0 when the VBO lacks BDA usage): builds the missing type
// BLASes and rebuilds the TLAS when the registry generation moved. Main thread.
void updateProps(uint64_t propVbo);
// Map unload (the prop geometry is going away): retires every prop BLAS, the records and the TLAS.
void invalidateProps();

}  // namespace mc2vk::rt::scene

#endif  // MC2_RT_PURE_ONLY
#endif  // MC2_VK_RT_SCENE_H
