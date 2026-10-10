<!-- doc-status: stale | reviewed:2026-10-11 -->
# TRACKV-VFX-PAYOFF-OPUS-1 — recon synthesis + plan

Branch `claude/trackv-vfx-payoff-1`, worktree `mc2-trackv-vfx-payoff`, off
canonical `claude/nifty-mendeleev` @ `7e5bb571` (after
TRACKV-LIGHTING-CONSISTENCY-OPUS-1 merged — required to avoid
`visual_tuning_profile.cpp` conflicts).

Goal: make VFX visually benefit from the post/lighting stack — soft particles,
simple lit smoke/dust, intentional bloom participation, capture/debug support.
NOT a GPU-sim parity opus.

## Recon ground truth (5 subagents A-E, all file:line verified)

### Render path (A)
- 5 gosFX classes GPU-routed via `mclib/particles/spawn.cpp:81-105`: Card,
  CardCloud, PointCloud, ShardCloud, Tube. Others CPU-MLR only.
- Blend mode is per-draw-group from the effect's `MLRState::AlphaMode`
  (`spawn_card.cpp:139-145`): 0=alpha (`GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA`),
  1=additive (`GL_SRC_ALPHA,GL_ONE`). Bridge applies at
  `gos_particle_bridge.cpp:452-457`. FS already receives `u_vfxIsAdditive`
  (`:462`, `particle_billboard.frag:37`) — the ready-made lit-vs-emissive
  discriminator.
- Beams/lasers/tracers are legacy CPU `gos_vertex` MLR (`weaponbolt.cpp`), NOT
  in the GPU particle path — out of scope.
- Shaders already evolved beyond docs: `particle_billboard.frag` has
  `u_debugMode` (0-4), `u_vfxBrightness`/`u_vfxAdditiveBrightness`/
  `u_vfxAlphaScale`, `u_vfxIsAdditive`. VFX-DEBUG-VIEWS-1, VFX-TUNING-UI-1,
  VFX-AGE-SAMPLE-1 all already shipped (recon docs are stale on this).
- Particle VS uses flat `u_worldToClipGL` (not the ViewUniforms UBO) +
  `u_cameraRight`/`u_cameraUp` view-aligned basis.

### Soft particles (B)
- Scene depth IS a sampleable texture: `gosPostProcess::getSceneDepthTexture()`
  → `sceneDepthTex_` (`GL_DEPTH24_STENCIL8`, `gos_postprocess.cpp:470-478`).
- BUT particles draw INTO the scene FBO whose depth they would sample →
  GL feedback loop = UB. **No depth copy/blit exists today.** Smallest honest
  MVP therefore = a gated depth-copy blit (~30 lines mirroring existing FBO
  creation) + shader fade. NOT a one-liner; small-medium.
- Reverse-Z confirmed active: `glClearDepth(0.0)`, `glClipControl(...,ZERO_TO_ONE)`,
  `GL_GEQUAL` everywhere. Window depth near=1.0 far=0.0. **SSAO's own
  depth-convention comments look forward-Z and may be latently wrong — do NOT
  copy SSAO depth math; re-derive from near=1/far=0.** `inverseViewProj_` is
  already plumbed (`gos_postprocess.cpp:864`) for linear reconstruction.
- First target: CardCloud (alpha smoke workhorse). Gate `MC2_VFX_SOFT_PARTICLES`
  default OFF; skip the blit entirely when off → zero added cost.

### Lit particles (C)
- Sun + ambient reachable WITHOUT new architecture via global `eye` camera
  (`mclib/camera.h`): `eye->lightDirection` (Stuff space), `eye->lightRGB`,
  `eye->ambientRGB` — same source terrain uses (`gos_terrain_lighting.cpp:719-730`).
- Light only alpha groups (`u_vfxIsAdditive==0` = smoke/dust); additive
  flashes/lasers/PPC stay emissive.
- Simplest model = wrapped hemispheric sun+ambient fill (no per-fragment normal
  needed). Sun-dir coordinate space: keep dot product in Stuff space before the
  `(-x,z,y)` swap, OR swap the dir.
- Tunable `vfxLitStrength` profile key — copy the just-merged mech-ambient key
  pattern in `visual_tuning_profile.cpp:205-217` EXACTLY, incl. the
  `saveCurrentToMission` round-trip (`:328-329`) or it silently drops.
