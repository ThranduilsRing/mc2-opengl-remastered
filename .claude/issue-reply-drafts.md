# GitHub issue reply DRAFTS — DO NOT POST WITHOUT REVIEW

Branch `claude/github-issue-fixes-1` (worktree A:/Games/mc2-issue-fixes, based on
nifty 994c3776). All "fixed in this batch" replies refer to commits on that branch;
they ship in the next public release after the branch merges to nifty.

---

## #42 — Can't salvage downed mechs (v0.5.0 regression) + AAR salvage list

Thanks for the report — this was two separate things, and the big one is confirmed
and fixed.

**(b) Cannot salvage / no info cursor on downed mechs (v0.5.0 only):** confirmed
regression. A crash-protection guard added shortly before v0.5.0 dropped EVERY
disabled target from hover processing — but "disabled, not destroyed" is exactly
the salvageable downed-mech state, so the info cursor, the info window, and the
salvage craft/order became unreachable. The guard is now narrowed to genuinely
torn-down objects only (commit `45747525`); v0.4c behavior for downed mechs is
restored while keeping the crash protection. This ships in the next release.

**(a) Salvaged mech missing from the after-action/logistics list:** we could not
reproduce this from code inspection alone — every static condition on the
carry-over path checks out. We've added a permanent headless repro fixture
(`MC2_SOAK_SALVAGE_TRADEMOVER`) that drives the REAL mid-mission salvage-craft
flow through mission completion, plus an existing diagnostic trail
(`MC2_LOG_LOGISTICS_FLOW=1`). Note that on v0.5.0 bug (b) masks (a) — you cannot
mid-mission salvage at all — so once the next release is out, please retest the
AAR half there; if a salvaged mech still vanishes from logistics, a run with
`MC2_LOG_LOGISTICS_FLOW=1` set (launcher env box) will pin the exact failing
condition.

---

## #50 — Reinforcement mech (Claymore's Atlas, mc2_08) never appears

Found it — and it wasn't missing assets (we verified the v0.5.0 payload is
complete: atlas.ini/atlas.tgl/atlas.csv/object2.pak/mc2_08.fit all intact). It's
a "link-up latch":

The Atlas spawns shut down and hidden, waiting for your lance to walk into line
of sight ("link up"). But "select all visible" / drag-select could select the
hidden mech anyway (a visibility filter had been commented out), and ANY order
auto-powers a shut-down mech. Once powered, the link-up check — which required
the mech to still be shut down — could never fire again: the mech stayed
invisible, off the pilots tab, off the tacmap, permanently, while still shooting
and playing voicelines. That matches every symptom you describe (E-select grabs
5 mechs, phantom gunfire, no 4th pilot).

Two fixes ship in the next release (commit `f659d48f`):
1. Hidden link-up mechs can no longer be selected before they appear.
2. The link-up check now also fires for an already-powered hidden mech, so even
   a latched save self-heals as soon as your team has line of sight to it.

If you can still reproduce on the next release, set `MC2_LINKUP_TRACE=1` and
send us the console log — it prints the hidden mech's link-up state once a
second and a `linkup-complete` marker when it appears.

---

## #43 — After-action report: destroyed vehicles show as gray blocks

Partially reproduced; three separate defects were live in the v0.4c build you
ran, and all are now fixed:

1. The vehicle kill-icon blit had no bounds checking in v0.4c — with any mod
   content (MCO/MC2X) whose icon atlas differs from stock dimensions it read
   heap garbage (fixed 2026-06-20/30).
2. The whole after-action report was squashed to ~45% height in v0.4c
   (RESULTS-SQUISH-1, fixed in the July AAR overhaul, shipped v0.5.0).
3. Fixed in this batch (commit `16b8011d`): column-EXTENDED icon atlases (MCO
   ships mcui_high8.tga 1080px wide vs the stock 1000px — extra icon columns)
   were treated as if upscaled, shifting every vehicle icon's sample offset and
   cross-contaminating cells. Destroyed vehicles palette-remap to the gray
   damage row, so a mis-sampled cell renders as a literal gray block — the
   symptom you screenshot. Extended atlases are now direct-indexed.

