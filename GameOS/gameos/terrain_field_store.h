// terrain_field_store.h - CPU-owned terrain inputs for native rendering.
//
// WHAT: the height field (mapSide^2 floats, whole-map and patch writes), the per-vertex cement words, and the 4x visual height
// bake with its mip maxes, each with a version that changes on every write. Versions are unique for the process (0 = never
// written), so a recreated store never matches a version a consumer copied from the previous one.
// Patch writes (including craters) change the version used by native consumers to refresh their copies.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include "../../RenderCore/KtxLoader.h"
#include "terrain_surface_asset.h"

namespace mc2terrain {

// A width x height row-major image of one texel type (the shore sidecars), empty until written.
template <typename Texel>
struct FieldImage {
    std::vector<Texel> texels;
    int width = 0;
    int height = 0;
    uint64_t version = 0;
};

// The terrain colormap atlas as its GL upload took it: RGBA8 texels (R first) or the level-0 BC7 blocks of a .ktx2 sidecar.
struct ColormapAtlas {
    static constexpr uint32_t kRgba8 = 37, kBc7Unorm = 145, kBc7Srgb = 146;   // VkFormat values
    std::vector<uint8_t> bytes;
    int width = 0;
    int height = 0;
    uint32_t vkFormat = 0;
    uint64_t version = 0;
};

// Cooked detail-normal layers: one BC7 linear image holding every layer, level-major (level 0 of all layers, then level 1, ...),
// with its full mip chain. Absent (null image) when any used layer has no cooked sidecar; the renderer then imports the TGA slots.
struct MaterialNormalArray {
    std::shared_ptr<const RenderCore::KtxImage> image;
    uint32_t layers = 0;
    uint64_t version = 0;
    bool grassSourceAo = false;   // grass layer is the cooked turf, whose normal-texel B channel holds source AO
};

enum class WaterShoreHeightKind : int32_t {
    RawHeight = 0,
    ShoreSdf = 1,
};

class TerrainFieldStore {
public:
    // Whole-map writes. Return false (with a log line) and keep the previous field on a null / empty / oversized input.
    bool setHeight(const float* elevations, int mapSide);
    bool setCementWords(const uint32_t* words, int count);
    bool setVisualHeight(const float* fine, int side, const float* mipMaxes, int mipFloats);
    void clearVisualHeight();   // mission change: a missing or rejected bake must not leave the previous one

    // A (quadCountX + 1) x (quadCountY + 1) row-major block at (originX, originY). Rejected (with a log line) unless it fits
    // the stored map of the same side.
    bool patchHeight(const float* rows, int originX, int originY, int quadCountX, int quadCountY, int mapSide);

    // The grid rectangles written by patchHeight since the last take (x0, y0, x1, y1 in vertices, inclusive); a whole-map setHeight clears it.
    struct HeightPatch { int x0, y0, x1, y1; };
    std::vector<HeightPatch> takeHeightPatches() { std::vector<HeightPatch> out; out.swap(patches_); return out; }

    const std::vector<float>& height() const { return height_; }
    int mapSide() const { return mapSide_; }
    uint64_t heightVersion() const { return heightVersion_; }

    const std::vector<uint32_t>& cementWords() const { return cement_; }
    uint64_t cementVersion() const { return cementVersion_; }

    // Fine bake (side^2 floats) followed by the mip maxes, the layout the shaders index.
    const std::vector<float>& visualHeight() const { return visual_; }
    const std::vector<float>& waterShoreCells() const { return waterShoreCells_; }
    int visualSide() const { return visualSide_; }
    int visualMipFloats() const { return visualMipFloats_; }
    uint64_t visualHeightVersion() const { return visualVersion_; }

    bool setTerrainTypes(const float* types, int side);
    bool setControlMap(const uint8_t* rgba, int side);
    bool setOverlay(const uint8_t* rgba, int width, int height);
    const FieldImage<float>& terrainTypes() const { return terrainTypes_; }
    const FieldImage<uint8_t>& controlMap() const { return controlMap_; }
    const FieldImage<uint8_t>& overlay() const { return overlay_; }

