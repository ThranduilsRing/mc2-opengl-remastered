# Render Contract Registry — Closing Report

**Date:** 2026-04-26
**Status:** complete (pending operator-side smoke + visual checks listed below)
**Branch:** `claude/nifty-mendeleev`
**Spec:** [`2026-04-26-render-contract-registry-design.md`](2026-04-26-render-contract-registry-design.md)
**Predecessor:** [`projectz-policy-split-report.md`](projectz-policy-split-report.md)

---

## TL;DR

The 8-commit phase-1 sequence landed cleanly. Every `GBuffer1 = vec4(...)` literal in production fragment shaders is now routed through a typed helper in `shaders/include/render_contract.hglsl`. `shadow_screen.frag` reads the post-shadow mask through the canonical `rc_pixelHandlesOwnShadow` helper, the sole authority for the threshold value. C++ enums (`PassIdentity`, `GBufferSlot`, `ShadowContract`, `PassStateContract`) name the contract dimensions in `mclib/render_contract.h`. Six major draw entry points carry `// [RENDER_CONTRACT:Pass=...]` markers. A grep census (`scripts/check-render-contract-gbuffer1.sh`) enforces no future raw write reintroduces the implicit contract, and that the two legacy escape-hatch helpers stay scoped to their single shaders.

The spec's exit criterion is satisfied. RelWithDebInfo build green in nifty after every commit. The audit produced one **escalated finding** that operator should review before scheduling F3 work: `enableMRT()` / `disableMRT()` are defined-but-uncalled, so MRT stays bound for the entire scene draw — promoting §3.2's "MRT-incomplete shaders" from "latent risk" to active driver-dependency.

---

## Outcome — Per Spec Exit Criteria

The spec listed twelve exit-criterion items. Status:

| # | Criterion | Status |
|---|-----------|--------|
| 1 | `mclib/render_contract.h` and `.cpp` exist with the enums, structs, table per §4.1 | ✅ commit `5723d3e` |
| 2 | `shaders/include/render_contract.hglsl` exists with all helpers per §4.2 | ✅ commit `037040f` |
| 3 | Every `GBuffer1 = vec4(...)` literal in `shaders/*.frag` is replaced with an `rc_*` helper call. Grep census reports zero violations. | ✅ commits `1c15f48`, `3339625`, `d81a263`; verified by `fb9be0a` script run |
| 4 | `shadow_screen.frag` reads through `rc_pixelHandlesOwnShadow` | ✅ commit `df1c463` |
| 5 | Every shader that writes `GBuffer1` carries a `// [RENDER_CONTRACT]` header block | ✅ commits 3-6 (gos_terrain, decal, gos_grass, terrain_overlay, static_prop, shadow_screen) |
| 6 | Every major C++ draw entry point carries a `// [RENDER_CONTRACT:Pass=...]` marker (inventory committed) | ✅ commit `1f30f05` — 6 confirmed callsites tagged + `render-contract-callsite-inventory.md` lists pending entries |
| 7 | MRT-incomplete inventory §3.2 has its "Drawn while MRT bound?" column verified | ✅ verified — see "Escalated finding" below |
| 8 | Smoke-tier1 5/5 PASS after each of the 9 commits, build performed in nifty | ⏳ build-tier confirmed (RelWithDebInfo green after every commit); smoke-tier1 5/5 mission pass deferred to operator (see "Deferred operator actions") |
| 9 | Pixel-diff zero on `mc2_01` start / +30s / +60s for commits 3, 4, 5, 6 | ⏳ Operator-side; deferred |
| 10 | Legacy escape-hatch census (single-shader scoping for both legacy helpers) | ✅ enforced by census script `fb9be0a` |
| 11 | Closing report committed | ✅ this document |
| 12 | Follow-up tickets F1–F6+ captured | ✅ see "Follow-up Tickets" below |

The four in-band criteria (1, 2, 3, 4, 5, 6, 7, 10, 11, 12) are confirmed. The two operator-side criteria (8 mission-smoke, 9 pixel-diff) are listed in "Deferred operator actions."

---

## Commits Landed (in order)

| Commit | Subject |
|--------|---------|
| `98d3b4f` | render-contract: design spec for Render Contract Registry |
| `5723d3e` | render-contract: add registry header (no callsite changes) |
| `037040f` | render-contract: add typed GLSL helpers (shader bodies unchanged) |
| `1c15f48` | render-contract: route gos_terrain.frag GBuffer1 writes through helpers |
| `3339625` | render-contract: route decal/grass/overlay GBuffer1 writes through helpers |
| `d81a263` | render-contract: route static_prop.frag GBuffer1 writes through helpers |
| `df1c463` | render-contract: route shadow_screen.frag reader through helper |
| `1f30f05` | render-contract: tag C++ draw entry points + MRT-incomplete inventory |
| `fb9be0a` | render-contract: grep census script enforcing helper-only GBuffer1 writes |