Could you tell us: your display resolution, and whether MCO/MC2X or other mod
content was installed? For a bone-stock install the blit math is provably
identity, so your answer picks between mechanisms (2) and (3). Either way,
please retest on the next release — if you still see gray blocks, run with
`MC2_LOG_MECHICON=1` and `MC2_ASSET_SCALE_TRACE=1` (no debug build needed) and
attach the console output.

---

## #34 — Near ground clips at an angle / unevenly when tessellation unavailable

Root cause confirmed: with the tessellation material missing, terrain and some
object paths fell back to CPU pipelines that "clip" by dropping WHOLE triangles
when any vertex crosses the near plane, while GPU-drawn geometry clips exactly
AT the plane — so at zoomed-in altitudes some things vanished early and others
clipped cleanly ("some things clip, others don't").

Fixed in two parts (commit `b346a0a5`, ships next release):
1. `MC2_TERRAIN_TESS=0` kill-switch — the no-tess configuration is now testable
   (previously it was only reachable via a shader compile failure).
2. Shadow behavior decoupled from the tessellation material: with tess off,
   CSM/static shadow maps now stay ON and the legacy CPU blob shadows (the
   worst whole-triangle-drop offender) stay OFF, so near-plane behavior is
   uniform.

Also relevant: since 2026-06-09 the terrain itself renders through a chunked
LOD path that has no tessellation dependency at all, which removed the main
terrain unevenness you reported.

---

## #49 — Window resolution locked to 800x600 (fixed on nifty; ships next release)

This is fixed in the development branch — the fix chain (WINDOWED-8006-1 +
FREE-RESIZE-1) landed after v0.5.0 was cut, so no released build has it yet.
It ships in the next release.

What was wrong: an internal HUD resolution clamp physically resized the OS
window to 800x600 on every screen-mode request (boot and every options-screen
apply), and the window was created without a resizable border so you couldn't
drag it back.

Supported flow in the next release: in the launcher's Display section check
"Windowed mode", optionally set Width/Height (blank = your options.cfg values),
then drag-resize freely — HUD, mouse and camera all adapt live.
(`MC2_WINDOW_RESIZABLE=0` opts back into a fixed border.)

This batch also closes the last gap (commit `ab181cd8`): booting windowed via
options.cfg `FullScreen=FALSE` alone (without the launcher checkbox) now also
gets a resizable border.

---

## #45 — Aspect-ratio distortion on widescreen (fixed on nifty; ships next release)

Fixed in the development branch, after your v0.4c build was cut:

- Since 2026-07-05 the in-mission camera builds its perspective projection from
  the actual window aspect (default ON; `MC2_CAMERA_ASPECT_NATIVE=0` restores
  the legacy stretched 4:3 look). Field-tested at 25:9.
- The literal option you proposed also exists: a "Force 4:3 (pillarbox)"
  launcher toggle (`MC2_FORCE_43`), default OFF, since 2026-06-20.

Both ship in the next public release. Please retest there and reopen if any
distortion remains at your resolution.

---

## #8 — Purchased pilots/mechs missing after mission (fixed on nifty; ships next release)

The root cause was found and fixed: stock mission data bakes backslashed paths
(`data\missions\PurBonus08.fit`) into save games; the archive (FST) lookup
hashes paths slash-sensitively, so the purchase file silently failed to load
and the pilots/weapons it unlocks never appeared. Since 2026-05-01 the file
layer normalizes separators (commit `fb4d2e52`), and the archive contents were
verified clean (all purbonus entries hash-match). Release builds published
before that date (v0.3 era) still have the bug — update to the next release.

This batch also adds evidence for the future (commit `1279d0de`): the
previously-silent failure branch now logs
`[LOGISTICS] appendAvailability: open FAILED '<path>'` under
`MC2_LOG_LOGISTICS=1`.

