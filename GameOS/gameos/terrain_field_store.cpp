// terrain_field_store.cpp - the CPU copy of the terrain fields (see terrain_field_store.h). No GL.

#include "terrain_field_store.h"
#include "water_shore_cells.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mdspan>

#include "mc2fmt.h"
#include "utils/mc2log.h"
#include "diagnostic_trace.h"

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

// Separable window filter over a row-major side x side field (rows, then columns), the window clamped at the map edge:
// kMin takes the window minimum, otherwise its mean. src and dst may be the same field.
template <bool kMin>
void windowFilter(const float* src, float* tmp, float* dst, int side, int radius) {
    const std::mdspan<const float, std::dextents<size_t, 2>> in(src, side, side);
    const std::mdspan<float, std::dextents<size_t, 2>> mid(tmp, side, side), out(dst, side, side);
    // One output: the window runs along the second argument of `at` (columns of the field, rows of the field).
    const auto along = [side, radius](const auto& at, int a, int b) {
        const int lo = (std::max)(b - radius, 0), hi = (std::min)(b + radius, side - 1);
        float acc = kMin ? at(a, lo) : 0.0f;
        for (int k = lo; k <= hi; ++k) {
            const float v = at(a, k);
            acc = kMin ? (std::min)(acc, v) : acc + v;
        }
        return kMin ? acc : acc / static_cast<float>(hi - lo + 1);
    };
    for (int a = 0; a < side; ++a)
        for (int b = 0; b < side; ++b)
            mid[a, b] = along([&](int r, int c) { return in[r, c]; }, a, b);
    for (int a = 0; a < side; ++a)
        for (int b = 0; b < side; ++b)
            out[b, a] = along([&](int r, int c) { return mid[c, r]; }, a, b);
}

}  // namespace

void TerrainFieldStore::bakeFogFloor() const {
    constexpr int kMinRadius = 8, kBlurRadius = 2;   // 1024 wu on the 128 wu grid, then a 4-cell box
    fogFloorVersion_ = heightVersion_;
    fogFloor_.assign(height_.begin(), height_.end());
    fogFloorMax_ = 0.0f;
    if (mapSide_ <= 0 || fogFloor_.size() != static_cast<size_t>(mapSide_) * static_cast<size_t>(mapSide_)) {
        fogFloor_.clear();
        return;
    }
    std::vector<float> tmp(fogFloor_.size());
    windowFilter<true>(fogFloor_.data(), tmp.data(), fogFloor_.data(), mapSide_, kMinRadius);
    windowFilter<false>(fogFloor_.data(), tmp.data(), fogFloor_.data(), mapSide_, kBlurRadius);
    fogFloorMax_ = *std::max_element(fogFloor_.begin(), fogFloor_.end());
}

bool TerrainFieldStore::setHeight(const float* elevations, int mapSide) {
    if (!elevations || mapSide <= 0 || mapSide > kMaxSide) {
        MC2_LOG(Core, Warn, "[TERRAIN_FIELDS v1] rejected a height field of side {:d}", mc2::fmt::as_i32(mapSide));
        return false;
    }
    height_.assign(elevations, elevations + static_cast<size_t>(mapSide) * static_cast<size_t>(mapSide));
    mapSide_ = mapSide;
    patches_.clear();   // a whole new field is a new bake, not a patch
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
    const size_t w = static_cast<size_t>(quadCountX) + 1;
    for (int row = 0; row <= quadCountY; ++row) {
        const float* src = rows + static_cast<size_t>(row) * w;
        float* dst = height_.data() + static_cast<size_t>((originY + row)) * static_cast<size_t>(mapSide_) + static_cast<size_t>(originX);
        std::copy(src, src + w, dst);
    }
    if (patches_.size() < 4096) patches_.push_back({originX, originY, originX + quadCountX, originY + quadCountY});
    heightVersion_ = nextVersion();
    return true;
}

