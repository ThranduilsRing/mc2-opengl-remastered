# Texture quality tiers, overlay size caps and overlay PBR (lane10 design)

Owner ask 2026-11-03: fix size limits for 4x+ road/runway/cement tiles, give overlays a real normal + ORM, and ship Low/Medium/High/Ultra texture tiers.

## 1. Size-limit inventory (what can stop a big overlay tile reaching the screen)

Overlay tiles (roads, runway edges, class-2 alpha cement) are drawn from the decal/overlay bake as ordinary sampled textures
(`vulkan_terrain_island_overlay.cpp`). They do NOT go through the cement atlas; only class-3 pure cement pads and base-tile cement cells do.

| Cap | Where | Hits 512/1024 px overlays? | Action |
|---|---|---|---|
| Hires gate range [128,1024], default 256, one fixed folder | `terrtxm.cpp` `MC2_OverlayTileHiresSize` | Yes: a 512 tile set is invisible unless the env is set; one edge for all | Replaced by a per-file ladder from the tier max edge down to 128, first file present wins |
| Header resync `<= 1024` | `terrtxm.cpp` initOverlay | No up to 1024 | Kept (documented); 2048 tiles are out of scope (1024 = 16x stock) |
| Overlay texture count `kOvlMaxTex = 480`, dim `<= 8192` | VK overlay island | Count only, at VRAM cost | Kept; per-tier edge bounds VRAM (see section 3) |
| Overlay mips | overlay import blit-mips RGBA8; `MC_MAX_MIP_LEVELS=1` only on the GL manager | No | Kept; sampler `maxLod 16` already minifies HD tiles correctly |
| `kMaxTgaEdge = 16384`, `kMaxSourceBytes = 512 MiB` | texture decode/assets | No | Kept |
| Cement atlas cell must equal `TERRAIN_TXM_SIZE`, 1024-cell cap, no gutters/mips | `gos_terrain_indirect_atlas.cpp` | Only mods with 256/512 base tiles (16384 px / ~1 GB at 1024 cells) | Reported, not changed: base-tile size is not an overlay size; a budgeted cell cap is a follow-up for HD base tiles |
| `texel_scale in (1,2,4,8,16)` | `tools/content_pipeline/texcook.py` | Object textures only | Reported; unrelated to overlays |

Stock 64 px tiles and mods are unchanged: the ladder is per file, a missing hires file falls back to `<TERRAIN_TXM_SIZE>Overlays/`, and mod 64 px tiles keep their priority rule.

## 2. Overlay PBR

- Convention: `<name>_n.tga` (tangent-space normal, OpenGL green-up, RGB8) and `<name>_orm.tga` (R = AO, G = roughness, B = metal, always 0 for roads) next to the albedo in the same `<edge>Overlays/` folder. Missing = flat normal (128,128,255) and ORM (255,255,0)/default roughness, so stock and mod tiles render as before.
- Loader: each overlay albedo handle gets optional sibling handles (probe on first use, cached with the albedo). The VK overlay island imports them as UNORM images and binds them at `set 1, bindings 1 and 2`; the dummy set binds 1x1 defaults.
- Shader (`terrain_overlay_tier1.frag`): when a sibling normal exists the shader uses it in place of the tiled world detail normal and multiplies AO, roughness from ORM into the existing `cdShade` lighting (same function as the terrain concrete-detail path, so overlays and terrain pads light identically). Albedo stays MC2's own; no recolouring.
- Standing owner rule (concrete-road-pbr-detail-not-recolor): take only material detail (normal, roughness, AO) from CC0 sets, keep MC2 albedo. The shared world-tiled detail array (`concrete_detail_road.ktx2` from Poly Haven clean_asphalt, `concrete_detail_pad.ktx2` from concrete_layers_02, cooked by `tools/mc2texcook/cook_concrete_detail.py`) already does that for every overlay; per-tile `_n/_orm` add authored per-tile relief (curbs, markings, cracks) on top.
- Provenance for the sets named by the owner (all CC0, read-only source `C:/Users/Joe/Downloads/GameAsset/Terrain`): clean_asphalt_2k and Road004_2K for paved roads and runways; gravel_road_2k, rocky_trail_2k, dirt_aerial_03_2k for dirt roads; cracked_concrete_2k and concrete_layers_02_2k for cement and pads. Road004 has baked lane markings, so only its normal/roughness channels would ever be used. Dirt roads need a third detail layer (gravel_road); that is a re-cook of the detail array and is queued, not done here.
- Generation for the paved set: `tools/overlay_upscale/gen_paved_road_pbr.py` derives `_n`/`_orm` from the same signed-distance, curb, marking and noise fields the approved road generator builds (it reads the stock 64 px tiles; the source script stays in `G:/mc2-tmp/r3/`).

## 3. Quality tiers

One options.cfg key `TextureQuality` (Long, in the `RenderOptions` table, in the graphics options window): `-1` Auto (shipped default), `0` Low, `1` Medium, `2` High, `3` Ultra.

| Class | Low | Medium | High | Ultra |
|---|---|---|---|---|
| Overlays max edge (px) | 64 (stock) | 128 | 256 | 1024 |
| Overlay `_n` / `_orm` | no | no | yes | yes |
| Terrain layers (cooked `*_albedo.ktx2`, hook) | 512 | 1024 | 2048 | 2048 + material arrays (hook) |
| Objects / UI | unchanged (mip bias hook) | unchanged | unchanged | unchanged |

Auto resolves once, at VK device init, from the physical device-local budget that `vk_render_infra.cpp` already computes: `<= 512 MB` or unknown = Low (the potato floor, no normal maps), `< 2 GB` Medium, `< 6 GB` High, otherwise Ultra. The resolved tier is published through `mc2::options` and logged as `[TEXTURE_TIER] ...`. It ships ON; there is no env gate. The terrain-material Ultra row hangs off the same value (`TextureTier::Ultra`); the terrain array binding is a separate lane.

## 4. Proof plan and known gaps

Rung: C++ harness/unit for the tier resolver and ladder, shader contract rows, smoke-lite. In-game tier shots, `ab.py` day and night, RGP captures and per-tier VRAM are listed in the lane report with what was and was not obtained.