If you still have the save from the report (`after_mission_mc2_09.zip`), we'd
love a retest on the next release: load it, open the purchase screen, and check
that the Claymore pilot is purchasable.

---

## #35 — Hard map edge / void visible (fixed on nifty; ships v0.5.0+)

This was fixed by the edge-fog family (sea-of-clouds out-of-bounds fog +
edge-fog post pass + water-boundary handling), which landed 2026-06-16/17 and
first shipped in v0.5.0 — your report was against a pre-fix v0.3/v0.4 build.
Both passes are default-ON in current builds. Please retest on v0.5.0 or later;
if a residual seam shows on your maps, the fog is tunable
(`MC2_EDGE_FOG_START/HEIGHT/MAX`, `MC2_OOB_FOG_COLOR`) and we'd take a
screenshot + map name to tune the defaults.

---

## #24 — Mech color swatches wrong (R/B swapped) (fixed on nifty; ships v0.4-beta+)

The R/B channel swap on the untextured shader path was fixed 2026-05-16
(commit `509fa8e8`, "fix(shaders): R/B swap on untextured path (#24)") and is
in v0.4-beta and every later tag — builds older than that still show swapped
swatch colors. Please update and retest; the 3D-preview feature request in the
title is tracked separately.

---

## #9 — Called shots feel like they do nothing

You were right, and we can now show exactly why. The whole called-shot
pipeline is intact (your salvage observation — head-dominant damage on
salvaged mechs — was the aim statistically working), but three stock-tuning
mechanics make it invisible per shot:

1. The stock modifiers (Head ×0.08 / Arm ×0.15 / Leg ×0.20) crush the final
   chance so hard that in our instrumented runs **every single aimed head
   shot clamped at the engine's 5% floor** (152/152 shots logged).
2. ~95% of aimed shots therefore miss — and a missed shot either lands
   visibly beside the target or, when the miss-position probes fail,
   *visually connects to the target for zero damage*. So it looks like the
   order is ignored.
3. LRMs never honor the aim at all: area-effect hits replace located damage
   with an explosion whose damage is randomly located per chunk (the code
   comment literally says "Hit or MISS, its the same!"). That's the
   limbs-flying-off-when-you-called-head experience.

Fix ships in the next release behind an opt-in toggle, `MC2_CALLED_SHOT_FIX=1`
(launcher env box), commit `fdbae140`:
- Aimed-shot modifiers floored at Head 0.30 / Arm 0.45 / Leg 0.50 (a mod's
  data value still wins if higher; Sharpshooter +20 unchanged). Measured
  effect: aimed head shots go from a flat 5% to 13-18% on mid-skill AI
  pilots (higher with good pilots/short range — the 5-95 engine clamp still
  applies).
- A successful aimed LRM hit now concentrates its damage at the called
  location (the splash explosion still plays, but cosmetically).

It's opt-in for now because it's a gameplay-balance change; if the tuning
feels right to you we'll consider making it the default — feedback welcome.

---

## #17 — Mechs reluctant to fire (poptarting / flat ground)

Two separate things here:

**Jumping (poptart) reluctance — already fixed.** During the jump gesture the
line-of-sight ray origin could go stale *below the terrain*, which failed LOS
in the mech's own map cell and silently folded every weapon into
"not locked" — nothing fires. Fixed in commit `e7be6175` (2026-05-01), which
landed just hours after your report; please retest on any current build.

**Your specific question — no, friendly units do NOT block line of fire.**
The LOS check only tests terrain elevation, building height, and forest
cells; movers are never occluders. Units firing through each other is stock
behavior, so the reluctance isn't your mechs politely holding fire.

