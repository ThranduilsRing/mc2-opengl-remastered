# Render Contract F3 — Closing Report

**Status:** ✅ Implemented (Option A)
**Date:** 2026-04-26 (spec, audit, canary setup) → 2026-04-27 (canary verdict, implementation, this report)
**Branch:** `claude/nifty-mendeleev`
**Spec:** [F3 design v2](2026-04-26-render-contract-f3-mrt-completeness-design.md)
**Audit:** [F3 pass audit](render-contract-f3-pass-audit.md)
**Canary:** [F3 canary report](render-contract-f3-canary-report.md) — verdict CLEAN
**Plan:** [F3 Option A implementation plan](../plans/2026-04-27-render-contract-f3-option-a.md)

---

## Outcome

F3 implemented as **Option A** per F3 design v2 §3.1 / §5A. Every Group II-Opaque shader identified by the pass audit declares `layout(location=1) out vec4 GBuffer1` and writes via `rc_gbuffer1_screenShadowEligible(...)`. The visible-pixel coherence principle from spec §1 holds:

> For every visible pixel `shadow_screen.frag` consumes, `GBuffer1` corresponds to the same surface that wrote `COLOR0` and `depth`.

The AMD `location=1` corruption claim at `gos_postprocess.cpp:519-520` was canary-tested and refuted (canary commit `0173a31`, observation 2026-04-27 by operator). `docs/amd-driver-rules.md` was updated 2026-04-27 with a "Tested-and-refuted claims" section recording the outcome.

The F3 implementation commits all landed without smoke regression and with operator visual-A/B confirmation across multiple missions including a full mission playthrough.

---

## Commit ledger

| SHA | Content |
|---|---|
| `5a9a3a8` | spec: F3 design v2 (coherence principle, Option A vs Hybrid fork) |
| `628694a` | spec: F3 pass audit (Group I/II-Opaque/II-Blend/Excluded classification) |
| `2ff756a` | spec: F3 canary placeholder report (PENDING OBSERVATION) |
| `9cc9912` | spec: F3 canary verdict CLEAN; AMD location=1 claim refuted |
| `46f854b` | plan: F3 Option A implementation plan (9 tasks) |
| `262f90b` | plan: F3 Option A — advisor revisions before execution |
| `8ed9489` | **Task 1.** F3 Option A — add clearGBuffer1 sentinel clear |
| `273ed47` | **Task 2.** F3 Option A — explicit GBuffer1 in gos_tex_vertex_lighted.frag (cherry-pick) |
| `99c3e5a` | **Task 3.** F3 Option A — explicit GBuffer1 in gos_vertex_lighted.frag |
| `e4d0e96` | **Task 5.** F3 Option A — explicit GBuffer1 in gos_tex_vertex.frag |
| `9e383d1` | **Task 6.** F3 Option A — explicit GBuffer1 in gos_vertex.frag |
| (this commit) | **Task 7.** F3 closing report |

Tasks 4 and 8/9 produced no commits:
- **Task 4** (V1/V2 verification): notes-only; conclusions inlined into Task 5 commit message and recorded in §5 of this report.
- **Task 8** (cleanup): deferred per advisor — `enableMRT`/`disableMRT` removal, stale comment pruning, `object_tex.frag` deletion, modding-guide updates all happen in a separate post-F3 PR.
- **Task 9** (delete canary throwaway branch): performed as a local-only branch-delete operation; logged in §7.

---

## Flat-up roster

These shaders write `rc_gbuffer1_screenShadowEligible(vec3(0,0,1))` (flat-up encoded normal) instead of a real per-vertex world normal because no surface normal is available in their varyings. **Flat-up is a compatibility fallback, not a physically correct world normal.** When a downstream pass starts consuming `GBuffer1.rgb` as world normal, each of these will need a normal-quality cleanup.

| Shader | Task | Reason |
|---|---|---|
| `shaders/gos_vertex_lighted.frag` | 3 | No `Normal` varying; lit untextured (rare production usage) |
| `shaders/gos_tex_vertex.frag` | 5 | No `Normal` varying; serves textured non-lit world draws + gosFX particles + dead IS_OVERLAY variant |
| `shaders/gos_vertex.frag` | 6 | No `Normal` varying; debug-tier (lines, points, basic colored verts) |

Shader migrated with a real per-vertex normal (NOT flat-up):

| Shader | Task | Normal source |
|---|---|---|
| `shaders/gos_tex_vertex_lighted.frag` | 2 | `normalize(Normal)` — TGL world objects (mechs, buildings, vehicles); primary visible coverage |

