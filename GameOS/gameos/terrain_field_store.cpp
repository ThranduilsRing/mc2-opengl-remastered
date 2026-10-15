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

}  // namespace mc2terrain