**Flat-ground reluctance — instrumented, needs your trace.** The fire
decision has six silent per-weapon gates (recycle, ammo, range, LOS+fire-arc,
hit-chance) and additional silent skips inside fireWeapon; none of them were
observable. The next release adds `MC2_FIRE_DECISION_TRACE=1` (with
`MC2_DIAG_TAGS=COMBAT`): every combat update where a unit holds an attack
order but fires nothing emits a JSONL event naming exactly which gate ate
each weapon (in our test runs it cleanly separated weapon-recycle vs range vs
LOS refusals). If you still see reluctance on a current build, one run with
that trace pins the culprit and the real fix becomes targeted.

---

## #10 — Scripted enemies never move / groups stall (Op2M7 east base, Op2M8)

Status update, and two corrections to the thread's working theory:

1. We diffed the port's entire AI decision core against the original 2001
   source — calcTacOrder, movement/combat decision trees, coreMoveTo/Attack,
   the goal-plan tail — and it is functionally byte-identical. The stall is
   NOT in the decision logic the thread was pointing at.
2. The "restore pursue chase broken by 2017 conversion" premise behind one
   earlier fix is actually false — the RETAIL source also ships those two
   chase calls commented out; retail chase is driven per-frame by the
   movement decision tree, which is intact. So "uncomment the second site"
   is not a valid fix, and we won't be applying it.

That leaves three suspect classes, all in the pathfinding/order substrate:
regional no-path (an area-graph misread — a proven port bug class), an
escape-tile alarm loop (a unit stuck re-issuing a priority alarm order
forever, during which its scripted orders are never consumed), and the
goal-plan's capture-divert (enemies within 150 m of a player-captured turret
divert to attack/capture it and may never complete that sub-order — which
matches your captured-turret observation and is, notably, retail-stock
logic).

The next release extends the `MC2_BRAIN_FREEZE_TRACE=1` diagnostic so every
brain tick reports order state, goal target, scripted-order code, position,
and the last pathfinding error — one 30-second run classifies every stuck
unit into the classes above (commit `ecda2201`). If you can still reproduce
the Op2M7/Op2M8 stalls, a log with that flag set (launcher env box, stderr
capture) tells us which class each stuck lance is in, and the targeted fix
follows from there.

---

## #32 — Data files: patches applied?

Good question, and it deserved a real answer — there's now a full write-up in
the repo at `docs/data-provenance.md`. Short version:

**Where the data comes from:** it is built from
[alariq/mc2srcdata](https://github.com/alariq/mc2srcdata) (the Linux port's
curated asset tree, published 2017) using its `build_scripts/` and the data
tools built alongside the engine (`makefst`, `makersp`, `pak`, …). The `.fst`
archives in a release are produced by that pipeline; nothing is lifted from a
patched retail install, and there is no patch-application step anywhere in
the build.

**Is mc2_patch.exe content included? No — and mc2_patch.exe turns out not to
be an official patch at all.** It's a fan-made rebalance by the modder Magic
(MC2X); Microsoft never shipped an official content patch. We verified the
shipped data against the patch's known content markers: weapon ranges are
stock `60/140/200` (the patch sets 80/160/240+), the called-shot modifiers
are stock `0.08/0.15/0.20`, the pilot roster is complete (the patch removes
about half of it, including Hacksaw), and `corebrain.abx` is alariq's stock
import plus his documented engine-compat fix. All stock.

**Where you may have seen Magic's files:** his rebalance IS in the repo — as
clearly-labelled *opt-in mods* (`mods/magic-gamesys-tweaks`,
`mods/magic-ballistic-weapons`) under the mod pathway. They're never applied
to the stock data and aren't part of the release zips. If your v0.3 install
showed patch-flavored files outside `mods/`, tell us which files and we'll
trace them.

**What this project changed vs stock:** additive only — new modern-engine
data files (`defs/`, `materials/`, etc.), optional 4× upscale overlays, and
texture caches. The doc also honestly lists what we did NOT verify (no
byte-level diff of all ~936 patch-touched files, and alariq's original
import source — retail CD vs the 2006 free release — is undocumented
upstream).