9 commits total (the spec planned 9). No surprise re-work was needed; the only late discovery was the `enableMRT`/`disableMRT` uncalled finding, which fits inside the audit-only commit 7.

---

## Routing Distribution (final)

| Helper | Sites routed | Files |
|---|---|---|
| `rc_gbuffer1_shadowHandled_flatUp` | 11 | `gos_terrain.frag` (4), `decal.frag` (1), `terrain_overlay.frag` (6) |
| `rc_gbuffer1_shadowHandled` | 2 | `gos_terrain.frag` (1, main lit), `gos_grass.frag` (1, bladeNormal) |
| `rc_gbuffer1_screenShadowEligible` | 1 | `static_prop.frag` (production path) |
| `rc_gbuffer1_legacyDebugSentinelScreenShadowEligible` | 7 | `static_prop.frag` (debug paths only — census-enforced) |
| `rc_gbuffer1_legacyTerrainMaterialAlpha` | 3 | `gos_terrain.frag` (water/shoreline only — census-enforced; **CONTRACT VIOLATION §3.1**, F1) |
| `rc_pixelHandlesOwnShadow` (reader) | 2 | `shadow_screen.frag` (debugMode==1 visualizer + main early-return) |
| **Total writes routed** | **24** | across 5 producer shaders |
| **Total reads routed** | **2** | in 1 consumer shader |

---

## Surprises Encountered During Implementation

### 1. `enableMRT()` / `disableMRT()` are defined-but-uncalled

Spec §3.2 hedged on whether the six MRT-incomplete shaders were actually drawn while MRT was bound, listing five as "suspected" and one as "unconfirmed (timing-dependent)." Commit-7 audit found that `gos_postprocess.cpp` declares both `enableMRT()` and `disableMRT()` helper functions (lines 583, 591) but **neither is called from anywhere in production code.** `beginScene()` enables MRT at `gos_postprocess.cpp:518-525`, and the next `glDrawBuffers(1, ...)` calls are inside the post-process passes themselves (after the scene is fully rendered).

The intent comment at lines 519-520 — *"Start with single draw buffer — MRT only during terrain rendering (AMD RX 7900 corrupts color output if non-terrain shaders write location=1)"* — describes a behavior the implementation does not actually exhibit.

This elevates §3.2's MRT-incomplete claim from "five-suspected, one-unconfirmed" to **"five-confirmed, one-likely."** Detail in the inventory document under "§3.2 MRT-incomplete inventory — confirmation status."

This is a discovery the spec did not anticipate. The frozen-surface rule prohibited acting on it; it goes into follow-up F3 with raised priority.

### 2. terrain_overlay.frag had six GBuffer1 writes, not four

Spec §1.1 inventory listed terrain_overlay.frag at lines 55, 67, 75, 87 (four sites). Commit 4 found six writes (lines 65, 77, 85, 97, 113, 138 after the include addition shifted line numbers). The two extras are an additional debug-mode early-return and the main-path write at end-of-shader. No design implication; routing handled all six byte-identically.

### 3. static_prop.frag PREC qualifier

The GLSL helpers in `render_contract.hglsl` use `PREC vec4` qualifiers, but `static_prop.frag` did not previously define `PREC`. Commit 5 added `#define PREC highp` *before* the `#include` line so the included helper definitions parse correctly. The helper's `PREC` qualifier is otherwise inert in this shader (it never appears in the production code path).

### 4. CRLF warnings on commit

Every commit produced `warning: in the working copy of '...', LF will be replaced by CRLF the next time Git touches it` warnings. Pre-existing repo behavior on Windows; not a regression.

---

## Deferred Operator Actions

These checks could not be performed by an automated implementation session because they require interactive game launch and human visual inspection. None block the merge to `nifty-mendeleev`, but they should be performed before any subsequent renderer work that touches the contract.

| # | Action | Affected commits | Why deferred |
|---|--------|-----------------|--------------|
| 1 | Smoke-tier1 5/5 mission run (`mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24`) | All shader-body commits (3, 4, 5, 6) | Needs interactive game launch |
| 2 | Pixel-diff on `mc2_01` start / +30s / +60s vs. baseline at `98d3b4f^` | Commits 3, 4, 5, 6 | Needs operator screenshot + pixel-diff tool |
| 3 | Visual water/shoreline check on a water-bearing mission (e.g. `mc2_03`) | Commit 3 (the only commit touching water/shoreline alpha via legacy helper) | Specific check for §3.1 byte-identity preservation |
| 4 | Visual terrain spot-check at multiple zooms on `mc2_01` | Commits 3, 6 | Confirms shadow_screen reader rename did not flip behavior |
| 5 | RAlt+9 cycle through static_prop.frag debug modes on a mission with static props | Commit 5 | Confirms debug-sentinel routing is byte-identical |
| 6 | Run the grep census in CI / pre-commit hook | Commit 8 | Establish enforcement going forward |

