# HANDOFF — MC2X importer (shipped) + Omnitech (scoped). 2026-06-14

## TL;DR
Built a manifest-driven importer that reproduces the **working** 0.4c `mc2x-compat` + `cveg`
mods from a user's **own** MC2X install (no content redistribution — recipe ships filenames,
bytes come from the user's install). 3 MC2X campaigns play; deterministic. Omnitech engine works
(Exodus plays) but Omnitech campaign packs (Wolf Dragoons) are blocked on object-type resolution.

## Where things are
- **Branch:** `claude/mc2x-importer-1b` (worktree `.claude/worktrees/mc2x-importer-1b`).
- **Tool:** `tools/mc2x_import/` — `mc2x_import.py`, `mc2x_recipe.json` (1.3MB, the recipe),
  `shims/` (12 compat files), `fst.py`, `manifest.py`, `.gitattributes`.
- **Deploy used for testing:** `A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1` (has a FRESH exe
  I built + deployed; also has test mods left in place — see "rc1 state" below).
- **Oracle (MC2X):** `A:/Games/mc2-opengl/mc2-win64-0.4c/mods/{mc2x-compat,cveg}` (the proven
  working manual build the recipe was derived from).
- **MC2X source install (user content):** `A:/Games/mc2-opengl/MC2X-CVE-G`.
- Memory: `mc2x_importer_manifest_driven.md` (+ INDEX-MISSION-DATA entry).

## What shipped (MC2X) — DONE
Run on any user machine:
```
py -3 tools/mc2x_import/mc2x_import.py --source <user's MC2X install> --deploy <game folder>
```
~2.5min. Reproduces 0.4c's mc2x-compat (12463 files) + cveg (1238) by extracting bytes from the
user's MC2X FSTs per `mc2x_recipe.json`, plus 12 bundled compat shims. **Deterministic** (13689
files sha256-identical across 2 runs). Campaign packs (DarkRain/POAR/TangoMaster, deps
`cveg`+`mc2x-compat`) are separate self-contained mods — copy them in alongside.

**Verified (smoke, fresh exe, `MC2_ACTIVE_MOD=<pack>`):** torrin (DarkRain), poar_01 (POAR),
clearwater (TangoMaster) all PASS on importer-built base.

### KEY LESSON (do not repeat)
The first ~4h went into a **delta-vs-base** importer (extract only files not already in base).
That was WRONG: it stripped 8382 files the working set needs (MC2X-modified `.abi` includes that
declare `noAttackCode` etc.; 0.4c hand-relocations `warriors/->profiles/`). The right design is
**reproduce the proven oracle exactly** (manifest from 0.4c, bytes from user install). Don't get
clever; match what works.

## What's next

### 1. Omnitech (MCO) — 6 CAMPAIGNS PLAY (2026-06-14)
Engine READY (40+11 ABL stubs in `code/ablmc2.cpp`); **MC2-Exodus PLAYS**; **6 MCO campaign STARTS
smoke-PASS** on the rc1 deploy with `MC2_ACTIVE_MOD=<folder>` + `MC2_MOD_DEPS=mco-compat`:
Wolf Dragoons `outreach` (+acamar/pirate2/styx), Volstand `zhukov`, Day of Heroes `doh_0`,
Clan Eagle `cfv2_mission1_escort`, MercStar `galatea`, Desert Fox `desfox1`.

**No per-campaign mod.json.** The player picks campaign + compatibility layer at LAUNCH; the launcher
exports `MC2_ACTIVE_MOD` + `MC2_MOD_DEPS`. ONE small engine change (`mclib/file.cpp`): IndexModData
now appends the comma/semicolon `MC2_MOD_DEPS` list (de-duped) to any mod.json deps, so metadata-less
third-party campaigns resolve their compat layer from the launcher selection. (Edit is on
`claude/mc2x-importer-1b` AND applied to the nifty `fx-force-spawn-fixture-v1` tree to build+deploy the
rc1 test exe.)

**LAUNCHER — DONE** (`tools/mc2_launcher/mc2_launcher.cpp`, rebuilt + deployed beside mc2.exe):
discovers any `mods/<folder>` with a `data/` subdir (no mod.json); compat layers (`-compat` name or
mod.json type=dependency) become CHECKBOXES, campaigns (have `data/missions`) fill the list. Per
campaign it auto-detects the compat layer (scan mission `.fit` + brains: ObjectNumber>1188 +
magicAttack → MCO; >1188 only → MC2X; else unknown → manual tick). Launch sets `MC2_ACTIVE_MOD` +
`MC2_MOD_DEPS=<checked>`; warns if a campaign launches with no compat ticked. Verified via
`mc2-launcher.exe --list`: cveg→mc2x, 6 MCO→mco, DarkRain/POAR/TangoMaster/keid-v→unknown (low-objtype
MC2X is indistinguishable from pure stock by objtype = the agreed checkbox fallback). Build with
cl.exe (`/SUBSYSTEM:WINDOWS user32 kernel32 gdi32`) or the `mc2_launcher` CMake target. Caveats: ANSI
build → ASCII-only display strings (em-dashes garble); size the window via `AdjustWindowRect`.

