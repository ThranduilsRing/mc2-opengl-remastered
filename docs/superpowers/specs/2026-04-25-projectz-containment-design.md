# projectZ Containment — Design Spec

Date: 2026-04-25
Status: design (no code yet)
Scope: containment + diagnostics only. **No replacement. No retirement. No admission-behavior change.**

## Core Thesis

`Camera::projectZ()` (declared inline at [`mclib/camera.h:386`](../../../mclib/camera.h)) is **not** merely a projection helper. Its bool return is a **legacy screen-space terrain-admission test**. The current renderer depends on that admission behavior *before* tessellation and submission. GPU homogeneous clipping is **not equivalent**, and shader-side emulation is **too late** for some failure modes (tessellation control-point validity, patch expansion, rasterized triangle area).

The first goal is **containment and measurement**, not replacement. We make the legacy admission contract explicit, expose its inputs and outputs, run candidate modern predicates alongside it, and identify which disagreement cases correlate with bad geometry. Retirement is a downstream decision driven by data.

This spec covers everything up to "we have the data." It does not cover what to do with the data.

## Background — What projectZ Actually Does

Inline body at `mclib/camera.h:386-425`:

```cpp
bool projectZ (Stuff::Vector3D &point, Stuff::Vector4D &screen)
{
    Stuff::Vector4D xformCoords;
    Stuff::Point3D coords;
    coords.x = -point.x;          // MC2 world → Stuff camera-space axis swap
    coords.y = point.z;
    coords.z = point.y;

    xformCoords.Multiply(coords, worldToClip);   // single composite matrix

    if (usePerspective) {
        float rhw = 1.0f;
        if (xformCoords.w != 0.0f)
            rhw = 1.0f / xformCoords.w;

        screen.x = (xformCoords.x * rhw) * viewMulX + viewAddX;
        screen.y = (xformCoords.y * rhw) * viewMulY + viewAddY;
        screen.z = (xformCoords.z * rhw);
        screen.w = fabs(rhw);                     // ← W-sign destroyed
    } else { /* parallel branch */ }

    if ((screen.x < 0) || (screen.y < 0) ||
        (screen.x > screenResolution.x) || (screen.y > screenResolution.y))
        return FALSE;

    return TRUE;
}
```

Two contracts inside one function:

1. **Bool clipper / admission gate** (lines 421-422): pixel-rectangle test on already-divided screen coordinates. Returns `false` if the post-divide xy is outside `[0..screenResolution]`. Does **not** test signed W, near, far, or homogeneous clip validity.
2. **Screen-XY oracle** (lines 406-409): produces post-divide screen pixel coords (and a sign-erased pseudo-W via `fabs(rhw)`).

`fabs(rhw)` at line 409 is the smoking gun. It deliberately destroys the W sign before any caller can use it. Behind-camera vertices whose post-divide xy happens to land in the screen rectangle return `true`. In-front-of-camera vertices one pixel off-screen return `false`. **No modern homogeneous-clip predicate can reproduce this exactly.**

## Why Previous Attempts Failed

Three attempts to remove or replace projectZ produced regressions: giant terrain triangles, transparent terrain wedges, FPS collapse, and gradient artifacts. Postmortem in [`docs/superpowers/plans/2026-04-17-render-contract-cleanup.md`](../plans/2026-04-17-render-contract-cleanup.md). The previously diagnosed cause — "GPU clipper makes the CPU pz gate redundant" — was empirically falsified.

Updated diagnosis: the failures share one mechanism. A "linear clip-space rewrite" produces *finite* clip values for vertices the legacy chain would have admission-rejected via screen-rect. Those finite values then drive tessellation factors, patch interpolation, or rasterized triangle area. By the time a fragment-stage test could discard them, the damage is done at vertex/tess stage.

The right framing is **wrong layer, not impossible math**. A shader can reproduce the legacy screen-rect arithmetic given the same matrix, viewport constants, and axis swap — but it must run *before* vertices become tessellation control points, which on the current pipeline means CPU pre-submission.

