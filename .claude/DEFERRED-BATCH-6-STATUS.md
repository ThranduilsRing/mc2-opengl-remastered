# DEFERRED-BATCH-6 status — 2026-07-22/23

Branch `claude/deferred-sweep-1`, worktree A:/Games/mc2-deferred-1.
Deployed to releases/mc2-win64-deferred-test (check manifest src_commit for
the live tip). Evidence: `.claude/captures/batch6/` (this worktree).

## Shipped

| Slice | Commits | Gates | Evidence |
|---|---|---|---|
| CAMERA-PARK port (item-2 prereq) | 2944c90b (pick of fe5509b0) + 848e7171 (smoothing interop: park freeze clears scroll/rot smoothing goals) | MC2_CAMERA_PARK (infra, allowlisted) | park fired at all batch6 poses ([CAMERA_PARK v1] lines in capture logs) |
| DEFS-UI cherry-pick (item 4) | 3500dadd / 569b15c1 / 4539cd31 (fixtures) + 0111aab4 (a2faa2af: briefing wrap, defs list icons, meter bridge, list clip — 17 files) | (existing ui gates) | conflicts resolved: missionbegin.cpp CRLF/LF whole-file conflict re-applied by hand (LF preserved); bay click-through = USER |
| SHADOW-FADE-CONTINUOUS-1 (item 1) | 2fc69061 | MC2_SHADOW_FADE_CONT (default ON, =0 restores 4-step ladder) | i1_fade_cont vs i1_fade_step (fade=40 exaggeration): step version shows abrupt fade boundary, continuous dissolves smoothly along shadow length |
| LIGHT-CULL-RADIUS-1 + MOVER-LIGHT-FORWARD-1 (items 5+9) | ea0b3363 | MC2_LIGHT_CULL_RADIUS (default ON), MC2_LIGHT_POOL_MOVER_FWD (default 24wu) | i5_cull_204907_fix/legacy, i5_edge_*; headless does NOT reproduce the live dropout (all updates run + positions project on-screen at parked poses) — mechanism fix, live A/B at 204753 vs 204907 = USER |
| POOL-CONSUMERS-1 (item 6) | 7dcbcd3a + 0418535b (regression fix) | MC2_LIGHT_POOL_OBJECTS (default ON, under MC2_TERRAIN_LIGHT_POOLS), MC2_LIGHT_POOL_OBJ_SCALE (default 0.7) | props/movers sample the pool SSBO (shaders/include/light_pools.hglsl) |
| WATER-SHORE-MASK-1 (item 3) | 3c864eac | MC2_WATER_SHORE_MASK (default ON) | i3_shoremask_on/off + crops: bright teal staircase ring strongly reduced at the mc2_10 worst-water pose |
| Registry regen | 53337108 | — | check-env-registry PASS (861 vars) |

## Root causes (one line each)

1. **Mech-shadow fade step** — SHADOW-DIST-FADE probed only 4 discrete
   reference depths; dNorm quantized to 5 levels drove BOTH fade smoothstep
   and penumbra radius at coherent boundaries. Now 8 midpoint refs +
   per-pixel half-step ladder jitter (all 3 sampler paths).
2. **Light pools popping / cluster dropouts (5+9, probably 7)** —
   `Camera::updateLights` position-only screen test overwrote owner intent
   every frame, and all owner light writers live in frustum-culled
   updateGeometry. Now: `TG_Light::ownerEnabled` (owner FoW/status intent,
   refreshed per-frame in update() for movers) composed with a light-VOLUME
   frustum test (pos ± farDistance probes). Buildings persist owner intent
   (static positions) + camera-side night gate.
3. **Teal staircase water ring** — the WATER shader's shallow tint + shore
   alpha smoothstep runs on tile-quantized WaterThickness and never consulted
   the cooked shoreline_mask (proof: MC2_TERRAIN_SHORELINE=0 changed 0.75% px).
   Water MDI frag now feathers the shore band along the mask signed-distance.
4. **Props-missing regression (caught live by user)** — my light_pools include
   initially sat inside `#ifdef MC2_COALESCE`; legacy static_prop variant
   failed to compile → GPU prop path disabled → shadows without meshes.
   Fixed 0418535b; propcheck_fixed.tga = full props, 0 shader errors.

## Item 2 (WATER-ZFIGHT-LOWANGLE) — honest state

True grazing pose "2453.333,-1984.0001,-176.75,12.5,2599.08@400" on mc2_01
NOW CAPTURABLE (camera park). Frame-pair diff (460 vs 462) shows NO gross
z-fight at that pose headless; [WATER_DEPTHPROBE v2] shows water/terrain MVP
fingerprints EQUAL every frame. NOT claiming fixed — the live artifact may
need camera motion/scroll or longer observation. Next: live-assisted session
with MC2_CAMERA_PARK + user confirming the artifact on-screen, then
MC2_WATER_DEBUG_MODE=2 alpha view at that exact state.

## Item 7 (world-shadow asymmetry)

i7_asym_204611 + i5_cull_204753_fix show the compound EVENLY lit after the
cull fix — hypothesis: the "asymmetry" was one cluster's lights culled by the
angle-dependent position test (same root as item 9). USER-EYEBALL to confirm
at the original poses; if a terrain-shadow asymmetry remains, reopen.

## Deferred (explicit, with reasons)

- **Generator emissive glow + runway edge-marker glows + tower-spotlight
  aim-direction bias** (mid-batch additions): new features, not fixes; pool
  budget (64-light cap) + "occupied base, not floodlit" taste tuning should
  be dialed with the user present. Pointers: bdactor SpotLight_ registration
  (bdactor.cpp ~3150-3292) for per-prop lights; marker splat alternative =
  overlay/decal pass. MOVER fwd-bias machinery (mover_light_fwd_offset) is
  reusable for the tower aim bias once node orientation is extracted.
- **Runway-edge blend apron + pad-outline fringe (204753/204950)**: graded
  as PRESENT in captures (hard asphalt→dirt line). Transition-tile ownership
  class (MISSING-RUNWAY-1 relative); needs the tile-census attribution pass.
- **Turrets not animating**: bdactor.cpp:3326 `if (inView || g_useGpuStaticProps)`
  gates spinMe/anim — with GPU props ON this runs regardless, so the frozen
  turrets the user saw at the 204907 pose need a separate repro; not fixed
  this batch. Investigate BldgAppearance::update anim path (frameNum advance)
  vs turret object update.
- **Shoreline GEOMETRY staircase**: the mask feather cures the glow ring;
  the water polygon outline itself is still tile-quantized mesh — needs the
  shorecontour mesh work (mc2-shorecontour arc), out of batch scope.
- **Cook shoreline masks for more maps** (mc2_01/05/18 + water-heavy set):
  tooling `tools/terrain_beautify/cook_shoreline.py`; mc2_10 verified working.
  Do after user confirms the mc2_10 look.

## Composite taste check

i7_asym_204611 doubles as the occupied-base night composite (all systems on,
defaults): base reads lit-but-night. PAD-AREA-GLOW at 204753 is the
brightest term — if anything reads floodlit to the user, dial
MC2_PAD_AREA_GLOW down first.
