# WATER-REFLECTION-QRES-RECON-1

**Recon only. No code, no build, no launch.** Worktree
`A:/Games/mc2-controlmap-sample-1` (branch `claude/controlmap-sample-1`, HEAD
`62d3d3c0`). This is the branch state; the reflection arc below was merged to
nifty (`68343329`) and is present here.

## ★ HEADLINE — the premise is already shipped, not greenfield

The task framed a ladder to *build* — (a) skybox/SH reflection → (c) 1/4-res
planar terrain RT. **Both rungs already exist, merged, and default states are
already flipped.** This recon is therefore a **status + hardening + gate-truth**
recon, NOT a design-from-scratch. What actually remains is: (1) the shipped
**HDRI per-fragment reflection is a measured ~10 ms/frame GPU regression** and is
**default-ON**; (2) the terrain RT (rung c) is shipped but **default-OFF and
empty at the gameplay camera** (the CLIP-1 depth-gate bug); (3) CLIP-1 (oblique
near-plane) is the one genuinely-unbuilt slice. Recommend the next slice be
**perf/correctness cleanup of what shipped**, not a new reflection.

---

## 1. Prior-attempt archaeology (the ruling died TWICE, tech survived and shipped)

Three distinct eras, all in-tree:

**Era 1 — original S3 "planar terrain reflection" (DEAD by mechanism).**
`docs/superpowers/specs/2026-05-17-water-v2-s3-planar-reflection-design.md`:
second per-frame `ComputeDispatch()` with a mirrored MVP. **Dual-adversarial
BLOCK×2** — it mutated global ring/fence state, cleared the shared
`g_indirectCmdBuffer`, and **unconditionally overwrote `g_dispatchMvp16`**
(re-breaking the water 1-frame-lag fix), plus primary-camera cull-window +
two-stage pixel-grid thin-VS. **The BLOCK was mechanism-specific, not
reflection-specific** (spec §"DUAL ADVERSARIAL OUTCOME", lines 197-234). Then an
Option-C "reflect the terrain ground **colormap** atlas" cut actually *shipped*
and was SHELVED (`24190b98`): the ground-color source gave a compass hue-swing
under orbit — "ugly as shit."