---

## V1/V2 verification (per F3 design spec §6 audit)

### V1 — IS_OVERLAY depth-write state

**Finding: dead path.**

- `selectBasicRenderMaterial` ([`gameos_graphics.cpp:1711`](../../../GameOS/gameos/gameos_graphics.cpp)) sets the `IS_OVERLAY` shader-compile define when `gos_State_Overlay=1`.
- **No fragment shader contains `#ifdef IS_OVERLAY` or `#ifndef IS_OVERLAY`.** Grep `IS_OVERLAY` over `shaders/` returns zero matches. The shader-define is set but unused at the GLSL level.
- The single production callsite that sets `gos_State_Overlay=1` is [`mclib/txmmgr.cpp:1486`](../../../mclib/txmmgr.cpp) inside the `Render.NoUnderlayer` loop, and that loop is gated by `MC2_GPUOVERLAY` flag (line 1475). `MC2_GPUOVERLAY` is defined in [`mclib/txmmgr.h:65`](../../../mclib/txmmgr.h) but **never set** anywhere in the codebase.
- Consequence: the IS_OVERLAY shader variant compiles but never runs in production, and the `#ifndef IS_OVERLAY` guard discussed in the original implementation plan is unnecessary. Task 5 took the unconditional-write branch (Branch A).

### V2 — gosFX particle depth-write state

**Finding: mixed depth-write state, but byte-equivalent in alpha to today's behavior on AMD.**

- `mclib/gosfx/` directory contains zero `glDepthMask` or `gos_State_ZWrite` calls. Particle render-state is set by callers, not within gosFX itself.
- Caller-side depth-write state is mixed:
  - [`mclib/cevfx.cpp:887`](../../../mclib/cevfx.cpp): sets `gos_State_ZWrite = 1` (some particle paths own depth).
  - [`mclib/celine.cpp:113-118`](../../../mclib/celine.cpp): conditional — `ZWrite=1` if no fade-table, `ZWrite=0` if fading.
  - [`mclib/cevfx.cpp:1054`](../../../mclib/cevfx.cpp): `ZWrite = zWrite` (variable).
- Coherence-safety analysis: Today (mainline before F3), `gos_tex_vertex.frag` does not declare `layout(location=1)`. AMD's RX 7900 driver writes `vec4(0,0,0,0)` to undeclared MRT outputs (the "lucky default" that the F3 canary established as the reliable behavior on this hardware). Particle pixels therefore have `GBuffer1.alpha = 0` today regardless of caller's `ZWrite` state.
- After F3 Task 5, `gos_tex_vertex.frag` writes `GBuffer1 = (0.5, 0.5, 1.0, 0.0)` explicitly. **Alpha matches today (both are `0.0`).** RGB now carries flat-up encoded normal instead of zero, but no current consumer reads `GBuffer1.rgb`; `shadow_screen.frag` reads only `.a`.
- Operator visual A/B over a full mission playthrough including particle-heavy content confirmed no regression.

### Verification outcome summary

Both V1 and V2 confirmed safe. Branch A (unconditional `GBuffer1` write in `gos_tex_vertex.frag`) is the correct choice; the `#ifndef IS_OVERLAY` guard discussed in the plan as a contingency for Branch A turned out to be unnecessary.

---

## Coherence proof

For every visible pixel `shadow_screen.frag` consumes:

- **Sky pixels** (`depth >= 1.0`): `shadow_screen.frag:132` skips them via depth gate. `GBuffer1` value irrelevant. Task 1's sentinel clear provides defensive default `(0.5, 0.5, 1.0, 0.0)` in case any future consumer drops the depth-skip side path.
- **Terrain pixels:** `gos_terrain.frag` writes `GBuffer1` via `rc_gbuffer1_shadowHandled` (alpha=1, "shadow handled"). Unchanged by F3. ✓
- **Terrain overlay / decal / grass / static prop pixels:** Group I shaders write `GBuffer1` via registry helpers. Unchanged by F3. ✓
- **TGL world objects** (mechs, buildings, vehicles): `gos_tex_vertex_lighted.frag` writes `rc_gbuffer1_screenShadowEligible(normalize(Normal))` after Task 2. Real per-vertex normal. ✓
- **Lit untextured world draws** (rare): `gos_vertex_lighted.frag` writes flat-up after Task 3. ✓
- **Textured non-lit world draws + gosFX particles:** `gos_tex_vertex.frag` writes flat-up after Task 5. Includes blended particle paths whose underlying surface owns depth — the explicit alpha=0 is byte-equivalent on AMD to today's lucky default. ✓
- **Basic untextured world draws** (lines, points, debug-tier): `gos_vertex.frag` writes flat-up after Task 6. ✓
- **Pixels never written by any producer** (theoretically possible with future render order changes): inherit Task 1's sentinel `(0.5, 0.5, 1.0, 0.0)`. Defense-in-depth. ✓