## Goals

- Make the legacy admission contract **explicit**: a named struct, a named function, a named decision point.
- Make it **measurable**: every callsite tagged by intent; per-vertex and per-submitted-triangle results recorded against multiple candidate predicates.
- Make it **replaceable** in principle: the wrapper is shaped so that future replacement is a localized change to one decision point, not a hunt across the codebase.
- **Preserve current behavior byte-for-byte** through this entire spec. No visible change to terrain admission, screen positions, lighting activation, or anything else.

## Non-Goals (explicit)

- Not removing `projectZ()`.
- Not changing terrain admission behavior anywhere.
- Not modifying terrain submission, the legacy CPU heightfield, or the `MC2_ISTERRAIN` flow.
- Not the `TerrainGeometryAuthority` refactor. The wrapper struct is its seed; growth is a future spec.
- Not D1a (per-triangle pz depth gate) or D1b (visibility producer cleanup). Both predicated on assumptions the line-421 viewport-rect test invalidates; both blocked on this spec's diagnostics.
- Not the terrain-material / decal / overlay modernization. Frozen until projectZ diagnostics return data.

## Frozen Surfaces (do not touch during this spec's commits)

- terrain submission (any path that calls `mcTextureManager->addTriangle(..., MC2_ISTERRAIN | ...)`)
- tessellation control-point validity
- terrain patch culling / `inView` / `canBeSeen` / `objBlockInfo`
- shadow terrain projection
- view-dependent terrain bounds
- camera math (`worldToClip`, `viewMulX/Y`, `viewAddX/Y`, `screenResolution`)

If a commit in this spec's sequence appears to require touching any of the above, **stop and flag**. The change has exited containment scope.

## Surfaces That May Continue in Parallel (carefully)

- material weightmap planning (no submission changes)
- overlay/decal import planning (no submission changes)
- shader cleanup that does not affect geometry admission
- debug UI
- cache/schema planning
- sidecar format planning

If team bandwidth is limited, prioritize this spec and freeze even the parallel-safe items. projectZ containment is a foundational blocker.

## Design

### LegacyProjectionResult

A structured result that exposes everything callers might want, without changing what `projectZ()` itself returns.

```cpp
struct LegacyProjectionResult {
    // Legacy contract — must match projectZ() byte-for-byte.
    bool             acceptedByLegacyScreenRect;  // current bool return
    Stuff::Vector4D  screen;                       // current screen Vector4D output

    // Newly exposed for diagnostics and future replacement design.
    Stuff::Vector4D  rawClip;       // xformCoords pre-divide (signed W preserved)
    float            signedW;       // xformCoords.w (NOT fabs'd)
    float            legacyRhw;     // matches current code: 1.0f when signedW == 0, else 1.0f/signedW
    bool             usePerspective;// branch taken
};
```

`legacyRhw` matches the current `projectZ()` arithmetic exactly — initialized to `1.0f`, divided only when `signedW != 0`. It does **not** introduce divide-by-zero. A separate `trueSignedRhw = 1.0f / signedW` (which can be ±Inf) is **deferred to the instrumentation commit (commit 3)**, not added in commit 1, and is diagnostic-only.

```cpp
// Added in commit 3, NOT commit 1:
//   float trueSignedRhw;   // diagnostic-only; ±Inf if signedW == 0; never matches legacy
```

Naming: `LegacyProjectionResult` (not `TerrainProjectionResult`) because the contract is camera-wide, not terrain-specific. Future evolution into `TerrainProjectionService` or `TerrainGeometryAuthority` is a separate spec.

### Wrapper Contract

Commit 1 must minimize the chance of compiler-induced drift in the hot path. **Preferred shape — preserve the existing `projectZ()` body as the primary implementation; emit `LegacyProjectionResult` as an optional sidecar:**

