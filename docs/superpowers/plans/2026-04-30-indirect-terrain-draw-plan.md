# Indirect Terrain Draw Implementation Plan (v2 — SOLID-only)

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Plan v1 was stop-the-lined at adversarial review; this is the revised v2 incorporating the orchestrator's decisions and the 8 mechanical fixes (M1-M8) + 4 scope-realignment additions (N1-N4) it required. See "Verification Appendix" at the end of this plan — every cited symbol has been grep'd against the actual source tree.

**Goal:** Reduce the per-frame CPU cost in `Terrain::geometry quadSetupTextures` (~3.01 ms post-slice-2b) by retiring the **SOLID-only main-emit setup** and drawing terrain solid via a single `glMultiDrawArraysIndirect` hooked AT `Render.TerrainSolid` inside `renderLists()`. Detail (`MC2_DRAWALPHA`), overlay (`gos_PushTerrainOverlay`), and mine state stay on legacy paths in this slice. Detail/overlay/mine consolidation is a follow-up slice with its own brainstorm. This is the architectural endpoint of the SOLID arc; the broader CPU→GPU offload arc (slice 23 of 24) continues with the deferred follow-ups.

**Architecture:** Three buffers, one draw — mirrors `memory/water_ssbo_pattern.md`. **Static `TerrainRecipeSSBO`** (`GL_SHADER_STORAGE_BUFFER`, dense, indexed by `vertexNum = mapX + mapY * realVerticesMapSide` — see Verification Appendix [V1]); recipe layout is the existing 9-vec4 / 144 B `TerrainQuadRecipe` struct from [gos_terrain_patch_stream.h:87-99](GameOS/gameos/gos_terrain_patch_stream.h:87) — **no growth in PR1**, since terrainHandle / uvMode / mineState live elsewhere. **Per-frame `TerrainThinRecordSSBO`** (existing 32 B `TerrainQuadThinRecord` from h:103-111, triple-buffered ring — no schema change). **Per-frame `TerrainIndirectCommandBuffer`** (`GL_DRAW_INDIRECT_BUFFER` — *not* an SSBO; the GL driver consumes 1 `DrawArraysIndirectCommand` for SOLID-only PR1; sized for ≤16 future buckets at `16 × 16 = 256 B` headroom). **Draw primitive: `glMultiDrawArraysIndirect`** — the thin VS uses `gl_VertexID` exclusively ([gos_terrain_thin.vert:55-59](shaders/gos_terrain_thin.vert:55)) and reads geometry from SSBOs, so no element/index buffer is required (the M2 path's `glDrawArrays` precedent confirms — see [gos_terrain_patch_stream.cpp:25, 32, 37](GameOS/gameos/gos_terrain_patch_stream.cpp:25)). VS = existing [shaders/gos_terrain_thin.vert](shaders/gos_terrain_thin.vert) (binding=1 RecipeBuf, binding=2 ThinRecordBuf — see [V3]); FS = unchanged terrain FS. Hook AT `Render.TerrainSolid` ([mclib/txmmgr.cpp:1330](mclib/txmmgr.cpp:1330)), replacing `TerrainPatchStream::flush()` when **preflight-armed** for SOLID — and **same-frame fallback to legacy SOLID is impossible by construction** because the gate-off has already fired at admit time (see Stage 3 Task 3.5).

**Tech Stack:** C++17, OpenGL 4.3 (SSBO + std430 + `glMultiDrawArraysIndirect`), GLSL 4.30, Tracy Profiler, Windows MSVC (RelWithDebInfo). MC2 codebase conventions (env-gated `[SUBSYS v1]` lifecycle prints, `[INSTR v1]` startup banner, `gos_RendererRebindVAO()` for AMD VAO 0 trap, `glEnableVertexAttribArray(0)` for AMD attribute 0 trap per [docs/amd-driver-rules.md:5](docs/amd-driver-rules.md:5), `setMat4Direct`/`setMat4Std` matrix-upload helpers).

---

## Source documents (read-first for executor)

- **Brainstorm (Q1–Q7 settled, with Q3 plan-time addendum below):** [docs/superpowers/brainstorms/2026-04-30-indirect-terrain-draw-scope.md](docs/superpowers/brainstorms/2026-04-30-indirect-terrain-draw-scope.md)
- **Design (HOW):** [docs/superpowers/specs/2026-04-30-indirect-terrain-draw-design.md](docs/superpowers/specs/2026-04-30-indirect-terrain-draw-design.md)
- **Recon handoff (Items 1–9 resolved):** [docs/superpowers/plans/progress/2026-04-30-indirect-terrain-recon-handoff.md](docs/superpowers/plans/progress/2026-04-30-indirect-terrain-recon-handoff.md)
- **Adversarial-plan-review skill (use before declaring v2 ship-ready):** [.claude/skills/adversarial-plan-review.md](.claude/skills/adversarial-plan-review.md)
- **Pattern template:** `memory/water_ssbo_pattern.md` — proven on renderWater Stage 1+2+3
- **Bring-up checklist:** `memory/gpu_direct_renderer_bringup_checklist.md` — 9 traps, all apply
- **Predecessor:** `memory/m2_thin_record_cpu_reduction_results.md` — what's already in the recipe + thin-record pipeline

## Brainstorm Q3 plan-time addendum (per N3)

Brainstorm Q3 ("interaction with recipe SSBO + thin-record SSBO — merge or stay parallel") inherited stale memory: the recorded decision claims "recipe contents stay the same: terrainHandle, overlayHandle, detailHandle, uvData, mine state cache" — but the actual `TerrainQuadRecipe` struct in [gos_terrain_patch_stream.h:87-99](GameOS/gameos/gos_terrain_patch_stream.h:87) carries **only** positions/normals/UV (9 vec4s, 144 B). `terrainHandle`, `flags`, and lighting live on `TerrainQuadThinRecord` (h:103-111). `overlayHandle` and `terrainDetailHandle` are NOT cached anywhere — they're computed per-frame inside `setupTextures()` and consumed by `addTriangle()`/`gos_PushTerrainOverlay` directly. Mine state lives in `quad.mineResult` (set inline) and `GameMap` (`tileMineCount` per slice 2b).

**Q3's answer is narrowed at plan time to: "parallel for SOLID-PR1; multi-bucket consolidation deferred to follow-up slice with its own brainstorm."** PR1 reuses the existing 144 B recipe and 32 B thin record schemas verbatim — no growth, no new fields. Detail/overlay/mine consolidation will need its own data-design pass that grep's the current call sites before proposing field additions.

## Slice arc summary

This slice closes the **SOLID-only** portion of the per-frame `quadSetupTextures` cost. Once it ships at default-on, terrain solid main-emit is "CPU writes the SSBO on mutation events; GPU reconstructs the solid pass every frame from the SSBO + camera state." Detail, overlay, and mine continue running through legacy `addTriangle`/`gos_PushTerrainOverlay`. The legacy M2 thin-record path remains as `MC2_TERRAIN_INDIRECT=0` opt-out fallback; physical deletion of the SOLID legacy is queued as a post-soak follow-up slice.

The 4-gate ladder applies to the cumulative slice (Stages 0–3 = PR1; Stage 4 = PR2). Each individual stage has its own narrower validation checkpoint defined in-stage.

---

## Out of scope (this slice — explicit rejection list)

- **Detail (`MC2_DRAWALPHA`) consolidation.** Legacy `setupTextures` still calls `addTriangle(terrainDetailHandle, MC2_DRAWALPHA)` — stays as-is. Future slice; needs its own brainstorm.
- **Overlay (`gos_PushTerrainOverlay`) consolidation.** Stays legacy; future slice.
- **Mine state cache migration.** `setMine` chokepoint, `mineResult.setMine()` per-quad cache — all stays on the slice 2b infrastructure. Future slice.
- **Depth-fudge addition to thin VS.** Per design doc Constraints #4 → #9, this is DEFERRED to its own slice. Adding `+ 0.001` would invalidate the existing M2 path's tier1 5/5 PASS baseline. Indirect-terrain reuses the thin VS as-is and inherits the same depth behavior the M2 path has had since M2d shipped. Plan v1 had this as Stage 1 — explicitly removed in v2.
- **`drawPass` outer-loop elimination via GPU compute.** Brainstorm Q1 (c) — explicit follow-up.
- **GPU compute writes the indirect command buffer.** Brainstorm Q2 (b) — explicit follow-up.
- **Mod content validation.** Per `memory/feedback_offload_scope_stock_only.md`, validation is stock missions only.
- **Recipe field growth.** PR1 keeps the 144 B / 9 vec4 layout intact. No `overlayHandle`, no `terrainDetailHandle`, no `classifierFlags`. Future slice will revisit.

---

## File structure (touched across all stages)

| File | Stage | Responsibility |
|---|---|---|
| `GameOS/gameos/gos_terrain_indirect.{h,cpp}` (NEW in 0; extended in 2, 3) | 0, 2, 3 | Stage 0: env-gate readers + parity-printer skeleton + counters. Stage 2: dense-recipe build + per-mission Reset/Build. Stage 3: per-frame thin-record packer, indirect-command builder, draw entry, preflight-arming bool, bridge state save/restore |
| `GameOS/gameos/gameos_graphics.cpp` | 3 | Bridge function `gos_terrain_bridge_drawIndirect` (state save/restore including `glColorMask`, sampler, depth pipeline, AMD `glEnableVertexAttribArray(0)`) |
| `GameOS/gameos/gameosmain.cpp` | 0 | `[INSTR v1]` banner extension; `_cbbuf` size grown 384 → 512 to absorb new fields |
| `mclib/terrain.cpp` | 2, 3 | `primeMissionTerrainCache` recipe Build/Reset call (sibling of `WaterStream::Reset() + Build()`); `Terrain::destroy` recipe Reset call |
| `mclib/mapdata.cpp` | 2 | `invalidateTerrainFaceCache` body adds per-entry `invalidateRecipeForVertexNum(vn)` calls; signature **unchanged** (still `void` — see [V2]) |
| `mclib/txmmgr.cpp` | 3 | Hook AT `Render.TerrainSolid` (line 1330) using preflight-armed bool |
| `mclib/quad.cpp` | 3 | Surgical SOLID-only gate-off of `addTriangle(MC2_DRAWSOLID)` lines inside `setupTextures()` (lines 466-467, 539-540, etc. — see [V8]) when `terrainIndirectSolidArmed` is true for the frame |
| `scripts/run_smoke.py` | 0, 4 | Env-allowlist propagation |
| `tests/smoke/smoke_missions.txt` | 4 | (Read-only — no edits; tier1 list is mc2_01, mc2_03, mc2_10, mc2_17, mc2_24) |
| `memory/indirect_terrain_solid_endpoint.md` (NEW) | 4 | Slice closeout memory |
| `memory/MEMORY.md` | 4 | Index entry for new memory |

---

## Stage 0 — Scaffolding, parity printer, & counters

**Scope:** No behavior change. Land env-gate plumbing, Tracy zone names, `[INSTR v1]` banner extension (with `_cbbuf` grown 384 → 512), parity-check printer/summary skeleton (silent until Stage 2 plugs in actual comparisons), and the **three counters from N1** (silent zeros until Stage 3). All subsequent stages assume this scaffolding exists.

**Files:**
- Modify: [GameOS/gameos/gameosmain.cpp:625-632](GameOS/gameos/gameosmain.cpp:625) (banner: grow `_cbbuf` 384→512, add `terrain_indirect=%d terrain_indirect_parity=%d`)
- Modify: `scripts/run_smoke.py` env-allowlist (add `MC2_TERRAIN_INDIRECT`, `MC2_TERRAIN_INDIRECT_PARITY_CHECK`, `MC2_TERRAIN_INDIRECT_TRACE`)
- Create: `GameOS/gameos/gos_terrain_indirect.h` (env-gate getters, parity-printer prototypes, counters interface)
- Create: `GameOS/gameos/gos_terrain_indirect.cpp` (env-gate readers + `[TERRAIN_INDIRECT_PARITY v1]` printer + 600-frame summary cadence; bodies left as no-ops Stage 2/3 fill in)

### Task 0.1 — Env-gate readers (boot-time once)

- [ ] **Step 1:** Add reader functions in `gos_terrain_indirect.cpp`:

```cpp
namespace gos_terrain_indirect {
// Stage 0–3 (default off):  read "is env=1 specifically set?"
// Stage 4   (default on):   inverted in Task 4.1.
bool IsEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}
bool IsParityCheckEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT_PARITY_CHECK");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}
}  // namespace
```

- [ ] **Step 2:** Declare both in `gos_terrain_indirect.h`.

### Task 0.2 — Extend `[INSTR v1]` startup banner; grow `_cbbuf` to 512

The current `_cbbuf` at [gameosmain.cpp:625](GameOS/gameos/gameosmain.cpp:625) is sized `[384]`. Adding two new `terrain_indirect=%d terrain_indirect_parity=%d` fields adds ~50 chars including formatting; with the existing format string already near 300 chars and the build-string suffix, 384 is at the truncation edge. Grow to 512.

- [ ] **Step 1:** Edit [gameosmain.cpp:625-632](GameOS/gameos/gameosmain.cpp:625):

```cpp
const bool tInd  = gos_terrain_indirect::IsEnabled();
const bool tIndP = gos_terrain_indirect::IsParityCheckEnabled();
char _cbbuf[512];   // was [384] — grew to absorb terrain_indirect{,_parity}
snprintf(_cbbuf, sizeof(_cbbuf),
    "[INSTR v1] enabled: tgl_pool=%d destroy=%d gl_error_print=%d "
    "smoke=%d water_fp=%d water_parity=%d vp_fast=%d vp_parity=%d "
    "terrain_indirect=%d terrain_indirect_parity=%d build=%s",
    tgl ? 1 : 0, destr ? 1 : 0, glprint ? 1 : 0, smoke ? 1 : 0,
    waterFp ? 1 : 0, waterPc ? 1 : 0, vpFast ? 1 : 0, vpPar ? 1 : 0,
    tInd ? 1 : 0, tIndP ? 1 : 0, build);
```

(The `_cbbuf` referenced by orchestrator brief item M8 is the **banner buffer** at [gameosmain.cpp:625](GameOS/gameos/gameosmain.cpp:625), not the indirect command buffer. The indirect command buffer is named `TerrainIndirectCommandBuffer` and is sized in Task 3.2 below at 320 B = 16 commands × 20 B headroom.)

- [ ] **Step 2:** Smoke-run unset; banner shows `terrain_indirect=0 terrain_indirect_parity=0`. Smoke-run with `MC2_TERRAIN_INDIRECT=1`; banner shows `=1`.

### Task 0.3 — Add the three N1 counters with PUBLIC increment functions (units = per-quad)

Per N1 — without these, Gate B can pass on "renderer time went down" while completely missing the CPU-offload goal. The advisor's cleanup also caught two issues: (a) translation-unit-static counters can't be incremented from `quad.cpp` directly — must wrap in functions; (b) the helper macro wraps individual `addTriangle` calls (TWO per quad in each cluster), so a naive `++counter` would double-count. Plan v2 fixes both: counters live as private file-scope statics in `gos_terrain_indirect.cpp`; the public API is a function-call interface that always counts in **per-quad units** (one call per cluster, not one per triangle admit).

- [ ] **Step 1:** Add counter storage (private to the cpp file):

```cpp
// gos_terrain_indirect.cpp — counter storage (private)
namespace {
    long long s_legacy_solid_setup_quads     = 0;  // SHOULD drop ≈0 when armed
    long long s_indirect_solid_packed_quads  = 0;  // SHOULD match (un-armed legacy SOLID quads)
    long long s_legacy_detail_overlay_quads  = 0;  // SHOULD remain non-zero (out of scope)
}
```

- [ ] **Step 2:** Add public function API in `gos_terrain_indirect.h` and bodies in `.cpp`:

```cpp
// gos_terrain_indirect.h — counter API (units = per-quad, NOT per-triangle).
namespace gos_terrain_indirect {
// Each call increments by exactly ONE quad. Callers must wrap a paired
// addTriangle cluster (e.g., quad.cpp lines 466-467) and call ONCE per
// cluster, not once per addTriangle.
void Counters_AddLegacySolidSetupQuad();      // un-armed legacy SOLID admit cluster
void Counters_AddIndirectSolidPackedQuad();   // armed indirect packer per packed quad
void Counters_AddLegacyDetailOverlayQuad();   // legacy DRAWALPHA detail/overlay/mine cluster
// Read-only access for the 600-frame summary printer.
long long Counters_GetLegacySolidSetupQuads();
long long Counters_GetIndirectSolidPackedQuads();
long long Counters_GetLegacyDetailOverlayQuads();
}  // namespace
```

- [ ] **Step 3:** The summary line in `ParityFrameTick` (Task 0.6 Step 1) reads counters via `Counters_Get*()`, not direct variable access (preserves the file-scope private boundary).

### Task 0.4 — Add `MC2_TERRAIN_INDIRECT*` to `scripts/run_smoke.py` env-allowlist

- [ ] **Step 1:** Locate the existing env-allowlist block (per renderWater Stage 2 convention; the executor session greps `MC2_VERTEX_PROJECT_FAST` to find it). Add **four** entries:

```python
"MC2_VERTEX_PROJECT_FAST",
"MC2_VERTEX_PROJECT_PARITY",
"MC2_TERRAIN_INDIRECT",
"MC2_TERRAIN_INDIRECT_PARITY_CHECK",
"MC2_TERRAIN_INDIRECT_TRACE",  # enables [TERRAIN_INDIRECT v1] lifecycle prints
"MC2_TERRAIN_COST_SPLIT",      # enables Stage 1 per-frame steady_clock accumulators
```

`MC2_TERRAIN_INDIRECT_TRACE` enables `[TERRAIN_INDIRECT v1]` lifecycle prints (`event=recipe_build`, `event=first_draw`, `event=hard_failure`, `event=zero_commands`, `event=invalidate`, `event=recipe_reset`). `MC2_TERRAIN_COST_SPLIT=1` enables the Stage 1 cost-split timers (default OFF — when unset, the timer scopes are zero-cost no-ops, see Task 1.1 Step 2 below). Stage 3 Task 3.7 Step 4b greps for these in artifacts to confirm the fast path actually drew under `INDIRECT=1`.

### Task 0.5 — Tracy zone reservations

- [ ] **Step 1:** Add zone-name constants in `gos_terrain_indirect.h` so future stages emit consistent names:

```cpp
// Stage 1 zones (recon re-baseline, no code wiring yet — see Stage 1):
//   "Terrain::SetupSolidBranch"      per-frame, around the SOLID-only addTriangle calls
//   "Terrain::SetupDetailOverlayBranch" per-frame, around DRAWALPHA/overlay/mine code
// Stage 2 zones:
//   "Terrain::IndirectRecipeBuild"   one-shot @ primeMissionTerrainCache build
//   "Terrain::IndirectRecipeReset"   one-shot @ Terrain::destroy / mission teardown
// Stage 3 zones:
//   "Terrain::ThinRecordPack"        per-frame @ packer entry
//   "Terrain::IndirectDraw"          per-frame @ glMultiDrawArraysIndirect call
// Aggregator (Stage 3, stretch):
//   "Terrain::TotalCPU"              per-frame @ outer-most caller scope
```

- [ ] **Step 2:** No code emits these yet — they're documented for the executor session to use verbatim.

### Task 0.6 — Parity-printer + counter-summary skeleton

- [ ] **Step 1:** Add the printer in `gos_terrain_indirect.cpp`:

```cpp
static int s_parityMismatchesThisFrame = 0;
static long long s_paritySummaryFrames = 0;
static long long s_paritySummaryQuads = 0;
static long long s_paritySummaryMismatches = 0;

void ParityPrintMismatch(int frame, int quad, const char* layer, int tri,
                         int vert, const char* field, uint32_t legacy, uint32_t fast) {
    if (s_parityMismatchesThisFrame >= 16) return;  // throttle 16/frame
    s_parityMismatchesThisFrame++;
    fprintf(stderr,
        "[TERRAIN_INDIRECT_PARITY v1] event=mismatch frame=%d quad=%d "
        "layer=%s tri=%d vert=%d field=%s legacy=0x%08X fast=0x%08X\n",
        frame, quad, layer, tri, vert, field, legacy, fast);
    fflush(stderr);
}

void ParityFrameTick(int quadsCheckedThisFrame) {
    s_paritySummaryFrames++;
    s_paritySummaryQuads += quadsCheckedThisFrame;
    s_paritySummaryMismatches += s_parityMismatchesThisFrame;
    s_parityMismatchesThisFrame = 0;
    if (s_paritySummaryFrames % 600 == 0) {
        fprintf(stderr,
            "[TERRAIN_INDIRECT_PARITY v1] event=summary frames=%lld "
            "quads_checked=%lld total_mismatches=%lld "
            "legacy_solid_setup_quads=%lld indirect_solid_packed_quads=%lld "
            "legacy_detail_overlay_quads=%lld\n",
            s_paritySummaryFrames, s_paritySummaryQuads, s_paritySummaryMismatches,
            Counters_GetLegacySolidSetupQuads(),
            Counters_GetIndirectSolidPackedQuads(),
            Counters_GetLegacyDetailOverlayQuads());
        fflush(stderr);
    }
}
```

- [ ] **Step 2:** Add prototypes in header.

### Task 0.7 — Stage 0 commit

- [ ] **Step 1:** Final smoke

```bash
py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 20
```

Expected: tier1 5/5 PASS, +0 destroys delta, banner contains the two new fields, both `=0`. No `[TERRAIN_INDIRECT_*` lines (everything silent).

- [ ] **Step 2:** Commit:

```bash
git add GameOS/gameos/gos_terrain_indirect.{h,cpp} GameOS/gameos/gameosmain.cpp scripts/run_smoke.py
git commit -m "$(cat <<'EOF'
feat(terrain-indirect): Stage 0 scaffolding — env gates, parity printer, counters

Adds MC2_TERRAIN_INDIRECT and MC2_TERRAIN_INDIRECT_PARITY_CHECK env
readers (default off). Extends [INSTR v1] startup banner with
terrain_indirect / terrain_indirect_parity fields; grows _cbbuf
384->512 to absorb the new fields without truncation. Lifts
run_smoke.py env-allowlist for both plus MC2_TERRAIN_INDIRECT_TRACE.
Lands [TERRAIN_INDIRECT_PARITY v1] printer + 600-frame summary
skeleton with 3 N1 counters (legacy_solid_setup_quads,
indirect_solid_packed_quads, legacy_detail_overlay_quads); bodies are
no-ops Stage 2/3 fill in.

No behavior change. tier1 5/5 PASS, +0 destroys delta.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

**Validation checkpoint:** tier1 5/5 PASS, banner extended with both fields `=0`, no `[TERRAIN_INDIRECT_*` output. **Rollback story:** revert the commit; nothing else depends on this scaffolding. **Safe-to-revert boundary:** yes.

---

## Stage 1 — Recon re-baseline & SOLID/detail-overlay cost split (per-frame nanosecond accumulators)

**Scope:** No production code change. Capture the cost split between the SOLID admit clusters (target of this slice) and the detail/overlay/mine clusters (out of scope) inside [TerrainQuad::setupTextures](mclib/quad.cpp:429). **This measurement sets Gate B's perf target** for Stage 3 — without it, Gate B's "≥50%" target is uncalibrated against the SOLID-only scope.

**Why per-frame accumulators, not per-quad Tracy zones:** wrapping each paired SOLID/detail-overlay admit cluster in `ZoneScopedN(...)` would create roughly 8 zones × 14 000 quads × 60 fps ≈ 6.7 M zones/sec — Tracy's queue saturates well below that, the resulting trace becomes noise, and the cost of `ZoneScopedN` itself (TLS lookup + queue write) becomes comparable to the work we're trying to measure. The right pattern in this codebase is per-frame summation (mirrors slice 2b's mine-state counters and the PARITY summary lines). One Tracy zone (`Terrain::geometry quadSetupTextures`) already wraps the parent loop at [terrain.cpp:1681](mclib/terrain.cpp:1681) and provides the total — Stage 1's job is to split that total into S (SOLID) and D (detail/overlay/mine) reported via the existing summary print, not to add intra-zone Tracy zones.

**Files:**
- Modify: [mclib/quad.cpp](mclib/quad.cpp) — add per-frame `std::chrono::steady_clock` nanosecond accumulators that bracket each SOLID admit cluster and each detail/overlay/mine cluster; reset and reported per frame
- Modify: [GameOS/gameos/gos_terrain_indirect.cpp](GameOS/gameos/gos_terrain_indirect.cpp) — extend the 600-frame summary line with `solid_branch_ns_per_frame=N detail_overlay_branch_ns_per_frame=N` columns

### Task 1.1 — Land the per-frame accumulators (env-gated; ZERO-cost when unset)

- [ ] **Step 1:** Add an env-reader and the module-private accumulators in `gos_terrain_indirect.cpp`:

```cpp
namespace gos_terrain_indirect {
bool IsCostSplitEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_COST_SPLIT");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}
}  // namespace

namespace {  // private storage
long long s_solidBranchNanosThisFrame   = 0;
long long s_detailOverlayNanosThisFrame = 0;
long long s_solidBranchNanosTotal       = 0;
long long s_detailOverlayNanosTotal     = 0;
int       s_costSplitFramesObserved     = 0;
}

namespace gos_terrain_indirect {
void CostSplit_AddSolidNanos(long long n)        { s_solidBranchNanosThisFrame  += n; }
void CostSplit_AddDetailOverlayNanos(long long n){ s_detailOverlayNanosThisFrame += n; }
void CostSplit_RollFrame() {
    if (!IsCostSplitEnabled()) return;
    s_solidBranchNanosTotal       += s_solidBranchNanosThisFrame;
    s_detailOverlayNanosTotal     += s_detailOverlayNanosThisFrame;
    s_costSplitFramesObserved++;
    s_solidBranchNanosThisFrame   = 0;
    s_detailOverlayNanosThisFrame = 0;
}
long long CostSplit_GetSolidNanosTotal()        { return s_solidBranchNanosTotal; }
long long CostSplit_GetDetailOverlayNanosTotal(){ return s_detailOverlayNanosTotal; }
int       CostSplit_GetFramesObserved()         { return s_costSplitFramesObserved; }
}  // namespace
```

- [ ] **Step 2:** Add the RAII timer helpers — they **early-out via a cached env-flag bool** when `MC2_TERRAIN_COST_SPLIT` is unset, so production runs (without the flag) pay zero `steady_clock::now()` cost. The `IsCostSplitEnabled()` check is a load of a `static const bool` — a single branch-predicted instruction.

```cpp
// at top of quad.cpp anonymous namespace — both helpers gate on the env flag.
struct CostSplitSolidScope {
    std::chrono::steady_clock::time_point t0;
    bool active;
    CostSplitSolidScope()
        : active(gos_terrain_indirect::IsCostSplitEnabled()) {
        if (active) t0 = std::chrono::steady_clock::now();
    }
    ~CostSplitSolidScope() {
        if (!active) return;
        gos_terrain_indirect::CostSplit_AddSolidNanos(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count());
    }
};
struct CostSplitDetailOverlayScope { /* analogous, calls AddDetailOverlayNanos */ };

// usage at e.g. lines 466-467:
{ CostSplitSolidScope _s;
  mcTextureManager->addTriangle(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
  mcTextureManager->addTriangle(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
}
```

When the flag is unset, the constructor does one `bool` read and the destructor does one `bool` read — no `steady_clock::now()` call. When set, full timing engages.

- [ ] **Step 3:** Bracket each `MC2_DRAWALPHA` detail / mine / overlay admit cluster with `CostSplitDetailOverlayScope`.

- [ ] **Step 4:** Call `CostSplit_RollFrame()` once per frame at the end of the `Terrain::geometry quadSetupTextures` Tracy zone block at [terrain.cpp:1687](mclib/terrain.cpp:1687) (just after the for-loop closes). Internally gated, so unset = no-op.

- [ ] **Step 5:** Extend the 600-frame summary in `gos_terrain_indirect.cpp` with two new columns. Suppress the columns when the env flag is unset (avoids confusing readers with all-zero noise):

```
[TERRAIN_INDIRECT_PARITY v1] event=summary ... solid_branch_ns_per_frame=N detail_overlay_branch_ns_per_frame=N frames_observed=N
```

(Averages = totals / frames_observed; only emitted when `frames_observed > 0`.)

### Task 1.2 — Capture the split

- [ ] **Step 1:** Smoke run with the cost-split env flag set:

```bash
MC2_TERRAIN_COST_SPLIT=1 py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 60
```

- [ ] **Step 2:** Grep the artifact `stdout.txt` for the summary line with the new fields. Record:
   - **S** = `solid_branch_ns_per_frame` (let it be e.g. ~1 200 000 ns ≈ 1.2 ms)
   - **D** = `detail_overlay_branch_ns_per_frame` (let it be e.g. ~600 000 ns ≈ 0.6 ms)
   - The Tracy `Terrain::geometry quadSetupTextures` median is **T** ≈ 3.01 ms (from recon handoff baseline). T - S - D = "other" (texture handle resolution, flag computation, mineResult CPU-cache walks — all unconditional, all out of scope).

- [ ] **Step 3:** Write the captured S, D, T into `progress/2026-04-30-indirect-terrain-stage1-baseline.md` and the Stage 1 commit message. **Recalibrate Gate B** for Stage 3 as: post-armed `solid_branch_ns_per_frame` ≤ 0.20 × Stage-1-S (≥80% reduction on the SOLID branch). The parent `Terrain::geometry quadSetupTextures` Tracy zone delta is the secondary readout: expected reduction ≈ S/T × 80%, so if S = 1.2 ms and T = 3.01 ms, the parent zone drops by ~0.96 ms (≈32% reduction on the parent). This bounds expectations honestly — without Stage 1, plan v1's "≥50% on `quadSetupTextures`" target was uncalibrated and likely unachievable on SOLID-only scope.

### Task 1.3 — Stage 1 commit

- [ ] **Step 1:** Smoke baseline holds:

```bash
py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 20
```

Expected with `MC2_TERRAIN_COST_SPLIT` UNSET: tier1 5/5 PASS, +0 destroys delta, **and zero accumulator overhead** because Task 1.1 Step 2's RAII helpers branch-predict-out the `steady_clock::now()` calls. The flag is added to `scripts/run_smoke.py`'s allowlist in Stage 0 Task 0.4 so callers can set it explicitly when they want the measurement; otherwise it's silent and zero-cost.

Expected with `MC2_TERRAIN_COST_SPLIT=1`: ~50 ns per cluster (steady_clock::now() pair) × ~8 clusters × ~14 000 quads ≈ 5.6 ms/frame of measurement overhead. Acceptable for the Stage 1 baseline capture; production Stage 2/3 perf measurements must run with this flag UNSET so overhead doesn't pollute Gate B.

- [ ] **Step 2:** Commit:

```bash
git add mclib/quad.cpp GameOS/gameos/gos_terrain_indirect.{h,cpp} mclib/terrain.cpp \
        docs/superpowers/plans/progress/2026-04-30-indirect-terrain-stage1-baseline.md
git commit -m "$(cat <<'EOF'
chore(terrain-indirect): Stage 1 — SOLID/detail-overlay cost split via per-frame timers

Adds per-frame nanosecond accumulators bracketing the SOLID admit
clusters and the detail/overlay/mine admit clusters inside
TerrainQuad::setupTextures. Reported via the existing 600-frame
summary line as solid_branch_ns_per_frame and
detail_overlay_branch_ns_per_frame. Gated under MC2_TERRAIN_COST_SPLIT=1
so Stage 2/3 production builds don't carry the steady_clock overhead.

Per-quad Tracy zones rejected: 8 clusters × 14K quads × 60fps would
saturate Tracy and the ZoneScopedN overhead becomes comparable to
the measured work. Per-frame summation is the right pattern here
(matches slice 2b mine-counter convention).

Captured baseline (mc2_01 max zoom, --duration 60):
  solid_branch_ns_per_frame         ___ ns ≈ ___ ms
  detail_overlay_branch_ns_per_frame ___ ns ≈ ___ ms
  Terrain::geometry quadSetupTextures total (Tracy) ___ ms
Stage 3 Gate B target (recalibrated): post-armed solid branch ≤ 0.20×
baseline (≥80% on SOLID); parent zone delta ≈ S/T × 80% (bounded).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

**Validation checkpoint:** tier1 5/5 PASS with `MC2_TERRAIN_COST_SPLIT=1`; baseline numbers captured. tier1 5/5 PASS without the env flag (default OFF — zero overhead). **Rollback story:** revert; the helpers were no-ops by default. **Safe-to-revert boundary:** yes.

---

## Stage 2 — Dense recipe SSBO build (SOLID-only) + per-mission Reset/Build

**Scope:** Build a dense `TerrainQuadRecipe[mapSide²]` array indexed by `vertexNum` at `Terrain::primeMissionTerrainCache`, sibling of `WaterStream::Reset() + WaterStream::Build()` at [terrain.cpp:597-599](mclib/terrain.cpp:597). **The recipe schema does NOT change** — uses the existing 144 B / 9-vec4 `TerrainQuadRecipe` from [gos_terrain_patch_stream.h:87-99](GameOS/gameos/gos_terrain_patch_stream.h:87) verbatim. Recipe contents per slot: corner positions (`wxN, wyN, wzN, _wpN`), corner normals (`nxN, nyN, nzN, _npN`), and UV extents (`minU, minV, maxU, maxV`). The terrainType packing into `_wp0.w` (read in the thin VS at [gos_terrain_thin.vert:122](shaders/gos_terrain_thin.vert:122) as `floatBitsToUint(rec.worldPos0.w)` — see [V1]) is preserved. Convert mutation chokepoint at [MapData::invalidateTerrainFaceCache](mclib/mapdata.cpp:213) (callsite [mapdata.cpp:1359](mclib/mapdata.cpp:1359)) to call **new** per-entry `invalidateRecipeForVertexNum(vn)` API; **the `invalidateTerrainFaceCache` signature stays `void` — see [V2].** When `MC2_TERRAIN_INDIRECT_PARITY_CHECK=1`, every frame compares dense-recipe contents against the per-quad `setupTextures`-equivalent computation. M2 path still drives the actual draw — this stage proves *the recipe is correctly built*.

**Files:**
- Modify: [GameOS/gameos/gos_terrain_indirect.h](GameOS/gameos/gos_terrain_indirect.h) — add new module-scope API: `BuildDenseRecipe()`, `ResetDenseRecipe()`, `IsDenseRecipeReady()`, `RecipeForVertexNum(int32_t vn)`, `InvalidateRecipeForVertexNum(int32_t vn)`, `InvalidateAllRecipes()`. **The dense recipe lives in `gos_terrain_indirect.cpp`, NOT inside `TerrainPatchStream` — keeps the module boundary clean.**
- Modify: [GameOS/gameos/gos_terrain_indirect.cpp](GameOS/gameos/gos_terrain_indirect.cpp) — add dense-array storage (`std::vector<TerrainQuadRecipe>` sized `mapSide²`), GL buffer (`g_recipeSSBO`), build / Reset / dirty-bitmap / GPU upload, parity-check body
- Modify: [mclib/terrain.cpp:597-599](mclib/terrain.cpp:597) (`primeMissionTerrainCache`: gate the new Reset()+Build() on `IsEnabled()` or `IsParityCheckEnabled()`; sibling pattern of `WaterStream::Reset/Build`)
- Modify: [mclib/terrain.cpp:659+](mclib/terrain.cpp:659) (`Terrain::destroy`: call `gos_terrain_indirect::ResetDenseRecipe()` for per-mission teardown — see Task 2.4 + [V6])
- Modify: [mclib/mapdata.cpp:213](mclib/mapdata.cpp:213) (`invalidateTerrainFaceCache(void)`: body adds `gos_terrain_indirect::InvalidateAllRecipes()` at the bottom — signature **unchanged**)
- Modify: [mclib/mapdata.cpp:1293+](mclib/mapdata.cpp:1293) (`MapData::setTerrain`: NEW — additional fine-grained call to `gos_terrain_indirect::InvalidateRecipeForVertexNum(vn)` where `vn = topLeftX + topLeftY * Terrain::realVerticesMapSide` per [mapdata.cpp:1104](mclib/mapdata.cpp:1104). Allows a precise invalidate even when the existing whole-array Shape-C invalidate fires.)

### Task 2.1 — Recipe API (uses existing struct verbatim — no schema change)

- [ ] **Step 1:** Re-verify by reading [gos_terrain_patch_stream.h:87-99](GameOS/gameos/gos_terrain_patch_stream.h:87). The struct is:

```cpp
struct alignas(16) TerrainQuadRecipe {
    float wx0, wy0, wz0, _wp0;
    float wx1, wy1, wz1, _wp1;
    float wx2, wy2, wz2, _wp2;
    float wx3, wy3, wz3, _wp3;
    float nx0, ny0, nz0, _np0;
    float nx1, ny1, nz1, _np1;
    float nx2, ny2, nz2, _np2;
    float nx3, ny3, nz3, _np3;
    float minU, minV, maxU, maxV;
};
static_assert(sizeof(TerrainQuadRecipe) == 144, ...);
```

`terrainType` is packed into `_wp0.w` via reinterpret-cast (the float bits hold an unsigned-int-encoded packed-corner-types DWORD; the thin VS at [gos_terrain_thin.vert:122](shaders/gos_terrain_thin.vert:122) reads it back via `floatBitsToUint(rec.worldPos0.w)`). Plan v2 preserves this. **No new fields. No `overlayHandle`. No `terrainDetailHandle`. No `classifierFlags`.** Where each non-recipe field actually lives:

| Field | Lives in | Reference |
|---|---|---|
| `terrainHandle` (slot index) | `TerrainQuadThinRecord.terrainHandle` (uint32, byte 4) | [gos_terrain_patch_stream.h:104-105](GameOS/gameos/gos_terrain_patch_stream.h:104) |
| `uvMode` | `TerrainQuadThinRecord.flags` bit 0 | [h:105](GameOS/gameos/gos_terrain_patch_stream.h:105) |
| `pzTri1Valid / pzTri2Valid` | `TerrainQuadThinRecord.flags` bits 1-2 | h:105 |
| `lightRGB[0..3]` | `TerrainQuadThinRecord.lightRGB0..3` (4 × uint32) | [h:108](GameOS/gameos/gos_terrain_patch_stream.h:108) |
| `overlayHandle` / `terrainDetailHandle` / mine state | NOT cached anywhere — computed per-frame in `setupTextures()` and consumed by `addTriangle()`/`gos_PushTerrainOverlay` directly. Mine state cached on `mineResult` (per-quad) and `GameMap::tileMineCount` (slice 2b). | quad.cpp:459-464 etc. |

The SOLID-only PR1 path needs: dense recipe (positions + normals + UV + terrainType bits) + per-frame thin record (terrainHandle slot + flags + lightRGB). Detail/overlay/mine are NOT part of the SOLID draw and stay legacy.

### Task 2.2 — Dense storage + `vertexNum` direct indexing (Option A: no sentinel offset)

- [ ] **Step 1:** In `gos_terrain_indirect.cpp`, add module-private storage:

```cpp
namespace gos_terrain_indirect {

static std::vector<TerrainQuadRecipe> g_denseRecipes;  // sized mapSide² when built
static std::vector<bool>              g_denseRecipeDirty;  // sized matches
static bool                           g_denseRecipeAnyDirty = false;
static GLuint                         g_recipeSSBO          = 0;
static int32_t                        g_recipeMapSide       = 0;
static bool                           g_recipeReady         = false;

const TerrainQuadRecipe* RecipeForVertexNum(int32_t vn) {
    if (vn < 0) return nullptr;                                 // blankVertex
    if (static_cast<size_t>(vn) >= g_denseRecipes.size()) return nullptr;
    return &g_denseRecipes[vn];
}

bool IsDenseRecipeReady() { return g_recipeReady && g_recipeSSBO != 0; }

}  // namespace
```

- [ ] **Step 2:** Confirm `vertexNum` invariant per [V1]: set at [mclib/mapdata.cpp:1104](mclib/mapdata.cpp:1104) as `topLeftX + (topLeftY * Terrain::realVerticesMapSide)`. Per `memory/quadlist_is_camera_windowed.md`, this is the stable identity; do NOT use `quadList[i]` slot index. Valid in-map vertices ∈ `[0, mapSide²)`; `-1` reserved for blankVertex (never indexes the recipe array).

- [ ] **Step 3:** Document the indexing convention in `gos_terrain_indirect.h`:

```cpp
// Dense recipe indexing convention (Option A):
//   vn (vertexNum) ∈ [0, mapSide²)  → g_denseRecipes[vn] is the slot.
//   vn == -1 (blankVertex)          → no recipe; lookup returns nullptr.
//   vn ≥ mapSide²                   → out-of-range; lookup returns nullptr.
// All references (parity-check, invalidation, GLSL shader-side indexing
// through TerrainQuadThinRecord.recipeIdx) consume vn DIRECTLY. There is
// no +1 offset.
```

### Task 2.3 — Build path at `primeMissionTerrainCache` (sibling of WaterStream::Build)

- [ ] **Step 1:** Add `BuildDenseRecipe()` in `gos_terrain_indirect.cpp`. It walks `MapData::getBlocks()` (the sibling pattern WaterStream uses), populates the dense array slot per vertexNum, and uploads the buffer:

```cpp
void BuildDenseRecipe() {
    ZoneScopedN("Terrain::IndirectRecipeBuild");
    if (!Terrain::land || !Terrain::land->getMapData()) return;
    g_recipeMapSide = Terrain::realVerticesMapSide;
    const size_t N = (size_t)g_recipeMapSide * (size_t)g_recipeMapSide;
    g_denseRecipes.assign(N, TerrainQuadRecipe{});
    g_denseRecipeDirty.assign(N, false);
    // Walk MapData::blocks[] and populate corner positions + normals + UV +
    // terrainType-packed-into-_wp0.w. Source the data the same way Shape C's
    // recipe-build does at quad.cpp's setupTextures hot path — but driven
    // off MapData directly, not through quadList (which is camera-windowed).
    // ... per-block fill body ...
    if (g_recipeSSBO == 0) glGenBuffers(1, &g_recipeSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_recipeSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 N * sizeof(TerrainQuadRecipe),
                 g_denseRecipes.data(),
                 GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    g_recipeReady = true;
    if (s_indirectTrace) {
        printf("[TERRAIN_INDIRECT v1] event=recipe_build mapSide=%d entries=%zu "
               "bytes=%zu ssbo=%u\n",
               g_recipeMapSide, N, N * sizeof(TerrainQuadRecipe),
               (unsigned)g_recipeSSBO);
        fflush(stdout);
    }
}
```

- [ ] **Step 2:** Add `ResetDenseRecipe()`. Per the WaterStream "keep buffer, CPU-clear state per mission" pattern (per [V6] — `WaterStream::Reset()` is called at [terrain.cpp:598](mclib/terrain.cpp:598) before `Build()`), this CPU-clears state but does NOT delete the GL buffer:

```cpp
void ResetDenseRecipe() {
    ZoneScopedN("Terrain::IndirectRecipeReset");
    g_denseRecipes.clear();
    g_denseRecipeDirty.clear();
    g_denseRecipeAnyDirty = false;
    g_recipeMapSide       = 0;
    g_recipeReady         = false;
    // g_recipeSSBO stays allocated — reused by next mission's BuildDenseRecipe.
    // (Mirrors WaterStream::Reset() pattern.)
    s_firstDrawPrintedThisMission = false;  // mission-latch reset for first_draw print
    if (s_indirectTrace) {
        printf("[TERRAIN_INDIRECT v1] event=recipe_reset ssbo=%u\n",
               (unsigned)g_recipeSSBO);
        fflush(stdout);
    }
}
```

- [ ] **Step 3:** Wire calls at [mclib/terrain.cpp:597-599](mclib/terrain.cpp:597), gated on `IsEnabled() || IsParityCheckEnabled()`:

```cpp
{
    ZoneScopedN("Terrain::primeMissionTerrainCache water_stream_build");
    WaterStream::Reset();
    WaterStream::Build();
    if (gos_terrain_indirect::IsEnabled() ||
        gos_terrain_indirect::IsParityCheckEnabled()) {
        gos_terrain_indirect::ResetDenseRecipe();
        gos_terrain_indirect::BuildDenseRecipe();
    }
}
```

### Task 2.4 — Per-mission teardown hook (M6)

Per [V6]: `TerrainPatchStream::init/destroy` are wired to `gosRenderer::init/destroy` ([gameos_graphics.cpp:2464,2472](GameOS/gameos/gameos_graphics.cpp:2464)) — process lifetime, NOT per-mission. Plan v2 must NOT use those as the per-mission teardown site. The actual per-mission teardown is `Terrain::destroy()` at [terrain.cpp:659](mclib/terrain.cpp:659), called from `Mission::destroy()` at [code/mission.cpp:3217](code/mission.cpp:3217) (`land->destroy()`).

- [ ] **Step 1:** Add a call to `gos_terrain_indirect::ResetDenseRecipe()` inside `Terrain::destroy()` at [terrain.cpp:659+](mclib/terrain.cpp:659), gated on `IsEnabled() || IsParityCheckEnabled()`. This fires on mission unload. The next mission's `primeMissionTerrainCache` re-runs `ResetDenseRecipe() + BuildDenseRecipe()` cleanly.

- [ ] **Step 2:** Confirm by reading `Mission::destroy` (mission.cpp:3165) that `land->destroy()` is the only path that fires per-mission and that no other code path bypasses it.

### Task 2.5 — Per-entry invalidate API (M2; precise OR whole-map, never both at the same site)

`MapData::invalidateTerrainFaceCache` signature is **`void`** ([mapdata.h:220](mclib/mapdata.h:220), confirmed by [V2]). Plan v2 does NOT change this signature. Add **new** module-scope APIs in `gos_terrain_indirect.{h,cpp}`:

```cpp
namespace gos_terrain_indirect {
void InvalidateRecipeForVertexNum(int32_t vn);  // precise, when caller knows vn
void InvalidateAllRecipes();                     // whole-map invalidate
}
```

**Important — advisor stop-the-line #2:** earlier draft also added `InvalidateAllRecipes()` to the body of `invalidateTerrainFaceCache(void)`. That defeats the per-entry story: `setTerrain` calls precise-then-whole-map at the same chokepoint, so the precise call is wasted and the dense recipe rebuilds in full on every mutation. **Plan v2 final position:** `invalidateTerrainFaceCache(void)`'s body gets **NO** new dense-recipe call. Whole-map invalidation lands ONLY at the two whole-map sites that already exist, and precise invalidation lands at the mutation-site BEFORE the call to `invalidateTerrainFaceCache` (where `vn` is computable from `(blockY, blockX)`).

- [ ] **Step 1:** Implement both APIs:

```cpp
void InvalidateRecipeForVertexNum(int32_t vn) {
    if (!IsEnabled() && !IsParityCheckEnabled()) return;
    if (vn < 0 || static_cast<size_t>(vn) >= g_denseRecipes.size()) return;
    rebuildRecipeSlotFromMapData(vn, g_denseRecipes[vn]);  // CPU recompute
    g_denseRecipeDirty[vn] = true;
    g_denseRecipeAnyDirty  = true;
    if (s_indirectTrace) printf("[TERRAIN_INDIRECT v1] event=invalidate vn=%d\n", vn);
}

void InvalidateAllRecipes() {
    if (!IsEnabled() && !IsParityCheckEnabled()) return;
    if (g_denseRecipes.empty()) return;
    for (size_t vn = 0; vn < g_denseRecipes.size(); ++vn) {
        rebuildRecipeSlotFromMapData((int32_t)vn, g_denseRecipes[vn]);
        g_denseRecipeDirty[vn] = true;
    }
    g_denseRecipeAnyDirty = true;
    if (s_indirectTrace) printf("[TERRAIN_INDIRECT v1] event=invalidate_all entries=%zu\n",
                                g_denseRecipes.size());
}
```

- [ ] **Step 2:** Map each existing `invalidateTerrainFaceCache` call site (per [V2]) to the appropriate new API. **Each site uses EITHER precise OR whole-map, never both.** The body of `invalidateTerrainFaceCache(void)` itself remains unchanged from the dense-recipe perspective:

| Call site | File:line | Existing context | Strategy | Where the dense-recipe call goes |
|---|---|---|---|---|
| `mapdata.h:220` | declaration | — | unchanged (still `void`) | none |
| `mapdata.cpp:149` | inside `MapData::destroy` | whole-map cleanup | **whole-map** | Add `gos_terrain_indirect::InvalidateAllRecipes();` next to (just before or after) the existing `invalidateTerrainFaceCache()` call at line 149. |
| `mapdata.cpp:191` | inside `MapData::newInit` | whole-map fresh init | **whole-map** | Add `gos_terrain_indirect::InvalidateAllRecipes();` next to the existing call (no-op when `g_denseRecipes.empty()`, so order doesn't matter). |
| `mapdata.cpp:1293+` | top of `setTerrain` body, BEFORE the call to `invalidateTerrainFaceCache` at line 1359 | per-mutation | **precise** | `int32_t vn = blockX + blockY * Terrain::realVerticesMapSide; gos_terrain_indirect::InvalidateRecipeForVertexNum(vn);` BEFORE the whole-array Shape-C invalidate fires inside `invalidateTerrainFaceCache`. The Shape-C cache invalidate (the only thing `invalidateTerrainFaceCache(void)` does today) stays untouched. |
| `mapdata.cpp:1359` | inside `invalidateTerrainFaceCache` body | the chokepoint itself | (no dense-recipe call here) | The body of `invalidateTerrainFaceCache(void)` **does NOT** call `InvalidateAllRecipes()`. Doing so would defeat the per-entry story at `setTerrain` (precise-then-whole-map = wasted precise call). |

Result: `setTerrain(blockY, blockX, t)` runs:
1. Plan-v2-new: `InvalidateRecipeForVertexNum(blockX + blockY * realVerticesMapSide)` — precise.
2. Existing: `invalidateTerrainFaceCache()` — Shape-C whole-array invalidate (NOT the dense recipe).

`MapData::destroy` and `MapData::newInit` run:
1. Existing: `invalidateTerrainFaceCache()` — Shape-C whole-array invalidate.
2. Plan-v2-new: `InvalidateAllRecipes()` — dense recipe whole-map.

Two separate cache systems; the dense recipe never gets precise + whole-map at the same site.

- [ ] **Step 3:** Add per-slot GPU upload helper (called by Stage 3's `DrawIndirect` BEFORE drawing):

```cpp
void FlushDirtyRecipeSlotsToGPU() {
    if (!g_denseRecipeAnyDirty || g_recipeSSBO == 0) return;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_recipeSSBO);
    for (size_t vn = 0; vn < g_denseRecipes.size(); ++vn) {
        if (!g_denseRecipeDirty[vn]) continue;
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(vn * sizeof(TerrainQuadRecipe)),
                        sizeof(TerrainQuadRecipe),
                        &g_denseRecipes[vn]);
        g_denseRecipeDirty[vn] = false;
    }
    g_denseRecipeAnyDirty = false;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}
```

### Task 2.6 — Recipe-only parity body

- [ ] **Step 1:** Add `ParityCompareRecipeFrame()` in `gos_terrain_indirect.cpp`. Walks live `quadList`, looks up dense recipe by `vertexNum` of the top-left corner, AND runs the legacy `setupTextures`-equivalent computation, byte-comparing the **input** fields per `memory/water_ssbo_pattern.md`:

| Field | Where in dense recipe | Legacy equivalent |
|---|---|---|
| corner positions (4 × vec4) | `recipe.wx0..wz3` | `quad.vertices[c]->pVertex->vx/vy/elev` |
| corner normals (4 × vec4) | `recipe.nx0..nz3` | `quad.vertices[c]->pVertex->normal` |
| UV extents | `recipe.minU..maxV` | derived from `quad.vertices[c]->pVertex` UV indices |
| terrainType bits | `recipe._wp0` (`floatBitsToUint`) | `quad.vertices[c]->pVertex->textureData & 0x0000ffff` packing |

- [ ] **Step 2:** Hook into the per-frame loop wrapping `Terrain::geometry quadSetupTextures` Tracy zone at [terrain.cpp:1681](mclib/terrain.cpp:1681), guarded by `IsParityCheckEnabled()`. Per-frame: invoke `ParityCompareRecipeFrame()`, then `ParityFrameTick(quadsChecked)`.

- [ ] **Step 3:** Compare on **inputs** ONLY — recipe contents. Per `memory/water_ssbo_pattern.md`: do NOT compare post-projection x/y/z/rhw — sub-1-ULP drift produces fake mismatches.

### Task 2.7 — Smoke + parity validation

- [ ] **Step 1:** Smoke unset (default-OFF baseline):

```bash
py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 20
```

Expected: tier1 5/5 PASS. No `[TERRAIN_INDIRECT v1]` lines.

- [ ] **Step 2:** Smoke `INDIRECT=0+PARITY=1` — recipe is built; M2 path drives draw; parity validates recipe. Run:

```bash
MC2_TERRAIN_INDIRECT_PARITY_CHECK=1 py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 20
```

Expected: tier1 5/5 PASS. Parity summary `total_mismatches=0` per mission.

- [ ] **Step 3:** Bug-hunt loop on mismatches — same triage as Stage 2 in plan v1: recipe coverage gap, blank-vertex skip drift, derived-byte patch.

- [ ] **Step 4:** Smoke `INDIRECT=1` (Stage 2 view = recipe built, M2 still drives draw):

```bash
MC2_TERRAIN_INDIRECT=1 py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 20
```

Expected: tier1 5/5 PASS. Recipe built; Stage 3 hook absent so M2 still drives draw.

### Task 2.8 — Cross-mission validation (Stage-2 narrow version)

The dense recipe lifecycle (`Build` per mission, `Reset` per mission) must survive a tier1 mission sequence within a single process. **This is the dress rehearsal for Stage 4's cross-mission Gate D quintuple — catching teardown bugs here is far cheaper than catching them at promotion.**

- [ ] **Step 1:** Run a sequenced smoke covering all five tier1 missions in one process. Use `--tier tier1` (which already iterates all 5 in sequence) and grep artifacts for:
  - One `event=recipe_build` per mission entry.
  - One `event=recipe_reset` per mission exit.
  - No GL errors (`[GL_ERROR v1]` absent).
  - Process RSS at logistics is flat across the sequence (no SSBO leak).

### Task 2.9 — Stage 2 commit

- [ ] **Step 1:** Final smoke triple (each tier1 5/5 PASS):

```bash
py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 20
MC2_TERRAIN_INDIRECT_PARITY_CHECK=1 py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 20
MC2_TERRAIN_INDIRECT=1 py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 20
```

- [ ] **Step 2:** Commit:

```bash
git add GameOS/gameos/gos_terrain_indirect.{h,cpp} mclib/terrain.cpp mclib/mapdata.cpp
git commit -m "$(cat <<'EOF'
feat(terrain-indirect): Stage 2 — dense recipe SSBO + per-mission Reset/Build

Builds a dense TerrainQuadRecipe array (mapSide² × 144B; ~9.4 MB at
256² stock max, ~21 MB at 384² Wolfman max) at primeMissionTerrainCache, sibling of
WaterStream::Reset()+Build() at terrain.cpp:597-599. Recipe schema
unchanged — uses existing 9-vec4 / 144B layout from
gos_terrain_patch_stream.h:87 verbatim. terrainType packed into
_wp0.w (read by gos_terrain_thin.vert:122 floatBitsToUint).

Per-mission teardown hook is Terrain::destroy() (terrain.cpp:659),
called from Mission::destroy → land->destroy(). Mirrors WaterStream's
"CPU-clear state, keep GL buffer" Reset pattern — no buffer churn at
mission boundary.

Mutation chokepoint: invalidateTerrainFaceCache(void) signature
unchanged; body adds InvalidateAllRecipes() defense-in-depth. setTerrain
(mapdata.cpp:1359) gets a precise InvalidateRecipeForVertexNum(vn)
call before the existing whole-map invalidate, where vn = blockX +
blockY * realVerticesMapSide per mapdata.cpp:1104.

When MC2_TERRAIN_INDIRECT_PARITY_CHECK=1, recipe contents compared
per frame against legacy quadSetupTextures-derived equivalents.
tier1 5/5 PASS triple (unset / INDIRECT=1 / PARITY=1) with zero
mismatches across [N] quads checked. Cross-mission sequenced run
clean: build/reset events match mission boundaries 1:1; no GL
errors; RSS flat at logistics across all 5 tier1 missions.

M2 thin path still drives the actual draw — Stage 3 wires the
indirect-draw hook + SOLID gate-off in the same PR.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

**Validation checkpoint:** tier1 5/5 PASS triple. `event=recipe_build`/`event=recipe_reset` fire per mission boundary with 1:1 pairing. Zero mismatches.

**Rollback story:** revert Stage 2 commit. Recipe-build call site at terrain.cpp:597-599 reverts to no-op. Per-mission teardown hook in `Terrain::destroy` reverts. The dense recipe disappears with the revert. Stage 0 + Stage 1 are untouched.

**Safe-to-revert boundary:** yes. Recipe is built but unused for draw. Wastes ~9.4 MB at default-OFF only when `IsEnabled() || IsParityCheckEnabled()`; otherwise no allocation. Acceptable indefinitely. **However: do not land Stage 3 in a separate PR — see N2 / Stage 3 prelude below.**

---

## Stage 3 (PR1 close) — Indirect SOLID draw + legacy SOLID gate-off

**Per N2: Stage 3a (indirect draw shipped) and Stage 3b (legacy SOLID gate-off shipped) MUST land in the same PR.** Do not land 3a alone — it can pass visual + smoke gates while completely missing the CPU-offload goal (legacy SOLID still iterates while indirect also draws SOLID; perf goes UP, not down). The N1 counters are how this hazard is detectable.

Within a single PR, the two pieces are organized as:
- **Stage 3a (Tasks 3.1–3.5):** indirect SOLID draw via `gos_terrain_bridge_drawIndirect`, hooked AT `Render.TerrainSolid` ([txmmgr.cpp:1330](mclib/txmmgr.cpp:1330)) using a **preflight-armed** bool computed once per frame (per orchestrator decision C=(iv)).
- **Stage 3b (Task 3.6):** legacy SOLID gate-off — surgical `if (terrainIndirectSolidArmed) skip` around the `addTriangle(MC2_DRAWSOLID)` lines in `setupTextures()`, narrowed per orchestrator decision B=(i). Detail/overlay/mine `addTriangle(...)` calls remain running.
- **Stage 3c (Task 3.7+):** cumulative validation under the 4-gate ladder + the N1 counters.

### Preflight arming — orchestrator decision C=(iv), advisor stop-the-line #1

**Critical safety invariant:** once the SOLID gate-off has fired (the legacy `addTriangle(MC2_DRAWSOLID)` calls have been skipped), `TerrainPatchStream::flush()` has nothing to flush for SOLID and **same-frame fallback is not a recovery path**. If the indirect draw also fails, the frame ships with no SOLID terrain — visually catastrophic. Plan v1 had the same hazard latent; advisor caught it; v2 fixes by lifting all draw-failure-prone work into preflight, before the gate-off decision. After preflight, `armed=true` is a contract: "everything needed for draw is set up; the only remaining work is the GL draw call itself, which by spec does not fail synchronously."

**Preflight sequence (run ONCE per frame, BEFORE the `setupTextures` loop fires at [terrain.cpp:1679](mclib/terrain.cpp:1679)):**

```cpp
// gos_terrain_indirect.h
namespace gos_terrain_indirect {
bool ComputePreflight();  // call once per frame; caches result + draw inputs.
bool IsFrameSolidArmed(); // read by the gate-off site in setupTextures, by DrawIndirect, etc.
                          // Result is stable for the rest of the frame.
}
```

```cpp
// gos_terrain_indirect.cpp — preflight does ALL the work that could fail.
namespace { bool s_frameSolidArmed = false; }

bool ComputePreflight() {
    ZoneScopedN("Terrain::IndirectPreflight");
    s_frameSolidArmed = false;

    // Static gates: things that don't change within a mission.
    if (!IsEnabled())                       return false;
    if (!IsDenseRecipeReady())              return false;
    if (!ResourcesReady())                  return false;  // bridge program, SSBOs, indirect cmd buffer all allocated
    if (InMissionTransition())              return false;

    // Dynamic per-frame work that could leave us with nothing to draw.
    FlushDirtyRecipeSlotsToGPU();
    int thinCount = PackThinRecordsForFrame();   // walks live quadList, packs thin record SSBO
    if (thinCount == 0) {
        if (s_indirectTrace) printf("[TERRAIN_INDIRECT v1] event=preflight_skip reason=zero_thin\n");
        return false;                            // nothing visible — let legacy SOLID draw it
    }
    int cmdCount = BuildIndirectCommands(thinCount);
    if (cmdCount == 0) {
        if (s_indirectTrace) printf("[TERRAIN_INDIRECT v1] event=preflight_skip reason=zero_cmd\n");
        return false;
    }
    s_frameSolidPackedThinCount = thinCount;
    s_frameSolidCmdCount        = cmdCount;
    s_frameSolidArmed           = true;
    return true;
}

bool IsFrameSolidArmed() { return s_frameSolidArmed; }
```

**Once `IsFrameSolidArmed()` returns true, the only remaining work is the GL draw issue itself, which does not fail synchronously by GL spec.** The bridge function still does state save/restore + uniform binds + the `glMultiDrawArraysIndirect` call, but no allocation, compile, or "could this fail" logic — all of that ran in preflight where failure means `armed=false` and legacy SOLID path runs uninterrupted.

**Cost note:** preflight runs the per-frame thin-record pack + indirect-command build BEFORE the legacy loop, even when armed=false (in which case the work is wasted that frame). For the un-armed early-mission-frame transient case, the waste is a few hundred microseconds at most — negligible. For the steady-state armed case, this is exactly the work that needs to happen anyway.

**Files:**
- Modify: [GameOS/gameos/gos_terrain_indirect.{h,cpp}](GameOS/gameos/gos_terrain_indirect.cpp) — extend with `PackThinRecordsForFrame`, `BuildIndirectCommands`, `ComputePreflight`, `IsFrameSolidArmed`, `DrawIndirect`, `ResourcesReady`, `InMissionTransition`, `ForceDisableArmingForProcess`, GL buffer for `TerrainIndirectCommandBuffer`
- Modify: [GameOS/gameos/gameos_graphics.cpp](GameOS/gameos/gameos_graphics.cpp) — add bridge `gos_terrain_bridge_drawIndirect` mirroring `gos_terrain_bridge_renderWaterFast` (state save/restore including `glColorMask` per M5; `glEnableVertexAttribArray(0)` per M4; depth pipeline; sampler)
- Modify: [mclib/txmmgr.cpp:1330](mclib/txmmgr.cpp:1330) — hook AT `Render.TerrainSolid`
- Modify: [mclib/terrain.cpp:1679](mclib/terrain.cpp:1679) — call `gos_terrain_indirect::ComputePreflight()` once before the per-quad loop (sets `IsFrameSolidArmed()` for the frame)
- Modify: [mclib/quad.cpp:466-467, 539-540, ...](mclib/quad.cpp:466) — surgical SOLID-only gate-off at the paired SOLID admit clusters inside `setupTextures()`. The exact count is small (~4-8 clusters across the four uvMode branches; per [V8] the executor enumerates them at implementation time by grep'ing `MC2_DRAWSOLID` between line 429 and the next `void TerrainQuad::draw` at line 1630). Distinct from the "16 emit sites" cited in design constraint #9, which spans `setupTextures` AND `TerrainQuad::draw` AND `TerrainQuad::drawWater` (depth-fudge scope, not setupTextures-only)

### Task 3.1 — Per-frame thin-record packer (called from preflight, before gate-off)

- [ ] **Step 1:** Add `PackThinRecordsForFrame()` walking live `quadList` with the canonical skip set (`memory/water_ssbo_pattern.md` lines 70-87). **Called from `ComputePreflight()` (NOT from `DrawIndirect` per plan v1) so failure to admit any quad surfaces as `armed=false`, BEFORE the gate-off site reads `IsFrameSolidArmed()`.**

```cpp
int PackThinRecordsForFrame() {
    ZoneScopedN("Terrain::ThinRecordPack");
    int packed = 0;
    for (long i = 0; i < numberQuads; ++i) {
        const TerrainQuad& q = quadList[i];
        // 1. Pointer guards
        if (!q.vertices[0] || !q.vertices[1] || !q.vertices[2] || !q.vertices[3])
            continue;
        // 2. Map-edge blankVertex skip — vertexNum < 0 sentinel from mapdata.cpp:1104
        const int32_t vn0 = q.vertices[0]->vertexNum;
        if (vn0 < 0 ||
            q.vertices[1]->vertexNum < 0 ||
            q.vertices[2]->vertexNum < 0 ||
            q.vertices[3]->vertexNum < 0)
            continue;
        // 3. Recipe coverage gate
        const TerrainQuadRecipe* r = RecipeForVertexNum(vn0);
        if (!r) continue;
        // 4. Per-tri pz check (drives flags bits 1, 2)
        // 5. Pack thin record (existing 32B layout — no schema change)
        TerrainQuadThinRecord tr{};
        tr.recipeIdx     = (uint32_t)vn0;  // direct vn → recipe index (Option A)
        tr.terrainHandle = tex_resolve(q.terrainHandle);  // mc2_texture_handle_is_live;
                                                          // exact signature confirmed at impl time per [V13]
        tr.flags         = pack_flags(q.uvMode, pzTri1Valid, pzTri2Valid);
        tr.lightRGB0     = inline_merge_selection(q.vertices[0]);  // matches quad.cpp:1793
        tr.lightRGB1     = inline_merge_selection(q.vertices[1]);
        tr.lightRGB2     = inline_merge_selection(q.vertices[2]);
        tr.lightRGB3     = inline_merge_selection(q.vertices[3]);
        // ... write tr into ring slot ...
        gos_terrain_indirect::Counters_AddIndirectSolidPackedQuad();  // N1 counter (per-quad)
        ++packed;
    }
    return packed;
}
```

- [ ] **Step 2:** Triple-buffer ring — same pattern as M2 thin record. `glBufferSubData` per frame.

### Task 3.2 — Indirect command buffer (DrawArrays variant — NO element/index buffer needed)

**Critical correction from advisor stop-the-line #3:** the M2 thin VS uses `gl_VertexID` exclusively ([gos_terrain_thin.vert:55-59](shaders/gos_terrain_thin.vert:55) — `vid % 6u` decomposes into `(triIdx, vertInRecord, recordIdx)`); it reads geometry from SSBOs and has NO `layout(location = N) in ...` vertex-attribute reads. The M2 path uses `glDrawArrays`, not `glDrawElements` (precedent comments at [gos_terrain_patch_stream.cpp:25, 32, 37](GameOS/gameos/gos_terrain_patch_stream.cpp:25)). **Plan v2 uses `glMultiDrawArraysIndirect` and `DrawArraysIndirectCommand` (16 B/struct, NOT 20 B); no `GL_ELEMENT_ARRAY_BUFFER` is required, and no EBO-lifecycle task is needed.** The indirect command buffer is bound to `GL_DRAW_INDIRECT_BUFFER` only.

- [ ] **Step 1:** Define the GL types and storage:

```cpp
struct DrawArraysIndirectCommand {
    GLuint count;          // = thinCount * 6 (six gl_VertexID values per quad)
    GLuint instanceCount;
    GLuint first;          // = 0 for SOLID-only PR1; future buckets bump this
    GLuint baseInstance;
};
static_assert(sizeof(DrawArraysIndirectCommand) == 16,
              "DrawArraysIndirectCommand is 4 GLuints = 16 B per GL spec");

// PR1 (SOLID-only) emits exactly 1 command per frame; future detail/overlay/
// mine slice may add up to 3 more buckets. Size for headroom:
//   16 commands × 16 B = 256 B total.
static constexpr size_t kIndirectCmdBufferBytes = 16 * sizeof(DrawArraysIndirectCommand);
static GLuint g_indirectCmdBuffer = 0;  // bound to GL_DRAW_INDIRECT_BUFFER (driver-only)

int BuildIndirectCommands(int thinCount) {
    if (thinCount == 0) return 0;
    DrawArraysIndirectCommand cmd{};
    cmd.count         = (GLuint)(thinCount * 6);  // 6 gl_VertexID values per quad
    cmd.instanceCount = 1;
    cmd.first         = 0;
    cmd.baseInstance  = 0;
    if (g_indirectCmdBuffer == 0) {
        glGenBuffers(1, &g_indirectCmdBuffer);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectCmdBuffer);
        glBufferData(GL_DRAW_INDIRECT_BUFFER, kIndirectCmdBufferBytes, nullptr, GL_STREAM_DRAW);
    } else {
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectCmdBuffer);
    }
    glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, sizeof(cmd), &cmd);
    return 1;
}
```

- [ ] **Step 2:** Naming: `TerrainRecipeSSBO` (shader-read, GL_SHADER_STORAGE_BUFFER, binding=1 per [V3]); `TerrainThinRecordSSBO` (shader-read, GL_SHADER_STORAGE_BUFFER, binding=2 per [V3]); `TerrainIndirectCommandBuffer` (driver-read only, GL_DRAW_INDIRECT_BUFFER, no binding slot, no EBO). Comments throughout `gos_terrain_indirect.cpp` use these names verbatim.

- [ ] **Step 3:** **No `GL_ELEMENT_ARRAY_BUFFER` task.** The thin VS does not consume vertex indices; `glMultiDrawArraysIndirect` walks `gl_VertexID` from `0..count-1`, which is exactly what the VS expects. Confirm at impl time by reading [gos_terrain_thin.vert:54-59](shaders/gos_terrain_thin.vert:54) — if the executor sees ANY `layout(location = N) in ...` declarations beyond the AMD attr-0 dummy (per V4), this assumption breaks and they should escalate.

### Task 3.3 — Bridge function (all 9 gotchas + AMD attr 0 + glColorMask save/restore)

- [ ] **Step 1:** Add `gos_terrain_bridge_drawIndirect` in [GameOS/gameos/gameos_graphics.cpp](GameOS/gameos/gameos_graphics.cpp), mirroring the renderWater bridge structure at [gameos_graphics.cpp:1909-2073](GameOS/gameos/gameos_graphics.cpp:1909). State save/restore per [V5] explicitly extends renderWater's set with **`glColorMask`** (per M5):

```cpp
bool gos_terrain_bridge_drawIndirect(/* ... params ... */) {
    if (!terrain_indirect_prog_ || !terrain_indirect_prog_->shp_) return false;
    GLuint prog = terrain_indirect_prog_->shp_;

    // Save state for restore. Same set as renderWater (Program, Blend, BlendFunc,
    // DepthMask, VAO, DepthFunc, Sampler) PLUS ColorMask (M5) — the bridge bypasses
    // applyRenderStates and a prior shadow pass at gos_postprocess.cpp:1134/1156
    // can leave glColorMask(FALSE,...) — without restore, terrain draws nothing.
    GLint savedProgram   = 0;        glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
    GLboolean savedBlend = glIsEnabled(GL_BLEND);
    GLint savedSrcRGB = 0, savedDstRGB = 0;
    glGetIntegerv(GL_BLEND_SRC_RGB, &savedSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &savedDstRGB);
    GLint savedDepthMask = 0;        glGetIntegerv(GL_DEPTH_WRITEMASK, &savedDepthMask);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint savedDepthFunc = 0;        glGetIntegerv(GL_DEPTH_FUNC, &savedDepthFunc);
    GLint savedVAO = 0;              glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVAO);
    GLboolean savedColorMask[4]; glGetBooleanv(GL_COLOR_WRITEMASK, savedColorMask);  // M5

    // #4 VAO 0 trap (AMD): rebind valid VAO.
    extern void gos_RendererRebindVAO();
    gos_RendererRebindVAO();

    // M4 — AMD attribute 0 trap: vertex attrib 0 must be enabled or AMD silently
    // drops the draw (docs/amd-driver-rules.md:5). The thin VS reads from SSBOs
    // only and has no `layout(location = 0) in` declaration, so this is the
    // mitigation — enable a dummy attr-0 array on the rebound VAO before draw.
    glEnableVertexAttribArray(0);

    glUseProgram(prog);

    // #1 uniform uint crash: use uniform int + cast inside shader (M2 thin VS already does)
    // #3 matrix transpose: terrainMVP GL_FALSE, mvp GL_TRUE (setMat4Direct vs setMat4Std)
    // ... uniform setup (mirrors renderWater bridge lines 1991-2040) ...

    // #9 depth state: terrain WRITES depth (unlike water which reads-only).
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);  // M5 — undo any prior shadow-pass mask

    // #5 sampler: atlas-tiled (CLAMP_TO_EDGE / LINEAR) — match M2 path; do NOT
    //              copy water's REPEAT (water is world-tiled).
    // ... sampler bind ...

    // SSBO binds — slot 1 = recipe, slot 2 = thin record (per [V3]).
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, g_recipeSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, g_thinRecordSSBO);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectCmdBuffer);
    glBindVertexArray(/*indirect path's VAO*/);

    // The single multi-draw indirect (DrawArrays variant — no EBO).
    {
        ZoneScopedN("Terrain::IndirectDraw");
        glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, /*drawCount=*/cmdCount, 0);
    }

    // Restore state, including ColorMask (M5).
    glColorMask(savedColorMask[0], savedColorMask[1], savedColorMask[2], savedColorMask[3]);
    glDepthMask((GLboolean)savedDepthMask);
    glDepthFunc((GLenum)savedDepthFunc);
    if (!savedDepthTest) glDisable(GL_DEPTH_TEST);
    if (!savedBlend)     glDisable(GL_BLEND);
    glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
    glBindVertexArray((GLuint)savedVAO);
    glUseProgram((GLuint)savedProgram);
    return true;
}
```

### Task 3.4 — `DrawIndirect()` is now a thin executor of preflight results

Per advisor stop-the-line #1: the failure-prone work moved to `ComputePreflight()` (above). `DrawIndirect()` runs only when armed; its job is to issue the bridge call and the multi-draw, both of which are by-spec non-failing for the no-error setup path. It returns `false` ONLY when `armed=false` — and in that case the gate-off ALSO did not fire, so the legacy SOLID path runs uninterrupted.

```cpp
bool DrawIndirect() {
    if (!IsFrameSolidArmed()) return false;  // legacy SOLID path runs uninterrupted

    // Armed. All allocation, packing, command building done in ComputePreflight().
    {
        ZoneScopedN("Terrain::TotalCPU");
        // (No work here — kept as the aggregator zone label per Recon Item 7. The
        // measured CPU cost is in Terrain::IndirectPreflight upstream.)
    }
    bool ok = gos_terrain_bridge_drawIndirect(s_frameSolidCmdCount);
    if (!ok) {
        // Bridge returned false post-arming: this is a HARD failure. The gate-off
        // already fired, so legacy SOLID has no records to draw this frame —
        // we cannot recover. Log; disable arming for the rest of the process to
        // avoid repeating the bad state; allow the next mission's recipe rebuild
        // (or operator-driven MC2_TERRAIN_INDIRECT=0 restart) to recover.
        static bool s_hardFailureLogged = false;
        if (!s_hardFailureLogged) {
            s_hardFailureLogged = true;
            fprintf(stderr,
                "[TERRAIN_INDIRECT v1] event=hard_failure reason=bridge_returned_false "
                "thin_count=%d cmd_count=%d "
                "advice=set MC2_TERRAIN_INDIRECT=0 to fall back to M2 legacy SOLID\n",
                s_frameSolidPackedThinCount, s_frameSolidCmdCount);
            fflush(stderr);
        }
        ForceDisableArmingForProcess();   // sticky bool — IsFrameSolidArmed() returns false thereafter,
                                           // including in the same frame's gate-off site (read AFTER preflight)
        return false;                      // current frame ships with the visual artifact;
                                           //   killswitch (=0) restart fixes the next process
    }

    // First-draw lifecycle print (once per mission via the recipe-reset latch).
    if (!s_firstDrawPrintedThisMission && s_indirectTrace) {
        s_firstDrawPrintedThisMission = true;   // reset by ResetDenseRecipe (Task 2.3 Step 2 / M7)
        printf("[TERRAIN_INDIRECT v1] event=first_draw thin_count=%d cmd_count=%d\n",
               s_frameSolidPackedThinCount, s_frameSolidCmdCount);
        fflush(stdout);
    }
    return true;
}
```

**Why this is safe:** by the time `DrawIndirect` runs, the preflight already proved every potentially-failing operation (recipe ready, resources ready, mission state stable, thin-record pack non-empty, command build non-empty). The only post-preflight failure surface is the bridge function's GL state setup, which in normal operation does not fail — the most common failure modes (missing program, VAO 0 trap, sampler creation) are pre-validated by `ResourcesReady()` in preflight. A `false` return from the bridge therefore indicates a genuine driver-level anomaly that warrants the hard-failure stance: log, disable arming process-wide, advise the operator to set `MC2_TERRAIN_INDIRECT=0`. Same-frame fallback to legacy SOLID is **not** a recovery path — the gate-off already fired, the legacy admit clusters were skipped, and `TerrainPatchStream::flush()` has nothing to flush for SOLID.

- [ ] **Step 1:** Implement `DrawIndirect()` per the above. Note: `s_frameSolidArmed`, `s_frameSolidCmdCount`, `s_frameSolidPackedThinCount` are populated by `ComputePreflight()` and remain stable for the rest of the frame.

- [ ] **Step 2:** `s_firstDrawPrintedThisMission` reset in `ResetDenseRecipe()` (Task 2.3 Step 2 / M7) — guarantees first_draw prints once per mission, not once per process.

- [ ] **Step 3:** Add `ForceDisableArmingForProcess()` (sets a process-sticky bool that `ComputePreflight()` and `IsFrameSolidArmed()` both consult). Once latched, remains latched until process exit. The killswitch path (`MC2_TERRAIN_INDIRECT=0` on next launch) is the documented recovery.

### Task 3.5 — Hook AT `Render.TerrainSolid` (NO same-frame fallback after gate-off)

Per advisor stop-the-line #1: same-frame fallback to `TerrainPatchStream::flush()` is NOT safe when SOLID gate-off has fired — the legacy SOLID admit clusters were skipped, so `flush()` has no SOLID records. Plan v2 final position: M2 fallback only runs when the frame was NEVER armed (in which case the gate-off ALSO did not fire, so legacy SOLID admits ran normally). When `armed=true` and `DrawIndirect` fails, the frame ships with the indirect-path attempt's outcome — there is no recoverable second path.

- [ ] **Step 1:** Modify [mclib/txmmgr.cpp:1330](mclib/txmmgr.cpp:1330):

```cpp
// Was:
//   if (TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed())
//       modernHandled = TerrainPatchStream::flush();

