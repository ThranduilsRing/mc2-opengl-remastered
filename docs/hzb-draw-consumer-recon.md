# HZB Draw-Consumer Recon — smallest SAFE consumer for the HZB substrate

**Status:** READ-ONLY RECON, 2026-06-11. No code changes. New doc (not an update to
`hzb-staticprop-cull-readiness.md` — that doc is the sealed record of the readiness
slice; this doc designs the consumer it green-lit).

**Question answered:** what is the smallest SAFE draw consumer for the existing
diagnostic-only HZB substrate?

**Answer (one line):** an HZB occlusion term ADDED to the existing C1b static-prop
GPU cull compute (`shaders/gpu_cull.comp`), default-OFF behind a new env gate,
shipped advisory-counters-first, with the readiness margin (`gap < -1e-4`), the
camera-discontinuity guard, and a fail-open path that degrades to today's
frustum-only result. Never terrain, never gameplay state.

**Inherited constraints (binding):**
- Render cull must NEVER feed gameplay update state (`docs/gpu-static-prop-cull-lessons.md`
  §"Five ways the gates are load-bearing": `objmgr.cpp:1748 setExists(false)` trap).
- Ordering = CPU chunk gate → GPU frustum over frozen records → HZB in-pass.
- All stop conditions in `docs/hzb-visibility-mvp.md` ("STOP conditions before real
  culling") and `docs/hzb-staticprop-cull-recon.md` (camera-discontinuity guard
  mandatory) and the acceptance bar in `docs/hzb-staticprop-cull-readiness.md`.

---

## (a) Current HZB state inventory

| Component | Location | State |
|---|---|---|
| Gate (build) | `MC2_HZB_BUILD`, resolved `GameOS/gameos/gos_postprocess.cpp:209`; registry slot `RenderCore/RendererFeatureRegistry.h:175` (`HzbBuild = 37`) | default OFF; byte-identical when unset (textures/FBO never created) |
| Gate (probe) | `MC2_HZB_PROBE`, `gos_postprocess.cpp:372`; requires BUILD | default OFF, diagnostic only |
| Depth source | `MainDepth` registry resource (full-res `GL_DEPTH24_STENCIL8` scene depth), registered by gos_postprocess after scene FBO build (`docs/hzb-visibility-mvp.md` §Implementation) | resolved scene depth of the CURRENT frame |
| Build site | `gosPostProcess::runHzbReduce()` `gos_postprocess.cpp:1021`, called from endScene at `gos_postprocess.cpp:1776-1779`, BEFORE any post pass | per-frame when gated on |
| Mip chain | one ceil-sized `GL_R32F` texture PER level (NOT a mip chain — AMD mip-incomplete attach trap), `hzbLevelTex_[kHzbMaxLevels]` `gos_postprocess.h:287`, shared FBO `:288`; custom MIN reduce `shaders/hzb_reduce.frag` compiled `gos_postprocess.cpp:363-366` | reverse-Z MIN pyramid, ceil ladder (`docs/hzb-depth-convention.md`) |
| Accessors | `getHzbLevelTexture(level)` `gos_postprocess.h:165`, `getHzbTexture()/getHzbMipCount()/getHzbWidth()/getHzbHeight()/isHzbEnabled()` `:167-171` | ready for an external consumer to bind |
| Diagnostic consumers | self-test probe `runHzbProbe()` `gos_postprocess.cpp:1108` (`[HZB_PROBE v1]`); object probe v2 `[HZB_PROBE_OBJ v2]` `:1437` with guard counters, `[HZB_PROBE_LOD]`, `[HZB_PROBE_CULLCAND]`, `[HZB_PROBE_MARGIN]`; ImGui preview `GuiRuntime/GraphicsOptionsWindow.cpp:1491` | all read-only, `neverAppliedToDraws=1` |
| Camera-discontinuity guard | `unsafeForCull` `gos_postprocess.cpp:1254-1268` (fwd-angle > 30° OR posDelta > 0.25×mapHalfExtent via `inverseViewProj_` unprojection); prev-pose state `gos_postprocess.h:299-302` | proven on mc2_17 180° snaps |
| Projection | `viewProj_` = `Camera::worldToClipGL()` GL-NDC reverse-Z, set in `setTerrainMVP` BEFORE the scene depth render → same-frame coherent with HZB source (`docs/hzb-staticprop-cull-readiness.md` §Camera-discontinuity) | convention proven (`tested>0` every gameplay frame across mc2_03/17/24) |
| Margin recommendation | `gap < -1.0e-4` reverse-Z window-depth units (`docs/hzb-staticprop-cull-readiness.md` §Margin sweep) | knee of the sweep; removes marginal/grazing candidates, keeps deep occlusions |
| **HZB ↔ cull compute coupling** | **NONE.** `grep -i hzb GameOS/gameos/gpu_cull_*.cpp` = zero hits | the two substrates are fully separate today |