```cpp
// Existing projectZ keeps its current body verbatim. New optional out-param:
bool Camera::projectZ(Stuff::Vector3D& point, Stuff::Vector4D& screen,
                      LegacyProjectionResult* optionalResult = nullptr);
```

The body fills `screen` and computes the bool exactly as today. If `optionalResult != nullptr`, it additionally writes `rawClip`, `signedW`, `legacyRhw`, `usePerspective`. Existing callsites (which pass no third argument) compile unchanged; only the wrapper internals see the new path.

A value-returning `projectZLegacy()` may be added later **only if** profiling and bit-equivalence both confirm zero drift. For commit 1, do not force `projectZ()` through a value-returning wrapper if it perturbs inlining or temporary lifetimes.

**Hard requirement:**

> The wrapper must preserve the exact operation order of current `projectZ()`. Golden-file comparison should be **bit-identical under the same compiler/settings**. Any difference must be investigated and either fixed or explicitly proven to be compiler-only drift before proceeding.

Verified by golden-file comparison on smoke-tier1 + one stress mission (camera pan to map edge). The first compiler that produces measurable last-bit drift between baseline and containment builds halts the commit; the drift is documented and either eliminated (e.g. by adjusting `volatile`/`-ffloat-store`/operation grouping) or accepted as compiler-only with explicit evidence (assembly diff showing identical FMA selection, etc.).

### Caller Inventory — Categories

Every callsite of `projectZ` and `inverseProjectZ` is tagged in source by intent, using these categories:

| Category | Meaning | Example callsite |
|---|---|---|
| `BoolAdmission` | Bool result gates submission/activation | quad.cpp:537 (`clipData = eye->projectZ(...)`) |
| `ScreenXYOracle` | Bool discarded; only `screen.xy` used | camera.cpp:864 (cell-grid corners) |
| `Both` | Bool gates AND screen.xy consumed | (suspected; confirm during inventory) |
| `InverseProjectionPair` | Paired with `inverseProjectZ` for picking/tacmap | (TBD during inventory) |
| `LightingShadow` | Light/shadow admission | camera.cpp:1749, 1775 |
| `SelectionPicking` | Mouse selection of terrain/objects | actor.cpp:290, 293 (suspected) |
| `DebugOnly` | Debug overlay / TacMap drawing | (TBD) |

Tagging mechanism: a comment marker `// [PROJECTZ:Category]` adjacent to each call. Greppable. Updated as inventory progresses. The marker is documentation; no code generation depends on it.

### Diagnostic System

Env-gated, per worktree CLAUDE.md and project auto-memory `debug_instrumentation_rule.md`. All env vars default off, including the summary.

**Env vars (all default OFF):**
- `MC2_PROJECTZ_TRACE=1` — per-call diagnostic record (heavy; use for short captures)
- `MC2_PROJECTZ_HEATMAP=1` — accumulate per-quad/per-triangle disagreement counters; surface in debug overlay
- `MC2_PROJECTZ_SUMMARY=1` — prints `[PROJECTZ v1] summary` every 600 frames + on shutdown with disagreement counts per category and per predicate. **Default off.** Even low-frequency log noise is unwelcome during normal play; opt in explicitly.

**Hard guard — instrumentation must be observation-only:**

> Instrumentation must not feed back into culling, submission, projection, selection, lighting, timing, or any code path that affects rendered output. It may log and draw debug overlays only. The diagnostic must never become a hidden behavior dependency. Code review on commits 3-4 must verify no diagnostic value is read by any non-debug code path.

**Stable callsite IDs:** every projectZ/inverseProjectZ callsite gets a hand-assigned stable ID at inventory time (e.g. `terrain_quad_cluster_a`, `light_active_test`, `actor_screen_pos`). The ID lives in the `// [PROJECTZ:Category id=<stable_id>]` source marker. Trace records emit both the stable ID and the file:line so captures stay comparable across edits to surrounding code.