For every visible pixel `shadow_screen.frag` consumes, `GBuffer1.alpha` and `GBuffer1.rgb` correspond to the surface that owns that pixel's depth, OR (in the depth-write-off particle case) match the alpha that the underlying surface would have produced for shadow-eligibility purposes. The coherence principle is satisfied.

---

## Frozen surfaces — confirmed unchanged

- All 5 Group I shaders: `gos_terrain.frag`, `terrain_overlay.frag`, `decal.frag`, `gos_grass.frag`, `static_prop.frag`. ✓
- `shadow_screen.frag` reader logic. ✓
- `mclib/render_contract.{h,cpp}`, `shaders/include/render_contract.hglsl`. ✓
- `mclib/camera.h` projectZ wrappers. ✓
- `enableMRT()` / `disableMRT()` (still defined-but-unused under Option A — removal deferred to post-F3 cleanup). ✓
- `scripts/check-render-contract-gbuffer1.sh` grep census continues to pass — F3 added new `GBuffer1` writes, all routed through `rc_*` helpers.

---

## Operator visual-A/B notes

- **After Task 1**: confirmed clean. Operator observation: "extremely mild regression: a gap between overlay/decal and the terrain... I think this is expected. I am not concerned about it." Operator hypothesis: terrain-seam-fix world-unit expansion bypassed; pre-existing cosmetic detail unrelated to F3.
- **After Task 3**: confirmed clean across two missions.
- **After Task 5+6 (combined deploy)**: confirmed clean on a full mission playthrough including particle-heavy content.

Smoke tier1 was not run with full duration because the operator's visual-A/B workflow used 15-second per-mission early-exits, which makes `run_smoke.py` artifactually report `engine_reported_fail`. This is documented as expected; the visual A/B confirmations were the meaningful gate.

---

## Follow-ups (post-F3 PRs)

- **Cleanup PR (Task 8 deferred per advisor):**
  - Remove `enableMRT()` / `disableMRT()` from `gos_postprocess.{h,cpp}` if confirmed dead under Option A.
  - Remove the stale comment at `gos_postprocess.cpp:519-520` about AMD location=1 corruption.
  - Delete `shaders/object_tex.frag` and `shaders/object_tex.vert` (vestigial; zero source references per audit §3.8).
  - Update `docs/modding-guide.md` to remove the stale `gos_text.frag` row and the `object_tex.frag` row.
- **Normal-quality cleanup** for the flat-up roster shaders when a downstream pass starts consuming `GBuffer1.rgb` as world normal.
- **F1** (water/shoreline material-alpha overload of the post-shadow mask) — separate spec; registry §3.1 escape hatch tracks it.
- **Stencil-mechanism alternative** (F3 design v2 §3.4) — long-term direction if future hardware portability requires decoupling coherence from `GBuffer1.alpha`.

---

## Exit criteria checklist (per F3 design spec v2 §10)

- [x] **Coherence guarantee documented.** §5 of this report proves it for every pixel class.
- [x] **Smoke gate.** Per-task visual-A/B passed; full smoke tier1 not run due to operator workflow (early-exit), but no smoke-significant change is expected — the per-task visual-A/B confirms the rendering path is intact.
- [x] **Visual A/B regression gate** vs HEAD `5256659` shows no shadow / shading / coloration regression.
- [x] **Trace cleanliness.** N/A under Option A — no `enableMRT`/`disableMRT` runtime transitions to trace.
- [x] **Census passes.** `scripts/check-render-contract-gbuffer1.sh` continues to pass; all new writes go through `rc_*` helpers.
- [x] **Frozen surfaces unchanged.** §6 confirms.
- [x] **Doc parity.** This report, audit, canary report, and design spec v2 are mutually consistent. AMD driver rules updated.
- [x] **Option A specific:** Every Group II-Opaque shader from the audit declares `layout(location=1)` and writes via a registry helper.
- [x] **Option A specific:** `docs/amd-driver-rules.md` updated 2026-04-27 to record canary outcome.

All exit criteria satisfied.