### Adjacent substrate: static-prop GPU cull (where the consumer slots)

| Component | Location | State |
|---|---|---|
| Cull compute | `shaders/gpu_cull.comp`; dispatch `gpu_cull::compute_dispatch()` `GameOS/gameos/gpu_cull_compute.cpp:826`, cull kernel `glDispatchCompute` `:1048`, then patch `:1212`, rollup; called from `mclib/txmmgr.cpp:2659` (between registry flush and batcher flush) | DEFAULT ON (C1b indirect authority); killswitch `MC2_GPU_CULL=0` (`gpu_cull_compute.cpp:12,105`) |
| Frustum predicate | `shaders/gpu_cull_predicate.glsl` (lockstep with `mclib/object_admission_predicate.cpp`); sphere-aware admit; dilation 0.08 (`gpu_cull_compute.cpp:112-120`) | conservative, proven |
| Static-prop visible path | `gpu_cull.comp:264-282` — `if (visible) && cat==CAT_STATIC_PROP` → bucket atomicAdd → `visibleIds[base+slot]` (binding 9), counts binding 10, caps binding 11 | **THE insertion point** |
| Sticky-block temporal superset | `gpu_cull.comp:283-318` else-if sibling: frustum-rejected props re-admitted if their terrain block was ever visible (binding 13 `blockVisBits`, set by `gpu_cull_block_rollup.comp`; one-shot zero `gpu_cull_compute.cpp:779-784`) | this is the "CPU chunk gate"-adjacent conservatism layer; HZB must NOT remove it |
| Frozen static records | M1 `MC2_GPU_CULL_STATIC_FROZEN_RECORDS`: `gpu_cull_substrate.cpp:62-75,251-287` (frozen prefix [0,S) refill), `gpu_cull_substrate.h:72-77`, registry side `gos_static_prop_registry.cpp:267-276,735-747` (`frozenRecordsArmed()`) | EXISTS, env-gated opt-in (default OFF). HZB consumer does not depend on it but composes cleanly (record layout unchanged) |
| C2 readback | binding 14 (`READBACK_SSBO_BINDING`, `gpu_cull.comp:140-155`, `gpu_cull_readback.cpp`) — per-actor visibility + counters readback | **reusable as the advisory-counter and oracle channel** |
| Record struct | `GpuActorRecord` 64 B std430, `gpu_cull_record.h` ↔ `gpu_cull.comp:28-39` lockstep; has spare semantics in `consumerFlags` | a `HzbEligible` consumer-flag bit fits without layout change |

### The one structural hazard: cross-frame HZB

`compute_dispatch()` runs at `txmmgr.cpp:2659` EARLY in frame N (before scene draw);
the HZB is built in endScene (`gos_postprocess.cpp:1776-1779`) AFTER frame N's scene
draw. So a cull-compute consumer necessarily tests frame N's `viewProj` bounds
against **frame N−1's pyramid**. This is the standard HZB-consumer arrangement
(occlusion culling is one frame late everywhere), but it is exactly the regime where
the mc2_17 camera-snap spike lives. Consequences (all already specced by the
readiness doc): the camera-discontinuity guard is MANDATORY (skip HZB term on unsafe
frames — frustum-only fallback), the 1e-4 margin is MANDATORY, and the first frame
after mission load / pyramid rebuild must treat the pyramid as invalid
(`hzbBuildCount_ == 0` → no HZB term, fail-open).

