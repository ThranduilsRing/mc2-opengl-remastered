# VK-TERRAIN sub-feature oracle + dynamic-shadow ship (2026-07-05)

Session: per-actor mech scheme cache + co-covered mech oracle + terrain material completion.
Branch `claude/vk-interop-bridge-1`. Deploy `A:/Games/mc2-opengl/mc2-win64-0.4c`.

## ORACLE METHOD (terrain)
Bookmark cameras frame mostly COARSE-LOD terrain that GL draws (VK terrain only co-writes
LOD0) → GL-vs-VK bookmark diff = 0.000 on all 11 mc2_01 bookmarks (VK LOD0 matches GL, but
the residual features aren't exercised). The exercising camera is the NATURAL close game
camera (free-capture, `MC2_VISUAL_CAPTURE_FRAME=200`, no bookmark) where VK LOD0 fills the
screen and the base structures are visible. Driver `.claude/mech-parity/freecap_driver.py`;
bookmark control `.claude/mech-parity/terrain_oracle.py`.

★Free-capture caveat: the natural camera drifts slightly run-to-run (not byte-stable), so
free-cap numbers carry ~8-10/ch drift noise in grass. Use free-cap for SHADOW-REGION numbers
(large, localized) and the BYTE-STABLE bookmark sweep for the gate-OFF control.

## SHIPPED: MC2_VK_TERRAIN_DYNSHADOW (commit 43e6bbc2)
Dynamic (building/mech) sun shadow on VK terrain — legacy single-map (non-CSM) path.
- Feed (gos_terrain_lod_chunk.cpp): pp->getDynamicShadowTextureRaw() + getDynamicLightSpaceMatrix()
  + dynShadowsEnabled (shadowsEnabled_ && getDynamicShadowFBO() && !CSM).
- Island (vulkan_terrain_island.cpp): 2nd D32 depth-compare image (binding 7), slot-parameterized
  ensure_shadow_image_slot + copy_shadow_slot (refactored the static path to share). UBO 256->512B.
- Frag (terrain_tier1.frag): shadow = min(static, dyn); pureConcrete -> dyn only. Reuses
  computeBakedShadow (VERBATIM legacy calcDynamicShadow: adaptiveScale 2.4->0.8, mix(0.4,1.0), 8 taps).

★ORACLE (mc2_01 free-cap f200, hut_shadow_blob region = building shadow on concrete):
  BEFORE (GL vs VK-nodyn): mean 81.9, 92% covered  (no dynamic shadow — bright concrete)
  AFTER  (GL vs VK-dyn):   mean  4.0,  5% covered  (building shadow lands, matches GL)
★GATE-OFF CONTROL (byte-stable bookmark sweep): dynshadow OFF vs ON = BYTE-IDENTICAL on all
  11 mc2_01 bookmarks → feature inert where no dynamic caster's shadow lands. Discipline held.

## DEFERRED terrain sub-features (oracle-before-feature plan for each)
Per handoff dependency order, dynamic-shadow was first (mechanism-proven by spike 773e514e).
Remaining, each its own gate riding MC2_VK_TERRAIN_LIGHTING, oracle at free-cap f200:
- **anti-tile** (`sampleAntiTileArr`, terrain_lod_chunk.frag:284-355): variance-preserving
  hashed-offset resample + fwidth per-layer near-gating. Oracle: grazing-angle grass tiling
  seams. Medium — needs the ddx/ddy gradient plumbing + hash.
- **POM** (`chunkParallax`, :282-357): per-material height-alpha ray-march (data-dependent loop,
  needs the atlas ALPHA displacement channel imported + pomParams). Oracle: steep-angle relief
  on rock/concrete. Larger — data-dependent loop + a new alpha texture import.
- **markings/overlay sidecar** (:626-660, u_overlaySidecar + u_overlayBounds): world-XY->UV
  authored cement/pad/RUNWAY overlay composite. Oracle: runway numbers on a runway map
  (mc2 airport). Medium — one more copy-once texture + world-bounds UV + additive composite.
- hemisphere ambient (:929-942, terrainLightingV1Strength) — env-gated OFF by default in GL
  too, so not a default-look gap. macro-variation (:860-887) similarly ImGui-gated.

## Files (this session, terrain)
- GameOS/gameos/vulkan_terrain_island.{h,cpp}, gos_terrain_lod_chunk.cpp
- shaders/vulkan/terrain_tier1.frag
- RenderCore/RendererFeatureRegistry.h (gate registered), scripts/run_smoke.py (allowlisted)
- Oracle artifacts: .claude/mech-parity/terrain/dynshadow_f200/, gateoff_ctrl/
