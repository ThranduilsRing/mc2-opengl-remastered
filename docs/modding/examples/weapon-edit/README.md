# Example Mod — Edit Weapon Stats (Double PPC Damage)

> **AVAILABILITY**
> - **Works on:** this checkout (any branch that has `mclib/file.cpp`'s mod-overlay block and `mclib/cmponent.cpp`'s `MasterComponent::loadMasterList`/`initEXCEL`). Both were spot-checked present on the current branch at the time this doc was written. This is a pure data-mod technique — no branch-only engine feature is involved.
> - **Core env var:** `MC2_ACTIVE_MOD` — unset by default. Unset = base game, stock `compbas.csv` values apply.
> - **Diagnostics env vars:** `MC2_LOG_FILE_RESOLVE` (0/1/2, default 0 = off), `MC2_REBUILD_MOD_CACHE` (any value = force rebuild, default off). See [Troubleshooting](#troubleshooting) below.
> - **No build required.** This is a whole-file CSV shadow. You never touch `shaders/` or C++ source, and you never touch the `mc2.exe` binary.
> - This doc does **not** use `MC2_EQUIPMENT_ADD` or `MC2_UNIT_PROFILE_DATA` — those are branch-only (`claude/unit-profile-seam`, unmerged) and irrelevant here. Editing an *existing* weapon's stats needs none of that; it's plain whole-file overlay.

Before you start, read [Chapter 0 — Getting Started](../getting-started.md) (mod folder anatomy, `MC2_ACTIVE_MOD`, the launcher picker, `mc2mod.py` packaging, diagnostics) and skim the [Mechanism Map](../mechanism-map.md) row for `compbas.csv`. This doc assumes you already know what `mods/<id>/data/...` means.

## What you'll do

Double the `PPC`'s damage stat by shipping a modified copy of `data/objects/compbas.csv` in a mod folder. The mod-overlay mechanism ([Chapter 0 §3](../getting-started.md#3-whole-file-shadowing-and-priority)) is a **whole-file** shadow — there's no per-row or per-field patch format, so your mod ships the *entire* 255-row weapon table with one value changed.

## 1. Why the whole file, and where the "truth" column names come from

`compbas.csv` is the master component/weapon table, loaded by `MasterComponent::loadMasterList` (`mclib/cmponent.cpp:630-678`) into a **fixed-size 255-row array** — the row count is hard-coded at the call site (`code/mission.cpp:1974`), not read from the file. The loader reads and discards line 1 (the header) unconditionally (`cmponent.cpp:663`), then reads exactly 255 more lines and hands each one to `MasterComponent::initEXCEL` (`cmponent.cpp:87-179`), which parses it with `strtok(..., ",")` — positional, not name-based. This means:

- Your mod's `compbas.csv` **must** have the header line + exactly 255 data rows, same column order, or the row-index-to-`masterID` mapping breaks.
- The header row's actual text is cosmetic — `initEXCEL` never reads it — but it documents the intended column names, and I'm using it below because it matches the field order `initEXCEL` actually consumes.

Verified column mapping (header text from the stock file, cross-referenced field-by-field against `initEXCEL`, `cmponent.cpp:92-197`):

| # | Header text (stock CSV, row 1) | Parsed into | Notes |
|---|---|---|---|
| 0 | `Component Table` | `masterID` | Must equal the row's own position (0-based); row `N` in the data section is component `N` |
| 1 | `Type` | `form` (`ComponentFormType`) | Must be one of the strings in `ComponentFormString[]` (`cmponent.cpp:25-47`), e.g. `EnergyWeapon`, `BallisticWeapon`, `MissileWeapon`, `Ammo`. Literal `undefined` marks the row free/unused and the parser returns early (`cmponent.cpp:111-114`) — the rest of that row is never read |
| 2 | `Name` | *(parsed, then discarded)* | **Trap:** `cmponent.cpp:118` reads this field into `field` and never stores it — `// name, ignore`. The in-game display name and abbreviation instead come from string resources `32000+masterID` / `33000+masterID` (`COMPONENT_NAME_START`/`COMPONENT_ABBR_START`, `cmponent.cpp:62-63,100-101`), loaded via `cLoadString`. **Editing this CSV column does not rename anything in-game.** Renaming a weapon is a separate, string-resource edit — out of scope for this doc |
| 3 | `crit hits` | `size` | "# of total spaces used" (`cmponent.h:164`) — total critical-slot cost for mechlab inventory purposes |
| 4 | `Recycle (if ammo = #shots)` | `recycleTime` | Weapon reload/cycle time (or shots-per-ton for ammo rows) |
| 5 | `Heat` | `heat` | Heat generated per shot |
| 6 | `Weight` | `tonnage` | |
| 7 | `Damage (if ammo = explosive dam)` | `damage` → `stats.weapon.damage` | **This is the column this tutorial edits.** For weapon-form rows (`COMPONENT_FORM_WEAPON_ENERGY`/`_MISSILE`/`_BALLISTIC`) this becomes per-shot damage (`cmponent.cpp:231`) |
| 8 | `BR` | `CV` | Comment in the header only says "CV for this component" (`cmponent.h:170`) — used as a value/cost field, exact consumer not traced in this pass |
| 9 | `RP` | `resourcePoints` | |
| 10 | `Range` | weapon range bucket | Must be `short`/`medium`/`long` (case-insensitive) or `0` |
| 11–18 | `Location Head` … `Location RL` (8 columns) | `criticalSpacesReq[0..7]` | Per-body-location slot requirement. `No` = can't mount there, `Yes` = mounts, uses 0 slots, or a number = exact slot count |
| 19 | `Missile type (0=Energy,1=Ballistic,LRM,ST,SRM)` | ammo-type bucket | |
| 20 | `Fields (1=streak,2=inferno,4=LBX,8=Artillery)` | component flags | |
| 21 | `Special FX ID` | `stats.weapon.specialEffect` | Binds to `effects.csv` — see [Part 1 of gosFX Modder Drop-In](../../../gosfx-modder-dropin-path.md#part-1--binding-works-today-csv-only) for the full weapon→effect chain. **Not covered here** to avoid duplicating that doc |
| 22 | `Ammo Master ID` | `stats.weapon.ammoMasterId` | `masterID` of the ammo row this weapon consumes |
| 23–28 | `Logistics Icon 1`, `Logistics Icon 2`, `String Index`, `Mechlipidia String`, `Icon X`, `Icon Y` | *(none — not consumed)* | Confirmed by grep: no member named for any of these appears anywhere in `cmponent.h`/`cmponent.cpp`. `initEXCEL` never calls `strtok` for them. Leftover columns from the original data table; safe to leave untouched |

The **PPC**'s stock row (`masterID` 145) is:

```
145,EnergyWeapon,PPC,3,6.67,10.00,6.00,7.00,441.04,5700,medium,No,Yes,Yes,Yes,Yes,Yes,No,No,0,0,4,0,MCL_MC_PPC.tga,MCL_MC_PPC,32145,34145,1,3
```

Column 7 (`Damage`) is `7.00`. This mod ships the same row with column 7 changed to `14.00` — everything else byte-identical.

**Where the stock reference came from:** this repo does not ship a loose `data/objects/compbas.csv` — in a real install it's packed inside a FastFile (`.fst`), and this repo's tooling has no FST-extraction utility. The stock values above (and the full 255-row file in this mod) were taken from `tools/mc2weapon/compbas.csv` in the `A:/Games/mc2-opengl/mc2-win64-v0.4/` deploy tree — a known-good reference copy that matches the engine's expected row count (255 data rows + 1 header = 256 lines) and column layout. If your own install has a different stock table (mod conflicts, prior edits), diff against your own extracted copy instead of assuming this one is authoritative for you.

## 2. The payload in this folder

```
mod/
  mod.json
  data/
    objects/
      compbas.csv   <- whole-file shadow of data/objects/compbas.csv, PPC row's Damage column doubled
```

`mod/mod.json`:

```json
{
  "id": "weapon-edit-example",
  "name": "Weapon Edit Example (Double PPC Damage)",
  "type": "campaign",
  "description": "Tutorial mod: doubles the PPC's Damage column in compbas.csv (7.00 -> 14.00). Whole-file shadow of data/objects/compbas.csv, otherwise byte-identical to stock.",
  "dependencies": []
}
```

## 3. Install it into your own game folder

**Do not point this at this repo's own dev deploy roots** — `mc2mod.py install` refuses those anyway ([Chapter 0 §5](../getting-started.md#5-packaging-and-installing-with-mc2modpy)). Use your own copy of the game.

### Option A — copy the folder directly (fastest for iterating)

```powershell
Copy-Item -Recurse "<this-repo>\docs\modding\examples\weapon-edit\mod" "<your-game-folder>\mods\weapon-edit-example"
```

### Option B — package + install with `mc2mod.py`

```powershell
cd <this-repo>
py -3 tools\mc2mod\mc2mod.py pack docs\modding\examples\weapon-edit\mod --out .\dist
py -3 tools\mc2mod\mc2mod.py verify-lite .\dist\weapon-edit-example-0.0.0.mc2mod
py -3 tools\mc2mod\mc2mod.py install .\dist\weapon-edit-example-0.0.0.mc2mod --deploy "<your-game-folder>"
```

## 4. Activate and verify

### Option A — PowerShell

```powershell
cd <your-game-folder>
$env:MC2_ACTIVE_MOD = "weapon-edit-example"
.\mc2.exe
```

### Option B — the launcher picker

Run `mc2_launcher.exe` in your game folder, pick **Weapon Edit Example (Double PPC Damage)** from the list (instead of "Base Game"), and it launches `mc2.exe` with the env var set for you.

### What you should see

Start (or load) any mission where you or an enemy fields a mech carrying a stock **PPC** (not ER PPC or Clan ER PPC — those are separate rows, unedited by this mod). Fire it at a target and compare damage-per-hit against a base-game run: it should be roughly double. The cleanest side-by-side check is the mechlab loadout screen if your build's UI surfaces per-weapon damage numbers, or a controlled single-PPC-vs-armor test in a skirmish/instant-action mission.

## 5. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Damage looks unchanged | Mod not active, or wrong folder name | Confirm `mods/weapon-edit-example/mod.json` exists and `MC2_ACTIVE_MOD=weapon-edit-example` (must match `mod.json`'s `id`, or the folder name if `mod.json` is missing) |
| Damage looks unchanged, mod *is* active | Stale mod index cache | `$env:MC2_REBUILD_MOD_CACHE = "1"` then relaunch (see [Chapter 0 — the stale-cache trap](../getting-started.md#the-stale-cache-trap)) — mostly relevant if you're editing files repeatedly, not on first install |
| Not sure if your file is even being picked up | — | `$env:MC2_LOG_FILE_RESOLVE = "1"` then relaunch; watch stdout for a line resolving `data/objects/compbas.csv` to `mod:weapon-edit-example` and for `[mod] [1/N] active 'weapon-edit-example': <count> files` |
| Game crashes or weapons look wildly wrong on load | Row count mismatch, or a row corrupted by editing | Your `compbas.csv` must have exactly 1 header line + 255 data rows, comma-delimited, same column count per row. Diff your file against the shipped one in this folder — only the PPC row's 8th column should differ |
| Renamed the weapon in the `Name` column but it still shows "PPC" in-game | Expected — see the `Name` column note in §1 | The CSV `Name` field is parsed then discarded; the in-game name comes from a string resource, not this file. Out of scope for this doc |

## 6. Related mechanisms (not covered here)

- **Weapon → visual effect binding** (which muzzle flash / hit effect a weapon uses) is a separate `Special FX ID` → `effects.csv` chain — see [Part 1 of `gosfx-modder-dropin-path.md`](../../../gosfx-modder-dropin-path.md#part-1--binding-works-today-csv-only) rather than duplicating it here.
- **Adding a new weapon beyond the fixed 255 rows** is not possible on this checkout (`MC2_EQUIPMENT_ADD` is branch-only, unmerged) — see the [Mechanism Map's "Branch-only" section](../mechanism-map.md#branch-only-not-present-on-this-checkout). What you *can* do on this checkout is fill an `undefined` row with new stats, which behaves like "adding" a weapon within the existing table.
- **Renaming a weapon or changing mechlab display text** requires editing the string-resource `.fit` file at `COMPONENT_NAME_START+masterID` / `COMPONENT_ABBR_START+masterID`, not this CSV — not covered in this doc.

## Validator output (run against this folder's payload)

> **Note on the `sha256` value below:** `pack` embeds a freshly generated `package.json` in the zip via Python's `zipfile.writestr(...)`, which stamps that entry with the wall-clock time of the run (`tools/mc2mod/mc2mod.py` — the `zf.writestr("package.json", package_json_bytes)` call has no explicit `ZipInfo`/`date_time`, so `zipfile` defaults to "now"). That timestamp is part of the bytes that get sha256'd as the outer package hash, so **`sha256` is different on every `pack` run — even with byte-identical payload input.** Confirmed by running `pack` twice back-to-back against this exact folder: `sha256=2bb1b64c...` then `sha256=ec9a75d5...`, no source changes between runs. **If your own `pack`/`verify-lite` output shows a different hash than the transcript below, that is expected and does not mean your payload is wrong** — check `id=`, `version=`, and `files=` instead; those are the reproducible fields.

```
$ py -3 tools/mc2mod/mc2mod.py pack docs/modding/examples/weapon-edit/mod --out <tmp>
pack: <tmp>/weapon-edit-example-0.0.0.mc2mod
  id=weapon-edit-example  version=0.0.0  files=2  sha256=<varies per run — see note above>

$ py -3 tools/mc2mod/mc2mod.py verify-lite <tmp>/weapon-edit-example-0.0.0.mc2mod
verify-lite PASS: id=weapon-edit-example  version=0.0.0  files=2  pkg_sha256=<matches the pack sha256 from the same run>
```

Also confirmed with `tools/mod_install/resolver.py` (the offline replica of `mclib/file.cpp`'s resolution ladder) against a synthetic deploy tree: requesting `data/objects/compbas.csv` with `active_mod="weapon-edit-example"` resolves to layer `mod:weapon-edit-example` with `base-loose` recorded as shadowed — i.e. this mod's file wins over a stock `data/objects/compbas.csv`, exactly as the whole-file-shadow mechanism promises.
