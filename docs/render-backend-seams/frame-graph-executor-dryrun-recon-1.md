<!-- doc-status: stale | reviewed:2026-10-11 -->
# RECON — FRAME-GRAPH-EXECUTOR-DRYRUN-RECON-1

Per-frame observe-and-diff seam for the first safe executor step.

> All `file:line` are drift-prone. Re-grep with `repo_query.py slice-preflight` before any
> slice derived from this doc.

---

## TL;DR

The engine already has a per-frame seam (`noteRenderPass` / `beginPass` in
`mclib/render_contract.cpp`) that fires at pass entry for 7 of the 11 frame passes.
Four passes — Shadow, Water, VFX, VegetationCards — **never call noteRenderPass or
beginPass** (HIGH: invisible to any dry-run). The ambient probe (default-ON guard)
captures colorMask/depthFunc/depthWrite per declared pass, and the FBO ledger captures
bound FBO for 4 scene passes; terrain-branch telemetry is already live via
`terrainPathCount()`. The dry-run's unique value-add is the FIRED-SET + ORDER
check across all 11 passes per frame — neither the ambient guard nor the FBO ledger
currently do that. The pure-kernel struct is small, fully offline-testable, and fits
the existing RenderCore/\*.h pure-header pattern. Proposed gate: `MC2_FRAMEGRAPH_DRYRUN`.

---

## Q1 — The Per-Frame Seam

**Seam location:** `mclib/render_contract.cpp:670`

```cpp
void noteRenderPass(PassIdentity id, const char* callerHint)
```

Signature: takes the fine-grained `PassIdentity` (not the coarse `RenderPassId`). It:
1. Calls `ambientProbeAtPassBegin(toRenderPassId(id))` unconditionally — this fires the
   ambient guard (default-ON) and the FBO ledger check.
2. If `MC2_RENDER_PASS_TELEMETRY=1` and on a sampled frame, emits `[RENDER_PASS v1]`
   with FBO GLuint, viewport, drawbuffers.

There is also `beginPass(RenderCore::RenderPassId id)` at `mclib/render_contract.cpp:1241`.
It calls `ambientProbeAtPassBegin` then runs the CONTRACT-3 resource-ordering audit
(`MC2_RENDER_PASS_ORDER=1`). **`beginPass` is only called from tests and the harness
— no live callsite in production render code.**

`noteRenderPass` is the ONLY live production seam.

**Pass coverage at the noteRenderPass seam (live callsites):**

| Pass (kFramePassOrder slot) | Callsite | PassIdentity used |
|---|---|---|
| Shadow | NONE | — |
| StaticPropOpaque | `GameOS/gameos/gos_static_prop_batcher_submit.cpp:38` | `StaticProp` |
| Terrain (LODChunk) | `GameOS/gameos/gos_terrain_lod_chunk_matalbedo.cpp:30` | `TerrainBase` |
| Terrain (gameos_graphics) | `GameOS/gameos/gameos_graphics_tparams.cpp:184` | `TerrainBase` |
| MechOpaque | `tgl.cpp:3016` + `mclib/txmmgr_render_phases.cpp:110` | `OpaqueObject` |
| TerrainDecal | `GameOS/gameos/gameos_graphics_tparams.cpp:632` | `TerrainDecal` |
| TerrainOverlay | `gameos_graphics.cpp:10053` | `TerrainOverlay` |
| VFX | NONE | — |
| Water | NONE | — |
| VegetationCards | NONE | — |
| UI | `gameos_graphics.cpp:7718` (beginPassScope only, not noteRenderPass) | `UI` (scope only) |
| PostProcess | `gameosmain.cpp:606` | `PostProcess` |

**Summary:** 7 of 11 passes hit `noteRenderPass`. **Shadow, Water, VFX, and
VegetationCards never call noteRenderPass or beginPass** — they are fully invisible
to any probe that relies on this seam.

---

## Q2 — "What Actually Happened" Coverage Table

For each of the 11 kFramePassOrder passes:

