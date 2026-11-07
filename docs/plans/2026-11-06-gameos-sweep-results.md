# GameOS legacy-idiom sweep (branch claude/cxx23-gameos-sweep, base 2d042f852)

Result: 253 -> 71 sites in GameOS/ for LG09/LG08/LG10/LG18 (ratchet --check --worktree: 182 fewer than baseline).
By rule: LG09 167 -> 12, LG10 26 -> 7, LG08 44 -> 36, LG18 16 -> 16.

## What changed
- LG09: functional casts `T(x)` -> `static_cast<T>(x)` on the flagged lines (arithmetic/enum types only; identical semantics,
  mechanical, compile-checked: a pointer-to-int functional cast would fail static_cast and did not occur). color_utils.cpp C-casts likewise.
- LG10: one-sided if-clamps -> `std::min/std::max` (exact same compare order, so NaN/ties unchanged); two-sided -> `std::clamp`
  (NaN passes through, as before). hzb uses `(std::max)` because of the Windows macro.
- LG08: `.first/.second` on map entries/pairs -> structured bindings / std::tie in engine_present_vk_imgui.cpp,
  vulkan_sky_island.cpp, vulkan_water_island_images.cpp, vulkan_terrain_island_overlay.cpp.

## Skipped (behavior or build risk)
- LG18 (all 16): reinterpret_cast of pointers to float*/DWORD* is not a std::bit_cast candidate (no value copy); vec.h/vec.cpp are matrix
  views. gos_crashbundle.cpp write_dword left as is.
- GameOS/src/platform_winbase.cpp, GameOS/include/platform_windef.h (9 LG09): Linux-only, cannot be compile-checked here; `intptr_t(file)`
  is a pointer functional cast (reinterpret_cast), not a static_cast.
- gameos.cpp:45 and platform_windef.h function-pointer types: false positives of the heuristic.
- LG10 skipped: asset_scale.cpp:394-395 (clamp sets a side-effect flag), gameos_graphics_params.cpp:62 (also rewrites ch),
  light_pool_culling.h:67,115 (extra assignments in the body), vec.h:284 (template max(min()) on DATA_TYPE).
- LG08 remaining: iterator `it->second` chains (engine_present_vk.cpp, gos_mech_batcher_*, vulkan_legacy_shape_island,
  vulkan_terrain_island_*, vulkan_vfx_island, vk_hdri_source.h): converting needs reshaping iterator lifetimes; left for a later pass.
- gameos.hpp / toolos.hpp and vendored code untouched.