---

## (b) Candidate consumers, ranked

**Hard exclusions first (never, by constraint):**
- **Terrain** — chunk path is its own pipeline (`mc2TerrainLodChunkEnabled()`),
  terrain is the OCCLUDER, never the occludee. Excluded.
- **Gameplay flags** — `objBlockInfo.active` / `objVertexActive` / anything reaching
  `update()`/`setExists` (lessons doc §1-2). Render-side only, always. Excluded.
- **Movers (mechs/GVs)** — animated, transform-freshness traps (lessons doc §4),
  not in the indirect bucket system (`gpu_cull.comp:266-267`). Excluded for v0.

Ranked candidates:

1. **(WINNER) HZB term inside the C1b static-prop bucket admit, `gpu_cull.comp:264-282`.**
   Scope: `cat == CAT_STATIC_PROP` records only, and only those already admitted by
   the frustum test. Add: project record AABB (`worldAabbMin/Max`, offsets 16/32) to
   a screen rect + `objClosest = max(clip.z/w)`, sample the bound HZB level
   (clamped coarser, `textureLod` on `hzbLevelTex_`), cull only when
   `objClosest < hzbMin - 1e-4` AND frame is safe AND `u_hzbValid`. The sticky-block
   else-if path (`:283-318`) is left untouched (it already only fires for
   frustum-rejected props — HZB never sees them). Smallest because: zero new draw
   plumbing (the indirect patch/draw pipeline is unchanged — fewer visibleIds is the
   whole effect), zero CPU-side iteration, one shader edit + one texture bind +
   3 uniforms in `compute_dispatch()`, and the existing `MC2_GPU_CULL=0` killswitch
   plus a new `MC2_HZB_CULL` gate both disable it.
2. **(Sub-rank within 1, for staged rollout) large buildings first via consumerFlags
   bit.** Counter-intuitive but right: large buildings are the props with the
   biggest fill cost AND the deepest, most stable `gap` values in the margin sweep;
   tiny props have grazing/marginal gaps. Stage 1 enables the HZB term only for
   records whose extent radius exceeds a threshold (CPU sets an `HzbEligible` bit in
   `consumerFlags` at record-emit time, `gos_static_prop_registry.cpp` emit path).
   Stage 2 widens to all static props.
3. **(Runner-up, rejected as v0) HZB-cull only the static-prop depth prepass**
   (`flushDepthPrepass`, `gos_static_prop_batcher.cpp:4967`). Attractive because a
   false negative there costs perf, never pixels (main pass still draws). Rejected:
   the prepass is itself default-OFF (foliage Lane A, user-gated), it shares the
   same visibleIds stream so isolating it needs a second bucket-count pass (MORE
   plumbing than candidate 1), and the win is bounded by prepass adoption.
4. **(Rejected) CPU-side HZB cull from the probe readback.** The probe's
   `glGetTexImage` readback stalls; turning the diagnostic into a per-frame CPU
   consumer inverts the cost model. Diagnostic only.
5. **(Rejected) new HZB-only draw filter outside gpu_cull.** Any second visibility
   authority recreates the D3D↔GL split-brain class the project keeps re-fixing
   (frustum-cull X-mirror `a280dde2`, shadow `a365e6ad`, props `09707cd8`). One
   admission pipeline, one matrix.

## (c) Advisory-counters-first rollout plan

Phase 0 — **counters, zero suppression** (`MC2_HZB_CULL_ADVISORY=1`, requires
`MC2_HZB_BUILD`):
- Add the HZB sample + comparison to `gpu_cull.comp` but DO NOT change the admit
  decision. Write `hzbWouldCull` per-actor + atomic totals into the existing C2
  readback buffer (binding 14, `gpu_cull_readback.cpp`) — new header words, no new
  SSBO. CPU logs `[HZB_CULL_ADV v1] tested= wouldCull= unsafeSkipped= invalid=`
  bounded (first 5 frames / every 600 / unsafe frames), mirroring `[HZB_PROBE_OBJ v2]`.