| Pass | pass-fired? | ambient (colorMask/df/dw) | FBO ledger | barrier | terrain-branch |
|---|---|---|---|---|---|
| Shadow | **MISSING** | **MISSING** | **MISSING** | n/a (metadata only) | n/a |
| StaticPropOpaque | have (noteRenderPass) | HAVE (declared in kPassAmbient) | HAVE (kPassFboTarget) | n/a | n/a |
| Terrain | have (noteRenderPass) | HAVE (declared, re-assert landmine modeled) | HAVE (kPassFboTarget) | n/a | HAVE (terrainPathCount + dominantTerrainPathLive) |
| MechOpaque | have (noteRenderPass via OpaqueObject) | HAVE (declared) | **MISSING** (not in kPassFboTarget) | n/a | n/a |
| TerrainDecal | have (noteRenderPass) | NOT declared in kPassAmbient | HAVE (kPassFboTarget) | n/a | n/a |
| TerrainOverlay | have (noteRenderPass) | NOT declared in kPassAmbient | HAVE (kPassFboTarget) | n/a | n/a |
| Water | **MISSING** | NOT declared | **MISSING** | n/a | n/a |
| VegetationCards | **MISSING** | NOT declared | **MISSING** | n/a | n/a |
| VFX | **MISSING** | NOT declared | **MISSING** | n/a | n/a |
| UI | partial (beginPassScope only) | NOT declared | **MISSING** | n/a | n/a |
| PostProcess | have (noteRenderPass) | HAVE (declared, consumes latch modeled) | NOT declared (intentional — timing uncertain) | n/a | n/a |

Notes:
- "barrier" column: `BarrierKind` in contract is metadata-only; no runtime barrier
  enforcement exists, nothing to observe.
- "terrain-branch": only applies to Terrain pass. `terrainPathCount()` counters in
  `RenderCore/terrain_path_telemetry.h` are live. `dominantTerrainPathLive()`
  (`terrain_subpass_contract.h:181`) reads them at any call point.
- **HIGH: Shadow, Water, VFX, VegetationCards have zero observation today.**
  A dry-run cannot determine fired/not-fired for these 4 without new callsites.

---

## Q3 — The Diff Model

**Per axis, what counts as divergence:**

**(a) Pass fired / not fired / out of order** — NEW. No existing check walks kFramePassOrder
each frame and records a fired-set. The dry-run's primary contribution. Both "a pass did
not fire this frame" (absent from fired-set) and "a pass fired but no entry in expected
kFramePassOrder position relative to peers" are detectable only with a per-frame trace.

**(b) Ambient mismatch (colorMask/depthFunc/depthWrite)** — ALREADY CHECKED by the
shipped `MC2_FRAMEGRAPH_AMBIENT_GUARD` default-ON guard (`render_contract.cpp:824`).
Dry-run can reuse `compareAmbient()` but should NOT duplicate the guard — it should
read the mismatch counter (`mc2_ambient_mismatch_count()`) rather than re-check.

**(c) FBO target mismatch** — ALREADY CHECKED by the FBO ledger guard
(`render_contract.cpp:872`). Reuse `mc2_fbo_mismatch_count()`. Dry-run adds value
only for passes not yet in `kPassFboTarget` (Shadow, MechOpaque, Water, VFX, Veg, UI).

**(d) Terrain active-branch != modeled default / multiple branches fired simultaneously**
— PARTIALLY have. `dominantTerrainPathLive()` and `terrainPathsThatDrew()` in
`terrain_subpass_contract.h:159/166` are pure, already compile-tested. The dry-run
must call `terrainPathsThatDrew()` at frame-end and flag >1 as a mutual-exclusion
violation, and flag IndirectBridge as dominant with `latchActuallyImplemented=false`
as a latch-miss divergence.

**(e) Indirect latch-miss reality** — ALREADY MODELED in `terrain_subpass_contract.h:77`
(`latchActuallyImplemented=false` for IndirectBridge). `allDeclaredLatchProducersImplemented()`
returns false today — the dry-run should surface this as a known pending divergence, not
a new error.

**Dry-run value-add (novel over shipped guards):**
- Fired-set check across all 11 kFramePassOrder passes per frame.
- Cross-pass sequencing (did Shadow fire before StaticPropOpaque?).
- Coverage of the 4 invisible passes (Shadow/Water/VFX/Veg) once callsites are added.
- Single structured report aggregating all axes per frame.

---

## Q4 — Pure-Kernel Boundary

### Proposed trace struct (RenderCore/frame_pass_trace.h — new file)

