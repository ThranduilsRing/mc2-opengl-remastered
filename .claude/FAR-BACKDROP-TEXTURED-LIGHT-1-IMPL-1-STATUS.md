# FAR-BACKDROP-LIT-IMPL-1 — implementation + verification status 2026-07-23

Branch `claude/nifty-mendeleev`, worktree A:/Games/mc2-main.
Lane FAR-BACKDROP-LIT-IMPL-1. Commit **`98ed1d32`**. Gate **`MC2_FAR_BACKDROP_LIT`** (default-OFF).
Follows the design in `FAR-BACKDROP-TEXTURED-LIGHT-1-STATUS.md` (option A).

> **2026-07-30 — PARTIAL RETRACTION (lane FALSE-CONCLUSION-RETRACT-1).** Two
> claims in this document rested on the `[OBJBATCHER] submit_trees=0` counter,
> which cannot support them. See the boxed RETRACTED notes at the headline, in
> §Verification, and at Disposition item 3. The document's *primary* conclusion
> (mc2_10's white horizon impostors are not drawn by `static_prop.vert`) is
> **left standing** — it rests on the independent exaggeration test, not on the
> counter. What is withdrawn is the counter citation, and the separate
> dismissal of the design's stale-`perVertexARGB` root-cause chain.
> Same treatment as the retracted albedo claim (`fc43338a`).

## Headline

Option A **implemented, built, deployed, gate-wiring verified working** — and the
verification bar **disproved the design's root-cause hypothesis for the specific
mc2_10 headline symptom**: the fullbright white horizon "impostors" are **NOT on
the static-prop path**.

> **RETRACTED (2026-07-30):** the original sentence here read "RenderDoc + the
> engine's own `[OBJBATCHER]` counter show `submit_trees=0` at this pose". The
> counter half of that citation is withdrawn. `submit_trees` is
> `submitted_instances_by_pop[Tree]`, incremented ONLY at
> `gos_static_prop_batcher.cpp:5613` on the **dynamic** `submitMultiShape` path.
> Once an actor registers a persistent recipe it replays every frame through
> `submitCachedInstance()` (`:9516`), which **deliberately does not tick these
> counters** (comment at `:9526`). `submit_trees=0` is therefore the *healthy
> steady state* for a scene full of drawing trees, and is evidence of nothing.
> The batcher's own comment at `:9262` records the contradiction directly:
> "733 trees eligible, all via the store, dyn-submit=0". A correct explanation
> of this counter already existed in-tree —
> `docs/superpowers/specs/2026-05-06-track-b-acceptance-envelope.md:46` — and
> was not consulted.
>
> **The conclusion survives on other evidence.** The exaggeration test below
> (force full red on all static props; airbase buildings turned solid red and
> near/mid tree geometry tinted, while the white impostors stayed pure white)
> is an independent and sufficient proof that the impostor pixels are not
> emitted by `static_prop.vert`. Note that this test also demonstrates the
> shader change reaching **registry-replayed** instances — precisely the
> population the counter is blind to — which is why it, and not the counter,
> is load-bearing here.

The white pixels are produced upstream of the
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
  DEFERRED-BATCH-8B's tonemap-composite confound).
  > **RETRACTED (2026-07-30):** this bullet ended "Engine counter confirms:
  > `[OBJBATCHER] submit_buildings=112 submit_trees=0` — no trees enter the
  > static-prop batcher at this pose at all." **It confirms nothing.**
  > `submit_trees=0` is the normal reading for registry-replayed trees that are
  > drawing every frame (see the headline note). Worse, the two numbers are not
  > even the same kind of quantity as used here: `submitted_instances_by_pop[]`
  > is incremented **once per ACTOR** per `submitMultiShape` call, not once per
  > instance — child packets go to the separate `submitted_children` counter. So
  > `submit_buildings=112` is 112 building **actors** that took the dynamic path,
  > not 112 instances, and it cannot be read against `submit_trees=0` as a
  > geometry-volume comparison. The RenderDoc attribution in this bullet is
  > unaffected and stands on its own.
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
   were static-prop-batched.

   > **RETRACTED (2026-07-30), and replaced with a measured verdict.** This item
   > originally continued: "`submit_trees=0` refutes it for this scene. It may
   > still describe a real defect for far static props elsewhere; unverified."
   >
   > **Why the refutation was invalid.** `submit_trees=0` cannot refute
   > anything (see the headline note). The chain was dismissed on a blind
   > counter and left flagged "unverified", so the hypothesis was recorded as
   > dead when it had never actually been tested.
   >
   > **It has now been tested, at mc2_03, and it is dead for a different and
   > better reason.** Lane FALSE-CONCLUSION-RETRACT-1, install
   > `C:/mc2-priv-fcr1` (private copy of v0.4, exe `src_commit 5c04048c`, which
   > contains `98ed1d32`), bookmark set `mc2_03_building_faces` (5 building
   > poses + 1 no-buildings control), frame 431, deterministic clock,
   > `MC2_CAPTURE_ENV_DECLARE` used so the gate actually reached the engine.
   >
   > *Noise floor first* (baseline vs identical baseline repeat): **0.000% on
   > four of six poses**; `sam_lightfacing` 0.355%; **`civstore_lightaway`
   > 20.019%** — that pose is nondeterministic run-to-run and any A/B reading
   > on it is worthless. A `MC2_FAR_BACKDROP_LIT=1 STRENGTH=0` plumbing control
   > reproduced the noise floor exactly (0.000% / 0.288% / 20.019%), confirming
   > both the harness and the gate's off-state.
   >
   > *Signal.* Gate ON at defaults moves **5.8% / 8.9% / 7.9% / 6.2%** of pixels
   > on the four usable building poses and **0.000%** on the no-buildings
   > control. That reproduces the magnitude that made this hypothesis look live.
   > **But it is not a repair:**
   >
   > 1. **The moved pixels are not black.** Baseline luma of the changed pixels
   >    is median 56–92 (p05 ≈ 29–37) — ordinary mid-tone lit prop pixels. The
   >    count of near-black (luma < 16) pixels changes by **8–26 pixels out of
   >    ~92,000**, and the ~92,000 is just the cinematic letterbox. No black
   >    quad is being lifted, because none is present in these frames.
   > 2. **The change is monotone brightening**, 17942↑/10↓, 27344↑/0↓,
   >    24203↑/15↓. A repair of a *stale day (bright)* seed would darken. Nothing
   >    darkens.
   > 3. **There is no near-vs-far asymmetry — the load-bearing measurement.**
   >    Re-running with `NEAR=0 FAR=1` (substitute at *all* depths, near geometry
   >    included) produces the *same* monotone brightening — 79183↑/114↓,
   >    83546↑/71↓ — over ~3–4× as many pixels (25.8% / 27.2% / 18.4% / 17.1%).
   >    If near LODs carried a correct baked seed and only far LODs were stale,
   >    overwriting the near seed would move pixels in **both** directions. It
   >    does not. The baked per-vertex seed is effectively **zero at every
   >    depth**, which independently corroborates the measurement recorded in
   >    `shaders/static_prop.vert` (PROP-BASE-LIGHT-DIR-1: scaling `seed` by even
   >    0.0 is byte-identical). **Far LODs are not special here.**
   >
   > **What the gate actually does**, therefore: with `base_light ≈ 0`,
   > `seed = mix(base_light, ambientTerm, fbT)` makes `lit = (seed +
   > ambientTerm)·f` ramp from `1×ambientTerm` to `2×ambientTerm` as `clip.w`
   > crosses 700→960. It is a **distance-ramped ambient doubler**, not a
   > stale-seed corrector. Note the implementation is gated on `clip.w`, *not*
   > on far-LOD-ness (this document says so in "What shipped") — so it fires on
   > every prop past 700 units whether or not that prop ever skipped a bake.
   > **Pixel movement under this gate is therefore not evidence for the
   > stale-`perVertexARGB` hypothesis and never could have been.**
   >
   > **Additional disqualifier for this test bed:** the hypothesis is that far
   > LODs miss the per-vertex **night** bake. `mc2_03` is a **day** mission
   > (`missions/mc2_03.fit`: `LightRed/Green/Blue = 242/242/225`). A frozen day
   > bake is *correct* in daylight, so no such defect can manifest at these
   > poses regardless. A genuine test of the chain needs a night mission plus a
   > pose that frames a **confirmed far-LOD** prop.
   >
   > **Verdict:** the stale-`perVertexARGB` far-LOD chain is **not confirmed and
   > not usefully refuted** — it remains untested where it could apply (night
   > + verified far LOD), and it is **not** the cause of anything observable at
   > mc2_03. `MC2_FAR_BACKDROP_LIT` should stay default-OFF: at mc2_03 turning
   > it on is a cosmetic distance-ramped brightening of far props with no defect
   > behind it.
   >
   > **Consequence for the mc2_03 black-quad report:** far-backdrop is **NOT**
   > the cause. Across all six poses there are no black quads to fix.

## Residual risk

- Fingerprint dirty-flag: exe embeds `98ed1d32` but `MC2_BUILD_GIT_DIRTY=1`
  because the worktree carries pre-existing unrelated dirty files (docs/scratch);
  my source files ARE committed at `98ed1d32` (code state reproducible).
- clip.w vs Euclidean distance: `clip.w` is view-depth, not radial distance;
  fine as a monotonic fade proxy but off-axis far props fade slightly later than
  the LOD metric. Env-tunable; immaterial while gate is OFF.