**Per-vertex record (when `MC2_PROJECTZ_TRACE=1`):**

```
[PROJECTZ v1] vertex callsiteId=terrain_quad_cluster_a
  file=quad.cpp line=537 cat=BoolAdmission branch=perspective
  point=(wx,wy,wz)
  signedW=...  legacyRhw=...  trueSignedRhw=...  rawClip=(x,y,z,w)
  screen=(sx,sy,sz,sw)  legacyAccepted=true|false
  predicates: legacyRect=... legacyRectFinite=... homogClip=... rectSignedW=... rectNearFar=... rectGuard=...
```

For `branch=parallel` records, the modern-clip predicates (`homogClip`, `rectSignedW`, `rectNearFar`) are emitted as `n/a` and **excluded from disagreement counts**. The orthographic branch has different semantics (no perspective divide, no homogeneous W); contaminating perspective-branch counts with it would produce noise. A separate parallel-branch policy is out of scope for this spec.

**Per-triangle record (terrain submission paths only):**

```
[PROJECTZ v1] tri callsiteId=terrain_quad_cluster_a
  quad=(qx,qy) cluster=0,1,2 file=quad.cpp line=537
  v0.legacy=true v1.legacy=true v2.legacy=false
  trianglePolicy_any=...        // would-be admit if ANY vertex passes
  trianglePolicy_all=...        // would-be admit if ALL vertices pass
  trianglePolicy_majority=...   // would-be admit if ≥2 of 3 pass
  currentSubmitted=true|false   // what the submission path actually did
  containsLegacyRejectedVertex=true|false
  screenAreaPixels=...          // post-divide screen-space cross-product magnitude
```

The three `trianglePolicy_*` fields are **observed candidates**, not selections. The capture report compares them against `currentSubmitted` and the visible wedge class without implying any one policy is active.

**Schema versioning:** `[PROJECTZ v1]` prefix. Schema bumps invalidate older log captures. Format change requires version bump; no backward-compat shims.

### Multi-Predicate Comparison Set

For every perspective-branch projectZ call, compute and record the result of each candidate predicate. None of these change behavior — they are observed only. Parallel-branch calls record `branch=parallel` and are excluded from these comparisons.

**Per-vertex predicates (computed inside / alongside `projectZ`):**

| ID | Predicate | Notes |
|---|---|---|
| `legacyRect` | Current line-421 screen-rect test (ground truth) | Reference; must equal current bool return |
| `legacyRectFinite` | `legacyRect && isfinite(screen.x,y,z,w)` | Catches cases where legacy admits despite non-finite output |
| `homogClip` | `signedW > 0 && abs(rawClip.x) ≤ rawClip.w && abs(rawClip.y) ≤ rawClip.w` | Standard homogeneous-clip predicate |
| `rectSignedW` | `legacyRect && signedW > 0` | Legacy admission with behind-camera reject restored |
| `rectNearFar` | `legacyRect && rawClip.z ≥ 0 && rawClip.z ≤ rawClip.w` | Legacy admission + clip-space depth gate |
| `rectGuard` | `legacyRect`, viewport expanded by `N` pixels (N tunable via `MC2_PROJECTZ_GUARD_PX`; default N=64) | Detects under-cull cases legacy hides |

**Per-submitted-triangle facts (recorded at submission time, not inside `projectZ`):**

| ID | Meaning |
|---|---|
| `currentSubmitted` | True if the submission path actually emitted this triangle today |
| `containsLegacyRejectedVertex` | True if any vertex of this triangle had `legacyRect == false` |
| `clusterIndexSet` | Which of `(0,1,2) (0,2,3) (0,1,3) (1,2,3)` was submitted |
| `screenAreaPixels` | Post-divide cross-product magnitude (approximate) |
| `trianglePolicy_any/all/majority` | Observed policies; not active |

