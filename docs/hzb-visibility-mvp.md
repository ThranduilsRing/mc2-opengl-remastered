<!-- doc-status: current | reviewed:2026-10-19 -->
# Vulkan static-prop HZB culling

The native Vulkan static-prop color pass uses a same-frame reverse-Z MIN
pyramid. It is render-only: input instance feeds, shadows, AI and logic
visibility are unchanged. `MC2_HZB_BUILD` defaults ON; `=0` keeps all color
instances and skips the pyramid. The old GL reducer and readback probe are
retired, including `MC2_HZB_PROBE` and the redundant `MC2_VK_POST_HZB` gate.

## Data flow

1. After earlier Vulkan depth writers and before the static-prop color draw,
   `GameOS/gameos/vulkan_post_hzb_native.cpp` samples the current shared depth.
2. `shaders/vulkan/hzb_reduce.frag` copies level zero, then MIN-reduces exact
   integer 2x2 footprints. Separate R32F images use ceil-sized dimensions, so
   odd source extents lose no texels.
3. `RenderCore/hzb_prop_bounds.h` projects each type's indexed-mesh AABB with
   this draw's matrix and depth bias. Missing/invalid bounds, viewport or
   near-plane intersections keep the whole object. Each padded footprint
   selects its own LOD.
4. `shaders/vulkan/hzb_cull.comp` reads every tile overlapping that footprint.
   An object is hidden only if its nearest depth is less than the footprint
   MIN minus 0.00002. Sky depth zero therefore prevents occlusion.
5. The GPU mask is consumed by the native color vertex shader. A failed build
   keeps all instances. No CPU visibility or gameplay state is filtered.

See `docs/hzb-depth-convention.md` and `tests/unit/test_depth_hzb.cpp` for the
mathematical contract. The options window exposes native dimensions, level
count and build count. `[VK_POST_HZB_NATIVE] [CULL]` logs tested/culled counts;
health records expose native use and fallback reasons.

## Initial proof (lane 2 s64)

Five standard saved poses plus the mc2_10_saved_204611 horizon were compared
on/off and viewed. Five were pixel-identical; the horizon had five water
pixels differing by more than 24, with no visible geometry disappearing.
Input feed dumps were byte-identical.

One uncapped 30-second pair per mission, two physical CPU cores, same binary:

| Mission | OFF median ms | ON median ms | Delta ms | Sampled peak culled |
|---|---:|---:|---:|---:|
| mc2_10 | 6.32 | 6.45 | +0.13 | 244 |
| mc2_24 | 5.68 | 5.80 | +0.12 | 0 |

This pair shows a small cost. It establishes no performance win. Captures and
raw performance records are archived under E:/mc2-archive-2026-10/lane2-s64.