bool TerrainFieldStore::setCementWords(const uint32_t* words, int count) {
    if (!words || count <= 0 || count > 2 * kMaxSide * kMaxSide) {
        MC2_LOG(Core, Warn, "[TERRAIN_FIELDS v1] rejected {:d} cement words", mc2::fmt::as_i32(count));
        return false;
    }
    // A one-plane write (PopulateRecipeCementWords) onto a two-plane store (plane 2 = road art) replaces plane 0 only.
    if (cement_.size() == 2 * static_cast<size_t>(count))
        std::copy(words, words + count, cement_.begin());
    else
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
    const size_t fineCount = static_cast<size_t>(side) * static_cast<size_t>(side);
    const auto shoreStart = std::chrono::steady_clock::now();
    waterShoreCells_ = bakeWaterShoreCells(fine, side);
    if (mc2_diag::tagRequested("TERRAIN_FIELDS"))
        MC2_LOG(Core, Info, "[TERRAIN_FIELDS v1] shore_cells side={} bake_ms={:.3f}", side,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shoreStart).count());
    visual_.resize(fineCount + static_cast<size_t>(mipFloats));
    std::copy(fine, fine + fineCount, visual_.begin());
    if (mipFloats > 0) std::copy(mipMaxes, mipMaxes + mipFloats, visual_.begin() + (std::ptrdiff_t)fineCount);
    visualSide_ = side;
    visualMipFloats_ = mipFloats;
    visualVersion_ = nextVersion();
    return true;
}

void TerrainFieldStore::clearVisualHeight() {
    visual_.clear();
    waterShoreCells_.clear();
    visualSide_ = 0;
    visualMipFloats_ = 0;
    visualVersion_ = nextVersion();
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
    image.texels.assign(texels, texels + static_cast<size_t>(width) * static_cast<size_t>(height) * texelsPerPixel);
    image.width = width;
    image.height = height;
    image.version = nextVersion();
    return true;
}

}  // namespace

bool TerrainFieldStore::setTerrainTypes(const float* types, int side) {
    return setImage(terrainTypes_, types, side, side, 1);
}
bool TerrainFieldStore::setControlMap(const uint8_t* rgba, int side) {
    return setImage(controlMap_, rgba, side, side, 4);
}
bool TerrainFieldStore::setOverlay(const uint8_t* rgba, int width, int height) {
    return setImage(overlay_, rgba, width, height, 4);
}

bool TerrainFieldStore::setShorelineMask(const uint8_t* rgba, int width, int height) {
    return setImage(shorelineMask_, rgba, width, height, 4);
}

bool TerrainFieldStore::setWaterShoreHeight(const float* heights, int width, int height) {
    return setWaterShoreHeight(heights, width, height, WaterShoreHeightKind::RawHeight);
}

bool TerrainFieldStore::setWaterShoreHeight(const float* heights, int width, int height,
                                             WaterShoreHeightKind kind) {
    if (!setImage(waterShoreHeight_, heights, width, height, 1))
        return false;
    waterShoreHeightKind_ = kind;
    return true;
}

void TerrainFieldStore::setTreePositions(std::vector<float> xy) {
    treeXY_ = std::move(xy);
    treeVersion_ = nextVersion();
}

void TerrainFieldStore::clearShoreFields() {
    shorelineMask_.texels.clear();
    shorelineMask_.width = 0;
    shorelineMask_.height = 0;
    shorelineMask_.version = nextVersion();
    waterShoreHeight_.texels.clear();
    waterShoreHeight_.width = 0;
    waterShoreHeight_.height = 0;
    waterShoreHeight_.version = nextVersion();
    waterShoreHeightKind_ = WaterShoreHeightKind::RawHeight;
}

bool TerrainFieldStore::setCementAtlas(const uint8_t* rgba, int width, int height) {
    return setImage(cementAtlas_, rgba, width, height, 4);
}

bool TerrainFieldStore::setColormapAtlas(std::span<const uint8_t> bytes, int width, int height, uint32_t vkFormat) {
    const bool bc7 = vkFormat == ColormapAtlas::kBc7Unorm || vkFormat == ColormapAtlas::kBc7Srgb;
    const bool sizeOk = width > 0 && height > 0 && width <= kMaxSide * 4 && height <= kMaxSide * 4;
    const size_t want = !sizeOk ? 0
                      : bc7     ? static_cast<size_t>(((width + 3) / 4)) * static_cast<size_t>(((height + 3) / 4)) * 16u
                      : vkFormat == ColormapAtlas::kRgba8 ? static_cast<size_t>(width) * static_cast<size_t>(height) * 4u : 0;
    if (bytes.empty() || want == 0 || bytes.size() != want) {
        MC2_LOG(Core, Warn, "[TERRAIN_FIELDS v1] rejected a colormap atlas of {:d}x{:d}, format {:d}, {:d} bytes",
                mc2::fmt::as_i32(width), mc2::fmt::as_i32(height), mc2::fmt::as_u32(vkFormat), mc2::fmt::as_size(bytes.size()));
        return false;
    }
    colormapAtlas_.bytes.assign(bytes.begin(), bytes.end());
    colormapAtlas_.width = width;
    colormapAtlas_.height = height;
    colormapAtlas_.vkFormat = vkFormat;
    colormapAtlas_.version = nextVersion();
    return true;
}