- Cross-check oracle (see (d)) runs here: GPU `hzbWouldCull` vs CPU probe verdicts.
- Exit criteria: across mc2_03/17/24 soak, GPU-vs-probe disagreement = 0 on tested
  props; `wouldCull` rates match the readiness-doc steady-state numbers; 0 GL
  errors; +0 destroys; frame time not worse (the sample itself costs ~nothing).

Phase 1 — **suppression ON behind gate, large buildings only**
(`MC2_HZB_CULL=1` + `HzbEligible` restricted to extent radius ≥ threshold):
- Admit becomes `visible && !(hzbOccluded && safeFrame && hzbValid)`. Margin 1e-4
  hardcoded floor, env-tunable upward only (`MC2_HZB_CULL_MARGIN`, clamped ≥ 1e-4).
- Validation: tier1 5/5; mc2_03/17/24 with the false-negative oracle FATAL-clean;
  interactive visual pass over mc2_17 intro snaps (no building pop); perf delta
  recorded via Tracy `Cull.CullShader` + draw-side zones.

Phase 2 — widen `HzbEligible` to all static props; same gates, same oracle.

Phase 3 — default-ON proposal ONLY after a multi-mission soak with oracle FATAL=0
and a one-line revert path (`MC2_HZB_CULL=0`) documented. (Mirrors the
`mc2TerrainLodChunkEnabled()` cutover pattern.)

Kill-switch ladder at every phase: `MC2_HZB_CULL=0` (consumer off, frustum-only) →
`MC2_HZB_BUILD` unset (pyramid never built; consumer must fail-open to frustum-only
when `isHzbEnabled()==false` or `hzbBuildCount_==0`) → `MC2_GPU_CULL=0` (whole
compute path off).

## (d) False-negative oracle — "invisible building detector"

A false negative (visible prop culled) is the only catastrophic failure. Three
independent detectors, cheapest first:

1. **GPU-vs-probe lockstep check (Phase 0+).** The CPU probe
   (`gos_postprocess.cpp:1108+`, `[HZB_PROBE_OBJ v2]`) and the GPU term must agree:
   any prop where GPU says `hzbWouldCull=1` but the probe (same frame, same margin,
   same guard) says wouldKeep ⇒ `[HZB_CULL_ORACLE] FATAL mismatch prop=… gap=…`.
   Plumb: probe already iterates props by registry index; readback binding 14 gives
   per-actor GPU bits; join on record index. Run under `MC2_HZB_PROBE=1` in smokes.
2. **Disocclusion-flicker counter (Phase 1+).** Track per-prop visibility bit
   frame-over-frame (CPU side from binding-14 readback, or reuse
   `prevVisibilityBit`, record offset 52). Pattern visible(N−1) → hzb-culled(N) →
   visible(N+1) on a SAFE frame = a pop candidate; count + log bounded sample with
   screen rect and gap. Budget: 0 on steady frames across the matrix.
3. **Frustum-superset invariant (every frame, free).** HZB may only ever SHRINK the
   frustum-admitted set: assert in shader logic structure (the term is nested inside
   `if (visible)`), and CPU-verify `visibleWithHzb ≤ visibleFrustumOnly` per bucket
   from binding-10 counts vs a periodic advisory recount. Any growth = wiring bug =
   FATAL.

Smoke wiring: extend the existing smoke-gate grep set with
`HZB_CULL_ORACLE.*FATAL` as a hard fail, like FASTPATH_DROP. mc2_17 stays in the
matrix permanently (pathological camera case, per readiness doc).

## (e) Risks