// Becomes (v2):
if (gos_terrain_indirect::IsFrameSolidArmed()) {
    // Indirect SOLID owns this frame. The SOLID gate-off in setupTextures() already
    // fired, so TerrainPatchStream has no SOLID records — do NOT fall back to flush()
    // when DrawIndirect returns false (advisor stop-the-line #1, plan-v2). A false
    // return is a hard failure that's already logged + has flipped the process-sticky
    // ForceDisableArmingForProcess; operator advice (set MC2_TERRAIN_INDIRECT=0) is
    // in the [TERRAIN_INDIRECT v1] event=hard_failure line.
    modernHandled = gos_terrain_indirect::DrawIndirect();
} else if (TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
    // Un-armed frame: gate-off did not fire, legacy admits filled TerrainPatchStream
    // normally. M2 thin-record-direct draw runs the SOLID for this frame.
    modernHandled = TerrainPatchStream::flush();
}
```

- [ ] **Step 2:** Verify the `if (modernHandled && MC2_ISTERRAIN) continue;` skip at [txmmgr.cpp:1340-1343](mclib/txmmgr.cpp:1340) is unchanged. With this corrected hook:
   - **Armed + DrawIndirect succeeded:** `modernHandled=true`, legacy DRAWSOLID bucket skipped — correct.
   - **Armed + DrawIndirect failed:** `modernHandled=false`, but the legacy DRAWSOLID bucket has nothing to draw anyway (gate-off skipped admits) — frame ships with no SOLID terrain. `event=hard_failure` logged, arming disabled process-wide; subsequent frames take the un-armed branch and M2 draws normally. The visual artifact is one-frame-only.
   - **Un-armed (preflight returned false for any reason):** `modernHandled=false` from the indirect branch; legacy `flush()` runs and `modernHandled=true` from M2; legacy DRAWSOLID bucket skipped — correct, same as pre-slice behavior.

### Task 3.6 — Legacy SOLID gate-off (Stage 3b — orchestrator decision B=(i)-narrowed)

Per [V8], the SOLID admission lines inside `setupTextures()` are at lines 466-467, 539-540, and equivalent positions in the four uvMode branches. They are **interleaved** with `addTriangle(MC2_DRAWALPHA)` (detail), mine `addTriangle(MC2_DRAWALPHA)`, and overlay code that stays running in this slice. Surgical gate-off only on the SOLID-only addTriangle calls (NOT the entire branch).

- [ ] **Step 1:** Add a helper at the top of `quad.cpp`'s anonymous namespace. **Counter increment is per-quad (per cluster), not per-triangle** — the helper wraps a *cluster* of admits via a small RAII pattern, so no matter how many `addTriangle` calls live inside the cluster, the counter bumps exactly once:

```cpp
// SOLID-only cluster gate + counter. When indirect SOLID is armed for this frame,
// the cluster's body becomes a no-op (caller skips the SOLID addTriangle pair).
// DRAWALPHA / mine / overlay clusters are NOT gated.
//
// Usage at e.g. quad.cpp lines 466-467:
//   if (BeginLegacySolidCluster()) {
//       mcTextureManager->addTriangle(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
//       mcTextureManager->addTriangle(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
//       EndLegacySolidCluster();
//   }
//
// BeginLegacySolidCluster returns false when armed (caller skips the body and
// does NOT call End). When un-armed, returns true and Begin/End bracket pairs
// the cluster + bumps the counter exactly ONCE per quad.
//
// (Implementation can be a struct with an inline-emitted RAII helper or just the
// two free functions — the executor picks based on which reads cleaner against
// the existing brace structure in setupTextures(). The contract is one counter
// bump per cluster, not per addTriangle call.)
static inline bool BeginLegacySolidCluster() {
    if (gos_terrain_indirect::IsFrameSolidArmed()) return false;
    return true;
}
static inline void EndLegacySolidCluster() {
    gos_terrain_indirect::Counters_AddLegacySolidSetupQuad();  // exactly one per quad
}
static inline void NoteLegacyDetailOverlayCluster() {
    gos_terrain_indirect::Counters_AddLegacyDetailOverlayQuad();  // passive — never gates
}
```

The counter API is the public function set defined in Task 0.3 — counters live as private file-scope statics in `gos_terrain_indirect.cpp`; cross-translation-unit access goes through `Counters_*` functions. **Units: per quad (per cluster), not per triangle.** A cluster of two `addTriangle(MC2_DRAWSOLID)` calls bumps `legacy_solid_setup_quads` by exactly 1.

- [ ] **Step 2:** At each paired `addTriangle(...MC2_DRAWSOLID)` admit cluster in `setupTextures()` (lines 466-467, 539-540, and similar in the four uvMode branches per [V8]), wrap the existing pair:

```cpp
// Was (e.g. quad.cpp:466-467):
//   mcTextureManager->addTriangle(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
//   mcTextureManager->addTriangle(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);

