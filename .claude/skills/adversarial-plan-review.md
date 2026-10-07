---
name: adversarial-plan-review
description: Code-grounded adversarial review of plans, specs, and designs — actively tries to break them by grep'ing every cited symbol against actual code. Use BEFORE handing a plan to an executor session for any architectural-endpoint slice, slice that retires legacy contracts, or slice that touches SSBO schemas / load-bearing infrastructure. Catches what prose-only review (3 rounds passed indirect-terrain plan v1 with fictional struct fields) misses.
---

# Adversarial Plan Review

Reviews a plan/spec/design doc with explicit instructions to BREAK it.

## Execution mode

**Default: run inline** in the current session (ONE lane, no fan-out). Only spawn a subagent or
Haiku grep workers if the user explicitly asks. Keep grep output terse to protect context.

The mechanical steps that could be offloaded on request:

| Step | Covers |
|---|---|
| Symbol check (Step 2) | grep every cited struct, function, signature, env flag, member field against source |
| Relationship check (Step 3) | grep both sides of every "interacts with / retires" claim |
| Census (Step 9) | exhaustive grep for all writers of any cross-cutting primitive the plan changes |

Classify hits as "matches claim / divergent / not found."

## Relationship to mc2-render-spine-advisor

For render-spine plans, run `mc2-render-spine-advisor` first. If the advisor returns structural
blockers (wrong cardinality, wrong lifetime, wrong authority, wrong seam), do not run this skill
yet — revise the plan first. Adversarial review is for code-grounding a structurally plausible
plan. Grep-verifying a plan with wrong denominator semantics wastes a full review pass.

If source access is unavailable (no repo access, plan text only), do not run this skill.
Return: **BLOCKED: source unavailable.** Optionally run `mc2-render-spine-advisor` structural
review instead, which does not require source access for most checks.

## Stance

Adversarial-plan-review is a hostile source-grounded prosecution of a plan. It assumes the plan
author is overconfident and that at least one claim is stale, fictional, or incomplete. The
reviewer's job is to embarrass the plan with receipts before the executor session wastes time on it.

This skill does not primarily ask "is there a better architecture?" That is greybeard / advisor
territory. This skill asks:
- Does the cited symbol exist?
- Does the cited field exist in that struct?
- Does the cited signature match what is actually in source?
- Do all writers/readers of the changed primitive appear in the plan?
- Does the plan actually retire the thing it claims to retire, or does the old path stay alive?

Use adversarial-plan-review when deciding *whether the prose matches the code*.

Distinct from `code-review` (which reviews code diffs) — this skill reviews PROSE against actual
code, looking for stale claims, fictional struct fields, wrong function signatures, missing
constraints, and load-bearing facts the plan glosses over.

## When to use

**Mandatory** for:
- Architectural-endpoint slices (the LAST slice in an arc — no follow-up slice to catch latent bugs)
- Slices that retire legacy contracts (e.g., killing `addTriangle`/`addVertices` for terrain, removing a fallback path)
- Slices that touch SSBO schemas, struct layouts, or shader binding slots
- Slices that claim "X interacts with Y" or "X retires Y" for shipped infrastructure
- Plans for any slice where 3+ rounds of normal review have already passed (the gap normal review misses)

**Recommended** for:
- Any plan whose perf gate target is ≥30%
- Any plan that adds a function signature change to a load-bearing API
- Any plan that gates legacy code off behind an env flag

**Optional** for:
- Mechanical follow-up slices (e.g., "delete dead code after soak")
- Slices that mirror a shipped pattern with predetermined stages
- Single-population slices with clear scope and one parity gate

## What this skill does (process)

1. **Read the plan with adversarial intent.** Goal is to find things wrong, not validate the plan. Pretend you've been told "this plan has at least 3 critical bugs in it — find them."

2. **For every cited symbol, grep it.** Every struct, function, file:line, signature, env flag name, member field referenced in the plan must be confirmed against source. List each grep hit and confirm "matches claim / divergent / not found." Cite the actual file:line in your findings.

   For large plans, prioritize load-bearing symbols first; non-load-bearing names may be sampled:
   - structs / fields being read or written
   - function signatures being called or changed
   - env vars / gates
   - shader binding slots / layout locations
   - GL state writers
   - lifecycle hooks (init, destroy, beginMission, endMission)
   - legacy paths being retired

   Examples of failure modes this catches:
   - Plan describes `TerrainQuadRecipe` with field `terrainHandle` — grep shows actual struct has 9 vec4s, no such field. **CRITICAL.**
   - Plan adds `(int blockX, int blockY)` arguments to `invalidateTerrainFaceCache` — grep shows signature is `void invalidateTerrainFaceCache(void)` with 4 callers. **CRITICAL.**
   - Plan says "bind SSBO at slot 1" — grep shows shader reads at slot 2. **MAJOR.**

