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

## ★★ORACLE BLOCKER (2026-07-05 session — anti-tile/POM/markings ALL DEFERRED, code-grounded)
Attempted anti-tile first (oracle-before-feature). Found NO same-deterministic-camera oracle
exists for ANY of the three remaining features. Chain of proof (all repeatable):

1. **Bookmark sweep (byte-stable) = 0.000 GL-vs-VK on all 11 poses** AND VKon-vs-VKoff = 0.000
   on all 13 poses tested (added near/low-alt/oblique airport bookmarks incl. alt 250-350).
   → VK terrain co-writes ZERO visible pixels at every bookmark camera. GL owns the frame →
   trivially identical. Nothing for a terrain feature to move.
2. **Why:** VK feed captures only `cmd.lodStep==1` (LOD0) patches from SubmitDrawCommands
   (gos_terrain_lod_chunk.cpp:1543). At bookmark free-cameras the LOD-chunk system selects
   NO lodStep==1 chunks in the captured frame, EVEN at alt 350 oblique over the airport apron
   where LOD0 detail visibly should apply (LOD_DIST_THRESH[0]=6000wu should cover it — but the
   bookmark free-camera's LOD origin does not drive the co-write feed the way the natural
   camera does; the "first co-write lod0Patches=7" log fires pre-settle, and the settled
   bookmark frame contributes 0). Airport_lowoblique bookmark renders a perfect near
   concrete+road+grass view (POM+anti-tile SHOULD show) yet VKon-vs-off=0.00.
3. **Free-cap (natural camera) IS the only camera that exercises VK LOD0** — but it is
   ENGINE-DECLARED non-deterministic (gos_visual_capture.cpp:38-43: "single-frame free-capture
   is inherently nondeterministic, the sim is live"). Measured: GL-config self-diff f200 = 0.26%
   (stable), but **VK-config self-diff f200 = 10.68%** (VK path perturbs camera/sim run-to-run);
   GL-vs-VK f200 = 60% dominated by a systematic camera-POSE shift the VK terrain gate induces
   (two screenshots frame the apron in different screen positions). 10.68% jitter swamps the
   subtle anti-tile/POM grass signal → free-cap cannot serve as a same-camera oracle for them.
   (dynshadow got away with free-cap ONLY because its signal — a building shadow blob — is large,
   localized, and robust to a few px of drift; anti-tile/POM/detail grass is not.)

**VERDICT: anti-tile, POM, markings-sidecar all DEFERRED — no oracle, do NOT port blind (redline).**
Mechanism is NOT the blocker (verbatim GL math is ready in terrain_lod_chunk.frag:284-419);
VERIFICATION is. Shipping unverified-parity behind a byte-OFF-only control violates
oracle-before-feature.

★MARKINGS NUANCE (partial good news): mc2_01's DEFAULT runway markings ("21","72G", dashed
centerlines, road markings) come from the LEGACY cement-word + cement/road atlas path, which VK
ALREADY imports and reproduces — runway_overview VKon (FORCE_LOD=0) shows all markings present,
matching GL. The `u_overlaySidecar` "overlay-V2" composite is a SEPARATE env-gated beauty feature
(MC2_TERRAIN_OVERLAY_V2, default-OFF, additive-over-legacy, byte-identical when off / no sidecar)
and mc2_01 has NO deployed overlay sidecar. So markings-in-the-DEFAULT-look are already at parity;
the overlay-sidecar port has neither a default-look gap NOR a map that exercises it here → N/A until
a map ships an overlay sidecar.

★UNBLOCK PATH (next-session, before any of these 3): build a DETERMINISTIC near-camera oracle that
actually drives VK LOD0 co-write. Options: (a) make the bookmark free-camera feed the LOD-chunk
lodStep==1 selection (route bookmark eye into the SubmitDrawCommands LOD origin so a low-alt
bookmark yields lodStep==1 patches → deterministic + VK-exercising); or (b) add a fixed-pose PINNED
game-camera capture mode (freeze the natural camera at a scripted world pose+frame so it's byte-stable
yet still the LOD-driving camera). Either unblocks anti-tile + POM together (both are the same
near-camera detail-normal path, terrain_lod_chunk.frag chunkDetailNormal, fwidth-gated). Est: the
oracle-tooling slice is the real work; the two feature ports are small once it exists.

## Files (this session, terrain)
- Diagnosis tooling (this session): .claude/mech-parity/terrain_freecap.py (natural-cam GL-vs-VK),
  terrain_oracle.py (existing bookmark sweep). Artifacts under .claude/mech-parity/terrain/
  (antitile_baseline_f200, gl_repeat, vk_repeat, airport_lowobl — the self-diff + VKon/off probes).
- GameOS/gameos/vulkan_terrain_island.{h,cpp}, gos_terrain_lod_chunk.cpp
- shaders/vulkan/terrain_tier1.frag
- RenderCore/RendererFeatureRegistry.h (gate registered), scripts/run_smoke.py (allowlisted)
- Oracle artifacts: .claude/mech-parity/terrain/dynshadow_f200/, gateoff_ctrl/