    // Clear the previous mission's shore data; native draws must not read an empty field.
    void clearShoreFields();

    // The water shore sidecars of the loaded mission: the shoreline mask (RGBA8: signed distance, wet, foam, valid) and the
    // hi-res shore height (R32F). Rejected (with a log line, previous image kept) on a null or out-of-range size.
    bool setShorelineMask(const uint8_t* rgba, int width, int height);
    bool setWaterShoreHeight(const float* heights, int width, int height);
    bool setWaterShoreHeight(const float* heights, int width, int height, WaterShoreHeightKind kind);
    const FieldImage<uint8_t>& shorelineMask() const { return shorelineMask_; }   // 4 bytes a texel
    const FieldImage<float>& waterShoreHeight() const { return waterShoreHeight_; }
    WaterShoreHeightKind waterShoreHeightKind() const { return waterShoreHeightKind_; }

    // The cement / road overlay atlas of the loaded mission (RGBA8, R first), as the GL atlas was uploaded. Rejected (with a log
    // line, previous image kept) on a null or out-of-range size.
    bool setCementAtlas(const uint8_t* rgba, int width, int height);
    const FieldImage<uint8_t>& cementAtlas() const { return cementAtlas_; }   // 4 bytes a texel

    // Rejected (with a log line, previous atlas kept) on an empty input, an edge out of range, an unknown format or a byte count
    // that is not one level of that format at that size.
    bool setColormapAtlas(std::span<const uint8_t> bytes, int width, int height, uint32_t vkFormat);
    const ColormapAtlas& colormapAtlas() const { return colormapAtlas_; }

    // Every layer must be BC7 UNORM, square, the same size and mip-complete; otherwise the previous array is cleared.
    bool setMaterialNormals(std::span<const RenderCore::KtxImage> layers);
    const MaterialNormalArray& materialNormals() const { return materialNormals_; }
    void setGrassSourceAo(bool on) { materialNormals_.grassSourceAo = on && static_cast<bool>(materialNormals_.image); }

    bool setSurfaceAsset(SurfaceAsset asset, uint64_t expectedSource);
    bool loadSurfaceAsset(std::span<const uint8_t> bytes, uint64_t expectedSource);
    void clearSurfaceAsset();
    const SurfaceAsset& surfaceAsset() const { return surfaceAsset_; }
    uint64_t surfaceVersion() const { return surfaceVersion_; }

private:
    std::vector<float> height_;
    std::vector<HeightPatch> patches_;
    int mapSide_ = 0;
    uint64_t heightVersion_ = 0;
    std::vector<uint32_t> cement_;
    uint64_t cementVersion_ = 0;
    std::vector<float> visual_;
    std::vector<float> waterShoreCells_;
    int visualSide_ = 0;
    int visualMipFloats_ = 0;
    uint64_t visualVersion_ = 0;
    FieldImage<float> terrainTypes_;
    FieldImage<uint8_t> controlMap_, overlay_;
    FieldImage<uint8_t> shorelineMask_;
    FieldImage<float> waterShoreHeight_;
    WaterShoreHeightKind waterShoreHeightKind_ = WaterShoreHeightKind::RawHeight;
    FieldImage<uint8_t> cementAtlas_;
    ColormapAtlas colormapAtlas_;
    MaterialNormalArray materialNormals_;
    SurfaceAsset surfaceAsset_;
    uint64_t surfaceVersion_ = 0;
};

}  // namespace mc2terrain

// The store owned by the terrain LOD-chunk subsystem: created by gos_TerrainLodChunk_Init, destroyed by
// gos_TerrainLodChunk_Destroy; null outside that window.
mc2terrain::TerrainFieldStore* gos_TerrainFieldStore();