3. **For every "interacts with X / retires Y" claim, grep both sides.** Confirm both ends of the relationship exist as described.

   Examples:
   - Plan says "retire `quadSetupTextures` per-frame iteration" — but the plan doesn't gate the loop body. Grep `quadSetupTextures` and confirm no early-return is added. **CRITICAL.**
   - Plan says "reuse thin VS path" — grep thin VS, confirm it can handle the new draw shape (e.g., does it have `gl_DrawID`? does its SSBO binding match?). **MAJOR if no.**

4. **For every "we'll add Y to X" claim, read X's current state.** Confirm the addition is mechanically feasible. Read all call sites of X. Confirm none break.

5. **Perf claims must be traced.** If plan claims Gate B (≥X% delta), confirm the cited mechanism actually retires the cited cost. If plan claims "retire CPU iteration" but the iteration still runs (because gate-off is missing or partial), the perf claim is false on its face.

6. **Load-bearing constraints checklist.** Cross-reference the plan against load-bearing memory files for the affected subsystem. Each must be addressed:
   - `cull_gates_are_load_bearing.md` (if touching object/static prop infrastructure)
   - `sampler_state_inheritance_in_fast_paths.md`
   - `gpu_direct_depth_state_inheritance.md`
   - `render_order_post_renderlists_hook.md`
   - `quadlist_is_camera_windowed.md`
   - `gpu_direct_renderer_bringup_checklist.md` (the 9 traps)
   - `mc2_argb_packing.md`
   - `terrain_mvp_gl_false.md` / `clip_w_sign_trap.md`
   - `mc2_texture_handle_is_live.md`
   - `feedback_offload_scope_stock_only.md` (validation scope)
   - AMD driver rules (`docs/amd-driver-rules.md`)

   For each: did the plan address it? If not, finding.

7. **Per-mission lifecycle checks.** Does the plan have hooks for: mission load, mission teardown, mid-mission state mutation? Most CPU→GPU offload plans miss per-mission teardown because `init`/`destroy` are wired to gosRenderer process-lifetime, not per-mission. WaterStream's actual pattern is "keep buffer, CPU-clear state per mission" — confirm the plan has this.

8. **Partial-landing hazard.** Can any single stage's commit pass visual + smoke gates while completely missing the stage's stated goal? (E.g., shipping the new draw without the gate-off shipping in the same PR.) If yes, finding — plan needs an explicit "do not land Stage X without Stage Y in the same PR" rule.

