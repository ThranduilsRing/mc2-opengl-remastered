// terrain_field_store.h - the CPU copy of the terrain fields both backends draw from.
//
// WHAT: the height field (mapSide^2 floats, whole-map and patch writes), the per-vertex cement words, and the 4x visual height
// bake with its mip maxes, each with a version that changes on every write. Versions are unique for the process (0 = never
// written), so a recreated store never matches a version a consumer copied from the previous one.
// WHY: these arrived as CPU arrays and were kept only inside GL SSBOs, so the Vulkan terrain island had to read the SSBOs back
// to draw. The store keeps the bytes; the GL upload and the Vulkan snapshot both read it, and a patch write (crater deform)
// now changes the version the Vulkan island keys its re-copy on. No GL here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc2terrain {

// A width x height row-major image of one texel type (the shore sidecars), empty until written.
template <typename Texel>
struct FieldImage {
    std::vector<Texel> texels;
    int width = 0;
    int height = 0;
    uint64_t version = 0;
};

class TerrainFieldStore {
public:
    // Whole-map writes. Return false (with a log line) and keep the previous field on a null / empty / oversized input.
    bool setHeight(const float* elevations, int mapSide);
    bool setCementWords(const uint32_t* words, int count);
    bool setVisualHeight(const float* fine, int side, const float* mipMaxes, int mipFloats);

    // A (quadCountX + 1) x (quadCountY + 1) row-major block at (originX, originY). Rejected (with a log line) unless it fits
    // the stored map of the same side.
    bool patchHeight(const float* rows, int originX, int originY, int quadCountX, int quadCountY, int mapSide);

    const std::vector<float>& height() const { return height_; }
    int mapSide() const { return mapSide_; }
    uint64_t heightVersion() const { return heightVersion_; }

    const std::vector<uint32_t>& cementWords() const { return cement_; }
    uint64_t cementVersion() const { return cementVersion_; }

    // Fine bake (side^2 floats) followed by the mip maxes, the layout the shaders index.
    const std::vector<float>& visualHeight() const { return visual_; }
    int visualSide() const { return visualSide_; }
    int visualMipFloats() const { return visualMipFloats_; }
    uint64_t visualHeightVersion() const { return visualVersion_; }

    // The water shore sidecars of the loaded mission: the shoreline mask (RGBA8: signed distance, wet, foam, valid) and the
    // hi-res shore height (R32F). Rejected (with a log line, previous image kept) on a null or out-of-range size.
    bool setShorelineMask(const uint8_t* rgba, int width, int height);
    bool setWaterShoreHeight(const float* heights, int width, int height);
    const FieldImage<uint8_t>& shorelineMask() const { return shorelineMask_; }   // 4 bytes a texel
    const FieldImage<float>& waterShoreHeight() const { return waterShoreHeight_; }

private:
    std::vector<float> height_;
    int mapSide_ = 0;
    uint64_t heightVersion_ = 0;
    std::vector<uint32_t> cement_;
    uint64_t cementVersion_ = 0;
    std::vector<float> visual_;
    int visualSide_ = 0;
    int visualMipFloats_ = 0;
    uint64_t visualVersion_ = 0;
    FieldImage<uint8_t> shorelineMask_;
    FieldImage<float> waterShoreHeight_;
};

}  // namespace mc2terrain

// The store owned by the terrain LOD-chunk subsystem: created by gos_TerrainLodChunk_Init, destroyed by
// gos_TerrainLodChunk_Destroy; null outside that window.
mc2terrain::TerrainFieldStore* gos_TerrainFieldStore();