**Do NOT "extract everything diff from stock."** Tried it: mirroring all ~992MB of MCO `data/` that
differs from base REGRESSED every campaign (MCO `data/art/buildings.csv` → silent load crash; MCO
`data/missions` stock content → play freeze). The FOCUSED `mco-compat` below is what makes all 6 pass.

Three content fixes, all in the `mco-compat` dependency mod, produced by
**`tools/mco_import/build_mco_compat.py`** (reproducible, bytes from the user's own MCO install,
deterministic — object2.pak + 3 .abx verified byte-identical across runs):

1. **object2.pak rebuilt** (the "can't create object" blocker). objTypeNum = PACKET INDEX into
   object2.pak (`objtype.cpp:355`); the name→FitID map is **`data/art/buildings.csv`** (Name, Type,
   FitID cols). MCO's buildings.csv goes to FitID **2692** but the shipped pak has only **1188 packets
   and ZERO mech packets** (every MECH row is FitID≥1188; no 2692-packet pak exists anywhere). Built a
   2693-slot pak: low 1188 verbatim from user pak; 257 high non-mech from loose `data/objects/<Name>.fit`;
   141 high mech = generated **BattleMechType stub** (`ObjectTypeNum=2`, Name/Appearance/ProfileName =
   `<csvname>`; real stats in the user's `<name>.csv`, read via ProfileName in `mech.cpp:664`). The
   mission's per-`[PartN]` `ObjectNumber` already equals the buildings.csv FitID (not stale).
2. **ABL libraries** — engine recompiles `data/missions/{orders,miscfunc,corebrain}.abx` at every
   mission start (`mission.cpp:2462`; `.abx` is ABL *source*). WD brains call `magicAttack` (in
   corebrain). orders+miscfunc from user MCO; corebrain from **MC2-Exodus** (MCO's own 2015 corebrain
   trips our ABL compiler: `(type 16) Incompatible types "range"`; the Exodus one defines magicAttack
   and compiles clean). The gated `execMagicAttack` stub (`ablmc2.cpp:7026`) stays OFF (shadow rule).
3. **`data/tgl`** (302MB) mirrored from user MCO — non-stock MCO mech appearances/shapes (chimera,
   awesome, firestarter…) absent from MC2 base ⇒ silent crash in mech_recipe_build (acamar's mechs
   are stock so it survived without this; outreach/pirate2/styx needed it).

Build: `py -3 tools/mco_import/build_mco_compat.py --source <MCO install> --corebrain
<MC2-Exodus>/data/missions/corebrain.abx --out <deploy>/mods/mco-compat` (`--skip-tgl` = objects/ABL
only). OPEN: drop the Exodus corebrain dep by teaching the engine ABL compiler MCO's `range` type;
1 residual non-fatal per-brain ABL bug (pirate2 `dredattack01.abl`); "piss ton" more MCO campaigns in
Downloads untested. Smoke a mod mission: temp line in nifty `smoke_missions.txt` +
`MC2_ACTIVE_MOD=<folder>` + `--exe <deploy>/mc2.exe`; clear `mods/<id>/.modindex-cache` after edits.

### 2. MC2X cleanup debt
- Spec `docs/superpowers/specs/2026-06-14-mc2x-importer-slice1-design.md` + plan
  `docs/superpowers/plans/2026-06-14-mc2x-importer-slice1.md` still describe the DEAD delta design.
- Delta tests (`tests/mc2x_import/test_validation.py`, `test_manifest.py`) were deleted, not
  replaced. Add a lean manifest-driven test (dry-run counts, --help, guard, idempotency).
- Then `finishing-a-development-branch` (PR/merge).

### 3. rc1 state (cleanup or keep)
rc1 `mods/` currently has: importer-built `mc2x-compat`+`cveg`, copied campaign packs
`DarkRain`/`PicturesOfARebeliion`/`TangoMaster`, `MCO-Wolf-Dragoons` (+ my added mod.json) + the
`mco-compat` objects experiment, plus `keid-v`. rc1's `mc2.exe` is the fresh build. Decide whether
to reset rc1 to a clean state.

## Gotchas
- Smoke a mod mission: add a temp line to
  `.claude/worktrees/nifty-mendeleev/tests/smoke/smoke_missions.txt`
  (`tier3 <stem> allow_asset_oob=1`), run with `MC2_ACTIVE_MOD=<folder>` exported + `--exe <deploy>/mc2.exe`,
  then revert the manifest line. (`active=<mod>` manifest key did NOT set MC2_ACTIVE_MOD; export it.)
- Always clear `mods/<id>/.modindex-cache` after changing a mod's files, or first launch mis-loads.
- Kill orphan mc2.exe by PID, never `--kill-existing` / `/IM` (CLAUDE.md smoke rule).
- Build: `<cmake> --build .claude/worktrees/nifty-mendeleev/build64 --config RelWithDebInfo --target mc2 -j`;
  deploy `mc2.exe`+`mc2.pdb` (+ `shaders/` for flat installs).