9. **Global-convention exhaustive census (MANDATORY for cross-cutting state changes).** If the plan changes a *global rendering convention or cross-cutting primitive* — depth function/direction, depth-clear value, blend mode, cull winding, clip control, a shared uniform/SSBO field semantic, a sign convention — the reviewer MUST produce a **fresh, code-derived, exhaustive census of every writer of that primitive**, generated independently from the plan's site list (e.g. `grep -rn 'glDepthFunc(' GameOS mclib` across ALL files, then classify each scene vs shadow vs neutral), and reconcile plan-floor-vs-census. Rules:
   - The plan's enumerated site list is an INPUT to verify and EXCEED, never the trusted scope. "Verify cited symbols" (step 2) is necessary but NOT sufficient here — the failure mode is the site the plan *didn't* enumerate.
   - A symbol named only as a vague `~line` pointer ("mech/static-prop batchers ~1063/~2899") in a file the reviewer did not open is **NOT verified** — it counts as a finding until the file is opened and the exact site grep-confirmed and classified.
   - The census must cover *separate files / sibling draw paths*, not just the file where the plan put its concrete line numbers. Per-batcher / per-renderer paths that hardcode their own state (bypassing the central authority) are exactly where cross-cutting flips leak.
   - State the census result explicitly: "Census = N writers across F files; plan enumerated E; delta = {sites}; each classified {flip/leave/neutral} with file:line." A cross-cutting change with no exhaustive census in the review is an automatic MAJOR (the review did not actually bound the blast radius).
   - Probe/gate coverage check: confirm at least one gate actually exercises each affected draw class. If the probe suite is structurally blind to a class (e.g. object/mech/building depth ordering for a depth-convention flip), say so — a green probe run on an unexercised class is not evidence.

## Output format

Findings categorized:

- **CRITICAL** (compile-error or guaranteed-runtime-failure class): plan cannot land as written. Includes fictional symbols, wrong signatures, missing required gates.
- **MAJOR** (slice will fail its own gate ladder): plan can compile but won't achieve its stated goal. Includes missing constraints from load-bearing memory, partial-landing hazards, undefined multi-bucket state cascades.
- **MINOR** (will work but suboptimal or fragile): documentation gaps, missing counters, missing rollback story.

Each finding cites:
- The specific plan line / section being faulted
- The actual code reference (file:line + grep'd content) that contradicts it
- The recommendation (mechanical fix / architectural decision / surface-to-user)

End with: "**Architectural decisions that need user/advisor sign-off before revision pass:**" — list any items where the right answer requires judgment beyond mechanical correction.

```
## Grep coverage
N symbols checked
M call sites checked
K cross-cutting writers checked
Source unavailable: yes / no
```

## What counts as failure

A no-finding review is only acceptable if it includes the grep/census evidence: N symbols checked,
M call sites confirmed, K constraints verified — all matched. Without that evidence list, a clean
verdict is indistinguishable from a prose-only review that missed the grep step entirely.

If the reviewer found nothing after a genuine code-grounded pass, state explicitly:
"Grep'd N symbols, M call sites, K constraints — all match claims. No findings."
with the symbol/site/constraint list attached.

## Origin

Created 2026-04-30 after indirect-terrain-draw plan v1 stop-the-line. Three rounds of normal review missed:
- Fictional `TerrainQuadRecipe` field references (struct is 9 vec4s, plan invented `terrainHandle`/`uvMode`/`mineState` fields that live elsewhere)
- Wrong `invalidateTerrainFaceCache` signature (added args that don't exist in the function)
- Missing `quadSetupTextures` gate-off (plan said "retire iteration" but didn't gate the loop body)
- 5 additional MAJOR findings (multi-bucket trap, AMD attrib 0, no per-mission teardown, etc.)

Adversarial review with explicit code-verification grep'd the actual headers + call sites and surfaced all findings in one pass. Three rounds of "does the plan flow logically?" review can pass while the plan describes fictional code, because reviewers read the plan against the brainstorm + design, not against the source tree.

Step 9 added 2026-05-18 after the reverse-Z scene-depth conversion escaped FOUR code-grounded gates (C1 adversarial, spec, code-quality, final whole-impl adversarial) + passing probes, then catastrophically regressed on first USER visual (mechs/buildings sinking into terrain). Root cause: the depth-func flip missed two SCENE draw paths (`gos_mech_batcher.cpp:1063`, `gos_static_prop_batcher.cpp:2934`) that hardcode their own `glDepthFunc` in separate files; the plan named them only as a vague `~1063/~2899` pointer; every gate verified against the one file (`gameos_graphics.cpp`) where the plan put concrete line numbers; no gate did an independent code-derived exhaustive census; the probe suite was structurally blind to object-vs-terrain depth ordering. The skill's *letter* (grep cited symbols) was met while its *intent* failed.

Reference: `memory/brainstorm_code_grounding_lesson.md`.

## Tradeoff

This skill is expensive — a thorough run is 1-2 hours of grep + cross-reference work for an architectural-endpoint plan. That's the cost. The benefit is catching CRITICAL findings BEFORE executor session runs, where the same finding costs days to debug + revert.

The "right balance" rule: use this skill for high-stakes plans (the trigger list above). Don't use it for mechanical follow-ups or slices that mirror a shipped pattern. Lower-stakes plans get prose-only review — that's fine for them.

For brainstorms specifically: a lightweight version of this skill (just step 2, grep cited symbols at answer-time) should be applied INSIDE the brainstorm session before plan-writing begins. That converts ~80% of CRITICAL findings into "caught at brainstorm" rather than "caught at adversarial review of plan." See `brainstorm_code_grounding_lesson.md` for the lightweight verification appendix format.
