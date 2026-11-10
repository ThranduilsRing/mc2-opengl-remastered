// water_reflect_dispatch.h -- dispatch-size math of the water reflection compute (header-only, no Vulkan).
//
// The reflection pass runs one 8x8 workgroup per water tile as an indirect 2-D dispatch kReflectRowTiles wide, so the tile
// count is bounded by maxComputeWorkGroupCount[1] (Vulkan spec minimum 65535) times the row width, not by [0]. Groups past
// the live tile count read kReflectSentinelTile (written by water_draw_args.comp.slang) and return.
#ifndef MC2_WATER_REFLECT_DISPATCH_H
#define MC2_WATER_REFLECT_DISPATCH_H

#include <cstdint>

namespace mc2vk {

constexpr uint32_t kReflectRowTiles = 1024;                 // keep equal to ROW_TILES in water_draw_args.comp.slang / water_reflection.comp
constexpr uint32_t kReflectSentinelTile = 0xFFFFFFFFu;

struct ReflectDispatch { uint32_t x, y; };

constexpr ReflectDispatch reflectionDispatchDims(uint32_t count) noexcept {
    return count == 0 ? ReflectDispatch{0, 0}
                      : ReflectDispatch{count < kReflectRowTiles ? count : kReflectRowTiles,
                                        (count + kReflectRowTiles - 1) / kReflectRowTiles};
}

// Tiles one frame may dispatch: `cap` (the tile-list capacity) limited by the device's maxComputeWorkGroupCount[1].
constexpr uint32_t reflectionMaxTiles(uint32_t cap, uint32_t limitY) noexcept {
    const uint64_t byDevice = static_cast<uint64_t>(limitY) * kReflectRowTiles;
    return byDevice < cap ? static_cast<uint32_t>(byDevice) : cap;
}

// Sentinel slots draw_args must write after the live tiles so the last row's spare groups read it.
constexpr uint32_t reflectionPadTiles(uint32_t count) noexcept {
    const ReflectDispatch d = reflectionDispatchDims(count);
    return d.x * d.y - count;
}

}  // namespace mc2vk

#endif
