// Vegetation population ownership and the per-frame GPU cull request. The population lives on the device (vk_vegetation_state);
// the frame sends camera parameters and the block-visibility field, never a per-instance list.
#include "mc2_czstring.h"
#include "gos_vegetation.h"
#include "vulkan_vfx_geometry.h"
#include "vk_vegetation_state.h"
#include "vulkan_context.h"
#include "texture_decode.h"
#include "vegetation_cutout.h"
#include "../../GameAdapters/vegetation_budget.h"
#include "../../RenderCore/KtxLoader.h"
#include <optional>
#include <string>
#include "gos_postprocess.h"
#include "Gates/gate.h"
#include "Gates/render_options.h"
#include "Gates/gate_cold.h"
#include "mc2fmt.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
extern const float* gos_GetTerrainMVPMat4();
namespace {
uint32_t populationCount = 0;   // instances resident on the device
auto cell(const GosVegetation::Instance& i) {
    return std::pair{static_cast<int>(std::floor(i.x/128.0f)),static_cast<int>(std::floor(i.y/128.0f))};
}
std::vector<uint32_t> visibility(1024, 2u);
uint32_t visibilityBlocks = 1024, footprintCount = 0;
bool initialized = false, atlasReady = false, summaryEmitted = false;
uint32_t peak = 0;
int frameDraws = 0, sessionDraws = 0;
const char* reason = "no_call";
const char* atlasOverride = nullptr;
bool trimCards = false;

// One log line when a card cannot be loaded: its layer reuses a neighbouring card instead of
// switching every card off (a lean install without the newer cards still draws vegetation).
void reportMissingCards(std::string_view kind, const std::array<bool, 8>& present, const std::vector<int>& source) {
    static constexpr std::array<const char*, 8> names{"grass_01", "grass_02", "grass_03", "grass_04",
        "bush_round", "bush_heath", "sapling_leaf", "sapling_pine"};
    std::string missing;
    for (size_t i = 0; i < present.size(); ++i)
        if (!present[i]) missing += std::string(missing.empty() ? "" : ",") + names[i] + "->" + names[static_cast<size_t>(source[i])];
    if (!missing.empty())
        mc2::fmt::print_err("[VK_RESIDENCY v1] vegetation {} cards unavailable, layers reuse a neighbour: {}\n", kind, missing);
}

// The old private vegetation_atlas_v2.png was never part of the payload. Build
// its eight shader slots from the tracked cards so a clean install is complete.
std::optional<mc2tex::DecodedTexture> builtinAtlas() {
    static constexpr std::array<const char*, 8> paths{
        "data/vegetation/cards/grass_01.png", "data/vegetation/cards/grass_02.png",
        "data/vegetation/cards/grass_03.png", "data/vegetation/cards/grass_04.png",
        "data/vegetation/cards/bush_round.png", "data/vegetation/cards/bush_heath.png",
        "data/vegetation/cards/sapling_leaf.png", "data/vegetation/cards/sapling_pine.png"};
    std::array<std::optional<mc2tex::DecodedTexture>, 8> tiles;
    std::array<bool, 8> present{};
    int cellWidth = 0, cellHeight = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        auto tile = mc2tex::decodeFile(paths[i], gos_Texture_Alpha, 0);
        if (!tile || tile->layout != mc2tex::PixelLayout::RGBA8) continue;
        if (!cellWidth) { cellWidth = tile->width; cellHeight = tile->height; }
        if (tile->width != cellWidth || tile->height != cellHeight) continue;
        tiles[i] = std::move(tile); present[i] = true;
    }
    const auto source = mc2::vegetation::cardLayerSources(present);
    if (source.empty()) return std::nullopt;
    reportMissingCards("png", present, source);
    mc2tex::DecodedTexture atlas;
    atlas.layout = mc2tex::PixelLayout::RGBA8;
    atlas.format = gos_Texture_Alpha;
    atlas.width = cellWidth * 4; atlas.height = cellHeight * 2;
    atlas.pixels.resize(static_cast<size_t>(atlas.width) * atlas.height * 4);
    for (size_t i = 0; i < paths.size(); ++i) {
        const auto& tile = tiles[static_cast<size_t>(source[i])];
        for (int y = 0; y < cellHeight; ++y) {
            const size_t dst = ((i / 4 * cellHeight + y) * atlas.width + i % 4 * cellWidth) * 4;
            std::copy_n(tile->pixels.data() + static_cast<size_t>(y) * cellWidth * 4, static_cast<size_t>(cellWidth) * 4,
                        atlas.pixels.data() + dst);
        }
    }
    return atlas;
}
std::optional<mc2tex::TextureArrayCpu> builtinCards() {
    constexpr std::array<const char*, 8> names{"grass_01", "grass_02", "grass_03", "grass_04",
        "bush_round", "bush_heath", "sapling_leaf", "sapling_pine"};
    mc2tex::TextureArrayCpu array;
    array.width = array.height = 512; array.layers = 8;
    array.vkFormat = mc2tex::kVkFormatBc7Unorm;
    array.levels.resize(10);
    std::array<RenderCore::KtxImage, 8> tiles;
    std::array<bool, 8> present{};
    for (size_t k = 0; k < names.size(); ++k) {
        const auto path = std::string("data/vegetation/cooked/") + names[k] + ".ktx2";
        auto& tile = tiles[k];
        present[k] = RenderCore::ktxLoadRgba8(path.c_str(), tile) && tile.width == 512 && tile.height == 512 &&
                     tile.mipCount == 10 && tile.vkFormat == array.vkFormat;
    }
    const auto source = mc2::vegetation::cardLayerSources(present);
    if (source.empty()) return std::nullopt;
    reportMissingCards("ktx2", present, source);
    for (size_t k = 0; k < names.size(); ++k) {
        const auto& tile = tiles[static_cast<size_t>(source[k])];
        for (int level = 0; level < tile.mipCount; ++level) {
            const auto begin = tile.mipByteOffsets[level];
            const auto end = level + 1 < tile.mipCount ? tile.mipByteOffsets[level + 1] : tile.pixels.size();
            auto& dest = array.levels[level];
            dest.insert(dest.end(), tile.pixels.begin() + begin, tile.pixels.begin() + end);
        }
    }
    return array;
}