// Becomes:
if (BeginLegacySolidCluster()) {
    mcTextureManager->addTriangle(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
    mcTextureManager->addTriangle(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
    EndLegacySolidCluster();
}
```

The executor enumerates every paired `MC2_DRAWSOLID` admit cluster in `setupTextures()` body (lines 429-1629 per [V9]). Care: only the SOLID lines; NOT the DRAWALPHA detail/mine/overlay calls that live alongside them in the same `if/else` branches.

- [ ] **Step 3:** At each `addTriangle(...MC2_DRAWALPHA)` detail/mine/overlay cluster in `setupTextures()`, drop a single `NoteLegacyDetailOverlayCluster();` call after the cluster — passive instrument only; do NOT gate these. The counter should remain non-zero throughout this slice, confirming detail/overlay still flow through legacy as intended.

- [ ] **Step 4:** Wire `ComputePreflight()` once at the top of the per-frame loop at [terrain.cpp:1679](mclib/terrain.cpp:1679), BEFORE the per-quad loop reads `IsFrameSolidArmed()`:

```cpp
TerrainQuadPtr currentQuad = quadList;
{
    ZoneScopedN("Terrain::geometry quadSetupTextures");
    gos_terrain_indirect::ComputePreflight();   // once per frame, BEFORE the loop.
                                                 // After this returns, IsFrameSolidArmed()
                                                 // is stable for the rest of the frame.
    for (i=0;i<numberQuads;i++)
    {
        currentQuad->setupTextures();
        currentQuad++;
    }
}
```

**Order is load-bearing:** preflight runs the thin-record pack and indirect-command build over the live `quadList`. `quadList` is filled by `vertexProjectLoop` earlier in the frame (precedes `Terrain::geometry quadSetupTextures` per the existing call shape). The pack/build do NOT depend on `setupTextures()` having run; they consume the `vertexNum` / `pVertex` data already populated by vertex projection.

### Task 3.7 — Smoke + 4-gate ladder (Gate B uses Stage 1's SOLID-branch baseline)

- [ ] **Step 1: Visual canary** at fixed seed/camera, side-by-side legacy/fast (per design "Validation gates" → A): shoreline (mc2_17), mine sites (mc2_24), Wolfman zoom, bridge boundary.

- [ ] **Step 2: Tracy delta — Gate B (recalibrated per Stage 1).** Target: `Terrain::SetupSolidBranch` ≤ 0.20 × Stage-1-baseline (≥80% reduction on the SOLID branch). Aspirational: SOLID branch ≈ 0 when armed. **Do NOT measure with PARITY=1 on** (parity adds diagnostic CPU work and pollutes the delta).

- [ ] **Step 3: Parity — Gate C.** Run with `INDIRECT=1+PARITY=1`; expect zero mismatches across tier1 5/5.

- [ ] **Step 4: tier1 5/5 PASS triple — Gate D.**

```bash
py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 20
MC2_TERRAIN_INDIRECT=1 MC2_TERRAIN_INDIRECT_TRACE=1 py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 20
MC2_TERRAIN_INDIRECT=1 MC2_TERRAIN_INDIRECT_PARITY_CHECK=1 MC2_TERRAIN_INDIRECT_TRACE=1 py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 20
```

- [ ] **Step 4b: Verify the fast path drew under `INDIRECT=1` AND that no hard failures occurred.** Grep artifacts:

```bash
grep -c "TERRAIN_INDIRECT v1] event=first_draw"     artifacts/<ts>/<mission>/stdout.txt   # ≥ 1 per mission
grep -c "TERRAIN_INDIRECT v1] event=preflight_skip" artifacts/<ts>/<mission>/stdout.txt   # 0 (or only at first 1-2 frames)
grep -c "TERRAIN_INDIRECT v1] event=zero_commands"  artifacts/<ts>/<mission>/stdout.txt   # 0 during gameplay
grep -c "TERRAIN_INDIRECT v1] event=hard_failure"   artifacts/<ts>/<mission>/stdout.txt   # MUST be 0 — any hit fails Gate D
```

**Gate D negative checks (advisor cleanup #3) — any of these is a Gate D failure regardless of tier1 PASS:**

| Negative signal | What it means | Why it fails Gate D |
|---|---|---|
| `event=hard_failure` ≥ 1 | Bridge returned false post-arming | Gate-off already fired; frame shipped without SOLID terrain. Even one hit means the failure mode the preflight design exists to avoid actually triggered |
| `event=zero_commands` ≥ 1 during active gameplay | Pack succeeded but cmd build returned 0 | Internal inconsistency in preflight — should not be reachable in a healthy run; preflight should set `armed=false` first |
| `event=first_draw` count = 0 across the run | Indirect path never drew | Indicates preflight returned false every frame (mission-load transient that never cleared, or sticky `ForceDisableArmingForProcess` engaged silently) |
| `event=preflight_skip` count > 60 frames per mission | Preflight failing for sustained periods | Steady-state arming should hold once recipe is built; sustained skips indicate a deeper bug |

- [ ] **Step 4c: Verify N1 counters tell the right story.** From the same artifacts, grep the 600-frame summary line for the three counter values:

```bash
grep "event=summary" artifacts/<ts>/<mission>/stdout.txt | tail -5
```

Expected behavior under each config:
- **`INDIRECT=1` armed run:** `legacy_solid_setup_quads` MUST be ≈ 0 across the run (SOLID gate-off worked); `indirect_solid_packed_quads` MUST be non-zero (matching the per-frame admitted quad count); `legacy_detail_overlay_quads` MUST remain non-zero (detail/overlay stay legacy as intended).
- **`INDIRECT=1` un-armed (e.g., recipe not ready first frame):** `legacy_solid_setup_quads` and `legacy_detail_overlay_quads` both non-zero; `indirect_solid_packed_quads` = 0. Acceptable transient.
- **Unset (legacy):** `legacy_solid_setup_quads` and `legacy_detail_overlay_quads` non-zero; `indirect_solid_packed_quads` = 0.

If the armed-run counters show `legacy_solid_setup_quads >> 0`, the gate-off failed silently — this counts as a Gate D failure regardless of tier1 PASS. (This is the failure mode N1 exists to surface — without it, perf could regress while tier1 PASSes.)

### Task 3.8 — PR1 commit (Stages 0+1+2+3 in one branch)

- [ ] **Step 1:** Final cumulative validation — re-run Step 4. All three states 5/5 PASS. N1 counters tell the right story.

- [ ] **Step 2:** Commit and open PR1:

```bash
git add GameOS/gameos/gos_terrain_indirect.{h,cpp} GameOS/gameos/gameos_graphics.cpp \
        mclib/txmmgr.cpp mclib/terrain.cpp mclib/quad.cpp
git commit -m "$(cat <<'EOF'
feat(terrain-indirect): Stage 3 — indirect SOLID draw + legacy SOLID gate-off (PR1 close)

Per-frame walks live quadList, packs thin record (existing 32B M2
layout — no schema change), builds 1 DrawArraysIndirectCommand
struct (PR1 SOLID-only), emits one glMultiDrawArraysIndirect via
gos_terrain_bridge_drawIndirect. Hook AT Render.TerrainSolid
(txmmgr.cpp:1330) using preflight-armed bool computed once per
frame. Surgical SOLID-only gate-off in setupTextures() — only the
addTriangle(MC2_DRAWSOLID) calls bypass when armed; detail
(MC2_DRAWALPHA), mine, and overlay code paths continue running.

Bridge addresses all 9 GPU-direct gotchas plus AMD attribute 0 trap
(glEnableVertexAttribArray(0) per docs/amd-driver-rules.md:5) plus
glColorMask save/restore (gos_postprocess.cpp:1134/1156 may have
left it FALSE for shadow pass; without restore the indirect path
would draw nothing on the frame after a shadow pass).

Tracy delta on Terrain::SetupSolidBranch (mc2_01 max zoom):
  pre-baseline ___ ms (Stage 1 measurement)
  post-armed   ___ ms (___% reduction, exceeds ≥80% target)
N1 counters (600-frame summary, INDIRECT=1 armed run):
  legacy_solid_setup_quads ≈ 0 (gate-off confirmed)
  indirect_solid_packed_quads ≈ ___ (matches admitted quad count)
  legacy_detail_overlay_quads ≈ ___ (detail/overlay stay legacy)
Parity-check: ___M checks across 5 tier1 missions, zero mismatches.
tier1 5/5 PASS triple (unset / INDIRECT=1 / INDIRECT=1+PARITY=1)
with +0 destroys delta.

Killswitch: MC2_TERRAIN_INDIRECT=0 explicit opt-out (default-off in
this slice; flipped default-on in follow-up PR2 after soak).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

**Validation checkpoint:** all four gates A/B/C/D green for the cumulative slice; N1 counters confirm gate-off worked. Slice ships at default-OFF.

**Rollback story:** revert Stage 3 commit. The hook at txmmgr.cpp:1330 reverts; the SOLID gate-off in quad.cpp reverts. Stage 2's recipe-build still runs when `INDIRECT=1` or `PARITY=1` but is unused for draw. M2 path drives production.

**Safe-to-revert boundary:** yes for the slice as a whole — the codebase can sit at default-OFF after Stages 0+1+2+3 indefinitely.

---

## Stage 4 — Default-on promotion (PR2, post-soak) + cross-mission Gate D quintuple

**Scope:** Two-line flip of `IsEnabled()` (mirrors Shape C `aee39cc`). Update `scripts/run_smoke.py` env-passthrough comment. Ship `memory/indirect_terrain_solid_endpoint.md`. Move orchestrator Status Board row Queued → Shipped.

**Soak gate before opening PR2:** ~2 weeks of default-on usage (operator manually flipped) after PR1 lands; one regression cycle (tier1 + manual missions); one user-driven gameplay session covering save/load, mid-mission camera extremes, mission restart; no env-flag-flipped-back-to-0 incidents.

**Files:**
- Modify: [GameOS/gameos/gos_terrain_indirect.cpp](GameOS/gameos/gos_terrain_indirect.cpp) (one-line invert in `IsEnabled()`)
- Modify: `scripts/run_smoke.py` (comment update; env still in allowlist)
- Create: `memory/indirect_terrain_solid_endpoint.md`
- Update: `memory/MEMORY.md` (index entry under "Rendering / shaders")
- Update: `docs/superpowers/cpu-to-gpu-offload-orchestrator.md` Status Board

### Task 4.1 — Flip `IsEnabled()` semantics

- [ ] **Step 1:** Edit [gos_terrain_indirect.cpp](GameOS/gameos/gos_terrain_indirect.cpp):

```cpp
bool IsEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT");
        // Stage 4 flip: default ON. Only literal "0" opts out.
        // Mirrors Shape C aee39cc convention.
        return !(v && v[0] == '0' && v[1] == '\0');
    }();
    return s;
}
```

### Task 4.2 — N4 cross-mission Gate D quintuple (default-on validation)

The 4-gate ladder's Gate D currently validates the three configs (UNSET / FAST=1 / FAST=1+PARITY=1) per individual mission. For Stage 4 promotion (default-on flip), expand to a **quintuple** validating cross-mission state under default-on:

- [ ] **Step 1: Default-on warm boot.** Run mc2_01 → mc2_03 → mc2_10 → mc2_17 → mc2_24 in **one process**, simulating the campaign mission-to-mission flow. Default `IsEnabled()` is now true; default `IsParityCheckEnabled()` is false. Use `--tier tier1` (which already chains the five missions). Expected: 5/5 PASS, +0 destroys delta on each, `event=recipe_build`/`event=recipe_reset` 5:5 paired (the per-mission Reset/Build cycle holds across the chain).

- [ ] **Step 2: Default-on cold start each mission.** Run each tier1 mission as its own fresh process (`--kill-existing` between). Expected: 5/5 PASS, +0 destroys delta.

- [ ] **Step 3: Killswitch warm boot.** With `MC2_TERRAIN_INDIRECT=0`, run the same warm-boot chain. Verify M2 fallback drives draw end-to-end; `event=recipe_build` absent.

- [ ] **Step 4: Killswitch cold start each mission.** Same as Step 2 with `=0`.

- [ ] **Step 5: Parity belt-and-suspenders.** With `MC2_TERRAIN_INDIRECT_PARITY_CHECK=1` (and default-on `IsEnabled()`), run the warm-boot chain. Expected: 5/5 PASS, summary `total_mismatches=0` across all five missions.

If any of the five fails, the default-on flip is rolled back; mission-transition state cleanup is the failure mode this gate exists to catch (per M6 — the per-mission teardown via `Terrain::destroy` → `ResetDenseRecipe`). Adversarial review caught no mid-mission `realVerticesMapSide` change on tier1 stock; the N4 quintuple is the runtime validation of that assumption.

### Task 4.3 — Memory file + index

- [ ] **Step 1:** Create `memory/indirect_terrain_solid_endpoint.md`:

```markdown
---
name: Indirect terrain SOLID slice closeout
description: Indirect-terrain SOLID slice (PR1+PR2) shipped. SOLID main-emit retired via dense recipe + glMultiDrawArraysIndirect. Detail/overlay/mine stay legacy.
type: project
---

# Indirect terrain SOLID — slice closed (2026-XX-XX)

What shipped:
- Stage 0 (PR1): scaffolding + N1 counters + parity printer
- Stage 1 (PR1): cost-split Tracy zones; baseline captured
- Stage 2 (PR1): dense TerrainQuadRecipe SSBO at primeMissionTerrainCache
- Stage 3 (PR1): indirect SOLID draw + surgical SOLID gate-off, preflight-armed
- Stage 4 (PR2): default-on flip; cross-mission Gate D quintuple PASS

4-gate ladder outcome:
- Gate A visual canary: clean across shoreline / mine / Wolfman zoom / bridge boundary
- Gate B Tracy delta on Terrain::SetupSolidBranch: __% reduction
- Gate C parity: 0 mismatches across __M checks
- Gate D tier1 5/5 PASS quintuple: warm-boot default-on, cold-start default-on, warm-boot killswitch, cold-start killswitch, warm-boot parity

What stays as opt-out fallback under MC2_TERRAIN_INDIRECT=0:
- TerrainPatchStream::flush() (the M2 SOLID main-emit path)
- The SOLID legacy admit at quad.cpp:466-467 / 539-540 / etc.
Detail (MC2_DRAWALPHA), overlay (gos_PushTerrainOverlay), mine —
NOT retired in this slice; future slice with own brainstorm.

Lessons surfaced during bring-up:
[fill from actual implementation experience — recipe coverage gaps,
parity bug classes caught, etc.]
```

- [ ] **Step 2:** Update `memory/MEMORY.md` with index entry under "Rendering / shaders":

```markdown
- ⭐ [Indirect terrain SOLID — slice closed (2026-XX-XX)](indirect_terrain_solid_endpoint.md) — SOLID main-emit retired via dense recipe + glMultiDrawArraysIndirect; detail/overlay/mine stay legacy; legacy SOLID opt-out via MC2_TERRAIN_INDIRECT=0
```

### Task 4.4 — Orchestrator Status Board update

- [ ] **Step 1:** Move "Indirect terrain draw" row from Queued → Shipped with delta + parity counts.
- [ ] **Step 2:** Add Queued row for the post-soak SOLID legacy retirement follow-up slice.
- [ ] **Step 3:** Add Queued row for the detail/overlay/mine consolidation slice (out of scope here; needs own brainstorm).

### Task 4.5 — PR2 commit

- [ ] **Step 1:** Final smoke per Task 4.2 quintuple — all 5/5 PASS.

- [ ] **Step 2:** Commit:

```bash
git add GameOS/gameos/gos_terrain_indirect.cpp scripts/run_smoke.py \
        memory/indirect_terrain_solid_endpoint.md memory/MEMORY.md \
        docs/superpowers/cpu-to-gpu-offload-orchestrator.md
git commit -m "$(cat <<'EOF'
feat(terrain-indirect): flip MC2_TERRAIN_INDIRECT default-on (SOLID-only)

Indirect-terrain dense-recipe + GPU-direct SOLID draw becomes the
default for Render.TerrainSolid main-emit. Detail (MC2_DRAWALPHA),
overlay, and mine paths remain on legacy contracts (out of scope for
this slice; future slice).

Tier1 cross-mission validation (Gate D quintuple): 5/5 PASS in each
of warm-boot default-on, cold-start default-on, warm-boot killswitch,
cold-start killswitch, warm-boot parity. ___M parity checks across
the soak window, zero mismatches.

Tracy delta on Terrain::SetupSolidBranch (mc2_01, max zoom):
- mean   ___ -> ___ ms (___% reduction)
N1 counters confirm SOLID gate-off active; detail/overlay counter
remains non-zero as intended.

Killswitch preserved: MC2_TERRAIN_INDIRECT=0 explicit opt-out.
SOLID legacy path remains intact as fallback; physical deletion
queued as a post-soak follow-up slice.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

**Validation checkpoint:** Gate D quintuple all 5/5 PASS. Banner shows correct flag state in each. Status Board updated. **Rollback story:** revert one-line `IsEnabled()` flip; default returns to OFF. **Safe-to-revert boundary:** yes — Status Board update reverted in same commit if rolled back.

---

## Slice-as-a-whole 4-gate ladder (canonical, recalibrated for SOLID-only)

PR1 (Stages 0+1+2+3) and PR2 (Stage 4) cumulatively must satisfy:

- **Gate A — Visual canary** at fixed seed/camera, side-by-side legacy/fast.
   - Shoreline (mc2_17), mine sites (mc2_24), Wolfman zoom, bridge boundary.
- **Gate B — Tracy delta on `Terrain::SetupSolidBranch`** (the new Stage 1 zone, NOT the parent `quadSetupTextures`).
   - Pre-baseline: captured at Stage 1 (mc2_01, max-zoom-out, --duration 60).
   - Target: ≤ 0.20 × Stage-1-baseline (≥80% reduction on SOLID branch).
   - Aspirational: SOLID branch ≈ 0 when armed.
   - **MUST measure with PARITY=1 UNSET** to avoid diagnostic-mode CPU pollution.
- **Gate C — `MC2_TERRAIN_INDIRECT_PARITY_CHECK=1`** zero mismatches across stock-content tier1.
- **Gate D — tier1 5/5 PASS triple (PR1)** + **5/5 PASS quintuple (PR2 — N4):** unset / `INDIRECT=1` / `INDIRECT=1+PARITY=1`, plus default-on warm-boot and default-on cold-start sequences across the five missions.
- **N1 counter check:** `legacy_solid_setup_quads ≈ 0`, `indirect_solid_packed_quads > 0`, `legacy_detail_overlay_quads > 0` under armed runs. Without this check, Gate B can pass while completely missing the CPU-offload goal.

---

## What stays behind (legacy as opt-out fallback after Stage 4)

Per orchestrator framing — "promotion and deletion are different risk classes." Slated for deletion in the post-soak follow-up:

- `TerrainPatchStream::flush()` (the M2 thin-record-direct SOLID emit path).
- The `addTriangle(MC2_DRAWSOLID)` admit lines in `setupTextures()` (lines 466-467, 539-540, etc. — under `IsFrameSolidArmed()` they're already no-ops; once SOLID legacy is deleted, the helper macro and call sites can be removed entirely).
- The `MC2_TERRAIN_INDIRECT=0` opt-out env flag itself (banner field stays as informational).

NOT slated for deletion — out of scope for this slice:

- Detail (`MC2_DRAWALPHA`) admit lines in `setupTextures()` — still drive draw.
- Overlay (`gos_PushTerrainOverlay`) call sites — still drive draw.
- Mine state cache + setMine chokepoint — still in use.

These three need their own consolidation slice with its own brainstorm.

---

## Follow-up slices queued (NOT part of this plan)

Recorded so the orchestrator's Queued table stays load-bearing for memory. Without these rows, future sessions opening the orchestrator see "queue empty, arc complete" and the dead code accumulates / the arc never completes:

1. **SOLID legacy M2 retirement (post-soak).** Mechanical deletion of `TerrainPatchStream::flush()` SOLID path and the gated-off SOLID admit lines. ~150 LoC of deletions; no new design surface; gate = tier1 5/5 PASS with parity ON. Soak gate: ~2 weeks default-on usage with no regressions.

2. **Detail / overlay / mine consolidation.** Brainstorm-required: how do the three buckets share a recipe vs stay parallel? What's the schema growth budget? Indirect command buffer becomes 4 commands; sampler state varies per bucket (atlas vs world-tile); mine state lifecycle differs. **Plan v2 explicitly does NOT decide this** — Q3's stale-memory gap means the answer must come from a fresh design pass with the actual struct layouts in hand.

Both rows landed in the orchestrator Status Board at Stage 4 Task 4.4.

---

## Verification appendix (M-fixes + scope-realignment items, code-grounded)

Every cited symbol in this plan has been grep'd against the source tree at planning time. Status legend: ✅ matches claim · ⚠️ divergent (must fix before ship) · ❓ needs follow-up.

| Tag | Claim in plan | Verification | Status |
|---|---|---|---|
| **[V1]** M1 — `TerrainQuadRecipe` layout | Existing struct at [gos_terrain_patch_stream.h:87-99](GameOS/gameos/gos_terrain_patch_stream.h:87) is 9 vec4s = 144 B (4 × worldPos vec4 + 4 × worldNorm vec4 + 1 × UV vec4); `terrainType` packed into `_wp0.w` via reinterpret-cast (read by [gos_terrain_thin.vert:122](shaders/gos_terrain_thin.vert:122) as `floatBitsToUint(rec.worldPos0.w)`); `static_assert(sizeof == 144)` at h:98 | Grep confirmed: struct definition lines 87-97; static_assert line 98; thin VS line 122 reads `floatBitsToUint(rec.worldPos0.w)` | ✅ |
| **[V1a]** M1 — fields NOT on recipe | `terrainHandle` lives on `TerrainQuadThinRecord.terrainHandle` (h:104); `uvMode/pzTri1Valid/pzTri2Valid` live on `TerrainQuadThinRecord.flags` bits 0-2 (h:105 + thin VS struct comment); `lightRGB[0..3]` lives on `TerrainQuadThinRecord.lightRGB0..3` (h:108); `overlayHandle/terrainDetailHandle/isCement` are NOT cached anywhere — computed per-frame in `setupTextures()` ([quad.cpp:459-464](mclib/quad.cpp:459)); mine state on `quad.mineResult` (per-quad, set inline) and `GameMap::tileMineCount` (slice 2b) | Grep confirmed each | ✅ |
| **[V2]** M2 — `invalidateTerrainFaceCache` signature | Signature is `void invalidateTerrainFaceCache(void)`; declared at [mclib/mapdata.h:220](mclib/mapdata.h:220), defined at [mapdata.cpp:213](mclib/mapdata.cpp:213); 4 hits total (1 declaration, 1 definition, 2 callers): callers at [mapdata.cpp:149](mclib/mapdata.cpp:149) (whole-map cleanup in `MapData::destroy`), [mapdata.cpp:191](mclib/mapdata.cpp:191) (whole-map fresh init), [mapdata.cpp:1359](mclib/mapdata.cpp:1359) (per-mutation in `setTerrain`). Plan v2 does NOT change the signature; adds new `gos_terrain_indirect::InvalidateRecipeForVertexNum(int32_t)` and `InvalidateAllRecipes()` APIs alongside | Grep `invalidateTerrainFaceCache` returned exactly the four hits | ✅ |
| **[V2a]** M2 — `vertexNum` packing | Set at [mclib/mapdata.cpp:1104](mclib/mapdata.cpp:1104) as `topLeftX + (topLeftY * Terrain::realVerticesMapSide)` per recon handoff Item 4 | Recon handoff line 217 cited; mapdata.cpp:1104 referenced (executor reads at implementation time to confirm exact line) | ✅ (cited from recon, not re-grep'd) |
| **[V3]** M3 — SSBO binding slots in thin VS | [gos_terrain_thin.vert:9](shaders/gos_terrain_thin.vert:9) `binding = 2` ThinRecordBuf; [gos_terrain_thin.vert:18](shaders/gos_terrain_thin.vert:18) `binding = 1` RecipeBuf. Plan v2's bridge binds via `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, g_recipeSSBO)` and `glBindBufferBase(..., 2, g_thinRecordSSBO)`. The new `TerrainIndirectCommandBuffer` is bound to `GL_DRAW_INDIRECT_BUFFER` (driver-only consumption) — **no SSBO slot conflict** | Grep on shader file showed exactly `binding = 1` at line 18 and `binding = 2` at line 9 | ✅ |
| **[V4]** M4 — AMD attribute 0 trap | `Attribute 0 must be active — AMD skips draws if vertex attrib 0 isn't enabled. Add layout(location = 0) with a dummy read.` at [docs/amd-driver-rules.md:5](docs/amd-driver-rules.md:5). Plan v2 mitigates via `glEnableVertexAttribArray(0)` after `gos_RendererRebindVAO()` in the bridge. Note: the thin VS has no `layout(location = 0) in` declaration (grep returned no matches) — the runtime `glEnableVertexAttribArray(0)` is the chosen mitigation; an alternative (adding a dummy attr-0 read in the VS) is logged as ❓ if the runtime mitigation fails on actual AMD hardware | Grep confirmed doc line 5; grep for `layout(location` in thin VS returned no matches | ✅ (with ❓ note) |
| **[V5]** M5 — `glColorMask` save/restore precedent | renderWater bridge at [gameos_graphics.cpp:1909+](GameOS/gameos/gameos_graphics.cpp:1909) saves: Program (line 1970), Blend enabled (1971), BlendSrcRGB/DstRGB (1972-1973), DepthMask (1974), VAO (1980), DepthTest enabled + DepthFunc (2086-2087), Sampler (2108-2113). It does NOT save/restore ColorMask. Plan v2's bridge extends this set with `glGetBooleanv(GL_COLOR_WRITEMASK, savedColorMask)` and restore. Reason: prior shadow pass at [gos_postprocess.cpp:1134](GameOS/gameos/gos_postprocess.cpp:1134) calls `glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE)`; line 1156 same; line 1165 restores — but if those ever change order or fail, the indirect bridge bypasses applyRenderStates and could draw nothing | Grep confirmed all renderWater saves; grep for `glColorMask` in postprocess.cpp returned the three sites cited | ✅ |
| **[V6]** M6 — per-mission lifecycle | `TerrainPatchStream::init` wired at [gameos_graphics.cpp:2464](GameOS/gameos/gameos_graphics.cpp:2464) inside `gosRenderer::init`; `TerrainPatchStream::destroy` at [gameos_graphics.cpp:2472](GameOS/gameos/gameos_graphics.cpp:2472) inside `gosRenderer::destroy`. **Process lifetime**, NOT per-mission. The per-mission seam is `Terrain::primeMissionTerrainCache` at [terrain.cpp:575](mclib/terrain.cpp:575) (build) + `Terrain::destroy` at [terrain.cpp:659](mclib/terrain.cpp:659) (teardown), called from `Mission::init` at [code/mission.cpp:2218](code/mission.cpp:2218) and `Mission::destroy` at [code/mission.cpp:3217](code/mission.cpp:3217). `WaterStream::Reset()` is called at [terrain.cpp:598](mclib/terrain.cpp:598) before `WaterStream::Build()` at [terrain.cpp:599](mclib/terrain.cpp:599) — the canonical "CPU-clear state, keep GL buffer" precedent. WaterStream API at [gos_terrain_water_stream.h:188-198](GameOS/gameos/gos_terrain_water_stream.h:188) declares both `Build()` and `Reset()` as namespace-scope free functions (not class methods) | Grep `TerrainPatchStream::(init\|destroy)` returned exactly the wiring sites; grep `WaterStream::Reset\|Build\|primeMissionTerrainCache` confirmed the per-mission flow | ✅ |
| **[V7]** M7 — `first_draw` mission-latch | Depends on M6. Plan v2 puts the latch reset (`s_firstDrawPrintedThisMission = false`) inside `ResetDenseRecipe()` (Task 2.3 Step 2), which is called from both [terrain.cpp:597+](mclib/terrain.cpp:597) per-mission build and [terrain.cpp:659+](mclib/terrain.cpp:659) per-mission teardown. Result: `first_draw` fires once per mission, not once per process | Hook site is the same per-mission seam confirmed in [V6] | ✅ |
| **[V8]** M8 / B(i) narrowing — `_cbbuf` size + SOLID gate-off branch | (1) `_cbbuf` (banner buffer) is `[384]` at [gameosmain.cpp:625](GameOS/gameos/gameosmain.cpp:625) (a SECOND `_cbbuf` exists at line 867 inside the heartbeat path — `[192]`, unrelated). Plan v2 grows the banner one to 512. (2) `TerrainIndirectCommandBuffer` (the indirect command buffer the brief M8 also asks about) is sized **256 B = 16 × 16 B `DrawArraysIndirectCommand`** in Task 3.2 — PR1 emits 1 command, future slices up to 4. (Plan v1's "320 B = 16 × 20 B" referred to `DrawElementsIndirectCommand`; v2 uses the DrawArrays variant — see [V15].) (3) SOLID admit lines inside `setupTextures()` (function spans [quad.cpp:429-1629](mclib/quad.cpp:429)): the 16 `MC2_DRAWSOLID` admit sites listed in design constraint #9 are spread across `setupTextures` AND `TerrainQuad::draw` AND `TerrainQuad::drawWater`. Within `setupTextures` specifically, paired SOLID admits at lines 466-467, 539-540 (and similar in the four uvMode branches; the executor enumerates exhaustively at impl time). DRAWALPHA detail/mine admits at lines 468-469, 497-508, 541-542, 570-581 are interleaved and stay running. Plan v2's gate-off helper `BeginLegacySolidCluster()` is applied **only at the SOLID clusters** — not the entire branch | Grep confirmed (1) line 625 declaration `_cbbuf[384]`; (2) `DrawArraysIndirectCommand` is 4 GLuints per GL spec; (3) the line-range pattern of paired SOLID + paired DRAWALPHA in `setupTextures` was verified by reading lines 425-580 directly | ✅ |
| **[V9]** B(i) — `setupTextures` boundaries | Function declared `void TerrainQuad::setupTextures (void)` at [quad.cpp:429](mclib/quad.cpp:429); next function `TerrainQuad::draw` starts at [quad.cpp:1630](mclib/quad.cpp:1630). The per-frame loop calling `currentQuad->setupTextures()` is at [terrain.cpp:1684](mclib/terrain.cpp:1684) inside the `Terrain::geometry quadSetupTextures` Tracy zone at terrain.cpp:1681-1687 | Grep confirmed function boundaries via `awk '/^void TerrainQuad::/'` and the loop at terrain.cpp:1681-1687 | ✅ |
| **[V10]** N1 counters location + units | Counters live as private file-scope statics in `gos_terrain_indirect.cpp` (Task 0.3 Step 1). Cross-translation-unit increments go through public function API (`Counters_AddLegacySolidSetupQuad`, `Counters_AddIndirectSolidPackedQuad`, `Counters_AddLegacyDetailOverlayQuad`) per advisor cleanup #1. **Units = per-quad (per cluster), NOT per-triangle.** Wired at: (a) `Counters_AddLegacySolidSetupQuad()` — at `EndLegacySolidCluster()` in the un-armed branch (Task 3.6 Step 1, called once per cluster of paired admits); (b) `Counters_AddIndirectSolidPackedQuad()` — at the end of each successful pack iteration in `PackThinRecordsForFrame` (Task 3.1 Step 1); (c) `Counters_AddLegacyDetailOverlayQuad()` — `NoteLegacyDetailOverlayCluster()` at each DRAWALPHA cluster site in `setupTextures` (Task 3.6 Step 3) | Counter API and units verified against the function structure in [V9] + [V8]; advisor cleanup #1 caught the prior translation-unit-static + per-triangle ambiguity | ✅ |
| **[V11]** N4 mission list | tier1 = `mc2_01, mc2_03, mc2_10, mc2_17, mc2_24` per [tests/smoke/smoke_missions.txt:11-15](tests/smoke/smoke_missions.txt:11). Cross-mission warm-boot uses `--tier tier1` (chains all 5 in one process); cold-start uses `--kill-existing` between | Grep returned exactly the five tier1 lines; `run_smoke.py` accepts `--tier tier1` per [scripts/run_smoke.py:146](scripts/run_smoke.py:146) | ✅ |
| **[V12]** Brainstorm Q3 stale-memory addendum | [Q3 at brainstorms/2026-04-30-indirect-terrain-draw-scope.md:170](docs/superpowers/brainstorms/2026-04-30-indirect-terrain-draw-scope.md:170) records "recipe contents stay the same: terrainHandle, overlayHandle, detailHandle, uvData, mine state cache" — but [V1] / [V1a] confirm these fields are NOT on the recipe. Plan v2's plan-time addendum (introduction section above) records this discrepancy and narrows Q3's answer to "parallel for SOLID-PR1; multi-bucket consolidation deferred" | Cross-grep of brainstorm Q3 and recipe struct showed the divergence | ✅ (addressed via addendum) |
| **[V13]** Stage 2 build path supporting symbols | `MapData::getBlocks(void) const { return blocks; }` exists at [mclib/mapdata.h:82](mclib/mapdata.h:82) — namespace-scope WaterStream pattern (`Build`/`Reset`) is namespace-scope free functions per [V6] not class methods, but the source data (`MapData::blocks`) is the same. Plan v2's `BuildDenseRecipe()` walks via `Terrain::land->getMapData()->getBlocks()`. `tex_resolve` family symbols live at [mclib/tex_resolve_table.{h,cpp}](mclib/tex_resolve_table.h) — the executor confirms exact signature at packing-time (used in `PackThinRecordsForFrame` Step 1 to convert mcTextureManager slot index to gosHandle per `memory/mc2_texture_handle_is_live.md`) | Grep returned both as real source-tree symbols | ✅ |
| **[V14]** Stage 1 measurement approach | Per-frame steady_clock accumulators in `gos_terrain_indirect.cpp` reported via existing 600-frame summary; gated under `MC2_TERRAIN_COST_SPLIT=1` so Stage 2/3 production runs are zero-overhead. Per-quad `ZoneScopedN` rejected (would generate ~6.7 M zones/sec at 8 clusters × 14 K quads × 60 fps; Tracy queue saturates and overhead pollutes the measurement) | Self-review caught this MAJOR finding; Stage 1 revised before plan v2 ship | ✅ |
| **[V15]** Draw primitive — DrawArrays vs DrawElements | The thin VS at [gos_terrain_thin.vert:54-59](shaders/gos_terrain_thin.vert:54) reads `gl_VertexID` exclusively and decomposes via `vid % 6u` into `(triIdx, vertInRecord, recordIdx)`; it has NO `layout(location = N) in ...` vertex-attribute declarations and reads geometry only from SSBOs (RecipeBuf binding=1, ThinRecordBuf binding=2). The M2 path uses `glDrawArrays` (precedent comments at [gos_terrain_patch_stream.cpp:25, 32, 37](GameOS/gameos/gos_terrain_patch_stream.cpp:25)). Plan v2 uses `glMultiDrawArraysIndirect` and `DrawArraysIndirectCommand` (4 GLuints = 16 B). **No GL_ELEMENT_ARRAY_BUFFER / EBO is required.** | Grep confirmed `gl_VertexID` use; grep `layout(location` returned no matches in the thin VS; M2 precedent comments grep'd | ✅ (advisor stop-the-line #3 caught the v1 mistake; corrected) |
| **[V16]** Hard-failure stance after gate-off | Once the SOLID gate-off has fired (`BeginLegacySolidCluster()` returned false in `setupTextures`), `TerrainPatchStream` has no SOLID records and `flush()` cannot recover same-frame. Plan v2's preflight (`ComputePreflight()`) lifts ALL failure-prone work BEFORE the gate-off site reads `IsFrameSolidArmed()` — pack, command build, dirty-recipe upload all run in preflight. After preflight returns true, the only remaining work is `glMultiDrawArraysIndirect`, which by GL spec does not fail synchronously. A `false` return from the bridge function is therefore a hard process-level failure: log `event=hard_failure`, set `ForceDisableArmingForProcess()`, advise operator to set `MC2_TERRAIN_INDIRECT=0`. Same-frame fallback to `flush()` is explicitly NOT a recovery path | Advisor stop-the-line #1 caught the latent hazard; preflight design verified against the existing call shape (vertexProjectLoop runs before `Terrain::geometry quadSetupTextures`, so quadList is populated when preflight needs it) | ✅ |
| **[V17]** Invalidation strategy — precise XOR whole-map per site | `mapdata.cpp:149` (in `MapData::destroy`) and `mapdata.cpp:191` (in `MapData::newInit`) get `gos_terrain_indirect::InvalidateAllRecipes()` next to the existing `invalidateTerrainFaceCache()` call. `mapdata.cpp:1293+` (in `setTerrain`) gets `InvalidateRecipeForVertexNum(vn)` BEFORE its existing call to `invalidateTerrainFaceCache()`. The body of `invalidateTerrainFaceCache(void)` itself gets NO new dense-recipe call — that would defeat the per-entry story (precise + whole-map at same site = wasted precise call) | Advisor stop-the-line #2 caught this; v2 ensures every site is precise XOR whole-map, never both | ✅ |

**Verification appendix summary:**
- 17 verified entries (V1–V17).
- 0 divergent (⚠️) entries.
- 1 needs-follow-up (❓) entry: V4 — if `glEnableVertexAttribArray(0)` runtime mitigation fails on actual AMD hardware during Stage 3 bring-up, the executor adds a dummy `layout(location = 0) in vec4 _attr0_dummy;` line to `gos_terrain_thin.vert` plus a small `glVertexAttrib4f(0, 0,0,0,0)` set, mirroring the doc's recommendation. **User signed off on runtime form as the default** (advisor preferred the shader form; user's call stands).

**Architectural decisions surfaced to user (open follow-ups):**
- **V4 (AMD attr-0 mitigation kind).** Plan v2 picks runtime `glEnableVertexAttribArray(0)` over shader-side `layout(location = 0)`. Both are valid per the AMD driver rules doc; the runtime form is less intrusive (no shader change, no parity surface). If bring-up surfaces a regression, the executor switches to the shader form and re-validates. Surfacing because the doc's wording prefers the shader form ("Add layout(location = 0) with a dummy read"); the choice is judgment-call territory.

---

## Cross-references

- Brainstorm: [docs/superpowers/brainstorms/2026-04-30-indirect-terrain-draw-scope.md](docs/superpowers/brainstorms/2026-04-30-indirect-terrain-draw-scope.md) — Q3 amended at plan time (see addendum above)
- Design: [docs/superpowers/specs/2026-04-30-indirect-terrain-draw-design.md](docs/superpowers/specs/2026-04-30-indirect-terrain-draw-design.md)
- Recon handoff: [docs/superpowers/plans/progress/2026-04-30-indirect-terrain-recon-handoff.md](docs/superpowers/plans/progress/2026-04-30-indirect-terrain-recon-handoff.md)
- Adversarial review skill: [.claude/skills/adversarial-plan-review.md](.claude/skills/adversarial-plan-review.md)
- Orchestrator: [docs/superpowers/cpu-to-gpu-offload-orchestrator.md](docs/superpowers/cpu-to-gpu-offload-orchestrator.md)

Pattern + gotcha references: same set as design doc Section "Cross-references" — `memory/water_ssbo_pattern.md`, `memory/m2_thin_record_cpu_reduction_results.md`, `memory/patchstream_m0b.md`, `memory/patchstream_shape_c.md`, `memory/water_rendering_architecture.md`, `memory/renderwater_fastpath_stage2.md`, `memory/gpu_direct_renderer_bringup_checklist.md`, `memory/uniform_uint_crash.md`, `memory/mc2_texture_handle_is_live.md`, `memory/terrain_mvp_gl_false.md`, `memory/sampler_state_inheritance_in_fast_paths.md`, `memory/render_order_post_renderlists_hook.md`, `memory/clip_w_sign_trap.md`, `memory/terrain_tes_projection.md`, `memory/quadlist_is_camera_windowed.md`, `memory/gpu_direct_depth_state_inheritance.md`, `memory/mc2_argb_packing.md`, `memory/feedback_offload_scope_stock_only.md`, `memory/stock_install_must_remain_playable.md`, `memory/debug_instrumentation_rule.md`, `memory/tracy_profiler.md`, `memory/feedback_smoke_duration.md`, `memory/brainstorm_code_grounding_lesson.md`.

---

## Execution handoff

Plan v2 saved to [docs/superpowers/plans/2026-04-30-indirect-terrain-draw-plan.md](docs/superpowers/plans/2026-04-30-indirect-terrain-draw-plan.md) (overwrites v1). The verification appendix above is the input to the next adversarial-plan-review pass; the planner session runs that skill against this plan v2 self-review before declaring it ship-ready.

Two execution options after sign-off:

1. **Subagent-driven (recommended)** — fresh subagent per task; review between tasks. **REQUIRED SUB-SKILL:** `superpowers:subagent-driven-development`.
2. **Inline execution** — same session walks Stage 0 → Stage 4. **REQUIRED SUB-SKILL:** `superpowers:executing-plans`.

Either way, the executor session walks Stage 0 → Stage 4 in order, with each stage's commit landing before the next stage starts. **PR1 = Stages 0+1+2+3 in one branch (per N2 — 3a + 3b cannot ship separately).** PR2 = Stage 4 (default-on flip + N4 cross-mission Gate D quintuple) after soak.
