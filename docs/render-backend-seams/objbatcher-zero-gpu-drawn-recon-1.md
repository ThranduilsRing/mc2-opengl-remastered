<!-- doc-status: current | reviewed:2026-10-11 -->
# OBJBATCHER-ZERO-GPU-DRAWN-RECON-1

Read-only. Classifies the suspicious `[OBJBATCHER v1] gpu_drawn_instances=0` observed
during MDI recon. **Do not fix blindly** — classify first. Source-verified vs nifty
`082070d1`, grounded by `[OBJBATCHER v1]` summaries from mc2_10 + mc2_24 traces.

## The observation (both missions)

```
mc2_10: eligible=977980 submitted_instances=4318 submit_buildings=4318 cpu_fallback=473
        fallback_rate=0.0005  gpu_drawn_instances=0
mc2_24: eligible=621609 submitted_instances=439  submit_buildings=439  cpu_fallback=2266
        fallback_rate=0.0036  gpu_drawn_instances=0
```

`gpu_drawn_instances=0` in BOTH — yet props clearly render: `submitted_instances` is
healthy (4318 / 439), `[PIPELINE_BIND] StaticPropOpaque` fired 2233x, `[FRAME_PLAN]
pass=StaticPropOpaque` fired, and the 2-mission smoke PASSes with props visible.

## Root cause: stale counter, NOT a draw failure

`gpu_drawn_instances` is incremented at exactly ONE site —
`GameOS/gameos/gos_static_prop_batcher_flush.cpp:400`:

```cpp
for (uint32_t typeID = 0; typeID < s_types.size(); ++typeID) {
    const TypeRangeSsbo& r = rit->second;
    if (r.instanceCount == 0 || type.packetCount == 0) continue;
    s_counters.gpu_drawn_instances += r.instanceCount;   // CPU-side count
```

It sums the **CPU-side** `TypeRangeSsbo.instanceCount`. But the live default draw path is
**coalesce v6 / C1b GPU-authority** (`glMultiDrawElementsIndirect` @6820/6842, C1b
@7188): the per-draw `instanceCount` is **authored by the GPU cull/patch shader directly
into the indirect command buffer** and is never read back to the CPU. So the CPU-side
`r.instanceCount` the counter reads is 0 → `gpu_drawn_instances` accumulates 0, even
though the GPU draws the correct (GPU-computed) instance counts.

**Verdict: dead/mismeasuring counter on the GPU-authority path.** It reflected reality
only on the older CPU-counted per-type path; under v6/C1b it is meaningless. NOT a
regression, NOT a rendering bug, NOT a routing problem.

## The real liveness signal

`submitted_instances` (incremented at submit time, before GPU cull) is the honest
"props entered the batch" signal — 4318 / 439, healthy. Combined with bind-positive
(`StaticPropOpaque`) and visual PASS, the static-prop GPU draw is confirmed working.

## cpu_fallback classified (the trickle is expected)

`cpu_fallback` rate is **0.05% (mc2_10) / 0.36% (mc2_24)** — a small, expected trickle.
`recordCpuFallback()` (@4490) fires for children the GPU batch legitimately skips:
helper/bone nodes (numVertices==0), untransformed/stale shapes, spotlight-not-night —
the documented skip-not-fail policy (@4528-4540). These fall to the legacy
`TG_Shape::Render` CPU path (classified DO_NOT_MODEL in
[static-prop-legacy-path-classify-1](static-prop-legacy-path-classify-1.md)). The
fallback route is correct; the rate is not a health concern.

## Acceptance (per the TD)

- **Zero-GPU reason identified:** counter reads CPU-side `instanceCount` (7011) which is 0
  under the GPU-authority v6/C1b path (counts live in the indirect buffer, never read back).
- **Fallback route classified:** 0.05-0.36% expected non-batchable children → legacy CPU
  render path; correct.
- **Not confused with pass routing:** props DO render (submitted_instances + bind-positive
  + visual PASS).
- **No behavior change** (read-only recon).

## Recommendation (future, optional — not this slice)

Either (a) **retire** `gpu_drawn_instances` in favor of `submitted_instances` (the honest
signal under GPU-authority), or (b) **wire it correctly** on the v6/C1b path — sum the MDI
command `instanceCount` fields via a small GPU→CPU readback of the indirect buffer (one
fence-gated read per N frames; do NOT stall the frame). Until then, treat
`gpu_drawn_instances` as a known-zero artifact on the default path and read
`submitted_instances` instead.

Out of scope: this is NOT the OBJBATCHER GPU-draw-coverage question — props ARE drawn.
There is no missing-geometry bug here.