// Legacy/mod atlas overrides keep their eight slots, with isolated layers and
// coverage-preserving mips. Their geometry stays untrimmed because its alpha
// hull is not the stock cook's hull.
mc2tex::TextureArrayCpu cardsFromAtlas(const mc2tex::DecodedTexture& decoded) {
    mc2tex::TextureArrayCpu array;
    if (decoded.width % 4 || decoded.height % 2) return array;
    uint32_t w = static_cast<uint32_t>(decoded.width / 4), h = static_cast<uint32_t>(decoded.height / 2);
    if (!w || !h) return array;
    array.width = w; array.height = h; array.layers = 8; array.vkFormat = mc2tex::kVkFormatRgba8Unorm;
    const size_t components = decoded.layout == mc2tex::PixelLayout::RGB8 ? 3 : 4;
    std::array<std::vector<uint8_t>, 8> layer;
    std::array<float, 8> coverage{};
    for (size_t k = 0; k < layer.size(); ++k) {
        layer[k].resize(static_cast<size_t>(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t src = ((k / 4 * h + y) * decoded.width + k % 4 * w + x) * components;
                const size_t dst = (static_cast<size_t>(y) * w + x) * 4;
                std::copy_n(decoded.pixels.data() + src, 3, layer[k].data() + dst);
                layer[k][dst + 3] = components == 4 ? decoded.pixels[src + 3] : 255;
            }
        }
        coverage[k] = mc2::vegetation::cutoutCoverage(layer[k]);
        mc2::vegetation::dilateCutoutRgb(layer[k], w, h);
    }
    for (;;) {
        auto& level = array.levels.emplace_back();
        for (const auto& tile : layer) level.insert(level.end(), tile.begin(), tile.end());
        if (w == 1 && h == 1) break;
        for (size_t k = 0; k < layer.size(); ++k) {
            layer[k] = mc2tex::boxDownsampleRgba8(layer[k], w, h);
            mc2::vegetation::preserveCutoutAlpha(layer[k], coverage[k]);
            const uint32_t nextWidth = std::max(1u, w / 2);
            const uint32_t nextHeight = std::max(1u, h / 2);
            mc2::vegetation::dilateCutoutRgb(layer[k], nextWidth, nextHeight);
        }
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    return array;
}

}
bool GosVegetation::isEnabled() { return !mc2::options::lowCost() && mc2::gate::get<"MC2_VEGETATION_CARDS">(); }
uint32_t GosVegetation::instanceCount() { return populationCount; }
void GosVegetation::setAtlasPath(mc2::czstring path) { atlasOverride = path; }
void GosVegetation::init() {
    if (initialized || !isEnabled()) return;
    const auto envPath = mc2::gate::str<"MC2_VEGETATION_ATLAS">();
    const char* path = !envPath.empty() ? envPath.c_str() : atlasOverride;
    const auto cooked = path ? std::nullopt : builtinCards();
    if (cooked) {
        mc2_vk_vfx_vegetation_atlas(*cooked);
        atlasReady = trimCards = true;
    } else {
        const auto decoded = path ? mc2tex::decodeFile(path, gos_Texture_Alpha, 0) : builtinAtlas();
        if (decoded) {
            const auto array = cardsFromAtlas(*decoded);
            if (!array.levels.empty()) {
                mc2_vk_vfx_vegetation_atlas(array);
                atlasReady = true;
            }
        }
        trimCards = false;
    }
    std::atexit([] { GosVegetation::emitSummary(); });
    initialized = true;
}
void GosVegetation::shutdown() {
    emitSummary();
    populationCount = 0;
    visibility.assign(1024, 2u);
    initialized = atlasReady = false;
    peak = 0;
    frameDraws = sessionDraws = 0;
    reason = "no_call";
    mc2_vk_vfx_vegetation_atlas(mc2tex::TextureArrayCpu{});
    if (mc2veg::State::exists()) mc2veg::State::instance().clear(mc2vk::getOrCreateEngineVulkanContext());
}
uint32_t GosVegetation::populationCapacity(uint32_t requested, float mapSpan) {
    if (!mc2vk::engineVulkanContextValid()) return 0;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(mc2vk::getOrCreateEngineVulkanContext().physicalDevice(), &properties);
    uint32_t cap = mc2::vegetation::storagePopulationCapacity(requested, properties.limits.maxStorageBufferRange);
    // Regions are 128-unit cells, so their count is bounded by the map area whatever the instance count.
    const double side = static_cast<double>(mapSpan) / 128.0 + 2.0;
    const uint64_t regionBound = static_cast<uint64_t>(side * side);
    uint32_t lo = 0, hi = cap;   // largest n in [0, cap] that fits whole (stride 1); fit is monotone in n
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo + 1) / 2;
        const uint64_t regions = static_cast<uint64_t>(mid) < regionBound ? static_cast<uint64_t>(mid) : regionBound;
        if (mc2veg::State::strideFor(mid, regions) == 1) lo = mid; else hi = mid - 1;
    }
    if (lo < cap) mc2::fmt::print_err("[VK_RESIDENCY v1] vegetation capacity sized to fit the largest whole population (no thinning): requested_max={:d} capacity={:d}\n", mc2::fmt::as_u32(cap), mc2::fmt::as_u32(lo));
    return lo;
}

