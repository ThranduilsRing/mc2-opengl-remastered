<!-- doc-status: current | reviewed:2026-10-11 -->
# Example Mod — Edit a Mission (mc2_01 Remixed)

> **AVAILABILITY**
> - **Works on:** this checkout (any branch that has `mclib/file.cpp`'s mod-overlay block and `code/mission.cpp`'s `Mission::init`). Both were spot-checked present on the current branch at the time this doc was written. This is a pure data-mod technique — no branch-only engine feature is involved.
> - **Core env var:** `MC2_ACTIVE_MOD` — unset by default. Unset = base game, stock `mc2_01.fit` applies.
> - **Diagnostics env vars:** `MC2_LOG_FILE_RESOLVE` (0/1/2, default 0 = off), `MC2_REBUILD_MOD_CACHE` (any value = force rebuild, default off). See [Troubleshooting](#8-troubleshooting) below.
> - **No build required.** This is a whole-file FIT shadow. You never touch `shaders/`, C++ source, or the `mc2.exe` binary.
> - **Honest verification status:** every mechanism claim below is source-verified with file:line anchors, and the payload passed the full offline pipeline (pack, verify-lite, install/uninstall roundtrip, resolution-replica check — transcripts at the bottom). **No live in-game capture was taken for this doc** — the four edits' expected in-game results are described from the verified code paths, and are marked for user verification the same way the sibling `weapon-add` example handled it.

Before you start, read [Chapter 0 — Getting Started](../getting-started.md) (mod folder anatomy, `MC2_ACTIVE_MOD`, the launcher picker, `mc2mod.py` packaging, diagnostics) and skim the [Mechanism Map](../mechanism-map.md). This doc assumes you already know what `mods/<id>/data/...` means.

## What you'll do

Ship a modified copy of `data/missions/mc2_01.fit` — the first campaign mission's definition file — in a mod folder. Four visible changes, each teaching one mission-modding mechanism:

1. **Time limit:** none → 15 minutes (an on-HUD countdown appears, with voice warnings at 2:00 and 0:30).
2. **Resource points:** 22,000 → 42,000 (a bigger in-mission support budget).
3. **Objective rename:** the first objective becomes "1. Investigate Abandoned Airfield (MODDED)" — and teaches the resource-string trap on the way.
4. **Enemy roster recolor:** every enemy unit's base paint color becomes bright red — and teaches why the same edit on *your own* units would silently do nothing.

The mod-overlay mechanism ([Chapter 0 §3](../getting-started.md#3-whole-file-shadowing-and-priority)) is a **whole-file** shadow — there's no per-block or per-field patch format, so your mod ships the *entire* mission `.fit` with 32 lines changed and everything else byte-identical to stock.

## 1. How a mission actually loads (and which files you can shadow)

`Mission::init` builds the path `data/missions/<stem>.fit` and opens it (`code/mission.cpp:2078-2091`). That open goes `FitIniFile::open` → `File::open` (`mclib/inifile.cpp:767-769`), and `File::open` consults the mod index **before** the base game's loose files and FastFiles (`TryModOpen`, `mclib/file.cpp:545-576`, called at `:950`). The overlay itself is initialized once at process start — `InitModSearchPaths("./mods/")` at `code/mechcmd2.cpp:587`, reading `MC2_ACTIVE_MOD` at `mclib/file.cpp:679`.

A mission is not one file, though. The full set, and whether each is mod-shadowable:

| File | Path | How the path is derived | Open path | Mod-overlay covered? |
|---|---|---|---|---|
| Mission definition | `data/missions/<stem>.fit` | mission name | `FitIniFile::open` → `File::open` (`mclib/inifile.cpp:769`) | **Yes** |
| Terrain + object placement | `data/missions/<stem>.pak` | **derived from the mission stem** — not a field in the `.fit` (`code/mission.cpp:2344`) | `PacketFile::open` → `File::open` (`mclib/packet.cpp:206-208`) | **Yes** |
| Mission script (objectives logic, triggers) | `data/missions/<script>.abl` | `[Script]` block, `ScenarioScript` key (`code/mission.cpp:2480-2488`) | ABL open callback → `File::open` (`code/ablmc2.cpp:7508-7519`) | **Yes** |
| Shared ABL libraries | `data/missions/orders.abx`, `miscfunc.abx`, `corebrain.abx` | fixed names (`code/mission.cpp:2460-2474`) | same ABL callback | **Yes** |
| Pilot profiles | `data/missions/profiles/<profile>.fit` | `[WarriorN]` block, `Profile` key (`code/mission.cpp:2542-2557`; `warriorPath`, `mclib/paths.cpp:38`) | `FitIniFile::open` | **Yes** |
| Pilot AI brains | `data/missions/profiles/<brain>.abl` | `[WarriorN]` block, `Brain` key (`code/mission.cpp:2569-2586`) | ABL callback | **Yes** |

Every row funnels through the same mod-aware `File::open`, so **any** of these files can be shadowed from `mods/<id>/data/...`. Shadowing is per-file: this example ships *only* the `.fit`; the `.pak`, `.abl`, and everything else fall through to the base game's `mission.fst` untouched. That's exactly what the offline resolution replica confirms (transcript at the bottom): `data/missions/mc2_01.fit` resolves to `mod:mission-edit-example`, while `mc2_01.pak` and `mc2_01.abl` miss the mod index and fall through.

**The stem-derived `.pak` is the fact to internalize before dreaming bigger:** a mission named `foo` *requires* `data/missions/foo.pak`. You can't point a mission `.fit` at a different map by editing a key — there is no such key. (Consequence for new-mission ambitions: see [§9](#9-whats-out-of-scope-honestly).)

## 2. Where the stock mission file came from

This repo does not ship loose stock mission files — in a real install they're packed inside `mission.fst`, and this repo's tooling can list FST members (`tools/mod_install/fst_listing.py`) but not extract them (the deployed `mission.fst` uses the LZSS FastFile variant; there's no decompressor tool here).

The copy in this mod came from the project's pristine source-data checkout (`mc2srcdata-fresh`, the git-clean tree the project's FastFiles are built from). Cross-check performed while writing this doc: the entry table of the deployed `mission.fst` declares `data/missions/mc2_01.fit` at exactly **50,027 bytes**, `mc2_01.pak` at 171,423, `mc2_01.abl` at 25,369 — all three byte-counts match the pristine tree's files exactly. (That's a size match, not a decompressed-content match — stated plainly so you know precisely how far the verification goes.) The unmodified copy ships in this folder as `mod/source/mc2_01_stock.fit` so you can diff the payload against it:

```powershell
cd <this-repo>\docs\modding\examples\mission-edit
git diff --no-index mod/source/mc2_01_stock.fit mod/data/missions/mc2_01.fit
```

32 changed lines, nothing else. If your own install's stock mission differs (prior mods, different game version), your baseline is your own file, not this one.

## 3. Edit 1 — Time limit

```
[MissionSettings]
f TimeLimit = -1.000000        ->  f TimeLimit = 900.000000
```

Read at `code/mission_init.cpp:203-219`: `-1.0` means the mission never times out; any positive value is a limit in **seconds**. Consumers, all verified:

- The in-mission GUI draws a countdown whenever `m_timeLimit > 0` (`code/controlgui_hudtabs.cpp:201-223`), and plays the "two minutes remaining" / "thirty seconds" Betty voice samples at those thresholds (`:379-387`).
- When the clock runs out, the mission terminates as a loss — `mis_PLAYER_LOST_BIG`, with a 5-second grace period (`code/mission.cpp:790-798`). One nuance: guard-a-structure objectives treat surviving to the time limit as *success* (`CGuardSpecificStructure::Status`, `code/objective.cpp:846-864` — returns `OS_SUCCESSFUL` at `:857-859` when the limit expires with the building alive and uncaptured); mc2_01's objectives aren't guard-type, so here the timeout is simply a loss.
- Multiplayer ignores the `.fit` value entirely (`m_timeLimit = MPlayer->missionSettings.timeLimit`, `code/mission_init.cpp:217-218`) — this edit is single-player only.

**Expected result:** a `15:00` countdown on the mission HUD from the moment the mission starts. *(User verification pending — described from code, not from a live capture.)*

## 4. Edit 2 — Resource points

```
[MissionSettings]
l ResourcePoints = 22000       ->  l ResourcePoints = 42000
```

Read at `code/mission.cpp:2303-2315` and handed to `LogisticsData::instance->setResourcePoints(numRPoints)` — this is the in-mission support budget (air strikes, sensor probes, salvage, etc.). Two overrides to know about, both verified:

- A `-rps <N>` command-line value overrides the `.fit` (`MaxResourcePoints`, default `-1` = no override, set by the `-rps` handler at `code/mechcmd2.cpp:180,2775-2779`; applied at `code/mission.cpp:2305-2306`). If you're testing with a custom launch script, make sure it isn't passing `-rps`.
- Multiplayer replaces the value with the host's session setting (`code/mission.cpp:2307-2314`).

**Expected result:** the resource-point counter in the in-mission support palette starts at 42,000 instead of 22,000. *(User verification pending.)*

## 5. Edit 3 — Objective rename (the resource-string trap)

```
[Team0Objective0]
st Title = "1. Investigate Abandoned Airfield"        ->  st Title = "1. Investigate Abandoned Airfield (MODDED)"
b TitleUseResourceString = TRUE                        ->  b TitleUseResourceString = FALSE
st Description = "1. Investigate Abandoned Airfield"  ->  st Description = "1. Investigate Abandoned Airfield (MODDED)"
b DescriptionUseResourceString = TRUE                  ->  b DescriptionUseResourceString = FALSE
```

Objectives live in `[Team0Objectives]` (`NumObjectives`, then one `[Team0ObjectiveN]` block each, plus per-objective `...ConditionN`/`...ActionN` blocks). They're loaded per team at `code/mission.cpp:3080-3083` → `CObjectives::Read` (block name formatted at `code/objective.cpp:2119`) → `CObjective::Read` (`code/objective.cpp:1674-`).

**The trap — same shape as the weapon `Name` column trap in [`weapon-edit`](../weapon-edit/README.md):** the loader reads *both* the literal `Title` string *and* the `TitleUseResourceString`/`TitleResourceStringID` pair (`code/objective.cpp:1687-1690`). At display time, `LocalizedTitle()` checks the boolean **first** — if it's `TRUE`, the literal string you edited is ignored and the text comes from the string-resource table instead (`code/objective.cpp:2036-2046`; same logic for `Description` at `:2048-`). Stock missions ship with the boolean `TRUE`. So: **editing only the `st Title` line changes nothing in-game.** Flip the boolean to `FALSE` in the same block, as this payload does.

The mission's own *name* has the identical dual mechanism — `MissionName` / `MissionNameUseResourceString` / `MissionNameResourceStringID` in `[MissionSettings]`, resolved the same way when mission-select and briefing screens read it (`code/logisticsdialog.cpp:1010-1022`; `readMissionInfo`, `code/logisticsmissioninfo.cpp:263-`). This payload leaves the mission name stock; rename it the same way if you want.

**Expected result:** the first objective reads "1. Investigate Abandoned Airfield (MODDED)" in the mission-start objectives display and the in-mission objectives review. *(User verification pending.)*

## 6. Edit 4 — Enemy roster recolor (and the player-color trap)

```
[Part4] .. [Part29] (every block whose TeamID = 1, 26 total)
ul BaseColor = -1944008   ->  ul BaseColor = -65536     (20 parts)
ul BaseColor = -5232600   ->  ul BaseColor = -65536     (6 parts — stock enemies ship two different base colors)
```

`-65536` is `0xFFFF0000` — opaque red in the ARGB packing these fields use (the stock player-mech green is `-16711936` = `0xFF00FF00`).

The unit roster is the `[Parts]`/`[PartN]` + `[Warriors]`/`[WarriorN]` block family. Per-part colors are read at `code/mission.cpp:2861-2871` (`BaseColor`, `HighlightColor1`, `HighlightColor2`, each falling back to player prefs if absent — or unconditionally in multiplayer) and applied to the spawned unit's appearance via `resetPaintScheme(...)` at `code/mission.cpp:1221-1223` (mechs) / `:1238-1240` (vehicles).

**The trap:** look one branch up, at `code/mission_update.cpp:724-725` — units belonging to *your own* commander are **always** painted with your player-preference colors (`prefs.baseColor` etc.), and the `.fit` part colors are ignored. The `.fit` colors only apply to units that are *not* on the home commander: enemies and allies. That's why this payload paints the **enemy** lance red instead of trying to recolor your own mechs — the player-mech version of this edit would silently do nothing, and you'd waste an evening suspecting your mod setup. (Recoloring your own units is an options-screen feature, not a mission-mod mechanism.)

For reference when making your own roster edits, the fields each `[PartN]` block feeds (all read in `code/mission.cpp:2726-2897`):

| Key | Read at (`code/mission.cpp`) | Meaning / notes |
|---|---|---|
| `ObjectNumber` | `:2775` | Which object (from the mission's object table) this part spawns. Don't change casually — it must exist for this mission |
| `ControlType`, `ControlDataType` | `:2780-2784` | Player vs AI control |
| `ObjectProfile` | `:2786` | Object profile name (max 9 chars read) |
| `CSVFile` | `:2789-2792` | Variant CSV name (e.g. `"Werewolf"`) — also used as a mod-tolerance fallback for the appearance name |
| `VariantNumber` | `:2794-2798` | Loadout variant index into the chassis CSV — the same variant blocks [`variant-add`](../variant-add/README.md) teaches |
| `Pilot` | `:2806` | 1-based index into this file's `[WarriorN]` blocks |
| `PositionX/Y`, `Rotation` | `:2812-2821` | Spawn placement |
| `TeamId`, `CommanderId` | `:2823-2843` | Allegiance. In single player, `TeamId = 2` (allied) is folded to team 0 (`:2827-2828`) |
| `BaseColor`, `HighlightColor1/2` | `:2861-2871` | Paint scheme — with the home-commander trap above |
| `Active`, `Exists` | `:2875-2879` | Spawn state (`gosASSERT` if missing) |
| `Damage` | `:2881-2887` | `>= 1.0` spawns the part destroyed |
| `MyIcon`, `Captureable` | `:2889-2897` | Icon index; capture flag (defaults `FALSE`) |

Warriors (`[WarriorN]`, read at `code/mission.cpp:2520-2600`): `Profile` names a pilot file under `data/missions/profiles/`, `Brain` names the AI brain `.abl` there (player-controlled parts conventionally use `"PBrain"`).

**Expected result:** the enemy mechs and vehicles you fight in mc2_01 — 4 Centipedes, 4 Harassers, 2 UrbanMechs, 7 LRM carriers, and the lone Starslayer — render with bright-red base paint. The roster's 8 enemy *Infantry* parts also carry the edit, but the verified `resetPaintScheme` apply covers only the mech and vehicle object classes (`code/mission.cpp:1205-1244` has exactly two cases, `BATTLEMECH` and `GROUNDVEHICLE`), so whether infantry visibly recolor is unverified — expect them stock-colored and don't debug your mod if they are. *(User verification pending.)*

> **Safe-edit vs. risky-edit line, honestly drawn:** the four edits this payload ships (settings values, objective text, part colors) touch fields whose full read-and-consume paths were verified above and which can't change the mission's object population. **Adding or removing** `[PartN]`/`[WarriorN]` blocks is a different risk class: `NumParts`/`NumWarriors` drive strict loops whose reads `gosASSERT`/`Assert` on any missing block or field (`code/mission.cpp:2726-2734`, `:2534-2543`), `Pilot` indices must stay consistent, and `ObjectNumber` must be valid for this mission. The pattern is mechanically obvious (bump the count, append complete blocks), but it was **not** exercised for this doc — treat it as unverified territory, keep the stock file open beside you, and expect an assert-crash at mission load (not silent breakage) when you get it wrong. The in-repo mission editor is the real tool for roster surgery.

## 7. Install and run it

The payload in this folder:

```
mod/
  mod.json
  data/
    missions/
      mc2_01.fit        <- whole-file shadow, 32 lines changed vs stock
  source/
    mc2_01_stock.fit    <- unmodified stock copy, for diffing (ships in the package too; harmless — the engine only reads data/)
```

**Do not point this at this repo's own dev deploy roots** — `mc2mod.py install` refuses those anyway ([Chapter 0 §5](../getting-started.md#5-packaging-and-installing-with-mc2modpy)). Use your own copy of the game.

### Option A — copy the folder directly (fastest for iterating)

```powershell
Copy-Item -Recurse "<this-repo>\docs\modding\examples\mission-edit\mod" "<your-game-folder>\mods\mission-edit-example"
```

### Option B — package + install with `mc2mod.py`

```powershell
cd <this-repo>
py -3 tools\mc2mod\mc2mod.py pack docs\modding\examples\mission-edit\mod --out .\dist
py -3 tools\mc2mod\mc2mod.py verify-lite .\dist\mission-edit-example-1.0.0.mc2mod
py -3 tools\mc2mod\mc2mod.py install .\dist\mission-edit-example-1.0.0.mc2mod --deploy "<your-game-folder>"
```

### Activate

```powershell
cd <your-game-folder>
$env:MC2_ACTIVE_MOD = "mission-edit-example"
```

…or run `mc2_launcher.exe` and pick **Mission Edit Example (mc2_01 Remixed)** from the list.

### Test it without replaying the campaign: the `-mission` quick-start flag

You don't have to click through logistics to see your edits. The engine has a shipped quick-start path — parsed at `code/mechcmd2.cpp:2617-2654`, started at `:1798-1806` (`MISSION_LOAD_SP_QUICKSTART`, bypasses the mech-bay/logistics screens entirely):

```powershell
$env:MC2_ACTIVE_MOD = "mission-edit-example"
.\mc2.exe -mission mc2_01
```

The overlay is process-wide (initialized before any mission loads), so quick-start picks up your mod exactly like the campaign flow does.

What quick-start does to your campaign progress: it never enters the logistics/save flow at all — the mission is initialized directly (`mission->init(...)` at `code/mechcmd2.cpp:1802`) instead of via the campaign screens where saves are managed. No code path was found that writes campaign-save state from a quick-started mission, but this wasn't exhaustively verified — if you have an irreplaceable in-progress campaign, back up your save files before experimenting, on general principle.

**Spelling matters:** it's `-mission` (one dash, game quick-start). `--mission` (two dashes) belongs to the smoke-test harness and is *refused* unless `MC2_SMOKE_MODE` is set (`GameOS/gameos/gos_smoke.cpp:98-105`) — if you typo it, you'll get a `[SMOKE v1] ... argv_error` line and an instant exit, not a mission.

### What you should see (all four at once)

Mission loads onto the stock mc2_01 map with your stock starting lance — then: a **15:00 countdown** on the HUD, **42,000 resource points** in the support palette, the first objective titled **"…(MODDED)"**, and **red enemies**. Clear the mission inside 15 minutes or watch the time-out loss fire — either way you've verified two mechanisms with one clock. *(As flagged in the availability banner: expected results are code-derived; no live capture was taken for this doc.)*

## 8. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Everything looks stock | Mod not active, or wrong id | Confirm `mods/mission-edit-example/mod.json` exists in your game folder and `MC2_ACTIVE_MOD=mission-edit-example` (must match `mod.json`'s `id`, or the folder name if `mod.json` is missing) |
| Still stock, mod *is* active | Stale mod index cache (only if you added/removed files after first launch) | `$env:MC2_REBUILD_MOD_CACHE = "1"` then relaunch ([Chapter 0 — the stale-cache trap](../getting-started.md#the-stale-cache-trap)). Note: *content* edits to an already-indexed file are always picked up fresh — the cache stores paths, not bytes |
| Not sure your file is even being read | — | `$env:MC2_LOG_FILE_RESOLVE = "1"` then relaunch; watch stdout for `[mod-hit] [mission-edit-example] data/missions/mc2_01.fit -> ...` during mission load |
| Renamed the objective but it still shows the stock text | Forgot to flip the boolean | `TitleUseResourceString`/`DescriptionUseResourceString` must be `FALSE` for the literal strings to win — see [§5](#5-edit-3--objective-rename-the-resource-string-trap) |
| Recolored a unit but it stayed player-colored | It's one of *your* units | Home-commander units always use your prefs colors, never the `.fit` colors (`code/mission_update.cpp:724-725`) — see [§6](#6-edit-4--enemy-roster-recolor-and-the-player-color-trap) |
| Resource points ignore your value | `-rps` on the command line, or multiplayer | Both override the `.fit` (`code/mission.cpp:2305-2314`) |
| No countdown in multiplayer | Expected | MP takes the session's time limit, not the `.fit`'s (`code/mission_init.cpp:217-218`) |
| Game asserts/crashes at mission load after your own further edits | Structural damage: removed/renamed a block or key inside the strict loops | Diff against `mod/source/mc2_01_stock.fit`; the Parts/Warriors loops `gosASSERT` on missing fields ([§6](#6-edit-4--enemy-roster-recolor-and-the-player-color-trap) safe/risky note) |
| `--mission` prints `[SMOKE v1] ... argv_error` and exits | Two dashes = smoke-harness flag | Use single-dash `-mission` ([§7](#test-it-without-replaying-the-campaign-the--mission-quick-start-flag)) |

## 9. What's out of scope (honestly)

- **Creating a brand-new mission from scratch** — the hard part is the terrain `.pak` (binary terrain + object placement), which is authored with the in-repo mission editor, a separate tool with its own workflow. No tutorial for it exists in this collection yet. Until one does, "mission modding" here means *editing* missions, not creating them.
- **Adding a new mission under a new stem** — mechanically closer than you'd think, and every seam is verified: a mod *can* ship `data/missions/mymission.fit` + `mymission.pak` + `mymission.abl` (all three resolve through the mod overlay — [§1](#1-how-a-mission-actually-loads-and-which-files-you-can-shadow)'s table), and a mod *can* inject a whole new campaign into the load-campaign list — the engine enumerates `data/campaign/*.fit` from the mod index and adds them to the UI (`EnumerateModCampaignFiles`, `mclib/file.cpp:594-611`, consumed at `code/logisticsdialog.cpp:571-606`; campaign `.fit` structure: `GroupCount`, then `Group<i>`/`Group<i>Mission<j>` blocks with `FileName` keys, parsed at `code/logisticsmissioninfo.cpp:77-260`). But no worked, payload-backed example of that pipeline exists yet — it needs a donor `.pak` you have the rights to redistribute and an end-to-end live test, neither of which this doc has. Documented here so you know the door is open; not walked through.
- **Mission script (`.abl`) editing** — the mission's objective/trigger logic is an ABL program (`data/missions/<script>.abl`), overlay-covered like everything else, but ABL is a programming language and out of scope for a data-edit tutorial.
- **The `mc2_01_*_specials.fit` files** you may see in this project's deploy tree — those are project test fixtures for an in-development scripting arc, not stock mission data; nothing in the stock mission load path reads them.
- **Multiplayer missions** — MP overrides several of the fields this doc edits (time limit, resource points, colors); everything above is single-player scoped.

## Validator output (run against this folder's payload)

All transcripts below are real output from this checkout, and the sha256 is reproducible — `pack` pins zip member timestamps, so packing this exact payload always yields this hash.

```
$ py -3 tools/mc2mod/mc2mod.py pack docs/modding/examples/mission-edit/mod --out <tmp>
pack: <tmp>/mission-edit-example-1.0.0.mc2mod
  id=mission-edit-example  version=1.0.0  files=3  sha256=75fb9bbbe5840b48a376f48ee654c3fd4b9acd79a6a4dc9a1cc6d12b0a9b9e73

$ py -3 tools/mc2mod/mc2mod.py verify-lite <tmp>/mission-edit-example-1.0.0.mc2mod
verify-lite PASS: id=mission-edit-example  version=1.0.0  files=3  pkg_sha256=75fb9bbbe5840b48a376f48ee654c3fd4b9acd79a6a4dc9a1cc6d12b0a9b9e73

$ py -3 tools/mc2mod/mc2mod.py install <tmp>/mission-edit-example-1.0.0.mc2mod --deploy <scratch-deploy>
install: mission-edit-example v1.0.0 -> <scratch-deploy>/mods/mission-edit-example
  files installed : 3

$ py -3 tools/mc2mod/mc2mod.py uninstall mission-edit-example --deploy <scratch-deploy>
uninstall: mission-edit-example from <scratch-deploy>
  files removed   : 3
  dirs removed    : 4        (mods/ left empty — clean roundtrip)
```

Resolution replica (`tools/mod_install/resolver.py`, the offline mirror of `mclib/file.cpp`'s ladder) against a synthetic deploy tree with this mod installed and `active_mod="mission-edit-example"`:

```
data/missions/mc2_01.fit -> layer 'mod:mission-edit-example'  (base-loose recorded as shadowed)
data/missions/mc2_01.pak -> layer 'MISS'  (expected — not shipped; falls through to mission.fst in a real deploy)
data/missions/mc2_01.abl -> layer 'MISS'  (same)
```

Payload self-check: `mod/data/missions/mc2_01.fit` and `mod/source/mc2_01_stock.fit` are both 2,345 lines (CRLF), differing on exactly 32 lines — 1 `TimeLimit`, 1 `ResourcePoints`, 4 objective-text lines, 26 enemy `BaseColor` lines. Nothing else.
