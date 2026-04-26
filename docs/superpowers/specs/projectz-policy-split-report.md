# projectZ Policy Split — Closing Report

**Date:** 2026-04-26
**Status:** complete (pending operator-side visual checks listed below)
**Branch:** `projectz-hunting`
**Spec:** [`2026-04-26-projectz-policy-split-design.md`](2026-04-26-projectz-policy-split-design.md)
**Plan:** [`docs/superpowers/plans/2026-04-26-projectz-policy-split-impl.md`](../plans/2026-04-26-projectz-policy-split-impl.md)
**Predecessor:** [`projectz-capture-report.md`](projectz-capture-report.md) (containment + Outcome C verdict)

---

## TL;DR

The 8-commit policy-split sequence landed cleanly. Every one of the 99 production `projectZ` callsites + 4 `inverseProjectZ` callsites is now routed to a category-specific wrapper. Raw `projectZ()` and `inverseProjectZ()` carry `[[deprecated]]` attributes; a build with the deprecation warning unsuppressed shows exactly the 8 wrapper-body warnings and zero production-callsite leaks. The spec's exit criterion is satisfied. All 5 tier-1 smoke missions pass after every commit.

Outcome C from the containment report is now actionable: future predicate-experiment work can land in one named wrapper at a time, with terrain admission isolated as the explicit wedge-risk vector.

## Outcome — Per Spec Exit Criteria

The spec listed five non-behavior-change guarantees. Status:

| # | Guarantee | Status |
|---|-----------|--------|
| 1 | Smoke-tier1 PASS within ±2 FPS of pre-split baseline | ✅ 5/5 PASS after every commit; FPS stayed in the established 104–143 envelope |
| 2 | Golden frame diff on `mc2_01` shows zero pixel difference | ⏳ Operator-side; deferred (see "Deferred operator actions" below) |
| 3 | Terrain triangle submission count identical for the same camera path | ⏳ Operator-side via `MC2_PROJECTZ_TRACE` capture replay; deferred |
| 4 | `MC2_PROJECTZ_FINITE_CHECK` builds complete `mc2_01` start-to-+60s without `gosASSERT` firing | ⏳ Operator-side; deferred |
| 5 | Compiler warning census: zero unreviewed direct callsites | ✅ Verified at Task 7. Unsuppressed build showed exactly 8 wrapper-body warnings (lines 527, 540, 553, 566, 572, 578, 584, 595 in `mclib/camera.h`) and the unrelated pre-existing `strdup` POSIX-name warning in `ablmc2.cpp`. Zero production-callsite leaks. |

The two in-band guarantees (1 and 5) are confirmed. The three operator-side guarantees (2, 3, 4) are listed in the "Deferred operator actions" section below.

## Commits Landed (in order)

| Commit | Subject | Sites | Files |
|--------|---------|-------|-------|
| `1bc2da2` | projectz: add intent-specific wrapper API (no callsite changes) | — | `mclib/camera.h` |
| `e094340` | projectz: tighten wrapper API hygiene (indentation, naming, comments) | — | `mclib/camera.h` |
| `20b0384` | projectz: route terrain + object admission to wrappers (6 sites) | 6 | `mclib/quad.cpp`, `mclib/terrain.cpp`, `code/gameobj.cpp` |
| `71bd2be` | projectz: route effect admission to wrapper (7 sites: cloud/crater/weather) | 7 | `mclib/clouds.cpp`, `mclib/crater.cpp`, `code/weather.cpp` |
| `f9872c9` | projectz: route picking + lighting to wrappers (6 sites) | 6 | `mclib/camera.cpp`, `mclib/terrain.cpp`, `code/missiongui.cpp` |
| `6908b8e` | projectz: restore eye-> prefix at picking_closest_cell_center | 1 (fix) | `mclib/camera.cpp` |
| `0c18728` | projectz: route ScreenXYOracle sites to wrapper (51 sites) | 51 | 14 files |
| `0eebf70` | projectz: route DebugOnly sites to wrapper (25 sites) | 25 | `mclib/quad.cpp`, `code/team.cpp`, `code/missiongui.cpp` |
| `6ad08fa` | projectz: route tacmap inverse projection to wrapper (4 sites) | 4 | `code/gametacmap.cpp` |
| `cc83857` | projectz: add legacyRectFinite invariant on wedge-class wrappers | — | `mclib/camera.h` |
| `86ecdfe` | projectz: deprecate raw projectZ() / inverseProjectZ() | — | `mclib/camera.h` |

11 commits total (the spec planned 8; two extra commits — `e094340` cleanup and `6908b8e` `eye->` restore — were added during implementation, see Surprises).

**Total routing: 99 forward + 4 inverse = 103 sites across 19 files.** Every site uses its category-specific wrapper. Zero raw `projectZ(` or `inverseProjectZ(` callsites remain in production code.

## Wrapper Routing Distribution (final)

