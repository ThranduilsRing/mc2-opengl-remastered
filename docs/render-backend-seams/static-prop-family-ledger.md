<!-- doc-status: current | reviewed:2026-10-11 -->
# Static-Prop Family Ledger

Ownership ledger for the static-prop draw-path nest. Companion to
[static-prop-path-ownership-recon-1](static-prop-path-ownership-recon-1.md). Enforced by
`scripts/check-static-prop-family.py` (registered `static_prop_family` in
check-contracts.sh): **every `applyPipeline` in `gos_static_prop_batcher.cpp` must carry a
dbgName** so every static-prop bind is observable. Source-verified vs nifty `5edb8708`
(+ STATIC-PROP-COLOR-LABEL-HARDENING-1).

## Ledger

| Path | PassId / label | PipelineDesc row | AttachmentContract | DrawBufferSet | Routed (applyPipeline @) | Bind-positive | Notes |
|---|---|---|---|---|---|---|---|
| Color (opaque) | `StaticPropOpaque` | StaticPropOpaque {0,1} | StaticPropOpaque | MainSceneMRT | ✅ 5377 | ✅ labeled | all MDI/instanced/coalesce variants |
| Color (alpha-test) | shares `StaticPropOpaque` | StaticPropAlphaTest (descriptive) | StaticPropOpaque path | MainSceneMRT | ✅ (via Opaque) | ✅ (via Opaque) | GL-identical to Opaque → no separate bind by design |
| Color re-test (Equal-depth) | `StaticPropOpaque` | StaticPropOpaque (depthFunc=Equal, no write) | StaticPropOpaque | MainSceneMRT | ✅ 6431 | ✅ labeled | depth-prepass companion |
| Depth prepass | `StaticPropDepth` | StaticPropDepth {f,f,f} | depth-only | Unspecified | ✅ 5138 | ✅ labeled | gate MC2_STATIC_PROP_DEPTH_PREPASS |
| Shadow (dynamic + static-bldg) | `ShadowStaticProp` | ShadowStaticProp | depth-only | ShadowDepthOnly | ✅ 7647 / 7800 | ✅ labeled | polygonOffset ON |
| Shadow (raw-GL variant) | — | ShadowStaticProp | depth-only | ShadowDepthOnly | ❌ raw `glUseProgram` 7444 | n/a | STATIC-PROP-LEGACY-PATH-CLASSIFY-1 |
| Legacy CPU/MLR fallback | — | — | — | — | ❌ | n/a | classify next; not live by default |
| Picking / editor preview | — | — | — | — | DO_NOT_MODEL | — | terminal |

## What the checker enforces (FAIL)

1. Every `applyPipeline(...)` in `gos_static_prop_batcher.cpp` passes a string dbgName
   (no invisible static-prop bind).
2. This ledger doc exists.

## Key facts (do not re-derive)

- **Alpha-test is not separately routed, by design.** `StaticPropOpaque` and
  `StaticPropAlphaTest` PipelineDesc rows produce byte-identical GL through
  `applyPipeline` (both `blend` → `glDisable(GL_BLEND)`); alpha-test differs only by
  shader `discard` + texture-array. A separate `applyPipeline(StaticPropAlphaTest)`
  would be a no-op. The AlphaTest row stays descriptive (DrawPacket metadata +
  bindProgram).
- **The color path was the blind spot.** Before hardening, the live color/depth
  `applyPipeline` calls passed no dbgName → no `[PIPELINE_BIND]`/`[FRAME_PLAN]`. Now
  labeled `StaticPropOpaque` / `StaticPropDepth` (bind-positive).

## Open (next slices)

- STATIC-PROP-LEGACY-PATH-CLASSIFY-1: the raw-GL shadow variant (`glUseProgram` @7444)
  and the CPU/MLR fallback — route, leave, or DO_NOT_MODEL.
- Out of scope (separate track): OBJBATCHER `gpu_drawn_instances=0` + `cpu_fallback` on
  mc2_10 — a GPU-draw coverage gap, not a routing/ownership issue.
