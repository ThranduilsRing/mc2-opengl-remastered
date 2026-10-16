<!-- doc-status: current | reviewed:2026-10-15 | lane 6 -->
# VK producer census (2026-10-15, lane 6, base e6f9ac11e)

WHAT: every GL-side function or data structure a VK island (`GameOS/gameos/vulkan_*.cpp`) reads, the island that reads it, whether
the data can only be obtained through a GL call today (TRUE dependency) or merely lives in a GL-family file while the bytes are
already CPU-side (PLACEMENT), and the proposed backend-neutral home.
WHY: the owner ruling is "GL is deleted after VK parity"; a GL producer can only be deleted once no island needs a GL context to get
its data. This is the work list for that, read from the code (lane 5's inventory section 8 / section 4 / section 3 is the GL side).

Owner column: L6 = this lane (textures, texture manager, per-frame producers); L2 = lane 2 (shadows, post, present, gl_state_guard);
the island files themselves are consumers and change only where a producer seam changes.

## TRUE dependencies (VK gets the bytes only via a GL call)

| # | producer (file:line) | data | consumer island (file:line) | GL call on the VK side | proposed home | owner |
|---|---|---|---|---|---|---|
| T1 | `gosTexture::createHardwareTexture` `gameos_graphics_texture.cpp:80`; handle -> GL name `gos_GetGLTextureId` `gameos_graphics.cpp:870` | every `gos` texture handle (mcTextureManager nodes: legacy shapes, overlays, mech paint / AO / normal) | `vulkan_legacy_shape_island.cpp:262-276` (`get_tex`), `vulkan_terrain_island_overlay.cpp:132-156` (`ovl_get_tex`), `vulkan_mech_island.cpp:552-563` (`get_tex_view`, ids from `gos_mech_batcher_flush.cpp:910-928`) | `glGetTexLevelParameteriv` + `glGetTexImage` | backend-neutral decode TU (`texture_decode.cpp`) + the decoded RGBA held by the handle (`gos_GetTextureRgba`); BC7 `.ktx2` handles (`gos_NewCompressedTexture2D`) have no CPU pixels yet | L6 (task 2) |
| T2 | `mc2_shadow_island_get_geometry_feed` `gos_static_prop_batcher_shadow.cpp:692`; staging freed at `gos_static_prop_batcher_finalize.cpp:124-125` | static-prop shared VBO / IBO (map-load) | `vulkan_color_prop_island.cpp:1238,1247`; `vulkan_shadow_island_live.cpp:824,835` | `glGetBufferSubData` | keep the staging bytes CPU-side for the map lifetime, hand pointers in the feed (the mech feed already does this, `vulkan_mech_island.h:63-71`) | L6 producer; shadow consumer L2 |
| T3 | `mc2_color_prop_island_get_material_feed` `gos_static_prop_batcher_shadow.cpp:862`; arrays built in `gos_static_prop_batcher_finalize_textures.cpp` | albedo `GL_TEXTURE_2D_ARRAY` alpha-OFF / alpha-ON (`s_texArrayOff/On`), all mips | `vulkan_color_prop_island.cpp:670-728` | `glGetTexLevelParameteriv`, `glGetCompressedTexImage`, `glGetTexImage` | array assembly as CPU layer blobs (RGBA8 or BC7 blocks) in a neutral builder; GL and VK each upload | L6 |
| T4 | `mc2_terrain_tier1_get_frame_feed` `gos_terrain_lod_chunk_feed.cpp:39` (`glHeightSsbo`); bytes enter at `gos_TerrainLodChunk_UploadHeightFull` / `_UploadHeightPatch` `gos_terrain_lod_chunk_upload.cpp:13,576` and are not retained | terrain height field (`mapSide^2` floats) | `vulkan_terrain_island_images.cpp:12-23` (`snapshot_height`) | `glGetBufferSubData` | neutral terrain field store (CPU copy + version) | L6 (task 3) |
| T5 | same feed (`glCementSsbo`); `gos_TerrainLodChunk_UploadCementWordsFull` `gos_terrain_lod_chunk_upload.cpp:547` | per-vertex cement words | `vulkan_terrain_island_images.cpp:408-429` | `glGetBufferSubData` | same store | L6 (task 3) |
| T6 | same feed (`glVisualHeightSsbo`); `gos_TerrainLodChunk_UploadVisualHeightFull` `gos_terrain_lod_chunk_upload.cpp:64` | 4x visual height bake + mip maxes | `vulkan_terrain_island_images.cpp:446-467` | `glGetBufferSubData` | same store | L6 (task 3) |
| T7 | `gos_terrain_indirect_getAtlasGLTex` `gos_terrain_indirect_atlas.cpp:718` (uploaded from a decoded `.ktx2` / RGBA image at `:222-273`, image not retained) | terrain colormap atlas (BC7 or RGBA8) | `vulkan_terrain_island_images.cpp:165-196` | `glGetCompressedTexImage` / `glGetTexImage` | keep the decoded atlas image in a neutral owner, both backends upload | L6 |
| T8 | `gos_terrain_indirect_getCementAtlasGLTex` `gos_terrain_indirect_atlas.cpp:724` (assembled at `:630-670`) | cement / road atlas RGBA8 | `vulkan_terrain_island_images.cpp:255-266` | `glGetTexImage` | neutral atlas assembly | L6 |
| T9 | `gos_GetTerrainNormalArrayTex` `gameos_graphics_bridge.cpp:71` (`terrain_normal_array_tex_`) | terrain material detail-normal array | `vulkan_terrain_island_images.cpp:371-385` | `glGetTexImage` (2D array) | neutral array builder (same shape as T3) | L6 |
| T10 | `gos_TerrainLodChunk_WaterShoreHeightTex` `gos_terrain_lod_chunk.cpp:294`; bytes enter at `gos_TerrainLodChunk_UploadWaterShoreHeight` `gos_terrain_lod_chunk_upload.cpp:360` | water shore height (R32F) | `vulkan_water_island_images.cpp:210-230` | `glGetTexImage(GL_RED, GL_FLOAT)` | terrain field store (CPU copy + generation) | L6 |
| T11 | `gos_TerrainLodChunk_ShorelineMaskTex` `gos_terrain_lod_chunk.cpp:286`; bytes enter at `gos_TerrainLodChunk_UploadShorelineMask` `gos_terrain_lod_chunk_upload.cpp:311` | shoreline mask RGBA8 | `vulkan_water_island_images.cpp:210-230` | `glGetTexImage` | terrain field store | L6 |
| T12 | `pp->getShadowTexture()` via `gos_terrain_lod_chunk_feed.cpp:120-130` | static sun shadow map | `vulkan_terrain_island_images.cpp:683-707,740` | `glGetTexImage(DEPTH)` | VK static shadow producer (VK-PARITY-GAPS row 17) | L2 |
| T13 | `pp->getDynamicShadowTexture()`, `dynShadowArrayTex_`, `dynamicFullMapTex_` (feed fields `glDynamicShadowTex`, `glCsmArrayTex`, `glCsmFullMapTex`) | dynamic / CSM shadow maps (fallback copy when the VK cascades declined) | `vulkan_terrain_island_images.cpp:636-790`; `vulkan_shadow_island_live.cpp:957-962` (`glGetTextureSubImage`) | `glGetTexImage` / `glGetTextureSubImage` | VK shadow island owns them (`mc2_vk_shadow_island_get_*_view`) | L2 |
| T14 | HDRI GL texture `hdriTex_` handed by `gos_postprocess_skybox.cpp:82` (sky) and `gameos_graphics_water.cpp:215` (water) | sky / reflection HDRI (BC6H or RGBA16F mips) | `vulkan_sky_island.cpp:155-201`; `vulkan_water_island_images.cpp:25-104` | `glGetCompressedTexImage` / `glGetTexImage(GL_HALF_FLOAT)` | neutral HDRI decode (the `.ktx2` / `.hdr` loader in `gos_hdri.cpp`) | L2 (post) |

## PLACEMENT (CPU data, only the file is GL-family)

| # | producer (file:line) | data | consumer (file:line) | proposed home |
|---|---|---|---|---|
| P1 | `mc2_color_prop_island_get_frame_feed` `gos_static_prop_batcher_shadow.cpp:785` | prop instances (persistent store + dynamic submits), world->clip | `vulkan_color_prop_island.cpp` run | static-prop scene producer TU (no GL) |
| P2 | `mc2_color_prop_island_get_material_feed` tables (`typeAlbedoLayer`, `packetAlbedoLayer`, uv scales, `typeHotColors`) `gos_static_prop_batcher_shadow.cpp:862` | per-type / per-packet material tables | `vulkan_color_prop_island.cpp` | same TU as P1 |
| P3 | `mc2_color_prop_island_get_light_feed` `gos_static_prop_batcher_shadow.cpp:913` | LightsData CPU (`MC_TextureManager::getLightDataPtr` `mclib/txmmgr_lights.cpp:716`), IBL SH, night | `vulkan_color_prop_island.cpp`, `vk_shared_light_feed.cpp` | light feed TU beside txmmgr lights |
| P4 | `mc2_mech_island_get_geometry_feed` `gos_mech_batcher_api.cpp:19` | mech VBO / IBO CPU bytes (`vboCpu` / `iboCpu`) | `vulkan_mech_island.cpp:882-918` (CPU path; the `glVbo`/`glIbo` names are carried but the readback was deleted) | mech producer TU (drop the GL names) |
| P5 | `mc2_mech_island_get_frame_feed` `gos_mech_batcher_api.cpp:62` | instances, bones, runs, shading params, LightsData CPU | `vulkan_mech_island.cpp:943-1250` | mech producer TU; texture ids become handles (T1) |
| P6 | `mc2_mech_island_get_shadow_feed` `gos_mech_batcher_api.cpp:45` | caster instances / bones / CPU geometry | `vulkan_shadow_mech_casters.cpp` | mech producer TU |
| P7 | `Mc2MechFrameFeed::lightDataGlSsbo` | LightsData GL SSBO, verification only | `vulkan_mech_island.cpp:1100-1112` (`MC2_VK_MECH_FEED_AUDIT` byte-compare) | delete with the GL light SSBO |
| P8 | `mc2_terrain_tier1_get_frame_feed` `gos_terrain_lod_chunk_feed.cpp:39` | LOD0 patch list, MVP, map params, atlas UV params, lighting params | `vulkan_terrain_island_run.cpp:100-600`, `vulkan_terrain_island_overlay.cpp` | terrain frame producer TU (the LOD0 selection is CPU) |
| P9 | `gos_GetTerrainLightDir` `gameos_graphics_tparams.cpp:635` | sun direction | `vulkan_post_screenfx_native.cpp:83`, terrain feed | scene-lighting TU (no GL) |
| P10 | `gos_GetProj2ScreenMat4` `gameos_graphics.cpp` (after `:886`) | projection-to-screen matrix | `vulkan_legacy_shape_island.cpp` | renderer view state (no GL) |
| P11 | `gos_StaticLightMatrixBuilt` `gameos_graphics_tparams.cpp:618` | static light matrix latch | `vulkan_terrain_island_images.cpp` (static shadow generation) | moves with T12 | 
| P12 | `Mc2LegacyMlrFrame` / `mc2_vk_legacy_shape_submit` (MLR draws) | legacy `gos_VERTEX` draws + texture handle | `vulkan_legacy_shape_island.cpp:530-534` | stays a submit API |
| P13 | `mc2_vk_overlay_submit` (overlay / decal verts) | overlay vertex lists + handle | `vulkan_terrain_island_overlay.cpp:355` | stays a submit API |

Not consumed by any island (answers lane 5 open question 4): the GL compute cull output. The colour feed reads the persistent-static
store (`gos_static_prop_batcher_shadow.cpp:754-775`, population-agnostic), not the culled visible set, and no island reads the light
grid (VK-PARITY-GAPS row 1 is open for that reason).

## Status (lane 6, updated 2026-10-15 evening)

| row | state | commit |
|---|---|---|
| T1 | CLOSED: decoded handles carry their RGBA8 (`gos_GetTextureTexels`); pre-compressed `.ktx2` handles carry their BC7 blocks and the islands create BC7 images from them; the texture-object readback is deleted | 242b9582c, e80153837 |
| T2 | CLOSED: both consumers (colour prop, lane 2's shadow island) copy from `mc2_static_prop_geometry_cpu` | 242b9582c, lane 2 e1e21176b |
| T4-T6 | CLOSED: terrain field store | 242b9582c |
| T10 / T11 | CLOSED: shore sidecars in the field store | 970156084 |
| P4 / P7 | CLOSED: mech feeds carry no GL names; the FEED-AUDIT readbacks and `MC2_VK_MECH_FEED_AUDIT` are deleted | fe0de149c |
| T3, T7, T8, T9 | OPEN (see below) | |

Accepted cost (lane 2 review should-fix 1): the static-prop CPU geometry copy (the shared VBO + IBO staging) stays resident for
the map's lifetime in every run, GL-only runs included, where nothing reads it. Measured from the `[VK_COLOR_PROP_ISLAND] prop
geometry uploaded once` lines of the VK tier1 run 2026-10-15T17-43-41 (exe 970156084): mc2_01 1.04 MB, mc2_03 2.33 MB, mc2_10
1.91 MB, mc2_17 2.01 MB, mc2_24 2.74 MB (VBO + IBO). Accepted rather than freed per backend: it is about 0.1 % of the process, GL is deleted after VK parity,
and freeing it would put a backend branch in the batcher. It is released (not just cleared) at map change.

Open, in the order to take them:
- T3 prop albedo arrays (largest: up to 47 MB for a 256x256x134 RGBA8 alpha-ON array on mc2_10, 2-9 MB elsewhere): GL builds
  the mips with `glGenerateMipmap`, so a CPU array builder changes texels unless it reproduces the driver's filter, or VK builds
  its own mips. Either is a texel change: needs the GL vs VK capture pair.
- T7 terrain colormap atlas (BC7 or RGBA8, image not retained), T9 terrain detail-normal array, T8 cement atlas: neutral
  atlas/array owner keeping the decoded image; T7 BC7 can reuse the T1 block path.

## Totals

- TRUE dependencies: **14** (L6: 11 = T1-T11; L2: 3 = T12-T14).
- PLACEMENT: **13** (P1-P13; P7 is verification-only and goes with the GL SSBO).
- Island-side GL readback sites behind them: `rg -c "glGet(TexImage|CompressedTexImage|BufferSubData|TextureSubImage)\(" GameOS/gameos/vulkan_*.cpp`.

## Order this lane takes them (by bytes per mission, among the ones that do not need a CPU BC7 path)

1. T1 (task 2): every `gos` handle texture; BC7 `.ktx2` handles keep a GL readback inside the texture object until a CPU KTX2 path exists.
2. T4-T6 (task 3a): the terrain field store; also fixes `_UploadHeightPatch` not bumping `g_terrainHeightVersion` (VK kept the
   pre-patch heights after a runtime height edit).
3. T2 (task 3b): static-prop shared geometry, CPU bytes in the feed; the shadow consumer switch is a lane 2 seam.
4. Then T3 / T7 / T9 (arrays and atlases, need CPU BC7 blobs), T8, T10 / T11.
