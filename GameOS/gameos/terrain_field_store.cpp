// terrain_field_store.cpp - the CPU copy of the terrain fields (see terrain_field_store.h). No GL.

#include "terrain_field_store.h"

#include <algorithm>
#include <atomic>

#include "mc2fmt.h"
#include "utils/mc2log.h"

namespace mc2terrain {
namespace {

// A terrain map side beyond this is a malformed input, not a map (stock maps are a few hundred vertices on a side).
constexpr int kMaxSide = 16384;

// Versions come from one process-wide sequence so a store created after a Destroy / Init never repeats a version an
// island already copied (the islands outlive the store).
uint64_t nextVersion() {
    static std::atomic<uint64_t> sequence{0};
    return ++sequence;
}

}  // namespace

bool TerrainFieldStore::setHeight(const float* elevations, int mapSide) {
    if (!elevations || mapSide <= 0 || mapSide > kMaxSide) {
        MC2_LOG(Core, Warn, "[TERRAIN_FIELDS v1] rejected a height field of side {:d}", mc2::fmt::as_i32(mapSide));
        return false;
    }
    height_.assign(elevations, elevations + (size_t)mapSide * (size_t)mapSide);
    mapSide_ = mapSide;
    heightVersion_ = nextVersion();
    return true;
}

bool TerrainFieldStore::patchHeight(const float* rows, int originX, int originY, int quadCountX, int quadCountY,
                                    int mapSide) {
    const bool fits = rows && mapSide == mapSide_ && mapSide_ > 0 && originX >= 0 && originY >= 0 && quadCountX >= 0 &&
                      quadCountY >= 0 && originX + quadCountX < mapSide_ && originY + quadCountY < mapSide_;
    if (!fits) {
        MC2_LOG(Core, Warn, "[TERRAIN_FIELDS v1] rejected a height patch at ({:d},{:d}) size {:d}x{:d} on a side-{:d} map (stored {:d})",
                mc2::fmt::as_i32(originX), mc2::fmt::as_i32(originY), mc2::fmt::as_i32(quadCountX + 1),
                mc2::fmt::as_i32(quadCountY + 1), mc2::fmt::as_i32(mapSide), mc2::fmt::as_i32(mapSide_));
        return false;
    }
    const size_t w = (size_t)quadCountX + 1;
    for (int row = 0; row <= quadCountY; ++row) {
        const float* src = rows + (size_t)row * w;
        float* dst = height_.data() + (size_t)(originY + row) * (size_t)mapSide_ + (size_t)originX;
        std::copy(src, src + w, dst);
    }
    heightVersion_ = nextVersion();
    return true;
}

bool TerrainFieldStore::setCementWords(const uint32_t* words, int count) {
    if (!words || count <= 0 || count > kMaxSide * kMaxSide) {
        MC2_LOG(Core, Warn, "[TERRAIN_FIELDS v1] rejected {:d} cement words", mc2::fmt::as_i32(count));
        return false;
    }
    cement_.assign(words, words + count);
    cementVersion_ = nextVersion();
    return true;
}

bool TerrainFieldStore::setVisualHeight(const float* fine, int side, const float* mipMaxes, int mipFloats) {
    if (!fine || side <= 0 || side > kMaxSide * 4 || mipFloats < 0 || (mipFloats > 0 && !mipMaxes)) {
        MC2_LOG(Core, Warn, "[TERRAIN_FIELDS v1] rejected a visual height bake of side {:d} with {:d} mip floats",
                mc2::fmt::as_i32(side), mc2::fmt::as_i32(mipFloats));
        return false;
    }
    const size_t fineCount = (size_t)side * (size_t)side;
    visual_.resize(fineCount + (size_t)mipFloats);
    std::copy(fine, fine + fineCount, visual_.begin());
    if (mipFloats > 0) std::copy(mipMaxes, mipMaxes + mipFloats, visual_.begin() + (std::ptrdiff_t)fineCount);
    visualSide_ = side;
    visualMipFloats_ = mipFloats;
    visualVersion_ = nextVersion();
    return true;
}

namespace {

// A field image (shore sidecar, cement atlas) covers the map at most a few texels per vertex; beyond kMaxSide * 4 on an edge it
// is malformed.
template <typename Texel>
bool setImage(FieldImage<Texel>& image, const Texel* texels, int width, int height, size_t texelsPerPixel) {
    if (!texels || width <= 0 || height <= 0 || width > kMaxSide * 4 || height > kMaxSide * 4) {
        MC2_LOG(Core, Warn, "[TERRAIN_FIELDS v1] rejected a field image of {:d}x{:d}", mc2::fmt::as_i32(width),
                mc2::fmt::as_i32(height));
        return false;
    }
    image.texels.assign(texels, texels + (size_t)width * (size_t)height * texelsPerPixel);
    image.width = width;
    image.height = height;
    image.version = nextVersion();
    return true;
}

}  // namespace

bool TerrainFieldStore::setShorelineMask(const uint8_t* rgba, int width, int height) {
    return setImage(shorelineMask_, rgba, width, height, 4);
}

bool TerrainFieldStore::setWaterShoreHeight(const float* heights, int width, int height) {
    return setImage(waterShoreHeight_, heights, width, height, 1);
}

bool TerrainFieldStore::setCementAtlas(const uint8_t* rgba, int width, int height) {
    return setImage(cementAtlas_, rgba, width, height, 4);
}

bool TerrainFieldStore::setColormapAtlas(const uint8_t* bytes, size_t byteCount, int width, int height, uint32_t vkFormat) {
    const bool bc7 = vkFormat == ColormapAtlas::kBc7Unorm || vkFormat == ColormapAtlas::kBc7Srgb;
    const bool sizeOk = width > 0 && height > 0 && width <= kMaxSide * 4 && height <= kMaxSide * 4;
    const size_t want = !sizeOk ? 0
                      : bc7     ? (size_t)((width + 3) / 4) * (size_t)((height + 3) / 4) * 16u
                      : vkFormat == ColormapAtlas::kRgba8 ? (size_t)width * (size_t)height * 4u : 0;
    if (!bytes || want == 0 || byteCount != want) {
        MC2_LOG(Core, Warn, "[TERRAIN_FIELDS v1] rejected a colormap atlas of {:d}x{:d}, format {:d}, {:d} bytes",
                mc2::fmt::as_i32(width), mc2::fmt::as_i32(height), mc2::fmt::as_u32(vkFormat), mc2::fmt::as_size(byteCount));
        return false;
    }
    colormapAtlas_.bytes.assign(bytes, bytes + byteCount);
    colormapAtlas_.width = width;
    colormapAtlas_.height = height;
    colormapAtlas_.vkFormat = vkFormat;
    colormapAtlas_.version = nextVersion();
    return true;
}

}  // namespace mc2terrain
