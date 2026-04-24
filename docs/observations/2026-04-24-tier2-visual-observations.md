# Tier2 visual + content observations — 2026-04-24

Observations captured during a tier2 smoke matrix run (24 missions @ 60s).
**Not bugs to fix in this session** — todos to triage later.

Source: user observation while monitoring the live tier2 run.

---

## Performance / GPU

- [ ] **GPU util ~100% in first few missions, calms down by the moon map.** Investigate why early-campaign missions saturate the GPU but late-campaign / moon doesn't. Possible causes: per-mission asset count, terrain complexity, post-process load, or a one-time warmup cost being misattributed.
- [ ] **GPU util spikes again on the mission AFTER the moon mission.** Correlated with the textureless mission below — possibly the same root cause.

## Rendering bugs (real defects to fix)

- [ ] **Power generator animated blue mesh renders below the terrain.** Z-order or terrain elevation lookup wrong for that prop type. Likely affects all instances of this animated prop.
- [ ] **Trees have no shadows.** Static-prop shadow casting is missing or filtered out for tree shapes specifically.
- [ ] **Mission AFTER the moon mission is not applying textures.** Identify which mission stem this is from the run log; possibly an FST/loose-file resolve regression for that map's terrain set.
- [ ] **Mission with dark green + brownish rock has no applied overlay texture.** Identify the stem. Overlay (alpha cement / road) path failing for that terrain combination.
- [ ] **Verify minefields still render.** Check across tier2 — visual confirmation only, not in any automated test.
- [ ] **All vehicles are blue.** The mech-color-channel-classifier fix from the mech texture rework needs to be applied to vehicle shapes too. Currently every vehicle defaults to (or accidentally selects) the blue paint variant.
  - Cross-reference: `memory/mech_paint_and_mipmap_system.md` for the existing mech path.

## Color / tone mapping

- [ ] **Terrain too grey overall.** Black/night maps appear as grey rather than black. Need to map the dark end of the colormap to actual black, not midtone grey.
- [ ] **Implement per-mission tone mapping.** Desert maps all look "same-same" — they should differentiate via tone curve, not just colormap. A per-mission tonemap LUT or exposure preset is the minimum viable.
- [ ] **Liao escort/ambush mission too grey.** Same colormap class as the broader greyness issue, but worth a specific check on this stem.
- [ ] **Moon missions need color map + texture tuning.** Currently reads as generic grey rock; should distinguish lunar regolith, craters, etc.

## Texture / detail

- [ ] **Add texture variety on sand/desert missions.** Dune-like textures should be woven into the terrain blend so large flat sandy areas don't look repetitive.
- [ ] **Turn down grey noise in rock and grass textures.** Currently too sharp / too noisy. Possibly needs anisotropic filtering or trilinear LOD bias adjustment.
  - **Dirt looks fantastic** — leave dirt alone, it's the reference for what good looks like.
- [ ] **Snow + dynamic-shadow interaction looks like two completely different things.** Snow surface lighting and the dynamic shadow overlay aren't agreeing on color/intensity. Probably a shading-model mismatch in the snow material vs the screen-space shadow pass.

## Snow

- [ ] **Snow still needs tuning** (general — not specific failure).
- [ ] **Possible feature: snow maps actually snow.** Particle system overlay for snow biomes. Lower-priority feature, not a bug.

## Water

- [ ] **Water still looks stock.** No PBR / improved shading applied yet.
- [ ] **DOC TASK: ensure no doc claims water is "done" or "shipped."** Audit `docs/architecture.md`, the spec/plan files, and any memory entries that mention water rendering. The current state is "stock with overlay path; not improved."
  - Cross-reference: `memory/water_rendering_architecture.md`.

## Mission timing / content

- [ ] **Liao mission needs +20s in the smoke harness to play through the cutscene.** Add a per-mission `duration` override in `tests/smoke/smoke_missions.txt` for that stem, OR investigate whether the cutscene itself can be skipped in smoke mode (similar to MC2_MENU_CANARY_SKIP_INTRO pattern).

## ABL VM / scripting

- [ ] **`mc2_20` (MOGM escort, `data/missions/warriors/mc2_20_mogm_escort_v1.abl`) hits "Unimplemented feature" at line 273 and logs `STOPABL RUNTIME ERROR data/missions/warriors/mc2_20_mogm_escort_v1.abl [line 273] - (type 3) Unimplemented feature` on **every script tick**.** First tier2 baseline run produced a 135 MB per-mission log (~200k+ repetitions of that line in 60 seconds) and pushed the per-mission walltime from the expected ~65s (60s duration + spawn) to ~160s — exceeding the runner's 120s cap, though the read loop drained eventually.
  - **Root cause:** missing ABL extension function (likely `corewait` based on the symbol name, but verify against the actual unimplemented-feature trace). NOTE: a separate ABL-integration task is already in flight that has the **real implementations** for all 51 missing extensions (sourced from old MC2 documentation — these are not no-op stubs but actual functions to port). The smoke harness simply surfaced this gap on a mission that triggers the unimplemented path.
  - **Two harness-side bugs also surfaced:**
    1. ABL VM should rate-limit "Unimplemented feature" errors (or abort on first occurrence with a clear summary). Currently logs-and-continues, creating per-tick spam.
    2. Smoke runner's walltime cap can be defeated by extreme stdout pressure (queue.get inner loop runs faster than the cap check fires when millions of lines/sec arrive). Defensive fix: check walltime BEFORE every queue.get, not just after each line.
  - Decision for tier2 manifest: leave `mc2_20 tier2` as-is. The ABL-integration task will resolve the underlying gap; no manifest workaround needed.

---

## Triage suggestions for the maintainer

When converting these to issues, group as:

1. **"Vehicle paint regression"** — single ticket; reuses mech paint path.
2. **"Terrain colormap dynamic range"** — covers grey-too-grey, black mapping, per-mission tone.
3. **"Static-prop z-order + shadow gaps"** — power generator + tree shadows.
4. **"Texture variety pass"** — desert dunes, snow tuning, moon textures.
5. **"Asset / texture resolution regressions"** — textureless mission, missing overlay on green+rock.
6. **"GPU load investigation"** — early-mission saturation.
7. **"Smoke harness mission overrides"** — Liao cutscene duration; one-line manifest edit.
8. **"Water still stock"** — both the rendering ticket AND the doc-audit ticket.
9. **"Snow as particles"** — feature, not regression.
10. **"Verify minefield rendering"** — investigation, not necessarily a bug.

---

## Not in this list (already known, in CLAUDE.md or memory)

- 207ms peak frame on tier1 mc2_03 (GameLogic activation) — known, baseline-tracked.
- mc2_17 86 fps avg / 271ms peak — known, baseline-tracked.

---

## Process note

This file lives in `docs/observations/` rather than `docs/superpowers/plans/progress/` because:

- It's not progress on any single in-flight plan
- A live tier2-baseline subagent is concurrently writing to `progress/`
- Observations from manual monitoring belong in their own home; later they get split into individual tickets / plans

Future tier-N runs can drop similarly-dated observation files here.