Vertex predicates and triangle facts are kept in separate records because submission is per-triangle (or per-cluster-of-triangles), not per-vertex-call. Conflating them is what hid the wedge class in earlier diagnostics.

The interesting question is **not** "does X match `legacyRect`" but "which disagreement pattern correlates with the wedge / FPS-collapse class observed in the prior failures." The diagnostic must let us answer that.

### Color-Coded Debug Overlay

Triangle-index-aware. Quads submit four different triangle clusters: `(0,1,2)`, `(0,2,3)`, `(0,1,3)`, `(1,2,3)`. The overlay must render each submitted triangle's color, not aggregate per-quad.

| Color | Meaning |
|---|---|
| Green | Legacy and chosen candidate predicate agree on this triangle |
| Yellow | Legacy accepts, candidate rejects (legacy more permissive — over-cull risk if we switch) |
| Orange | Candidate accepts, legacy rejects (candidate more permissive — wedge risk if we switch) |
| Red | Submitted triangle contains a legacy-rejected vertex (the actual wedge condition) |
| Purple | NaN/Inf anywhere in the pipeline, OR projected triangle screen-area exceeds threshold (default: half the viewport area) |

Toggle hotkey: `RAlt+P` (P for projectZ). Cycles through candidate predicates so the same overlay can be re-read against `homogClip`, `rectSignedW`, etc. without restart.

Red and purple are the wedge suspects. The capture goal is to drive red and purple to zero on stock + custom campaigns by choosing the right replacement policy in a *future* spec.

### Capture Methodology

Once instrumentation lands:

1. **Stock baseline.** Capture summary + 60-second trace on each tier1 mission (`mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24`) with default camera behavior. Record disagreement counts per predicate.
2. **Stress capture.** For each tier1 mission, additionally capture: max-zoom-out, edge-of-map pan, 90°+ rotation pan, behind-mountain occlusion approach, max-altitude (Wolfman-mode if available). These are the conditions that historically produced wedges.
3. **Custom campaigns.** Repeat (2) for at least one mission each from CVE-G, Carver5O, Omnitech, Wolfman if launchable. Document any campaign that cannot be captured.
4. **Compare and report.** Disagreement counts and red/purple triangle counts per predicate per scenario. The output document is what the *next* spec consumes to decide replacement policy.

## Success Criteria

This spec is complete when:

> We can explain exactly why every terrain vertex and submitted triangle was accepted or rejected, compare that result against candidate modern predicates, and identify which disagreement cases produce bad geometry — without having changed any admission behavior in the process.

Concrete checks:
- [ ] `LegacyProjectionResult` struct exists; `projectZ()` preserves its current body and operation order and optionally fills `LegacyProjectionResult` via `optionalResult`. No `projectZLegacy()` is introduced.
- [ ] Byte-equivalence verified on smoke-tier1: every terrain triangle submitted in baseline build is also submitted in containment build, with identical screen coords.
- [ ] Every projectZ/inverseProjectZ callsite is tagged with `// [PROJECTZ:Category]`.
- [ ] `MC2_PROJECTZ_TRACE`, `MC2_PROJECTZ_HEATMAP`, `RAlt+P` overlay all functional.
- [ ] All five+ candidate predicates computed per call and exposed.
- [ ] Triangle-index-set-aware records emitted on terrain submission paths.
- [ ] Capture data exists for stock tier1 + at least one stress scenario per tier1 mission.
- [ ] Capture data exists for ≥1 custom campaign mission.
- [ ] Summary report identifies which predicate(s) match `legacyRect` closely and which disagreement patterns correlate with red/purple triangles.

## Outcome Decision Matrix (NOT decided here — driven by capture data)

The capture report from this spec hands off to a *future* replacement-policy spec. Outcomes are recorded for clarity but **not pre-decided**:

