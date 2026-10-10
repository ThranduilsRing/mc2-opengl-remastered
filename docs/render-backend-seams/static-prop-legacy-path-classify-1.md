<!-- doc-status: current | reviewed:2026-10-11 -->
# STATIC-PROP-LEGACY-PATH-CLASSIFY-1

Read-only. Classifies the two non-`applyPipeline` static-prop paths left open by
[static-prop-family-ledger](static-prop-family-ledger.md): route, leave, or
DO_NOT_MODEL. Source-verified vs nifty `b1c4a80f`. Files `GameOS/gameos/gos_static_prop_batcher.cpp`
unless noted.

## 1. Raw-GL foliage shadow caster — `flushShadow()` (glUseProgram @7451)

**What:** the camera-visible static-prop SUN shadow caster that routes tree/foliage
types. Default-on (kill `MC2_SHADOW_ENABLE=0`). Picks its program at RUNTIME via
`resolvePropShadowProgram(usingAlphaProg)` (@7429) — the alpha-tested prop-shadow program
(SHADOW-PROP-ALPHA-1, leaf-shaped silhouettes) when available, else the empty-frag
program. Sets uniforms (`setupPropShadowAlphaUniforms`), then `glUseProgram(shadowProg)`
(@7451) and draws. Save/restores program/VAO/elem-buf/SSBO-0 around the pass (@7445-7449)
— correct (a leak here poisons the later cull dispatch / indirect draw).

**Why it's raw-GL:** its fixed-function state mirrors the `ShadowStaticProp` PipelineDesc
row (DepthFunc::Less, polygonOffset ON, no color), but the PROGRAM is selected at runtime
between two shaders. `applyPipeline`'s single `glProgramName` model can't express a
runtime program swap, so the pass hand-binds.

**Distinct from the routed shadow draws:** the `[PIPELINE_BIND] ShadowStaticProp` lines
seen in traces (9072x mc2_24 / 9208x mc2_10) come from the **building / dynamic-prop**
shadow draws (`applyPipeline(ShadowStaticProp)` @7647/@7799), NOT this one. So
`flushShadow` is a second, currently-invisible (raw-GL, no `[PIPELINE_BIND]`) live path.

**Verdict: ROUTABLE-LATER (low priority, no bug).** `applyPipeline` skips `glUseProgram`
when `glProgramName==0`, so a future small slice could call
`applyPipeline(ShadowStaticProp-with-prog-0, "ShadowStaticPropFoliage")` to ASSERT the
fixed-function state + get a label, then keep its own `glUseProgram(shadowProg)` for the
runtime-selected program. That would also make it bind-positive observable. NOT
DO_NOT_MODEL. Not urgent — it already mirrors the row and save/restores correctly.

## 2. CPU fallback — legacy `TG_Shape::Render` (mclib/tgl.cpp ~2530)

**What:** `recordCpuFallback()` (@4489) counts actors that could NOT be GPU-batched and
fell back to the legacy CPU TG_Shape render path. Triggers: program load failed
(`s_programLoadFailed`), invalid/empty multishape, or per-leaf ineligibility
(helper/bone node, untransformed/stale shape — see the skip policy @4528-4540). The
`cpu_fallback=N` smoke marker (e.g. mc2_10 = 4) reports these.

**Verdict: DO_NOT_MODEL (this arc).** This is the legacy TGL CPU rendering substrate, not
a GL pipeline pass — it has no `applyPipeline`/PipelineDesc and is exactly what the
RenderWorld arc is migrating AWAY from. Modeling it as a pass-attachment contract is the
wrong frame. The mc2_10 `gpu_drawn_instances=0 + cpu_fallback=4` symptom is the known
OBJBATCHER GPU-draw coverage gap — a separate track (improve GPU-batch eligibility), not
a routing/ownership slice.

## Picking / editor preview

DO_NOT_MODEL (terminal) — consistent with the pipeline-pass-coverage ledger.

## Net: static-prop family is now fully classified

| Path | Verdict |
|---|---|
| Color opaque / alpha-test / re-test | ROUTED + labeled (STATIC-PROP-COLOR-LABEL-HARDENING-1) |
| Depth prepass | ROUTED + labeled |
| Shadow (building / dynamic-prop) | ROUTED + labeled |
| Shadow (foliage, flushShadow raw-GL) | ROUTABLE-LATER (state mirrors row; runtime program swap) |
| CPU fallback (TG_Shape::Render) | DO_NOT_MODEL (legacy substrate; RenderWorld migration target) |
| Picking / editor preview | DO_NOT_MODEL |

Arc complete: every live static-prop GL pass is either routed+labeled or has an explicit
classified verdict. The only remaining routable item (foliage shadow) is documented, no
bug, low priority.