```cpp
namespace RenderCore { namespace framegraph {

// Per-pass entry filled at noteRenderPass / new notePassFired callsite.
// All fields default to "not observed" so missing passes are detectable.
struct FramePassEntry {
    RenderPassId    id          = RenderPassId::None;
    bool            fired       = false;
    int             sequenceIdx = -1;     // order in which it was observed this frame
    AmbientSample   ambient;              // from ambient_contract.h; zero = not sampled
    RenderResourceId fboTarget  = RenderResourceId::Unknown;  // from ledger resolve
    TerrainPath     terrainBranch = TerrainPath::Count;       // Count = not terrain pass
    bool            terrainMultiBranch = false;               // >1 terrain branch fired
};

// One per kFramePassOrderCount slot.
struct FramePassTrace {
    FramePassEntry entries[kFramePassOrderCount];
    int            observedCount = 0;   // how many distinct passes fired
    int            frameSeq      = 0;   // frame number (from frameBegin tick)
};

// Pure comparison kernel — no GL, no globals. Takes a filled trace and the declared
// contracts, returns a structured report. Fully offline-testable.
struct PassDivergence {
    RenderPassId id;
    bool didNotFire;            // declared in kFramePassOrder but not observed
    bool outOfOrder;            // fired before a declared prerequisite
    bool ambientMismatch;       // compareAmbient returned any()
    bool fboMismatch;           // declared vs observed FBO target differ
    bool terrainMultiBranch;    // >1 terrain paths drew simultaneously
    bool terrainLatchMissing;   // dominant path has latchActuallyImplemented=false
};
struct DryRunReport {
    PassDivergence divergences[kFramePassOrderCount];
    int            divergenceCount;
    bool           anyDivergence;
};

DryRunReport dryRunCompare(const FramePassTrace& trace,
                           const RenderPassId* declaredOrder, int orderCount,
                           const RenderPassContract* contracts, int contractCount);

}} // namespace RenderCore::framegraph
```

### Thin runtime layer

The ONLY runtime mutation is calling a new `notePassFired(RenderPassId, AmbientSample, RenderResourceId fboActual)` at each pass entry — replacing or augmenting existing `noteRenderPass` callsites. For passes with no callsite today (Shadow, Water, VFX, Veg), new callsites are required. Gate: `MC2_FRAMEGRAPH_DRYRUN`.

`dryRunCompare()` is pure and has no GL dependency — it lives in `RenderCore/frame_pass_trace.h` (header-only, inline, no TU needed). The report dumps to the existing `debug_state/diagnostic_trace.jsonl` block under `"frame_graph"` key, surfaced via `get_render_health()` MCP.

---

## Q5 — Readiness Gaps and Next Steps

Handoff states executor readiness ~35-45%. After a dry-run lands:

**Remaining gaps toward a real (island-owning) executor:**
1. Shadow/Water/VFX/VegetationCards still have zero observation seams — must add
   `notePassFired` callsites at actual draw sites before an executor can own them.
2. The ambient ledger is sparse — TerrainDecal, TerrainOverlay, Water, VFX, UI, Veg
   have no `kPassAmbient` rows. An executor that emits barriers or reorders passes must
   know these axes for all passes.
3. The FBO ledger covers 4 scene passes; Shadow FBO (`dynShadowFBO_`, `shadowFBO_`)
   and post-process internal FBOs (`ssaoFBO_`, `hzbFBO_`) are not registered.
4. The Indirect terrain latch-miss (`terrain_subpass_contract.h:77`) is a real bug —
   the dry-run will flag it immediately when IndirectBridge is dominant. Fix before
   claiming executor owns Terrain.
5. `beginPass` / `endPass` calls exist only in tests and the harness, not in live render
   code. An executor needs these wired everywhere before it can sequence passes.

**First safe executor-owned island candidate:** PostProcess.

Evidence: (a) it has a `noteRenderPass` callsite (`GameOS/gameos/gameosmain.cpp:793`), (b) its FBO
target (`kPassFboTarget`) is intentionally not declared yet (timing-uncertain) but the
probe data will resolve this, (c) it has no `producesTerrainLatch` or
`reassertsColorMaskAllOn` side-effect that other passes depend on, (d) it is the last
pass in `kFramePassOrder` — an executor owning only the last pass cannot break ordering
for any pass that precedes it, (e) the dry-run will confirm whether PostProcess fires
every frame and in last position before an executor claims ownership.

Shadow, Terrain, and objects are NOT safe first islands: Shadow has zero observation;
Terrain has latch-miss and 4-branch path variability; objects (MechOpaque) has the
ambient ledger gap for `AlphaObject` / `OpaqueObject` conflation.

---

## Q6 — Sizing, Risk, Gate Name

**Behavior-change risk: ZERO.** The dry-run is observe-and-diff only. All GL state
reads are already performed by the ambient guard and the FBO ledger. The only new code
is: (a) `FramePassTrace` accumulator reset at `frameBegin()`, (b) trace-entry fill at
each `noteRenderPass` callsite (adds one struct assignment behind the `MC2_FRAMEGRAPH_DRYRUN`
gate check), (c) `dryRunCompare()` call at frame-end, (d) dump to JSONL.