**Era 2 — the "camera-independence" ruling (SUPERSEDED, twice).** The
`f(WorldPos,time)`-only ruling in the S1 handoff
(`2026-05-17-water-v2-S3-HANDOFF.md:121-141`) was **explicitly withdrawn by the
user 2026-05-18**: *"that decision was mostly due to it being ugly as shit lmao,
not some invariant principle"* (`docs/water-reflection-plan.md:13-23`). Camera-
dependent reflection is permitted; quality is judged at the visual gate. **The
FS header comment at `gos_terrain_water_mdi.frag:198-200` ("MC2 has no sun for
now… ONLY camera-dependent term will be S3 … deferred") is STALE** — reflection
is live above it. The lesson banked in the shader: *wrong source, right
machinery* — the reflect-vector/Fresnel/waveLOD-mix math was always fine.

**Era 3 — the reborn arc (SHIPPED, merged `68343329`).** Sky-first, then RT:
`dad0cf9c` sky reflection → `1dab976d` 1/4-res RT substrate → `b09da4be` mirrored
terrain into RT (C1) → `1fb8731d` water samples RT (C2) → `615865d6`/`d027d6a9`/
`d671343e` projection/axis/azimuth fixes → `37a10cd6` **WATER-HDRI-REFL-1 +
WATER-REFL-DEFAULT-ON** → `d6183ecd` Reinhard + LOD → `451c9e49` **perf
diagnosis of the 10 ms regression**. Plans: `docs/water-reflection-plan.md`
(arc), `docs/water-reflection-pass-plan.md` (C1/C2 pass + POSTSCRIPT with the
corrected CLIP-1 root cause).

**Verdict:** prior attempt did NOT stay dead. The task's rung (a) and rung (c)
are both in `gos_terrain_water_mdi.frag` + `RenderWaterReflectionPass()` today.

## 2. Water render today (the consumer side — all three reflection sources present)

FS `shaders/gos_terrain_water_mdi.frag`, `o_isWater==1` branch. It already
carries the **entire ladder** as gated blocks:
- **rung (a) SH-L2 sky** — `kWaterSkySh[9]` inlined from `RenderCore/IblShCoeffs.h`,
  `waterEvalSkySh()` (`:118-164`). Orbit-stable, near-zero cost.
- **rung (a+) live HDRI equirect** — `waterEvalHdri()` (`:135-147`), Reinhard-
  toned, `textureLod(u_hdri, …, u_waterHdriLod)` on **unit 3**; falls back to SH
  when `u_waterHdriLod < 0` (`:246-251`). **This is the ~10 ms cost.**
- **rung (c) terrain RT** — `u_waterReflRT` sampler **unit 2** (`:40`), screen-UV
  `gl_FragCoord.xy / u_waterScreenSize` + wave-normal-perturbed, blended OVER the
  sky by `rtSample.a * u_waterRtStrength` (`:254-267`).
- **Fresnel/mix** — Schlick `REFL_F0 + (1-F0)*pow(1-max(vdir.z,0),5)` (`:269`),
  `reflMix = clamp(fres * u_waterReflStrength * waveLOD, 0, REFL_MAX=0.55)`,
  `col = mix(col, reflectCol, reflMix)` (`:270-271`).
- Wave normal = analytic fBm gradient `nzGrad` (aspect-corrected, `:229-231`) →
  `waveNormal` → `reflect(-vdir, waveNormal)` (`:230-233`). Axis swap MC2 Z-up →
  SH/HDRI Y-up at `:234-242` (horizontals negated 180° empirically — load-bearing).
- Debug modes 7 (SH sky) / 8 (RT sample) / 9 (reflect blend) already in the FS
  `u_waterDebugMode` enum (`:50-52`, `:281-308`).

Water plane elevation: single global `Terrain::waterElevation` (per-mission, from
`.fit`; `docs/water-rv-arc-recon.md §1`). Depth state: GEQUAL reverse-Z,
AlphaBlend, cull None, depthWrite ON (`water-armed-pipeline-registration-1.md`).
Reflection needs NO new varying — `WorldPos`, `cameraPos`, `gl_FragCoord` all
present.

## 3. Planar reflection architecture — ALREADY IMPLEMENTED; insertion point is live

`gos_terrain_indirect::RenderWaterReflectionPass()`
(`GameOS/gameos/gos_terrain_indirect.cpp:3846-3930`), declared
`gos_terrain_indirect.h:456`, **called from `code/gamecam.cpp:544`** — exactly
between `renderLists()` (terrain SOLID drawn, ring slot consumed, atlases warm)
and `renderWaterFastPath()`. As-built:
- Gate `MC2_WATER_REFLECTION_RT` (**default OFF**, `:3847-3851`); `IsFrameSolidArmed()`
  guard.
- **Mirror MVP** built on CPU (`:3862-3871`): `R` reflects world-Z across the
  plane (`z' = 2·we − z`); with `G[r][c]=M[c*4+r]` (uploaded `GL_FALSE`), negate
  column 2, `col3 += 2·we·col2`. **No fresh matrix build/sample** — it mirrors the
  live `gos_GetTerrainMVPMat4()`.
- `gos_SetTerrainMVP(mir)` → `ComputeDispatch()` (fresh ring slot, refills shared
  cmd buffer) → bind 1/4-res `waterReflectionFBO`, quarter-res viewport,
  `glClearDepth(0.0)` (reverse-Z far), `DrawIndirect()` into the FBO
  (`:3888-3897`).
- **RESTORE (load-bearing):** production `gos_SetTerrainMVP(saved)` AND the
  `g_dispatchMvp16`/`Fp`/`FrameIdx` snapshot (`:3884-3907`) — because the GPU
  **water** fast path reads `g_dispatchMvp16` as its `u_worldToClipGL`; restoring
  only `terrain_mvp_` makes water draw with the mirror matrix and vanish. This is
  the exact global-mutation hazard the Era-1 adversarial BLOCK warned about,
  paid off here by full save/restore. ~15 same-frame consumers read the MVP after
  this pass.
- Throttled `glReadPixels` PROOF (whole-RT coverage, `:3909-3920`) — a **CPU
  sync-stall on a handful of frames**; acceptable as diag, would be cut for perf.

Scene contents in the RT: **terrain SOLID only** — no water (no recursion), no
props/mechs, no skybox, no clip plane. SH-sky is the off-terrain fallback via
RT `alpha=0`. 1/4-res color+depth FBO in `gos_postprocess`
(`getWaterReflectionFBO/Width/Height`).

**Framegraph / backend-seam leverage (answers the task's "clean insertion
point" question):**
- `RenderCore/top_level_pass_executor.h:246` explicitly notes the reflection pass
  as **default-OFF, self-restoring, and OUTSIDE the executor** — the framegraph
  executor (`MC2_FRAMEGRAPH_EXECUTOR`, default-OFF) does **not** own it. It is a
  hand-bracketed pass in `gamecam`. So the framegraph gives **no** current
  insertion leverage; migrating the reflection pass into the executor as a
  registered node (with declared MVP-global read/write so the reorderer treats it
  as a producer of the RT) is a **future** cleanup, not a prerequisite.
- Pipeline-registration seam: water is DESCRIPTIVE-registered only
  (`water-armed-pipeline-registration-1.md`, `PipelineId::WaterArmed`); the
  reflection terrain draw rides the existing `DrawIndirect()` bridge, which is why
  `INDIRECT-BRIDGE-RETIRE-1` (`11c1450e`) **retained `DrawIndirect` specifically
  for water reflection** even while retiring the indirect SOLID caller. Any future
  seam work must not retire that path.

## 4. Ladder + recommendation (re-based on shipped reality)

| Rung | Task's plan | **Actual state** |
|---|---|---|
| (a) skybox/SH reflection | "start here, near-zero cost" | **SHIPPED, default-ON** (`MC2_WATER_REFLECTION`, strength 1.5) |
| (a+) live HDRI reflection | not in task | **SHIPPED, default-ON — the ~10 ms regression** |
| (b) SSR local silhouettes | "middle rung" | **rejected at design** (oblique ~30° cam → smear/holes); not built. Not recommended. |
| (c) full planar 1/4-res | "likely end state" | **SHIPPED, default-OFF, empty at gameplay cam** (CLIP-1 depth-gate bug) |

**Recommended next slice is NOT a new reflection — it is cleanup of what shipped,
in this order:**
1. **WATER-HDRI-REFL-PERF-1** (highest value). The default-ON per-fragment HDRI
   equirect (`atan2/asin` + `textureLod` of a 4K map) costs ~10 ms
   (`451c9e49`). Fix per that doc: **precompute the reflect direction on CPU**
   (flat water plane ⇒ reflection is a cheap per-vertex/CPU term) or bump
   `MC2_WATER_HDRI_LOD` default, or gate HDRI behind its own env with SH as the
   cheap default. Capped-FPS masked it in smoke — this is a real GPU frame cost.
2. **WATER-REFLECTION-CLIP-1** (makes rung-c actually visible). Per
   `water-reflection-pass-plan.md` POSTSCRIPT: the RT is empty at the ~20° oblique
   gameplay camera because the terrain SOLID compute depth gate `pzOk`
   (`gpu_driven_terrain_solid.comp:213-215`) culls the mirrored quads —
   reflecting world-Z shifts camera-forward depth by `Δs ≈ 2·eye_height·sin(pitch)`,
   pushing mirrored corners past the reverse-Z far plane → 0 instances. Fix =
   **Lengyel oblique near-plane at the water plane** in the mirror projection
   (keeps mirrored geometry in-band AND excludes below-water geometry without an
   FS discard). Interim hack: `u_reflectionPass` flag relaxing `pzOk` + `GL_DEPTH_CLAMP`.
3. **Only then** consider props/mechs in the RT (Phase D), or fold the pass into
   the framegraph executor.

## 5. Fresnel / distortion — already present, tunable

- Fresnel: Schlick with flat +Z water normal (`vdir.z`), grazing-boosted; `REFL_F0=0.02`.
- Distortion: `nzGrad * REFL_WAVE_SLOPE` offsets the RT screen-UV (`:263`) and
  perturbs the reflect vector for SH/HDRI (`:230-233`). Analytic fBm gradient,
  aspect-corrected (`WATER-ASPECT-CORRECT-1`, `:229`). No new work needed unless
  tuning.
- `waveLOD` (distance, not angle) fades reflection at range (`:172`, mixed into `reflMix`).

## 6. Gates (as-shipped — note the DEFAULTS are already flipped, contra the task)

| Env | Meaning | **Default** |
|---|---|---|
| `MC2_WATER_REFLECTION` | SH+HDRI sky reflection strength (1.5) | **ON** (=0 kills to byte-identical) |
| `MC2_WATER_REFLECTION_RT` | terrain RT fill pass + FS RT blend (0.85) | **OFF** |
| `MC2_WATER_HDRI_LOD` | HDRI sample LOD (default 2.5; `d6183ecd`→lowered) | env override |
| `MC2_WATER_SHINE` | Blinn-Phong sun spec (0.25) | OFF |
| `MC2_WATER_DEBUG_MODE` | FS debug (7 SH / 8 RT / 9 blend) | 0 Final |

**Ruling needed from user (§8):** the task says "gate `MC2_WATER_REFLECTION`
default OFF" — but it shipped **default-ON** (`WATER-REFL-DEFAULT-ON`,
`gameos_graphics.cpp:3096-3099`). Reverting to default-OFF is a one-line change
but is a **visual default flip** and collaterally hides the 10 ms cost rather
than fixing it. Recommend: **fix the perf first (item 1), keep sky-SH default-ON
(cheap), gate HDRI/RT explicitly** — rather than a blanket default-OFF.

Res/mip knobs: RT is hard-1/4-res (`getWaterReflectionWidth/Height`); HDRI LOD is
the reflection-sharpness knob. No res env today; a `MC2_WATER_REFLECTION_RT_DIV`
knob is a trivial add if wanted.

## 7. Landmines (depth/MVP lockstep + AMD)

1. **MVP + `g_dispatchMvp16` lockstep (THE landmine).** Any pass that installs a
   terrain MVP must restore BOTH `terrain_mvp_` and the `g_dispatchMvp16` snapshot
   (+`Fp`/`FrameIdx`) — the GPU water fast path reads the snapshot as its clip
   matrix. `RenderWaterReflectionPass` does this (`:3884-3907`); replicate exactly
   for any variant. Missing it = water drawn with mirror MVP → vanish/flicker
   (the Era-1 BLOCK, paid off).
2. **`ComputeDispatch()` mutates global ring/fence/cmd state.** The reflection
   pass advances a fresh ring slot and refills the SHARED cmd buffer — safe ONLY
   because it runs AFTER the main SOLID draw consumed its slot and BEFORE water.
   Do not reorder it earlier.
3. **`DrawIndirect` retention.** `INDIRECT-BRIDGE-RETIRE-1` kept the indirect
   SOLID bridge alive **only** for this pass. Terrain-retire work must not remove it.
4. **CLIP-1 depth gate (`pzOk`).** Mirrored quads culled at oblique camera →
   empty RT. This is why RT ships default-OFF; don't "enable RT" without CLIP-1.
5. **HDRI default-ON perf.** ~10 ms; capped FPS hides it in smoke — measure with
   an uncapped/GPU-time probe, not the FPS gate.
6. **`glReadPixels` PROOF stall.** The diagnostic readback in the pass is a
   sync-stall on frames 1/5/30/120/…; strip or make async before any perf claim.
7. **Stale FS comment `:198-200`** asserts the dead ruling — don't trust in-shader
   comments over the live code below them.
8. **Axis-swap sign (`:234-242`)** was tuned by eye (horizontals negated 180°);
   any HDRI/SH basis change re-opens sun-azimuth correctness.

## 8. Acceptance & perf budget

- **Missions:** `mc2_17` (river) + a `gaea`-family water map (gaea missions exist
  in-tree — `tests/smoke/artifacts/*/gaea_mountain_01.*`; pick the water-bearing
  gaea peaks map) with **static cams** for A/B stability. Also `mc2_01`/`mc2_24`
  (heavy water). Rely on logs + user eyeball (foreground-race caveat on pixel
  captures); **NEVER `--kill-existing`**, `--duration ≤ 30`.
- **RT-pass proof:** `[WATER_REFL_RT]`-style coverage/alpha_cov log from the pass
  (`:3909-3920`) — expect `alpha_cov>0` ONLY after CLIP-1 at the gameplay camera
  (today ~0, the bug).
- **Perf budget:** target **1/4-res RT pass ≤ ~1 ms on 7900 XTX**; the HDRI FS
  term must come **down from the measured ~10 ms** (item 1) — that, not the RT
  pass, is the current budget breach.
- Gate-OFF regression: `MC2_WATER_REFLECTION=0` + `MC2_WATER_REFLECTION_RT` unset
  → byte-identical to pre-arc.

## 9. Open rulings for the user
1. **Default-ON vs OFF** for `MC2_WATER_REFLECTION` — task says OFF, ship says ON.
   Recommend keep SH-sky ON (cheap), gate HDRI/RT; fix perf rather than blanket-OFF.
2. **Is the ~10 ms HDRI reflection worth keeping default-ON**, or SH-only default
   with HDRI opt-in? (drives item 1's shape.)
3. **CLIP-1 now or later** — it's the only genuinely-unbuilt slice and is what
   makes the *terrain* reflection actually appear at gameplay pitch. Oblique
   near-plane vs the interim `pzOk`-relax hack.
4. Props/mechs in the RT (Phase D) — deferred; confirm out-of-scope.