bool TerrainFieldStore::setMaterialNormals(std::span<const RenderCore::KtxImage> layers) {
    materialNormals_ = {};
    if (layers.empty() || layers.size() > 16) return false;
    const auto& first = layers.front();
    int fullChain = 1;
    for (int edge = first.width; edge > 1; edge >>= 1) ++fullChain;
    // BC7 (16-byte 4x4 blocks) or lossless RGBA8 (4 bytes a texel); every layer shares slot 0's format.
    const bool rgba8 = first.vkFormat == ColormapAtlas::kRgba8;
    const auto levelBytes = [rgba8](int side, int level) {
        const size_t e = static_cast<size_t>(std::max(1, side >> level));
        if (rgba8) return e * e * 4u;
        const size_t b = (e + 3) / 4; return b * b * 16u;
    };
    for (const auto& l : layers) {
        bool ok = l.vkFormat == first.vkFormat && (rgba8 ? (!l.isCompressed && l.blockSizeBytes == 0)
                                                         : (l.isCompressed && l.vkFormat == ColormapAtlas::kBc7Unorm && l.blockSizeBytes == 16)) && l.width > 0 &&
                  l.width == l.height && l.width == first.width && l.width <= kMaxSide && l.mipCount == fullChain &&
                  l.mipByteOffsets.size() == static_cast<size_t>(fullChain);
        size_t offset = 0;
        for (int level = 0; ok && level < fullChain; ++level) {
            ok = l.mipByteOffsets[level] == offset;
            offset += levelBytes(l.width, level);
        }
        if (!ok || l.pixels.size() != offset) {
            MC2_LOG(Core, Warn, "[TERRAIN_FIELDS v1] rejected a cooked detail-normal layer of {:d}x{:d}, format {:d}, {:d} mips",
                    mc2::fmt::as_i32(l.width), mc2::fmt::as_i32(l.height), mc2::fmt::as_u32(l.vkFormat), mc2::fmt::as_i32(l.mipCount));
            return false;
        }
    }
    auto array = std::make_shared<RenderCore::KtxImage>();
    array->width = array->height = first.width;
    array->vkFormat = first.vkFormat; array->isCompressed = !rgba8; array->blockSizeBytes = rgba8 ? 0 : 16;
    array->mipCount = fullChain;
    for (int level = 0; level < fullChain; ++level) {
        array->mipByteOffsets.push_back(array->pixels.size());
        const size_t bytes = levelBytes(first.width, level);
        for (const auto& l : layers) {
            const auto begin = l.pixels.begin() + static_cast<ptrdiff_t>(l.mipByteOffsets[level]);
            array->pixels.insert(array->pixels.end(), begin, begin + static_cast<ptrdiff_t>(bytes));
        }
    }
    materialNormals_ = {std::move(array), static_cast<uint32_t>(layers.size()), nextVersion()};
    return true;
}

bool TerrainFieldStore::setSurfaceAsset(SurfaceAsset asset, uint64_t expectedSource) {
    if (!validSurfaceAsset(asset, expectedSource)) return false;
    surfaceAsset_ = std::move(asset);
    surfaceVersion_ = nextVersion();
    return true;
}

bool TerrainFieldStore::loadSurfaceAsset(std::span<const uint8_t> bytes, uint64_t expectedSource) {
    auto candidate = decodeSurfaceAsset(bytes, expectedSource);
    if (!candidate) return false;
    return setSurfaceAsset(std::move(*candidate), expectedSource);
}

void TerrainFieldStore::clearSurfaceAsset() {
    surfaceAsset_ = {};
    surfaceVersion_ = nextVersion();
}

}  // namespace mc2terrain