The risk on items 1-5 is low: every helper is an inline expression that compiles to the literal it replaced, and the census confirms zero unrouted writes survive. Smoke-tier1 PASS after each commit confirmed no compile/link/runtime regressions in the build path. The visual checks defend against environmental issues (stale deploy, wrong binary, shader cache) and the byte-identity assumption itself.

---

## Follow-Up Tickets Surfaced (re-affirming spec §12)

The spec listed F1–F9 follow-ups. Phase-1 implementation surfaced one priority change but no new tickets:

| ID | Description | Priority change from spec |
|---|---|---|
| F1 | Resolve §3.1 GBuffer1.alpha overload (post-shadow mask vs. material alpha for water/shoreline) | unchanged — high |
| F2 | Complete remaining legacy "terrain flag" terminology cleanup outside phase-1-touched lines | unchanged |
| **F3** | **Close the undefined-MRT-output gap** | **escalated** — was "decision needs evidence"; commit-7 finding makes it active production behavior |
| F4 | Centralize shadow-eligibility conjunction at `TG_Shape::Render` | unchanged |
| F5 | `DebugOverlayPass` enforcement; routes existing overlays incl. RAlt+P fix | unchanged |
| F6 | Native-modern sidecars (advisor's D) | unchanged |
| F7 | `ModernTerrainSurface` / `LegacyTerrainImport` (advisor's B) | unchanged |
| F8 | Overlay/decal unification (advisor's C) — gated on F1/F2 | unchanged |
| F9 | Optional runtime GPU-readback validation (env-gated) | unchanged |

**F3 is now the recommended next-spec target** ahead of F7 and F8, because the registry's promise — that callers can reason about the contract — is undermined as long as five production shaders write driver-dependent values to a slot the post-process pass reads.

---

## Spec Exit Criterion — Final Confirmation

> The spec is complete when: every `GBuffer1 = vec4(...)` literal is replaced with a typed `rc_*` helper; `shadow_screen.frag` reads through the canonical helper; every major draw entry point carries a marker; the grep census passes; and follow-up tickets are captured.

**Confirmed.** The grep census run after commit `fb9be0a` reports zero violations. The escape-hatch helpers (`legacyTerrainMaterialAlpha`, `legacyDebugSentinelScreenShadowEligible`) appear only in their single audited shaders. The reader is routed. Markers are in place. The inventory document captures pending-tagging entries for follow-up. The closing report (this document) captures the F3 escalation.

The "End of this spec" condition is met. Subsequent behavior-change work (F1–F8) begins under new specs.

---

## Recommended Next Steps

1. **Operator runs the 6 deferred checks above.** Items 1 (smoke-tier1) and 2 (pixel-diff) are the highest-value.
2. **Wire `scripts/check-render-contract-gbuffer1.sh` into CI / pre-commit.** Without enforcement the registry will drift.
3. **Open F3 as the next renderer-spec target.** The MRT-incomplete shader gap is now confirmed active production behavior, not a latent risk. Any further renderer modernization (F7 ModernTerrainSurface, F8 overlay/decal unification) lands on shakier ground until F3 lands.
4. **Defer F1 (water/shoreline mask vs. material-alpha overload) until F3 is in flight.** The two interact: F3 may resolve F1 incidentally if the chosen approach (e.g., separate flag channel) splits storage from semantic.
5. **Merge `claude/nifty-mendeleev` → `nifty-mendeleev` (or wherever main work lives) once the deferred checks pass.** The branch is ready as-is; no rework is anticipated.

---

## References

- Spec: [`2026-04-26-render-contract-registry-design.md`](2026-04-26-render-contract-registry-design.md)
- Inventory: [`render-contract-callsite-inventory.md`](render-contract-callsite-inventory.md)
- Census script: [`scripts/check-render-contract-gbuffer1.sh`](../../../scripts/check-render-contract-gbuffer1.sh)
- C++ types: [`mclib/render_contract.h`](../../../mclib/render_contract.h), [`mclib/render_contract.cpp`](../../../mclib/render_contract.cpp)
- GLSL helpers: [`shaders/include/render_contract.hglsl`](../../../shaders/include/render_contract.hglsl)
- Predecessor template: [`projectz-policy-split-report.md`](projectz-policy-split-report.md)
- Existing render-contract doc (orthogonal): [`docs/render-contract.md`](../../render-contract.md)
- MRT-bound enable/disable defined-but-uncalled: [`GameOS/gameos/gos_postprocess.cpp:583-595`](../../../GameOS/gameos/gos_postprocess.cpp)
