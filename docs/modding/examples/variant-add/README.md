# Example Mod — Add a New Mech Variant (Pure Data)

> **AVAILABILITY**
> - **Works on:** this checkout (any branch with `mclib/file.cpp`'s mod-overlay block, `code/mech.cpp`'s `BattleMech::init(DWORD variantNum)`, and `code/logisticsdata.cpp`'s mechlab variant-discovery loop). All three were spot-checked present on the current branch at the time this doc was written. This is a **pure data-mod** technique — no branch-only engine feature is involved, and no C++ rebuild is needed.
> - **Core env var:** `MC2_ACTIVE_MOD` — unset by default. Unset = base game, stock `atlas.csv` (3 variants) applies.
> - **Diagnostics env vars:** `MC2_LOG_FILE_RESOLVE` (0/1/2, default 0 = off), `MC2_REBUILD_MOD_CACHE` (any value = force rebuild, default off). See [Troubleshooting](#5-troubleshooting) below.
> - **No build required.** This is a whole-file CSV shadow. You never touch `shaders/` or C++ source, and you never touch the `mc2.exe` binary.
> - This doc does **not** use `MC2_EQUIPMENT_ADD` or `MC2_UNIT_PROFILE_DATA` — those are branch-only (`claude/unit-profile-seam`, unmerged) and unnecessary here. Adding a *variant* of an *existing* chassis needs none of that; see the [Coming soon](#coming-soon-branch-only-unit-profile-system) box at the bottom for what those actually cover.

Before you start, read [Chapter 0 — Getting Started](../getting-started.md) (mod folder anatomy, `MC2_ACTIVE_MOD`, the launcher picker, `mc2mod.py` packaging, diagnostics) and skim the [Mechanism Map](../mechanism-map.md) row for "Variant block layout". This doc assumes you already know what `mods/<id>/data/...` means.

## Variant vs. mesh — read this first

This doc adds a **loadout/stats variant of the stock Atlas chassis**. It does **not** touch the Atlas's 3D model, textures, or animations in any way. The new variant renders with the *exact same mesh* as every other Atlas — same silhouette, same hardpoint art, same walk cycle. All that changes is which weapons are bolted onto that same mesh and what the variant is named in the mechbay.

These are two completely separate mechanisms in this engine:

| | This doc (variant add) | Mesh replacement (different doc) |
|---|---|---|
| What changes | A 97-row block appended to `<chassis>.csv` — loadout, armor-current, a name | The actual geometry — a glTF/FBX/ASE model swapped in via an `[Import]` block |
| Entry point | `BattleMech::init(DWORD variantNum)`, `code/mech.cpp:1405` | `mclib/mech3d.cpp:346-360` (`[Import]` block probe) |
| Drop path | `data/objects/<chassis>.csv` | `data/tgl/<mech>.ini` |
| Visual result | Same model, different loadout/name in the mechbay list | Different model, same stat block |

If you came here wanting to give the Atlas a new *look*, you want the mesh-import doc, not this one. If you want a new *loadout choice* for players (e.g. "Atlas with dual PPCs instead of dual Clan ER PPCs"), you're in the right place.

## 1. How the mechanism works (verified against source)

`data/objects/atlas.csv` (like every chassis CSV) is one **fixed header block** (chassis-wide stats: tonnage, base armor, max armor — shared by all variants) followed by **N repeating 97-line variant blocks**. The row arithmetic is hard-coded with a fixed stride of 97 rows per variant, read by `BattleMech::init`:

| What | Formula | Source |
|---|---|---|
| Variant name | row `23 + 97 * variantNum`, column 2 | `code/mech.cpp:1451` |
| "Hidden from mechbay" flag | row `21 + 97 * variantNum`, column 4 | `code/logisticsvariant.cpp:337` |
| Inventory item's component ID | row `26 + 97 * variantNum + curItem` (curItem 0–71), column 5 | `code/mech.cpp:1545,1602,1670` and `code/logisticsvariant.cpp:354` |
| Critical-space→item-slot map | row `102 + 97 * variantNum + curSpace`, column varies by body location | `code/mech.cpp:1761` |
| Per-body-location current internal structure | row `114 + 97 * variantNum`, column varies by body location | `code/mech.cpp:1740` |
| Per-body-location current armor | row `115 + 97 * variantNum`, column varies by body location | `code/mech.cpp:1487-1517` |

**Column numbers above are 1-based, literal comma positions** — column 1 is always the row's own text label (e.g. `Item22MasterId`), column 2 is the first real value. For example, `code/mech.cpp:1432`'s `readInt(10,5,chassisBR)` reads stock row 10 (`Animation,atlaswalk,,ChassisBR,115,...`), landing on `115`, the 5th comma-separated field — confirmed against `A:/Games/mc2-srcdata/objects/atlas.csv:10`.

`MAX_MOVER_INVENTORY_ITEMS` is `72` (`code/mover.h:588`), which is why each variant block has exactly `Item0MasterId` … `Item71MasterId` — 72 inventory rows.

**Concretely, one variant block is 97 consecutive CSV lines**, running from the blank line right before `VariantInfo` through the block's final `CurrentArmorPoints` line. In the stock `atlas.csv` the three stock variants sit at lines 19–115 (`Atlas Prime`, variant 0), 116–212 (`Claymore's Atlas`, variant 1), and 213–309 (`Renard's Atlas`, variant 2). Each range is exactly 97 lines wide (e.g. `212 - 116 + 1 = 97`), the stride between consecutive block-start lines is exactly 97 (`213 - 116 = 97`), and `VariantName` lands on line 23/120/217 for variant 0/1/2 — matching `23 + 97*variantNum` exactly in all three cases.

### No hard-coded variant-count cap — the mechbay auto-discovers blocks

The mechlab/logistics screen doesn't read a "number of variants" field anywhere. It scans forward from row 23 in steps of 97, reading a `VariantName` string at each step, and **stops the first time that read fails** (`code/logisticsdata.cpp:319-332`):

```cpp
int row = 23;
...
while (NO_ERR == mechFile.readString(row, 2, buffer, 256)) {
    LogisticsVariant* pVariant = new LogisticsVariant;
    ...
    row += 97;
}
```

This means adding a variant is purely additive: append one more well-formed 97-line block right after the last one, and the very next scan pass picks it up automatically. There is no separate "variant count" registry to update, and no risk of breaking the existing 3 stock variants as long as you don't touch their lines.

### The "Hidden" flag matters

Two of the three stock Atlas variants (`Claymore's Atlas`, `Renard's Atlas`) ship with `Identification,,Hidden,1,...` at row `21 + 97 * variantNum` of their block — `bHidden = true`, read at `code/logisticsvariant.cpp:337`. Hidden variants are parsed and exist internally but are **not shown in the normal mechbay purchase list** (they're reserved for named-pilot/story placements). The stock `Atlas Prime` (variant 0) has a blank `Identification` row — not hidden, and it's the only Atlas variant a normal player sees when buying an Atlas.

**If you want your new variant to actually show up as a purchasable choice, set `Hidden` to `0`** (or leave the field blank, like variant 0). This mod does that explicitly.

### What's *not* read by the engine (decorative CSV columns)

Only column 5 (`CompBas#`) of each `ItemNMasterId` row is read for gameplay — it's the `masterID` that indexes into `compbas.csv`'s loaded `MasterComponent::masterList` at runtime, which is where the weapon's real damage/heat/range/name actually come from (see the [Mechanism Map](../mechanism-map.md#weapons--equipment-compbascsv) weapon-name trap — same rule applies here). The `Name`, `Range`, `Heat`, `Weight`, `1Hit Damage`, `Dam/10Sec`, `BR`, and `C-Bills` columns on each item row, and the whole per-variant `Totals` line, are **not read anywhere** in `code/mech.cpp` or `code/logisticsvariant.cpp` (confirmed by grep — the only rows those two files read are chassis-header rows 1–18 and the per-variant offsets in the table above). They exist purely so a human editing the CSV in a spreadsheet can sanity-check a loadout at a glance. This mod keeps them consistent with the real `compbas.csv` values for readability, but a stale decorative column would **not** break or change gameplay.

## 2. What this mod does

Swaps both arm weapons on a new 4th Atlas variant from **Clan ER PPC** (`compbas.csv` masterID 154) to plain **ER PPC** (masterID 146) — same weight (6 tons) and heat (15) per weapon, so the block's decorative `Weight`/`Heat` totals stay accurate even though we didn't touch them. Appends the new 97-line block after the stock file's 3 existing variants; the stock 309 lines are untouched byte-for-byte.

| Field | Stock (`Renard's Atlas`, cloned as the template) | This mod's new variant |
|---|---|---|
| `VariantID` | `0002` | `0003` |
| `VariantName` | `Renard's Atlas` | `Widowmaker's Atlas` |
| `Hidden` | `1` (not purchasable) | `0` (purchasable) |
| Left arm weapon (`Item22MasterId`, col 5) | `154` (Clan ER PPC) | `146` (ER PPC) |
| Right arm weapon (`Item30MasterId`, col 5) | `154` (Clan ER PPC) | `146` (ER PPC) |
| Everything else (armor, other 70 inventory slots, critical-space slot map) | — | byte-identical to `Renard's Atlas` |

Both `masterID` values (154, 146) are verified live, non-`Removed`/non-`undefined` rows in `compbas.csv` (`A:/Games/mc2-srcdata/objects/compbas.csv:148,156`):

```
146,EnergyWeapon,ER PPC,3,6.67,15.00,6.00,7.00,649.96,8000,long,No,Yes,Yes,Yes,Yes,Yes,No,No,0,0,5,0,MCL_MC_ERPPC.tga,MCL_MC_ERPPC,32146,34146,1,3
154,EnergyWeapon,Clan ER PPC,3,4.55,15.00,6.00,7.00,952.79,14900,long,No,Yes,Yes,Yes,Yes,Yes,No,No,0,0,5,0,MCL_MC_ClanERPPC.tga,MCL_MC_ClanERPPC,32154,34154,1,3
```

In-game the weapon will display as **"ER PPC"** — that text comes from string resource `32000 + 146 = 32146`, which resolves to `"ER PPC"` in `data/defs/text/en_us/mc2res_legacy.fit:24277-24284` (not from the CSV's own `Name` column — see the decorative-columns note above).

**Where the stock reference came from:** this repo's own `data/` tree has no `data/objects/` folder (it only ships `art/`, `defs/`, `hdr/`, `model_overrides/`). The stock `atlas.csv` used as the base for this mod, and the `compbas.csv` values quoted above, come from the `A:/Games/mc2-srcdata/objects/` sibling checkout — a known-good reference copy of the stock data tree. If your own install has a modified `atlas.csv` (another mod, prior hand-edits), diff against your own copy before assuming this one matches.

## 3. The payload in this folder

```
mod/
  mod.json
  data/
    objects/
      atlas.csv   <- whole-file shadow of data/objects/atlas.csv:
                     stock 309 lines unchanged, + 97 new lines (variant 3)
```

`mod/mod.json`:

```json
{
  "id": "variant-add-example",
  "name": "Atlas Variant Add Example",
  "type": "campaign",
  "version": "1.0.0",
  "description": "Tutorial mod: adds a 4th Atlas variant (Widowmaker's Atlas) as a pure-data CSV block.",
  "dependencies": []
}
```

The new block, exactly as appended (lines 310–406 of the mod's `atlas.csv`; line 310 is the blank separator line, matching the same pattern before every other variant block):

```
VariantInfo,,,,,,,,,,,,,
Identification,,Hidden,0,,,,,,,,,,
VariantID,0003,,,,,,,,,,,,
VariantName,Widowmaker's Atlas,,,,,,,,,,,,
...(unchanged 72-item inventory, critical-space slots, armor rows — cloned from Renard's Atlas)...
Item22MasterId,18,-1,-1,146,ER PPC,long,15.00,6.00,7.00,10.49,650,8000,LEFT ARM : ...
...
Item30MasterId,18,-1,-1,146,ER PPC,long,15.00,6.00,7.00,10.49,650,8000,RIGHT ARM : ...
...
CurrentInternalStructure,3,17,17,21,21,31,21,21,,,
CurrentArmorPoints,9,4,5,7,8,9,8,9,2,2,4
```

The `ComponentPlacement`/`Slot1`–`Slot12` rows and `CurrentInternalStructure`/`CurrentArmorPoints` rows reference **item indices** (0–71), not `masterID` values — since we only changed the weapon *inside* the existing item-22 and item-30 slots (not which slots hold weapons), those rows needed no changes and are byte-identical to `Renard's Atlas`'s block.

**Known limitation, left as-is on purpose:** the appended block's `Totals` line still shows `Renard's Atlas`'s numbers, not recalculated for the ER PPC swap. Because ER PPC and Clan ER PPC have identical `Weight`/`Heat` per compbas.csv (both 6 tons, 15 heat), the `Totals` row's `Weight` and `Heat` columns happen to still be accurate — but its `1Hit Damage`/`Dam/10Sec`/`BR`/`C-Bills` sums are stale (they were computed from the old Clan ER PPC's recycle time and resource-point cost). As established above, this line is never read by the engine — it's a stale-but-harmless spreadsheet convenience column. If you want it fully accurate for your own bookkeeping, recompute it by hand; the mod works identically in-game either way.

## 4. Install and activate

**Do not point this at this repo's own dev deploy roots** — `mc2mod.py install` refuses those anyway ([Chapter 0 §5](../getting-started.md#5-packaging-and-installing-with-mc2modpy)). Use your own copy of the game.

### Install — Option A: copy the folder directly (fastest for iterating)

```powershell
Copy-Item -Recurse "<this-repo>\docs\modding\examples\variant-add\mod" "<your-game-folder>\mods\variant-add-example"
```

### Install — Option B: package + install with `mc2mod.py`

```powershell
cd <this-repo>
py -3 tools\mc2mod\mc2mod.py pack docs\modding\examples\variant-add\mod --out .\dist
py -3 tools\mc2mod\mc2mod.py verify-lite .\dist\variant-add-example-1.0.0.mc2mod
py -3 tools\mc2mod\mc2mod.py install .\dist\variant-add-example-1.0.0.mc2mod --deploy "<your-game-folder>"
```

### Activate — Option A: PowerShell

```powershell
cd <your-game-folder>
$env:MC2_ACTIVE_MOD = "variant-add-example"
.\mc2.exe
```

### Activate — Option B: the launcher picker

Run `mc2_launcher.exe` in your game folder, pick **Atlas Variant Add Example** from the list (instead of "Base Game"), and it launches `mc2.exe` with the env var set for you.

### What you should see

Open the mechbay/logistics screen and select the **Atlas** chassis. The variant list (previously just `Atlas Prime` — the only non-hidden stock variant) should now show a second entry, **`Widowmaker's Atlas`**, as a purchasable choice. Selecting it and inspecting its loadout should show **ER PPC** in both arm hardpoints instead of the stock `Renard's Atlas`'s **Clan ER PPC** (which itself stays hidden and unpurchasable, unaffected by this mod). Everything else about the mech — tonnage, armor values, walk animation, silhouette — is identical to the other Atlas variants, per the [variant vs. mesh](#variant-vs-mesh--read-this-first) distinction above.

## 5. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| New variant doesn't appear in the mechbay list at all | Mod not active, or wrong folder name | Confirm `mods/variant-add-example/mod.json` exists and `MC2_ACTIVE_MOD=variant-add-example` (must match `mod.json`'s `id`, or the folder name if `mod.json` is missing) |
| Mod is active but variant still missing | Stale mod index cache | `$env:MC2_REBUILD_MOD_CACHE = "1"` then relaunch (see [Chapter 0 — the stale-cache trap](../getting-started.md#the-stale-cache-trap)) |
| Not sure your file is even being picked up | — | `$env:MC2_LOG_FILE_RESOLVE = "1"` then relaunch; watch stdout for a line resolving `data/objects/atlas.csv` to `mod:variant-add-example`, and for `[mod] [1/N] active 'variant-add-example': <count> files` |
| Only 3 variants show, never a 4th, even with the mod confirmed active | Your appended block isn't exactly 97 lines, or has a blank/malformed row before it | The mechbay's discovery loop (`code/logisticsdata.cpp:319-332`) stops the instant `readString` fails at the expected row — any drift in line count between blocks silently truncates the scan. Diff your file against this folder's `mod/data/objects/atlas.csv` line-by-line; the new block must start exactly at line 310 |
| Crash or garbage stats on mission load with a squad using variant 3 | Row/column count mismatch inside the new block | `BattleMech::init` (`code/mech.cpp:1405`) is far less forgiving than the mechbay scan — a malformed block can misread armor/inventory columns. Re-diff against the shipped block; only the two `masterID` values (col 5, item rows 22 and 30), `VariantID`, `VariantName`, and `Hidden` should differ from `Renard's Atlas`'s block |
| New variant shows up but is still "hidden"/unpurchasable | `Hidden` flag left at `1` | Confirm the `Identification` row of your new block reads `Identification,,Hidden,0,...` (row `21 + 97*variantNum`, column 4) |

## Coming soon: branch-only unit-profile system

This doc's technique — append a fixed 97-row block to a chassis CSV, one weapon `masterID` per hardpoint — is the mechanism that **ships today, on this checkout**. There is a separate, more ergonomic data-driven unit/equipment system (`MC2_UNIT_PROFILE_DATA`, `MC2_EQUIPMENT_ADD`) that exists only on the **unmerged local branch `claude/unit-profile-seam`** (tip `59f16541`) — it is **not present on this checkout**, `grep`ing either env var here returns nothing, and setting them here does nothing. If you're reading this on a checkout that doesn't have that branch merged in, ignore any other doc that references those two env vars — use the CSV-block technique in this doc instead.

## 6. Related mechanisms (not covered here)

- **New mech archetypes** (a genuinely new mech type, not a variant of an existing chassis) require a new `ObjectClass` enum value — that's engine-source modding, out of scope for data mods. See the [Mechanism Map](../mechanism-map.md#mechs).
- **Swapping the 3D model/mesh** for a variant is the `[Import]` block mechanism in `data/tgl/<mech>.ini`, completely separate from this CSV — see the mesh-import example doc and the [variant vs. mesh](#variant-vs-mesh--read-this-first) table above.
- **Adding a genuinely new weapon** beyond the fixed 255-row `compbas.csv` table is not possible on this checkout — see the [weapon-edit example](../weapon-edit/README.md) and the [Mechanism Map's "Branch-only" section](../mechanism-map.md#branch-only-not-present-on-this-checkout).
- **Renaming a weapon or changing its mechlab display text** requires editing the string-resource `.fit` file, not `compbas.csv` or `atlas.csv` — not covered in this doc.

## Validator output (run against this folder's payload)

```
$ py -3 tools/mc2mod/mc2mod.py pack docs/modding/examples/variant-add/mod --out <tmp>
pack: <tmp>/variant-add-example-1.0.0.mc2mod
  id=variant-add-example  version=1.0.0  files=2  sha256=<64-hex-digest>

$ py -3 tools/mc2mod/mc2mod.py verify-lite <tmp>/variant-add-example-1.0.0.mc2mod
verify-lite PASS: id=variant-add-example  version=1.0.0  files=2  pkg_sha256=<same 64-hex-digest as above>
```

**Note on the sha256 value:** `mc2mod.py pack` doesn't strip per-file zip timestamps, so the digest is **not** reproducible across runs even against this exact, unchanged payload — two consecutive `pack` invocations in this checkout produced `e61b4251b65cfb73287da9754193b1993278bc8d6c1fb63678674ad3017c57d2` and `99571f2073b25b582715dabcb1035ffcdbd30bdfd02488846e3b1a4afcd977be`. Both runs still reported `id=variant-add-example version=1.0.0 files=2` and `verify-lite PASS` with the digest matching between `pack`'s own output and `verify-lite`'s re-read of the same file — that agreement (not the literal hex value) is the thing to check. Don't diff your own `pack` output's hash against any hash printed in this doc; diff the `pack` line against the matching `verify-lite` line from the *same* run instead.

Also confirmed with `tools/mod_install/resolver.py` (the offline replica of `mclib/file.cpp`'s resolution ladder) against a synthetic deploy tree: requesting `data/objects/atlas.csv` with `active_mod="variant-add-example"` resolves to layer `mod:variant-add-example` with `base-loose` recorded as shadowed — i.e. this mod's file wins over a stock `data/objects/atlas.csv`, exactly as the whole-file-shadow mechanism promises.

Line-count / block-boundary self-check on the payload itself:

```
$ python3 -c "print(len(open('mod/data/objects/atlas.csv','rb').read().split(b'\r\n'))-1)"
406   # 309 stock lines + 97 new-variant lines, verified against BattleMech::init's
      # 23 + 97*variantNum formula: new VariantName lands on line 314 = 23 + 97*3
```