| Outcome | When | Implication |
|---|---|---|
| A — Legacy required forever | No candidate predicate matches `legacyRect` closely enough on the wedge-prone scenarios | `projectZLegacy` becomes the permanent admission API; `projectZ` is renamed/retained as a shim. Renderer modernization proceeds inside that contract. |
| B — Single modern predicate suffices | One candidate (e.g. `rectSignedW`) agrees with legacy on >99.9% of admission decisions and produces zero red/purple triangles in capture | Migrate `BoolAdmission` callsites to that predicate; legacy becomes a debug-only fallback. |
| C — Per-callsite policy split | Different categories need different predicates (terrain submission needs legacy, lighting/shadow needs homogClip, picking needs something else) | Replace `projectZ` with category-specific named functions; legacy stays for the categories that require it. |

## Commit Sequence

Each commit is reviewable in isolation and can be reverted independently. None changes admission behavior.

1. **`projectz: add LegacyProjectionResult sidecar to projectZ`** — Add `LegacyProjectionResult` struct; add an `optionalResult` out-param sidecar to `Camera::projectZ()`. Preserve the existing `projectZ` body and operation order as the primary implementation. **No `projectZLegacy()`. No delegation/value-returning wrapper.** No callsite changes. Smoke-tier1 + golden-file diff. First commit must be visually and behaviorally inert; if anything observable changes, it failed.
2. **`projectz: tag callsites by intent`** — Add `// [PROJECTZ:Category]` markers at every callsite. No code logic change. Inventory document committed alongside under [`docs/superpowers/specs/projectz-callsite-inventory.md`](projectz-callsite-inventory.md).
3. **`projectz: instrumentation (env-gated)`** — `MC2_PROJECTZ_TRACE`, `_HEATMAP`, `_SUMMARY` env vars; `[PROJECTZ v1]` log format; per-vertex and per-triangle records. **All three env vars default off**, including the summary; the 600-frame summary cadence applies only when `MC2_PROJECTZ_SUMMARY=1`.
4. **`projectz: debug overlay (RAlt+P)`** — Triangle-index-aware color overlay; predicate cycling.
5. **`projectz: capture report`** — Run captures, commit results document. **End of this spec.**

Replacement policy work begins under a *new* spec that consumes commit-5's output.

## Open Questions

1. **Where does `projectZ` get called from worktrees outside `mclib/` and `code/`?** Inventory commit should grep all worktrees, not just nifty-mendeleev. If other worktrees diverge on call patterns, document it.
2. **Does `inverseProjectZ` need the same containment treatment?** Likely yes (TacMap + picking); confirm during inventory. May warrant a separate, smaller follow-on spec.
3. **What's the right N for `rectGuard`?** Start at 64px; capture data should show whether smaller (16) or larger (128) draws a cleaner separation. Tunable in the env-var system.
4. **How is "rasterized triangle area" measured for the purple threshold?** Approximation: post-divide screen-space cross-product magnitude. GPU readback would be more accurate but is overkill for this stage.
5. **Are there custom-campaign missions we *cannot* capture due to crashes/blockers?** Document them; treat as known unknowns rather than gaps in the report.
6. **Does `worldToClip` ever change mid-frame?** If yes, per-call diagnostics need a frame counter to distinguish capture sessions. Likely no, but confirm in the wrapper commit.

## References

- [`mclib/camera.h:386`](../../../mclib/camera.h) — `projectZ()` definition
- [`mclib/camera.cpp:1806`](../../../mclib/camera.cpp) — `inverseProjectZ()` definition
- [`docs/superpowers/plans/2026-04-17-render-contract-cleanup.md`](../plans/2026-04-17-render-contract-cleanup.md) — failed-removal postmortem; D1a "reverted, genuinely required" finding
- Project auto-memory: `debug_instrumentation_rule.md` (lookup via `MEMORY.md` index) — debug instrumentation conventions
- Worktree [`CLAUDE.md`](../../../CLAUDE.md) — Tier-1 instrumentation env-var pattern, schema versioning, smoke gate
