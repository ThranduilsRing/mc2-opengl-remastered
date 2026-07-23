# FAR-BACKDROP-LIT-IMPL-1 — implementation + verification status 2026-07-23

Branch `claude/nifty-mendeleev`, worktree A:/Games/mc2-main.
Lane FAR-BACKDROP-LIT-IMPL-1. Commit **`98ed1d32`**. Gate **`MC2_FAR_BACKDROP_LIT`** (default-OFF).
Follows the design in `FAR-BACKDROP-TEXTURED-LIGHT-1-STATUS.md` (option A).

## Headline

Option A **implemented, built, deployed, gate-wiring verified working** — and the
verification bar **disproved the design's root-cause hypothesis for the specific
mc2_10 headline symptom**: the fullbright white horizon "impostors" are **NOT on
the static-prop path**. RenderDoc + the engine's own `[OBJBATCHER]` counter show
`submit_trees=0` at this pose, and the white pixels are produced upstream of the
fullscreen tonemap composite in the HDR scene FBO by a **separate program** — the
design's own flagged "distinct backdrop blit" caveat, now confirmed with hard
evidence. My static-prop fix is correct + safe + default-OFF and correctly
night-shades genuine far static-prop geometry (buildings proven under
exaggeration), but it cannot touch this particular defect. Gate stays default-OFF.

## What shipped (commit `98ed1d32`)

Distance-gated shader-side `base_light` night-substitution for far static-prop
geometry. Realized **distance-gated via `clip.w`** rather than a per-instance
far-LOD flag, because the steady-state far impostors flow through the
persistent-registry replay path (`markVisibleChecked`) where instance flags are
frozen at registration and only `lightDataIndex` is patched per frame — a
per-frame far-LOD flag would need registry + VK-island-mirror plumbing (high
blast radius, and the VK island struct is owned by a live lane). `clip.w`
(view-depth) works identically for replay and dynamic instances with zero
per-instance state.

Files:
- `shaders/static_prop.vert` — non-window branch: `seed = mix(base_light,
  ambientTerm + u_farBackdropTint, fbT)` where `fbT` fades 0→1 across
  `[u_farBackdropNear, u_farBackdropFar]` on `clip.w`, scaled by
  `u_farBackdropStrength`. `u_farBackdropLit==0` → `fbT` never used → seed ==
  base_light → byte-identical. Lifts black-seeded far geometry UP and pulls
  white/day-seeded geometry DOWN to the mission night object-light ambient in ONE
  op, no hard LOD-pop boundary.
- `GameOS/gameos/gos_static_prop_batcher.cpp` — env statics + `ProgramLocs`
  fields + `glGetUniformLocation` + per-frame `glUniform*` upload in BOTH the
  coalesce and legacy uniform blocks. OFF → uploads `u_farBackdropLit=0`.
- `RenderCore/RendererFeatureRegistry.h` — `FarBackdropLit=46` enum + kFeatureTable
  entry + 4 kAuxEnvVars tunables (NEAR/FAR/STRENGTH/TINT).
- `docs/tier1_env_vars.md`, `scripts/run_smoke.py` passthrough allowlist.

Gate verdict: `gate_status(MC2_FAR_BACKDROP_LIT)` = **OK** (registered, documented,
smoke-allowlisted, default OFF, 3 readers). Build clean (RelWithDebInfo, mc2
relinked). Deployed to lane `0.4`, manifest `src_commit 98ed1d32`.

## Verification (build `98ed1d32`, install mc2-win64-v0.4, poses via visual_verify)

Night poses `mc2_10_saved_204611 / 204907 / 205952`, deterministic frame 431.

- **Gate OFF vs ON, impostor + ridge crops (all 3 poses):** pixel-**identical**
  (mean/peak RGB unchanged; 204611 impostor peak stays `(255,255,253)` both). The
  headline white impostors do NOT lift → first signal the symptom is off-path.
- **Exaggeration proof (`MC2_FAR_BACKDROP_LIT=1 NEAR=0 FAR=1 STRENGTH=1
  TINT=1,0,0`, force full red on ALL props):** airbase **buildings turned solid
  red**, near/mid tree geometry tinted, but the **white horizon impostors stayed
  pure white**. Proves (a) gate/uniform/env plumbing reaches the static-prop
  program on both coalesce+legacy paths, and (b) the impostors are NOT drawn by
  `static_prop.vert`. (Diagnostic used a temporary deployed-shader edit, since
  reverted; deployed shader == committed, verified by diff.)
- **RenderDoc attribution (ridge/impostor pixels, `debug_state/fbl_rdc_frame432.rdc`):**
  region `(1360,530,220,110)` → 2 writers: `glClear` + **eid 7201 `glDrawArrays()`,
  VS/PS 246/247, depth_test=false, depth_write=false, blend OFF**. A control
  attribution on the dark **foreground** terrain `(1400,1450,...)` returns the
  SAME eid 7201 writing 121/121 → **eid 7201 is the fullscreen tonemap/HDR
  composite**, not the impostor geometry. The impostor pixels are therefore baked
  into the **pre-composite HDR scene FBO** by an upstream program (exactly
  DEFERRED-BATCH-8B's tonemap-composite confound). Engine counter confirms:
  `[OBJBATCHER] submit_buildings=112 submit_trees=0` — no trees enter the
  static-prop batcher at this pose at all.
- **LOD zoom sweep:** N/A at mc2_10 — the white "impostors" are a backdrop blit,
  not LOD geometry, so there is no static-prop LOD pop to cure here.
- **Day-pose / gate-OFF byte-identity:** by construction (gated shader branch +
  zero-upload when off); OFF==ON confirmed on all sampled regions incl. buildings.
- **Cinematic letterbox bars:** bottom bar `(0,0,0)` byte-identical OFF vs ON —
  untouched (this path never writes them).

## Disposition / handoff

1. **Keep** commit `98ed1d32` (default-OFF, byte-identical off, mechanically
   correct for genuine far static props — buildings proven; trees where they DO
   batch in other missions/poses). Zero risk. Gate stays OFF pending either a
   scene that demonstrates a real far static-prop night win, or the backdrop fix.
2. **The mc2_10 headline white-impostor defect is a SEPARATE seam**, off the
   static-prop path. Next step to fix it: run RenderDoc **pixel-history on the
   HDR scene color FBO** (not the backbuffer) at pose 204611 to pin the upstream
   program that draws the fullbright horizon billboard, read its shader source,
   and add the mission night-light term there. `tools/renderdoc/attribute.py`
   currently samples the backbuffer (final composite); this needs a scene-FBO
   target + a pre-composite eid. Evidence .rdc preserved:
   `debug_state/fbl_rdc_frame432.rdc`.
3. The design doc's root-cause chain (stale `perVertexARGB` on
   `TransformMultiShape_HierarchyOnly`-registered far LODs) is **not** the
   mechanism for the mc2_10 horizon symptom — that theory assumed the impostors
   were static-prop-batched; `submit_trees=0` refutes it for this scene. It may
   still describe a real defect for far static props elsewhere; unverified.

## Residual risk

- Fingerprint dirty-flag: exe embeds `98ed1d32` but `MC2_BUILD_GIT_DIRTY=1`
  because the worktree carries pre-existing unrelated dirty files (docs/scratch);
  my source files ARE committed at `98ed1d32` (code state reproducible).
- clip.w vs Euclidean distance: `clip.w` is view-depth, not radial distance;
  fine as a monotonic fade proxy but off-axis far props fade slightly later than
  the LOD metric. Env-tunable; immaterial while gate is OFF.