- Gate `MC2_VFX_LIT_PARTICLES` default OFF; strength tune var default-noop.

### Bloom (D)
- VFX ALREADY feed bloom. Scene FBO is RGBA16F unconditionally
  (`gos_postprocess.cpp:462`, not HDR-gated). Particles draw into it before
  bloom/tonemap; `runBloom()` bright-passes `sceneColorTex_` directly. Additive
  groups accumulate >1.0 (no clamp anywhere on scene→bloom path) and bloom
  naturally. **There is no clamp to fix and no plumbing to add.**
- Sparse single sprites may sit below the tuned 1.2 threshold; dense additive
  accumulates over. Lever already exists: `MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS`
  + per-mission `bloomThreshold`/`bloomIntensity`.
- => Bloom slice RECLASSIFIED to **doc/tuning only**. Do NOT add
  `MC2_VFX_BLOOM_SCALE` (duplicates the additive-brightness tuner).

### Capture (E)
- Best missions: `mc2_10` (primary VFX combat), `mc2_24` (mech/dense). The
  `tests/smoke/matrices/vfx.json` matrix already defaults to both.
- `run_smoke.py` flag is `--mission` (singular, repeatable), NOT `--missions`.
  No `--kill-existing` (forbidden). `--keep-logs` always.
- VFX debug modes 0-4 already exist (`MC2_VFX_DEBUG_MODE`).
  `capture_baseline.py --vfx-debug-mode N`; `smoke_with_screenshots.py` for PNGs.
  Presets `vfx_combat_10`/`vfx_combat_24` exist.
- Particles are TRANSIENT — confirm activity via log (`GOSFX_GPU sprites=N`),
  not by assuming a frame is populated.
- Stale `--kill-existing` in `VISUAL-CAPTURE-MATRIX-1-DESIGN.md:110`; MCP
  `run_capture_baseline` emits `--missions`+`--kill-existing` (broken/forbidden).
  Flag/fix in capture slice.

## Classification

### MUST-DO MVP
1. `VFX-GATE-DEFAULT-OFF-GUARDS-1` — register `MC2_VFX_SOFT_PARTICLES` +
   `MC2_VFX_LIT_PARTICLES` as Feature gates (enum + kFeatureTable rows + COUNT
   bump + guardrail test), default OFF. No consumer yet → zero behavior change.
2. `VFX-SOFT-PARTICLES-MVP-1` — gated depth-copy blit + reverse-Z linear
   soft-fade in `particle_billboard.frag`, alpha groups (CardCloud first).
3. `VFX-LIT-PARTICLES-MVP-1` — wrapped sun+ambient on alpha groups from
   `eye->light*`; `vfxLitStrength` profile key; `MC2_VFX_LIT_PARTICLES` gate.
4. `VFX-BLOOM-PARTICIPATION-1` — **doc/tuning only**: document that VFX already
   bloom; add per-mission `vfxBloomBoost` profile key mapping to the existing
   additive-brightness setter (default 1.0 no-op); capture proof on mc2_24.
5. `VFX-PAYOFF-CAPTURE-MATRIX-1` — extend `vfx.json` matrix with debug-view +
   payoff-stack entries; doc; fix stale `--kill-existing`.

### NICE-TO-HAVE (defer)
- Pseudo-spherical billboard normal for lit particles (option B).
- Promote VFX debug modes into canonical `RenderDebugView` mask
  (`kDebugViewMask_Vfx==0` today).
- Fix MCP `run_capture_baseline` `--missions`/`--kill-existing` bug.
- ImGui slider for soft-particle fade distance.

### BLOCKED / DEFER
- Deterministic scripted-fire hook (prerequisite for pixel-exact VFX baselines).
- Nothing hard-blocked: depth IS sampleable, so soft particles proceed.

### DO-NOT-TOUCH (hard global constraints)
- GPU-sim parity / CardCloud compute, stable particle IDs/birth tracking,
  CPU-sim bypass/deletion, decals, heat/wetness/steam, HZB/culling, asset cook,
  gameplay semantics, default visual behavior. SSAO depth-convention bug =
  flag only, out of scope.

## Commit structure
1 gates → 2 soft → 3 lit → 4 bloom doc/tuning → 5 capture. Each independently
committable, default OFF / byte-identical.