void GosVegetation::uploadInstances(const Instance* data, uint32_t count) {
    if (!initialized) return;
    populationCount = 0;
    if (!mc2vk::engineVulkanContextValid()) return;
    mc2veg::State& gpu = mc2veg::State::instance();
    if (!data || !count) { gpu.clear(mc2vk::getOrCreateEngineVulkanContext()); return; }
    // Placement is immutable between uploads. Group nearby clumps once so the device walk rejects regions instead of revisiting the whole map.
    std::vector<Instance> sorted(data, data + count);
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return cell(a) < cell(b); });
    // Over the preset ceiling or the live GPU headroom: keep every Nth clump (the sort keeps the thinning even per cell) instead of dropping all vegetation.
    {
        size_t regions0 = 0;
        for (size_t k = 0; k < sorted.size(); ++k) if (k == 0 || cell(sorted[k]) != cell(sorted[k - 1])) ++regions0;
        const uint32_t stride = mc2veg::State::strideFor(sorted.size(), regions0);
        if (stride > 1) {
            const size_t before = sorted.size();
            size_t kept = 0;
            for (size_t k = 0; k < sorted.size(); k += stride) sorted[kept++] = sorted[k];
            sorted.resize(kept);
            mc2::fmt::print_err("[VK_RESIDENCY v1] thinned=vegetation stride={:d} instances={:d}->{:d}\n", mc2::fmt::as_u32(stride), mc2::fmt::as_u64(before), mc2::fmt::as_u64(kept));
        }
    }
    std::vector<mc2veg::GpuInstance> gpuInstances(sorted.size());
    for (size_t k = 0; k < sorted.size(); ++k) {
        const auto& i = sorted[k];
        gpuInstances[k] = mc2::vegetation::packInstance(i.x, i.y, i.z, i.yaw, i.scale, i.atlasFrame, i.seed);
    }
    std::vector<mc2veg::GpuRegion> gpuRegions;
    for (size_t begin = 0; begin < sorted.size();) {
        size_t end = begin + 1;
        const auto key = cell(sorted[begin]);
        while (end < sorted.size() && cell(sorted[end]) == key) ++end;
        float lo = sorted[begin].z, hi = lo, pad = 0.0f;
        for (size_t j = begin; j < end; ++j) {
            lo = std::min(lo, sorted[j].z); hi = std::max(hi, sorted[j].z);
            pad = std::max(pad, 1.5f * sorted[j].scale);
        }
        const auto [cellX, cellY] = key;
        gpuRegions.push_back({(cellX + 0.5f) * 128.0f, (cellY + 0.5f) * 128.0f, (lo + hi) * 0.5f,
            std::sqrt(2.0f * 64.0f * 64.0f + (hi - lo) * (hi - lo) * 0.25f) + pad, static_cast<uint32_t>(begin), static_cast<uint32_t>(end), 0u, 0u});
        begin = end;
    }
    if (gpu.upload(mc2vk::getOrCreateEngineVulkanContext(), gpuInstances, gpuRegions)) {
        populationCount = static_cast<uint32_t>(sorted.size());
        peak = std::max(peak, populationCount);
    }
}
void GosVegetation::setBlockVisibility(const uint32_t* data, uint32_t count) {
    visibilityBlocks = count;
    visibility.assign(data, data + count);
}
void GosVegetation::setUnitFootprints(std::span<const Footprint> data) {
    footprintCount = static_cast<uint32_t>(data.size());
    packUnitFootprints(visibility, visibilityBlocks, data);
}
void GosVegetation::flush(float lx, float ly, float lz, float lw, float time,
    float cx, float cy, float cz, float halfMap, float blockSide, int chunkSide) {
    (void)lx; (void)ly; (void)lw;
    frameDraws = 0;
    if (!isEnabled()) { reason="disabled"; return; }
    if (populationCount == 0) { reason="no_instances"; return; }
    if (!atlasReady) { reason="no_atlas"; return; }
    const auto* mvp = gos_GetTerrainMVPMat4();
    auto* pp = getGosPostProcess();
    if (!mvp || !pp) { reason="missing_buffers"; return; }
    const bool force = false;
    const float maxDist = mc2::gate::get<"MC2_VEG_MAX_DIST">();
    const float fadeStart = maxDist*0.5f;
    // Conservative clip-space sphere planes (region sphere and per-instance tests run on the device).
    mc2veg::CullParams cp{};
    for (int plane=0; plane<4; ++plane) {
        const int axis=plane/2;
        const float sign=(plane&1) ? -1.0f : 1.0f;
        for (int c=0; c<4; ++c) cp.plane[plane][c]=mvp[c*4+3]+sign*mvp[c*4+axis];
        const auto* p=cp.plane[plane];
        cp.planeLen[plane]=std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
    }
    cp.clipRow[0]=mvp[3]; cp.clipRow[1]=mvp[7]; cp.clipRow[2]=mvp[11]; cp.clipRow[3]=mvp[15];
    cp.cam[0]=cx; cp.cam[1]=cy; cp.cam[2]=cz; cp.cam[3]=maxDist;
    cp.misc[0]=fadeStart;
    cp.misc[1]=0.5f*pp->getOutputHeight()*std::sqrt(mvp[1]*mvp[1]+mvp[5]*mvp[5]+mvp[9]*mvp[9]);
    cp.misc[2]=std::max(lz,0.0f);
    cp.misc[3]=time;
    cp.grid[0]=halfMap; cp.grid[1]=blockSide;
    cp.grid[2]=static_cast<float>(visibilityBlocks); cp.grid[3]=static_cast<float>(footprintCount);
    cp.counts[3]=chunkSide>0 ? static_cast<uint32_t>(chunkSide) : 0u;
    cp.flags[0]=force ? 1u : 0u;
    cp.flags[2]=trimCards ? 1u : 0u;
    if (mc2_vk_vfx_vegetation_run(cp,mvp,visibility,pp->getWidth(),pp->getHeight())) {
        frameDraws=1; ++sessionDraws; reason="submitted";
    } else reason="gpu_declined";
}
GosVegetation::VegFrameStats GosVegetation::getFrameStats() {
    return {static_cast<int>(populationCount),frameDraws,reason};
}
void GosVegetation::emitSummary() {
    if (!isEnabled() || summaryEmitted) return;
    mc2::fmt::print_err("VEG_SUMMARY instance_count={:d} draw_calls={:d} flush_reason={}\n",
        mc2::fmt::as_u32(peak),mc2::fmt::as_i32(sessionDraws),reason);
    // Upload volume follows population changes, not frames: these two numbers must not grow with the frame count.
    if (mc2veg::State::exists()) {
        const mc2veg::State& gpu = mc2veg::State::instance();
        mc2::fmt::print_err("[VEG_GPU] summary population_uploads={:d} population_bytes={:d} frame_upload_bytes={:d} last_survivors={:d}\n",
            mc2::fmt::as_u64(gpu.stats().uploads),mc2::fmt::as_u64(gpu.stats().uploadBytes),
            mc2::fmt::as_u64(gpu.frameUploadBytes()),mc2::fmt::as_u32(gpu.lastSurvivors()));
    }
    std::fflush(stderr); summaryEmitted=true;
}