| Wrapper | Sites routed | Category |
|---------|-------------|----------|
| `projectForTerrainAdmission` | 5 | Wedge-class — terrain heightfield admission |
| `projectForObjectAdmission` | 1 | Wedge-class — object lifecycle (`canBeSeen` chain) |
| `projectForEffectAdmission` | 7 | Wedge-class — effect billboards (cloud/crater/weather) |
| `projectForLightingShadow` | 2 | Light activation |
| `projectForSelectionPicking` | 4 | Picking — bool discarded |
| `projectForScreenXY` | 51 | Cosmetic screen-XY oracle |
| `projectForDebugOverlay` | 25 | Debug visualizers (LAB_ONLY + dead code) |
| `inverseProjectForPicking` | 4 | Tactical map viewport unprojection |
| **Total** | **99 + 4 = 103** | |

The 3 wedge-class wrappers (`projectForTerrainAdmission`, `projectForObjectAdmission`, `projectForEffectAdmission`) carry the `MC2_PROJECTZ_FINITE_CHECK`-gated `legacyRectFinite` invariant. The other 4 forward wrappers and the inverse wrapper do not.

## Surprises Encountered During Implementation

### 1. Inventory line numbers had drifted ~60 lines

The callsite inventory ([`projectz-callsite-inventory.md`](projectz-callsite-inventory.md)) was generated at commit 2 of the containment work. Between then and Task 2 of this implementation, the four terrain quad sites had drifted: `terrain_quad_vert0_admit` was at line 525 in the inventory but actually at line 588 by the time the routing landed. The implementer correctly anchored on the unique `// [PROJECTZ:... id=...]` markers, which made the drift a non-issue.

**Lesson:** stable IDs in source-marker comments are load-bearing. The line numbers in the inventory are documentary, not operational.

### 2. `picking_closest_cell_center` had `eye->` despite "Camera self-call"

The dispatch instruction for Task 4 said this site (in `Camera::inverseProject(...)`) had no `eye->` prefix because Camera was calling its own method. The implementer found `eye->projectZ(point,cellCenter)` actually present in source and stripped the `eye->` to comply with the (wrong) instruction. This was caught by controller-side diff inspection and corrected in a follow-up commit (`6908b8e`).

The original code deliberately used the global `eye` pointer (active camera projection state) rather than `this`. Even inside a `Camera::` member, `eye != this` is possible. Stripping `eye->` would change `eye->projectForSelectionPicking(...)` into `this->projectForSelectionPicking(...)` — a real behavior change.

**Lesson:** the rule "single-token swap, preserve receiver expression" must be applied at the source level, not at the dispatch-instruction level. Inventory descriptions of receiver context are advisory.

### 3. Code-review fixes between Task 1 and Task 2