1. **Cross-frame pyramid (frame N−1 depth vs frame N bounds)** — the structural
   hazard above. Mitigation: discontinuity guard (already built and proven), 1e-4
   margin, `hzbBuildCount_==0` fail-open, NO HZB term on unsafe frames. Residual:
   fast-but-smooth camera under guard thresholds; the margin sweep showed deep gaps
   survive 1e-3, so headroom exists; flicker counter (d.2) is the watchdog.
2. **Where does the compute shader get the matrix?** `compute_dispatch()` uses
   `gos_GetTerrainMVPMat4()` (`gpu_cull_compute.cpp:839`); the probe uses
   `viewProj_`/`worldToClipGL`. These must be verified IDENTICAL in convention
   before Phase 0, or the GPU term reprojections won't match the probe (oracle
   would catch it, but verify up front). The clip-w sign trap
   (`gpu_cull_predicate.glsl:9-12`) applies to the screen-rect projection: corners
   with `clip.w ≤ 0` ⇒ conservatively KEEP (probe already does `nearClippedKeep`).
3. **Sticky-block interaction.** The temporal-superset path (`gpu_cull.comp:283`)
   re-admits frustum-rejected props; HZB must never be applied to that branch (it
   exists precisely to paper over stateless-test misses). Keep the HZB term strictly
   inside the `if (visible)` branch.
4. **State inheritance / binding leaks.** Binding the HZB texture in
   `compute_dispatch()` must restore texture-unit state (the chunk-terrain
   transparency saga lesson: bolt-on passes must own ALL state they touch;
   `gos_mech_batcher.cpp:750` shows binding leaks already poisoned this dispatch
   once).
5. **AMD `textureLod` on the per-level (non-mipped) R32F textures**: each level is a
   separate texture, so the shader binds ONE chosen level (uniform) or a small array
   — no mip chain to sample. LOD-selection logic must clamp coarser like the probe
   (`Lmin`, dims ≤ 256) — clamping coarser is strictly conservative.
6. **Perf inversion.** HZB sampling per static prop (~2.6k records) is trivial, but
   the pyramid build itself costs a few fullscreen MIN passes per frame once
   `MC2_HZB_BUILD` is on by default. Measure build cost on the Tracy lane before
   Phase 3; if build > cull win, the whole feature is net-negative ("measure the
   FIX not just the hotspot" — pick-recon lesson).
7. **Editor.** Editor runs the same compute path; keep the consumer env-gated OFF
   there until separately verified (editor picking/selection must not depend on
   visibleIds — verify before Phase 2).

## (f) Go / No-Go criteria

GO to Phase 0 (advisory) when:
- [ ] `gos_GetTerrainMVPMat4()` vs `viewProj_` convention identity verified (risk 2).
- [ ] HZB texture bind + restore pattern reviewed against the state-inheritance rule.

GO to Phase 1 (real suppression, gated, large buildings) when ALL:
- [ ] Phase 0 oracle (d.1): GPU-vs-probe mismatches = 0 across mc2_03/17/24 soak.
- [ ] Guard fires on mc2_17 snaps and the GPU term provably skips those frames
      (unsafe-frame `wouldCull` routed to a skipped counter, mirrors
      `skippedCameraDiscont`).
- [ ] HZB self-test stays `wouldCull=0 integrityMismatch=0`; 0 GL errors; 0
      FBO-incomplete; +0 destroys; tier1 5/5.
- [ ] Margin ≥ 1e-4 enforced in code (not just env).

GO to Phase 2 (all static props): Phase 1 soak with flicker counter (d.2) = 0 on
steady frames + interactive visual pass (mc2_17 intro, wolfman zoom) shows no pop.

GO to Phase 3 (default-ON): multi-mission soak FATAL=0, measured net frame-time win
(pyramid build + cull sample < draw savings) on a dense map, revert line documented.

NO-GO / STOP (any ⇒ freeze at current phase, consumer stays gated):
- Any oracle FATAL or visible-prop pop reproduced.
- Pyramid build cost exceeds the measured draw savings.
- Any pressure to feed HZB results into update/gameplay state (hard constraint —
  this is how `setExists(false)` ate buildings before).
- mc2_17 removed from the validation matrix (it stays, permanently).
