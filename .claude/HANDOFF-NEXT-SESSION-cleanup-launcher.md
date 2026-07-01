# HANDOFF — next session (cleanup + launcher + mod proof). CAVEMAN.

**Where:** UNIT + EQUIPMENT data-driven arc. Two-arc strangler foundation DONE + source-flip switches DONE + generated-source shadow run GREEN (tier1 5/5 Δ+0, both gates active). Example checkbox mods in progress. Branch `claude/unit-profile-seam` (LOCAL-ONLY, not pushed). Full detail: `memory/HANDOFF_2026_06_29_unit_profile_seam.md` + roadmap `docs/superpowers/plans/2026-07-01-unit-profile-arc-roadmap.md` + recons in `.claude/*RECON*.md`.

**Work style:** CAVEMAN mode. SUBAGENT-HEAVY (dispatch per slice, terse report back, keep main context lean). Follow STANDARD SLICE-DISPATCH CONTRACT (roadmap): worktree `.claude/worktrees/unit-profile-seam`, verify branch via git (preflight_fast hardwired to nifty = false-negative), grep-anchored edits, gate default-OFF, offline doctest RED→GREEN first, per-slice verify = mc2_01 + deploy v0.3, tier1 at milestones, STOCK-ONLY for conversion. Redlines: no firing rewrite, no widen till >254 live rows, no generated-default before shadow run, no hardpoint enforce (mech bay).

## CLEANUP (throwaway proof scaffolding to strip once real path proven)
- Throwaway proof gates + hooks + their `[…-PROOF]` logs + run_smoke allowlist entries: `MC2_UNIT_PROFILE_VEHICLE_JUMP_PROOF`, `MC2_UNIT_PROFILE_NONMECH_PROOF` (+ `mc2_unitprofile_test_grant_vehicle` if any residue / `nonmechDataUnitMountProof`), `MC2_EQUIP_IDALIAS_PROOF`, `MC2_EQUIP_PRESENTATION_PROOF`. These were opt-in proofs; delete the hooks, keep the real gated paths.
- KEEP (make proper, not throwaway): `[UNIT-SOURCE=generated]`/`[EQUIP-SOURCE=generated]` source-flip logs, `[UnitProfile:VALIDATE]`/`[UnitDefinition:VALIDATE]` findings, fire-executor + registry dumps — but gate behind a diag flag, not spammy per-frame.
- Dead code: `WEAPON_AMMO_OFFSET` (`mclib/cmponent.h:50`, zero callers) — delete + drop the schema-comment refs.
- E3 EQUIPMENT-REF-TYPE deferred (typedef prep for E4 widen) — do WITH E4, not before.
- run_smoke.py grew several env-allowlist passthroughs for proofs — prune for removed proofs.
- Branch reconcile: docs history partly on `claude/animated-prop-cook-recon-1`, code on `claude/unit-profile-seam`; both local-only. Decide push/merge/PR (ask user). CRLF warnings on the roadmap md — harmless.

## LAUNCHER (the "these should be checkbox mods" work — user 2026-07-01)
Mod system recon: `.claude/MOD-SYSTEM-RECON-1.md`. Launcher `tools/mc2_launcher/mc2_launcher.cpp` already has scrollable add-on checkboxes (type:assets → `MC2_MOD_DEPS=<folder>`). TODO:
1. **Generated-source toggle in launcher** — a checkbox/option that sets `MC2_UNIT_DEFINITION_SOURCE=generated` + `MC2_EQUIPMENT_DEFINITION_SOURCE=generated` (+ `MC2_UNIT_PROFILE_DATA=1`) so users flip to generated-as-source from the UI, not env vars. Default OFF (legacy).
2. **Example mods enumerate as checkboxes** — confirm `examples/mods/*` (or wherever EXAMPLE-MODS-1 put them) install into `<deploy>/mods/` and show as add-on checkboxes; checking → `MC2_MOD_DEPS` reaches exe → generated defs overlay (recon §4: zero new plumbing).
3. **Deploy generated defs by default** — `deploy_payload.py` should ship `data/defs/{units,equipment}/` (converter `--batch` output) so generated-source works from a normal install (right now they're hand-copied to v0.3 during slice verify).
4. (stretch) surface modlint result / modder-first errors in launcher when a bad mod is checked.

## ★★ THE BIG GAP surfaced by EXAMPLE-MODS-1 (`c531c124`): OVERLAY-not-ADD
Source-flip = OVERLAY. It overlays EXISTING masterList rows / ALREADY-SPAWNED units by matching stock ids. A mod can **override** stock content LIVE, but a NEW `sample:`-namespaced def is **NOT auto-instantiated** in a mission by the current engine — proven only at the offline/modlint layer. Making "modder ADDS a brand-new weapon/unit → appears in-game" work =
- **ADD path**: registry becomes an authoritative SOURCE the engine instantiates from (new equipment rows appended to/served beside masterList; new units registered as spawnable types), not just an overlay onto stock ordinals.
- **enabled→purchase wire**: surface new/dormant content in the mechlab/purchase catalog (today gated by purchase/RP layer, no `enabled` consumer).
- **E4 widen** once new equipment pushes >254 live rows.
This is the real "override stock" → "add new content" line, and the headline moddability work after cleanup/launcher. Example mods (`examples/mods/`) prove the flow offline + prove OVERRIDE; ADD-live is the follow-on.

## NEXT SLICES (roadmap "MODDABILITY PHASE" order)
EXAMPLE-MODS-1 (running) → MOD-PACKAGE-MANIFEST-1 (reuse existing mod.json) → MOD-LOAD-ORDER-CONFLICT-REPORT-1 (dup ids / core reserved / override visibility — never silent) → MOD-ROUNDTRIP-SMOKE-1 (modlint+preview+generated-load) → **MODDER-FIRST ERROR SURFACE** (thread which-mod/file/id/field/expected/fallback through modlint + source-flip loaders + conflict report) → MCO/MC2X-CONVERSION-RECON-1 (real extensibility test, AFTER sample mods) → INVENTORY-EQUIPMENTREF-WIDEN=E4 (ONLY when live content >254 rows; 0xff-sentinel + ammoMasterId + .mdf format hazards).

## DOGFOOD RECIPE (works now)
Converter `--batch` → `data/defs/{units,equipment}/`, then `MC2_UNIT_PROFILE_DATA=1 MC2_UNIT_DEFINITION_SOURCE=generated MC2_EQUIPMENT_DEFINITION_SOURCE=generated`. Unset = instant legacy. Waived facets (unit hardpoints/sockets/UI, equipment firing/presentation) stay legacy either way — cover those as authoritative before flipping the DEFAULT.