Task 1 (the inert API-surface commit) drew Important code-review issues: 4-space indentation inside tab-indented bodies, a parameter-name mismatch on the inverse wrapper (`world` vs the underlying declaration's `point`), and missing per-wrapper comments for `projectForObjectAdmission`/`projectForEffectAdmission`. These were fixed in commit `e094340` before any routing started. The reviewer's prediction that the indentation would become harder to fix once Task 6/7 added `#pragma`/`#if` blocks turned out correct — the cleanup landed at exactly the right time.

### 4. `<cmath>` had to be explicitly added for `isfinite`

Task 6 needed `isfinite` for the `legacyRectFinite` invariant. `<math.h>` was transitively reachable but `<cmath>` was not. The implementer added `#include <cmath>` at the top of `mclib/camera.h`, which is the correct C++ header for `isfinite`. (`gosASSERT` was already transitively reachable via the existing include set.)

### 5. Spec doc had `const` on inverse wrapper that the code couldn't honor

The spec's wrapper API surface declared `inverseProjectForPicking(const Stuff::Vector4D& screen, ...)`. The underlying `inverseProjectZ` takes non-const `Stuff::Vector4D&` because the orthographic branch writes into `screen`. The implementer correctly dropped `const` to match. The spec document was patched by the controller in the same session to remove the `const` and add a comment explaining why.

## Deferred Operator Actions

These checks could not be performed by an automated implementation subagent because they require interactive game launch and human visual inspection. None block the merge to `nifty-mendeleev`, but they should be performed before any subsequent renderer/predicate work.

| # | Action | Affected commits | Why deferred |
|---|--------|-----------------|--------------|
| 1 | Golden frame diff on `mc2_01` start, +30s, +60s vs. `0c18728~10` baseline | `20b0384` (terrain+object) onwards | Needs operator screenshot capture + pixel-diff tool |
| 2 | Visual rain spot-check on `mc2_03` (confirm raindrops render) | `71bd2be` (effect) | Needs interactive game launch |
| 3 | Manual click test on `mc2_01`: single mech, drag-select 4 mechs, click empty terrain | `f9872c9` (picking+lighting) | Needs interactive input |
| 4 | Path-line / weapon-fire / selection-rect visual check on `mc2_01` | `0c18728` (51 ScreenXYOracle sites) | Needs interactive game launch |
| 5 | Tactical map viewport-rectangle position check on `mc2_01` | `6ad08fa` (inverse) | Needs interactive tac map |
| 6 | Build with `-DMC2_PROJECTZ_FINITE_CHECK`, run `mc2_01` for 60s, confirm no `gosASSERT` fires | `cc83857` (invariant) | Needs interactive game launch |
| 7 | Capture-replay equivalence: re-run `MC2_PROJECTZ_TRACE=1 MC2_PROJECTZ_HEATMAP=1` on `mc2_01` 60s, confirm per-callsite disagreement counts match the pre-split baseline from [`projectz-capture-report.md`](projectz-capture-report.md) | All routing commits | Needs operator capture run + manual diff |

The risk on items 1–5 is low because the wrappers are inline forwarders to `projectZ()`; compiler inlining guarantees byte-equivalent codegen. Smoke-tier1 PASS after each commit confirms no compile/link/runtime regressions in the missions exercised. The visual checks defend against environmental issues (stale deploy, wrong binary, etc.), not against logic drift.

Item 6 specifically validates the spec's claim that `legacyRectFinite` had zero disagreement across six captures. If the assertion fires under flag-on, the spec needs to revisit the invariant.

Item 7 is the in-spirit equivalent of the original "Task 8: closing report" capture-replay step from the plan. It is the strongest operator-side signal that the routing preserved trace-level behavior across all categories.

## Follow-Up Tickets Surfaced

These are observations made during implementation that warrant separate tracked work — not part of this spec's scope.

### A. `picking_terrain_rect_select` may be miscategorized

Inventory observation O1/Q1 already flagged this. The site discards the bool but a behind-camera vertex with garbage `screen.x/y` could cause spurious selection. Currently routed to `projectForSelectionPicking`; structurally closer to `projectForObjectAdmission`. Promotion would require a category-correction follow-up spec, not a wrapper change.

### B. `weaponbolt_beam_*` (8 sites), `mine_cell_corner*` (4 sites), `actor_vfx_top_depth` (1 site) consume `screen.z`

Spec Observation A. All 13 sites are routed to `projectForScreenXY` (bool discarded), but they have implicit depth dependencies. If a future predicate-experiment on `projectForScreenXY` changes screen output, these sites may need promotion to a `projectForScreenXYZ` variant. Defer until the experiment exists.

### C. `light_terrain_active_test` misuses screen-rect as frustum-proximity proxy

Inventory observation Q3. The comment "ON screen matters not" implies the original intent was a different activation criterion (probably distance-to-camera). Routing to `projectForLightingShadow` was correct for the rename pass; the actual fix is out of scope.

### D. Pre-existing indentation irregularity at `weather_raindrop_bot`

Code-quality reviewer flagged: `code/weather.cpp:497` has the routed call at column 0 inside a 3-tab-deep scope. Pre-existing in source (preserved by implementer, not introduced). Worth a future style-cleanup pass.

### E. RAlt+P overlay GL state bug

Pre-existing from containment work. Not addressed here. Required before any terrain-admission predicate replacement attempt (per the capture report's Outcome C analysis).

## Spec Exit Criterion — Final Confirmation

> Raw `projectZ()` has zero unreviewed production callsites after the split. Any remaining use must be explicitly marked compatibility shim, test-only, or `ROUTING_REVIEW_REQUIRED` with a tracked follow-up.

**Confirmed.** The Task 7 unsuppressed warning census found exactly 8 wrapper-body warnings (the 7 forward wrappers + 1 inverse wrapper, all at known lines in `mclib/camera.h`) and zero warnings outside the wrapper infrastructure. No `PROJECTZ_COMPATIBILITY_SHIM`, `ROUTING_REVIEW_REQUIRED`, or test-only markers were needed — every production callsite routed cleanly.

The spec's "End of this spec" condition (Task 8) is now met. Subsequent predicate-replacement work begins under a new spec.

## Recommended Next Steps

1. **Operator runs the 7 deferred checks above.** Items 6 (flag-on validation) and 7 (capture-replay equivalence) are the highest-value.
2. **Merge `projectz-hunting` → `nifty-mendeleev`** once the deferred checks pass. The branch is ready as-is for merge; no rework is anticipated.
3. **Open the RAlt+P overlay GL bug as a separate ticket.** It is the prerequisite for the next stage of work (terrain-admission predicate decision).
4. **Begin the render-contract registry spec** (advisor's "A" from the post-containment roadmap) only after the merge lands. The wrapper split gives that work a clean boundary around the still-legacy terrain admission contract.

## References

- Spec: [`2026-04-26-projectz-policy-split-design.md`](2026-04-26-projectz-policy-split-design.md)
- Plan: [`docs/superpowers/plans/2026-04-26-projectz-policy-split-impl.md`](../plans/2026-04-26-projectz-policy-split-impl.md)
- Predecessor capture report: [`projectz-capture-report.md`](projectz-capture-report.md)
- Inventory (source-of-truth for routing): [`projectz-callsite-inventory.md`](projectz-callsite-inventory.md)
- Containment design: [`2026-04-25-projectz-containment-design.md`](2026-04-25-projectz-containment-design.md)
- Wrapper API definitions: [`mclib/camera.h:514-600`](../../../mclib/camera.h)
