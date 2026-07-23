# VK-P10 TERRAIN as in-place shared-GBuffer region — recon verdict (CORRECTED)

Verified @ `cf3da40f`. ★**Supersedes the terrain sizing in `.claude/VK-P14-RECON.md`, which pointed at
RETIRED code.**

## ★STALE-RECON CORRECTION
`VK-P14-RECON` sized terrain L→XL against `gos_terrain_indirect.cpp` (recipe/thin SSBOs, 2 compute
passes, `glMultiDrawArraysIndirect`). That INDIRECT path is **RETIRED** on this branch
(INDIRECT-BRIDGE-RETIRE-1 / PATCHSTREAM-THIN-RETIRE-1): `mclib/txmmgr.cpp:3069-3078` does NOT call
`DrawIndirect()`, sets `modernHandled=true`, fires a regression tripwire. Leave the indirect TU +
`gos_terrain.frag` + `gpu_driven_terrain_solid.comp` ALONE (retained only for water reflection,
`gamecam.cpp:544`).

## LIVE terrain = LOD-chunk plain indexed draws. Size **M** (probe **S**).
`mc2TerrainLodChunkEnabled()` DEFAULT-ON (`mclib/terrain.cpp:143`; OFF path vestigial, no production
renderer). Path: `Terrain::flushDrawCommands()` (`gamecam.cpp:530`) →
`gos_TerrainLodChunk_SubmitDrawCommands()` (`GameOS/gameos/gos_terrain_lod_chunk.cpp:782`) = CPU loop of
`glDrawElements(GL_TRIANGLES,...)` over cached patch VBOs (`:1453` + skirts `:1480`). **NO compute, NO
MDI, NO recipe/thin/fence ring.** Simpler on the draw side than props/mechs (no batcher instancing).

## GBuffer contract (2 MRT, same shape as props, ONE alpha difference)
- loc0 FragColor (color), loc1 GBuffer1 RGBA16F `.rgb=normal*0.5+0.5`, `.a`=shadow-class
  (`terrain_lod_chunk.frag:136-137`).
- ★**Terrain `.a = 1.0`** (`rc_gbuffer1_shadowHandled`, literal `vec4(0.5,0.5,1.0,1.0)` `:430,1078`) =
  "pixel handles own shadow, shadow_screen.frag SKIPS it". PROPS use `.a=0.25`. VK terrain MUST emit
  `.a=1.0`, NOT the prop encoding. Same 16F, same normal encoding, no space mismatch. No objectId attach.

## Contiguity — YES, clean VK-owned opaque PREFIX (1 flip)
Opaque order: shadow (`gamecam.cpp:505`) → terrain (`:530`, BEFORE renderLists `:535`) → props
(`txmmgr.cpp:3335`) → mechs (`:3384`) → overlays. Terrain is the bottom, draws first. VK terrain =
write terrain color+normal+depth into the shared attachments first → ONE ownership flip to GL for
props/mechs. Exactly the "contiguous VK prefix, minimize flips" invariant. (Water reflection is a
separate LATER region — out of scope.)

## Hard-part ranking (live path)
1. **(HIGHEST) AMD state contract under VK** — double-sided (`glDisable(GL_CULL_FACE)` because terrainMVP
   bakes the GL-NDC X-flip → inverted winding, `:883,905`), reverse-Z `GL_GEQUAL` (`:856,882`), explicit
   depth-write ownership (Phase-10.3 "transparent terrain" regression `:837-845`). Port to VK pipeline
   fixed winding/cull/depthCompare/depthWrite EXACTLY. This is the real work.
2. **(MED) SSBO+texture binding** — height SSBO (`TERRAIN_HEIGHT_SSBO_BINDING`, `:909`), type(24),
   cement(25), visual-height(26), colormap/cement-atlas/shadowMap + `shadow.hglsl`. All GpuBufferOwner-
   registered → interop tractable. VBO = int16 (lx,ly) local coords, trivial to share.
3. **(LOW) the draw** — `glDrawElements` loop → `vkCmdDrawIndexed` loop. Boring.
4. **(N/A) compute** — live path has NONE (baked NdotL in-frag `terrain_lod_chunk.frag:889`).

## Smallest probe (S)
Port the LOD-chunk opaque draw to a VK region consuming the GL height/type/cement SSBOs + colormap/atlas
via interop, writing color0 + gbuffer1(.a=1.0) + depth IN-PLACE into the shared attachments; keep CPU-side
chunk selection/patch-cache on GL/CPU. First sub-slice: near band (LOD0) or single patch, A/B pixels vs GL
static camera → proves in-place co-write + `.a=1.0` handoff + reverse-Z/double-sided winding under VK
before scaling to all bands/skirts. Do NOT drag in the retired indirect/compute code.

## Top-3 risks
1. Reverse-Z + baked-MVP X-flip winding under VK (z-fight props / see-through terrain — the Phase-10.3
   regression). Validate against the shared scene depth (`MC2_VK_SHARED_SCENE_DEPTH`) first.
2. gbuffer1 `.a=1.0` fidelity (default/copy-prop's-0.25 → double-shadow/terminator corruption).
3. Feed coherency on skirt/edge/LOD-stitch chunks (CPU-computed per frame; present a coherent SSBO
   snapshot, NO CPU readback in the handoff).

## Files (LIVE port — NOT the indirect ones)
`gos_terrain_lod_chunk.cpp` (:782 submit, :908-915 binds, :1453/:1480 draws, :837-884 state contract),
`shaders/terrain_lod_chunk.frag` (:136-137 MRT, .a=1.0), `shaders/terrain_lod_chunk.vert` (height-SSBO
fetch), `render_contract.hglsl:20,37,52,92` (.a contract), `terrain.cpp:143`, `gamecam.cpp:530`,
`txmmgr.cpp:3054-3094`.