**Gate name:** `MC2_FRAMEGRAPH_DRYRUN` — default-OFF, matches the arc's default-OFF
measure-first pattern for all prior slices (ambient probe, FBO ledger).

**Surfacing:** dump block `"frame_graph_dryrun"` in `debug_state/diagnostic_trace.jsonl`
alongside the existing `"frame_graph"` block. Exposed via `get_render_health()` MCP
(field `framegraphDryrunDivergences`). CI: a new `scripts/check-framegraph-dryrun.py`
reads the dump and exits 1 if `divergenceCount > 0` on a non-known-pending divergence
(the Indirect latch-miss is whitelisted until fixed).

**Effort:** Medium.
- New file: `RenderCore/frame_pass_trace.h` (~120 lines, pure header).
- Edit: `mclib/render_contract.cpp` — add trace accumulator, fill at each
  existing `noteRenderPass` callsite, add 4 new `notePassFired` callsites for Shadow /
  Water / VFX / VegetationCards at their draw sites.
- Edit: `GameOS/gameos/debug_state_dump.cpp` — emit `frame_graph_dryrun` block.
- New test: `tests/unit/test_frame_pass_trace.cpp` — offline pure-kernel tests for
  dryRunCompare() covering: all-fired-in-order=clean; missing pass=divergence;
  wrong-order=divergence; terrain multi-branch=divergence.
- New script: `scripts/check-framegraph-dryrun.py`.

---

## MODELING / SLICE PROPOSAL (not built)

### New files

| File | Purpose |
|---|---|
| `RenderCore/frame_pass_trace.h` | `FramePassEntry`, `FramePassTrace`, `DryRunReport`, pure `dryRunCompare()` |
| `tests/unit/test_frame_pass_trace.cpp` | doctest coverage of dryRunCompare() offline |
| `scripts/check-framegraph-dryrun.py` | CI gate reading dump JSON, exits 1 on non-whitelisted divergence |

### Modified files

| File | Change |
|---|---|
| `mclib/render_contract.cpp` | trace accumulator reset at `frameBegin()`; fill `FramePassEntry` at each `noteRenderPass` callsite; add 4 new callsites for invisible passes; `dryRunCompare()` at frame-end; dump emission |
| `GameOS/gameos/debug_state_dump.cpp` | `"frame_graph_dryrun"` JSON block |
| `mclib/render_contract.h` | expose `notePassFired()` or extend `noteRenderPass()` signature to carry `RenderPassId` directly |

### Test cases that gate the slice

1. **all-passes-fired-in-order** — trace with all 11 passes in kFramePassOrder sequence
   → `anyDivergence=false`.
2. **missing-pass** — Shadow omitted from trace → `divergences[Shadow].didNotFire=true`.
3. **out-of-order** — StaticPropOpaque fires before Shadow → `outOfOrder=true`.
4. **terrain-multi-branch** — trace shows both LODChunk and IndirectBridge fired →
   `divergences[Terrain].terrainMultiBranch=true`.
5. **terrain-latch-miss** — IndirectBridge dominant,
   `latchActuallyImplemented=false` → `divergences[Terrain].terrainLatchMissing=true`.
6. **ambient-mismatch** — Shadow entry has depthFunc=SceneGEqual (wrong) →
   `divergences[Shadow].ambientMismatch=true`.

### HIGH findings

- **Shadow/Water/VFX/VegetationCards have ZERO observation seams today.** A dry-run
  that only hooks existing `noteRenderPass` callsites cannot determine whether these
  passes fired, in what order, or with what GL state. New callsites at the actual draw
  sites are prerequisite work for the slice. Shadow in particular is declared to
  produce `ShadowDynamicMap` — if it silently skips, downstream passes read stale depth
  and no existing check catches it.

- **`beginPass` / `endPass` are test-only today** (`mclib/render_contract.cpp:1258/922`).
  The CONTRACT-3 resource-ordering audit they implement is never exercised in live
  render code. This is the structural gap between "declared contracts pass offline" and
  "contracts checked at runtime per frame." The dry-run's `notePassFired` approach
  closes this gap incrementally without requiring a full `beginPass`/`endPass` retrofit
  across all pass owners.

- **IndirectBridge latch-miss already flagged** (`terrain_subpass_contract.h:77`,
  `latchActuallyImplemented=false`). The dry-run will immediately report it when
  IndirectBridge is dominant. This is a REAL existing bug, not a dry-run artifact.
  Whitelist it in `check-framegraph-dryrun.py` until `markTerrainDrawn` is called from
  the Indirect draw path.
