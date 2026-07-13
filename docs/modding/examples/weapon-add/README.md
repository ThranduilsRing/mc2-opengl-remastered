# Example Mod — Add a Weapon (Fill an Undefined `compbas.csv` Row)

> **AVAILABILITY**
> - **Works on:** this checkout. Every mechanism this doc relies on was spot-checked present at the anchors cited below: `mclib/file.cpp`'s mod-overlay block, `mclib/cmponent.cpp`'s `MasterComponent::loadMasterList`/`initEXCEL`, and `GameOS/gameos/MC2Strings.cpp`'s FIT-backed string catalog — including its mod-overlay text loading *and* its `LegacyTextAlias` precedence layer, which the base game uses on every stock `undefined` row and which a rename mod must also override (see [§2](#2-the-display-name-wall--and-the-data-only-way-through-it)). This is a pure data-mod technique — no branch-only engine feature is involved, no rebuild needed.
> - **Core env var:** `MC2_ACTIVE_MOD` — unset by default. Unset = base game, stock `compbas.csv` and stock strings apply.
> - **Diagnostics env vars:** `MC2_LOG_FILE_RESOLVE` (0/1/2, default 0 = off), `MC2_REBUILD_MOD_CACHE` (any value = force rebuild, default off), `MC2_STRINGS_TRACE` (any value = on, default off, appends to `mc2-strings.log` next to `mc2.exe`), `MC2_LOG_STRINGS` (any value = on, default off, stdout). See [§6 Troubleshooting](#6-troubleshooting).
> - **No build required.** Whole-file CSV shadow + one small additional string-override FIT file. You never touch `shaders/` or C++ source, and you never touch the `mc2.exe` binary.
> - **`MC2_EQUIPMENT_ADD` / `MC2_UNIT_PROFILE_DATA` do not exist on this checkout** — they live only on the unmerged local branch `claude/unit-profile-seam` (tip `59f16541`). See the "Coming soon" box in [§4](#4-coming-soon-the-branch-only-equipment-add-registry).

Before you start, read [Chapter 0 — Getting Started](../getting-started.md) (mod folder anatomy, `MC2_ACTIVE_MOD`, the launcher picker, `mc2mod.py` packaging, diagnostics) and skim the [Mechanism Map](../mechanism-map.md) rows for `compbas.csv` and the mod-overlay core. This doc assumes you already know what `mods/<id>/data/...` means. It's also worth skimming the sibling [`weapon-edit` example](../weapon-edit/README.md) — it covers *editing* an existing weapon's stats and has the full `compbas.csv` column-by-column reference; this doc covers *adding* a new one and focuses on the two things that are different: filling a free row, and the display-name trap.

## What you'll do

1. Fill row 63 of `compbas.csv` — currently `Type=undefined`, i.e. a free/unused master-component slot — with a new light energy weapon, "Prototype Needle Laser".
2. Ship a small string-override FIT file that gives it a real in-game display name and abbreviation instead of the stock placeholder text "undefined" — this needs *two* block types (`String` + `LegacyTextAlias`), not just one; see [§2](#2-the-display-name-wall--and-the-data-only-way-through-it) for why.
3. Verify both pieces load, using the engine's own trace logs and code-level reasoning (no rebuild, no guessing).

## 1. Why a "free row" instead of a new row

`compbas.csv` is the master component/weapon table, loaded by `MasterComponent::loadMasterList` (`mclib/cmponent.cpp:630-678`) into a **fixed-size 255-row array** — the row count is hard-coded `255` at both call sites (`code/mission.cpp:1974`, `code/saveload.cpp:846`), not read from the file. `initEXCEL` (`cmponent.cpp:87-179`) parses each line positionally with `strtok(..., ",")`, and elsewhere in the engine components are looked up by **direct array index** on `masterID` — e.g. `MasterComponent::masterList[inventory[curItem].masterID]` (`code/gvehicl.cpp:1082` and about a dozen more call sites in `gvehicl.cpp`/`gameobj.cpp`). That means:

- You cannot add a 256th row — there is no row 256.
- The row's position in the file **must equal** its own `masterID` value in column 0, or every other system that indexes `masterList[masterID]` reads the wrong component.
- "Adding" a weapon on this checkout means finding a row whose `Type` column already reads `undefined` and filling it in — which is exactly what the stock game's own unused-slot rows are for.

The `Type` field is checked before anything else is parsed (`cmponent.cpp:105-114`):

```cpp
for (formIndex = 0; ComponentFormString[formIndex] != NULL; formIndex++)
    if (strcmp(field, ComponentFormString[formIndex]) == 0)
        break;
if (ComponentFormString[formIndex] == NULL)
    return(-1);

if (strcmp(field, "undefined") == 0) {
    masterID = -1;
    return(NO_ERR);
}
```

Row 63 in the stock table is one such free slot:

```
63,undefined,undefined,0,na,na,0,na,0,0,0,0,0,0,0,0,0,0,0,na,0,0,0,0,0,32063,34063
```

This example fills it in as an `EnergyWeapon`:

```
63,EnergyWeapon,Prototype Needle Laser,1,3.50,2.00,1.00,1.50,55.09,1200,short,No,Yes,Yes,Yes,Yes,Yes,No,No,0,0,12,0,MCL_MC_Laser.tga,MCL_MC_Laser,32063,34063,1,1
```

That's `masterID=63` (unchanged position), `Type=EnergyWeapon`, 1 critical slot, 3.50s recycle, 2.00 heat, 1.00 ton, 1.50 damage/shot, `short` range, mountable everywhere except head, `Special FX ID=12` (reused from the stock `Laser` row — it binds to an *existing* `effects.csv` entry so this mod doesn't need to add new visual-effect data; see [Related mechanisms](#6-related-mechanisms-not-covered-here)), no ammo, and stock `Laser` icon art reused (`MCL_MC_Laser.tga`) since this tutorial doesn't ship new icon textures. Column layout, full field-by-field mapping, and the trap that the `Name` column (column 2) is parsed and then discarded (`cmponent.cpp:118`, `// name, ignore`) are all covered in the [`weapon-edit` example's column table](../weapon-edit/README.md#1-why-the-whole-file-and-where-the-truth-column-names-come-from) — not repeated here.

One more trap worth calling out here: the CSV header (row 1 of `compbas.csv`) names the last two columns of that line `String Index` (`32063`) and `Mechlipidia String` (`34063`). Neither one is read by `initEXCEL` — the parser's `strtok` walk stops after `Ammo Master ID` (column 22) for this component form (`cmponent.cpp:193-196`) and never reaches columns 25/26 at all. The actual rename mechanism (§2 below) is computed purely from `masterID` (`COMPONENT_NAME_START+masterID` / `COMPONENT_ABBR_START+masterID` = `32063`/`33063`), not read from either of these columns. `String Index`'s value happens to equal `32063` by convention (it matches what the engine *would* compute anyway), but it's decorative — the engine never looks at it. `Mechlipidia String`'s `34063` is likewise unused by anything in this tutorial's chain and is left untouched. Don't confuse it with `33063` (the abbreviation legacyId) — they're unrelated numbers in unrelated systems.

**Stock reference used:** `A:/Games/mc2-srcdata/objects/compbas.csv` (256 lines: 1 header + 255 data rows). Byte-identical to the copy shipped at `tools/mc2weapon/compbas.csv` in the `A:/Games/mc2-opengl/mc2-win64-v0.4/` deploy tree (diffed, no differences) — this repo itself does not ship a loose `data/objects/compbas.csv` (it's packed into a FastFile in a real install).

## 2. The display-name wall — and the data-only way through it

The `Name` column is a trap, as the `weapon-edit` doc notes: it's parsed and discarded. The actual in-game name and abbreviation come from string resources `32000+masterID` / `33000+masterID` (`COMPONENT_NAME_START`/`COMPONENT_ABBR_START`, `cmponent.cpp:62-63`), read via `cLoadString` right when the row is parsed (`cmponent.cpp:100-101`):

```cpp
masterID = atoi(field);
cLoadString(COMPONENT_NAME_START+masterID, name, MAXLEN_COMPONENT_NAME);
cLoadString(COMPONENT_ABBR_START+masterID, abbreviation, MAXLEN_COMPONENT_ABBREV);
```

`cLoadString` (`mclib/utilities.h:100-114`) → `gos_GetResourceString` (`GameOS/gameos/gameos_res.cpp:168-184`) → `MC2Strings::GetByLegacyId(32063)` (and `33063` for the abbreviation).

### It's not just a `String{}` override — there's an alias layer in front of it

`GameOS/gameos/MC2Strings.cpp` builds an in-memory string catalog once per process (`MC2Strings::Load()`, line 818): it scans the base `data/defs/text/en_us/` directory, then — after the mod index is built — calls `EnumerateModFitFiles()` to load every `.fit` file any active mod ships under that same path (`Load()`, `MC2Strings.cpp:865-876`; `LoadModFits()`, `MC2Strings.cpp:1044-1072`, wired in at `code/mechcmd2.cpp:1247-1248`). That much matches the naive "ship a `String{}` override" plan. But `GetByLegacyId()` does **not** go straight to the `String{}`/`legacyId` table. It checks a *separate* alias table first, and only falls through if the alias lookup comes up empty (`MC2Strings.cpp:911-937`):

```cpp
const char* GetByLegacyId(long legacyId)
{
    if (legacyId <= 0)
        return 0;

    Load();

    /*
        Lookup order:
        1. Explicit LegacyTextAlias to the clean localization layer.
        2. Direct mc2res_legacy/String legacyId coverage.
        3. Missing marker at caller.
    */
    const std::map<long, std::string>::const_iterator aliasIt = State().keyByLegacyAlias.find(legacyId);
    if (aliasIt != State().keyByLegacyAlias.end())
    {
        const char* aliasText = Get(aliasIt->second.c_str());
        if (aliasText && aliasText[0])
            return aliasText;                 // <-- returns here; keyByLegacyId is never consulted
    }

    const std::map<long, std::string>::const_iterator it = State().keyByLegacyId.find(legacyId);
    if (it == State().keyByLegacyId.end())
        return 0;

    return Get(it->second.c_str());
}
```

`keyByLegacyAlias` is populated by `LegacyTextAlias { legacyId = ...; textKey = ... }` blocks, unconditionally overwritten on every load (`StoreLegacyAlias`, `MC2Strings.cpp:616-627` — no source-precedence check, just last-write-wins). The stock game ships `data/defs/text/en_us/mechlopedia_text_bridge.fit`, which contains exactly such a block for masterID 63's legacyIds:

```
LegacyTextAlias {
    legacyId = 32063
    textKey = "mechlopedia.component.063_undefined.name"
}
```

...and `"mechlopedia.component.063_undefined.name"` resolves to the non-empty literal text `"undefined"` (same file, a `String{}` block a few thousand lines earlier). Since `aliasText` is non-empty, `GetByLegacyId(32063)` returns `"undefined"` **and returns immediately** — a `String{}`/`legacyId=32063` override sitting only in `keyByLegacyId` is never reached, no matter what its `source` field says. This isn't unique to row 63: `mechlopedia_legacy_aliases.fit` ships 1050 `LegacyTextAlias` blocks, one per stock `undefined` masterID slot (there are 125 of them in `compbas.csv`), so **every** free row you might fill in has this same alias sitting in front of it.

### The actual data-only fix: override the alias too

Because `StoreLegacyAlias` always overwrites and the mod's FITs always load after the base directory scan (`Load()`, `MC2Strings.cpp:865-876`; base-directory files are also loaded in sorted order, `TryLoadDirectory`, `MC2Strings.cpp:726-742`), a mod can win deterministically by shipping its **own** `LegacyTextAlias` block that redirects the legacyId to the mod's own key. This is not a workaround improvised for this doc — it's the pattern the engine's own maintainers left a comment about for exactly this purpose (`MC2Strings.cpp:1051`, referring to a different mod's alias-correction file):

```cpp
// Last-write-wins: z_cveg_aliases.fit (sorted after mechlopedia_legacy_aliases.fit)
// will overwrite any wrong keyByLegacyAlias entries with correct cveg keys.
```

So the fix needs two blocks per legacyId, not one: a `String{}` block defining the mod's text, and a `LegacyTextAlias{}` block pointing that legacyId at the mod's key. This mod's override file, `mod/data/defs/text/en_us/weapon_add_strings.fit`:

```
String {
    key = "mod.weapon-add.component.63.name"
    text = "Prototype Needle Laser"
    legacyId = 32063
    source = "mod:weapon-add"
}

LegacyTextAlias {
    legacyId = 32063
    textKey = "mod.weapon-add.component.63.name"
}

String {
    key = "mod.weapon-add.component.63.abbrev"
    text = "P.Needle"
    legacyId = 33063
    source = "mod:weapon-add"
}

LegacyTextAlias {
    legacyId = 33063
    textKey = "mod.weapon-add.component.63.abbrev"
}
```

**So: yes, a data-only rename path exists on this checkout — but it requires the alias redirect, not just a `String{}` override.** A `String{}` block alone (as an earlier draft of this doc shipped) silently loses to the stock alias for every one of the 125 stock `undefined` rows and leaves the weapon showing "undefined" in-game with no error anywhere.

Syntax rules that matter and are easy to get wrong (verified against the parser, `MC2Strings.cpp:268-463,616-627`):

- `key` must be **globally unique** across the entire string catalog (first `key` wins if duplicated, `StoreStringRecord`, `MC2Strings.cpp:546-552`) — hence the `mod.weapon-add.` prefix. Two mods both using a generic key like `"my_weapon_name"` would silently collide.
- `LegacyTextAlias` blocks only read `legacyId` and `textKey` (`MC2Strings.cpp:699-704`) — a `source` field on that block type is parsed by nothing and has no effect; don't rely on it for anything.
- The `String{}` block's own `source` field (`"mod:weapon-add"` here) still matters for the *fallback* path — if some other legacyId you touch has no colliding alias, the `!= "mc2res"` precedence rule from `StoreStringRecord` (`MC2Strings.cpp:557-596`) is what lets that `String{}` win on its own. Keep setting it even though row 63 specifically needs the alias redirect too.

## 3. The payload in this folder

```
mod/
  mod.json
  data/
    objects/
      compbas.csv                          <- whole-file shadow, row 63 filled in
    defs/
      text/
        en_us/
          weapon_add_strings.fit           <- additive string override for legacyId 32063/33063
```

`mod/mod.json`:

```json
{
    "id": "weapon-add",
    "name": "Example Mod: Add a Weapon",
    "type": "campaign",
    "description": "Tutorial mod that fills an undefined compbas.csv row with a new weapon and renames it via a string-override FIT file.",
    "version": "1.0.0",
    "dependencies": []
}
```

## 4. "Coming soon": the branch-only Equipment ADD registry

If you've seen references elsewhere to `MC2_EQUIPMENT_ADD` or an `EquipmentDefinition` facade that lifts the fixed-255-row cap — **that mechanism does not exist on this checkout.** It lives only on the local, unmerged branch `claude/unit-profile-seam` (tip `59f16541`); `grep`ing this repo's working tree for `MC2_EQUIPMENT_ADD` returns nothing. When/if that branch merges, adding a weapon should stop requiring a free row at all. Until then, filling an `undefined` row (this doc) is the way to add a weapon on this checkout.

## 5. Install it into your own game folder

**Do not point this at this repo's own dev deploy roots** — `mc2mod.py install` refuses those anyway ([Chapter 0 §5](../getting-started.md#5-packaging-and-installing-with-mc2modpy)). Use your own copy of the game.

### Option A — copy the folder directly (fastest for iterating)

```powershell
Copy-Item -Recurse "<this-repo>\docs\modding\examples\weapon-add\mod" "<your-game-folder>\mods\weapon-add"
```

### Option B — package + install with `mc2mod.py`

```powershell
cd <this-repo>
py -3 tools\mc2mod\mc2mod.py pack docs\modding\examples\weapon-add\mod --out .\dist
py -3 tools\mc2mod\mc2mod.py verify-lite .\dist\weapon-add-1.0.0.mc2mod
py -3 tools\mc2mod\mc2mod.py install .\dist\weapon-add-1.0.0.mc2mod --deploy "<your-game-folder>"
```

### Activate

```powershell
cd <your-game-folder>
$env:MC2_ACTIVE_MOD = "weapon-add"
.\mc2.exe
```

Or run `mc2_launcher.exe` in your game folder and pick **Example Mod: Add a Weapon** from the list instead of "Base Game".

### What you should see

This is a data-catalog change, not a mech loadout change — filling row 63 makes `masterID` 63 a valid, named `EnergyWeapon` in the master table, but nothing in the stock game *equips* it on any mech by default (that's a separate mechanism — appending an inventory-row block to a chassis CSV like `atlas.csv`, xref'd by `masterID`; see the [Mechanism Map's Mechs section](../mechanism-map.md#mechs) — out of scope here). Combine that with the display-name alias trap from [§2](#2-the-display-name-wall--and-the-data-only-way-through-it) — there is no single log line that says "the rename worked" — and the honest "did it work" check for this doc is mostly the engine's own load-time diagnostics plus code-level reasoning, not a screenshot:

1. **Confirm the mod's files were resolved.** Set `MC2_LOG_FILE_RESOLVE=1` and launch. Watch stdout for:
   - `[mod] [1/N] active 'weapon-add': 2 files`
   - A resolve line for `data/objects/compbas.csv` naming layer `mod:weapon-add`
   - A resolve line for `data/defs/text/en_us/weapon_add_strings.fit` naming layer `mod:weapon-add`

2. **Confirm the FIT file itself parsed correctly.** Set `MC2_STRINGS_TRACE=1` and launch, then check `mc2-strings.log` (written next to `mc2.exe`) for a per-file summary line for your mod's file:
   ```
   loaded 2 String, 0 ResourceTextAlias, 2 LegacyTextAlias, 0 MenuTextBinding from ...\weapon_add_strings.fit
   ```
   (emitted from `LoadFile`, `MC2Strings.cpp:721-723`, for *every* `.fit` file loaded, mod or base). `2 String` + `2 LegacyTextAlias` confirms all four blocks in the file were recognized as valid typed blocks — a `0 LegacyTextAlias` count here means the redirect blocks weren't parsed (check for stray characters or unbalanced braces) and the rename will silently lose to the stock alias (see [§2](#2-the-display-name-wall--and-the-data-only-way-through-it)).

   Note there is **no** separate "override won" trace line to grep for beyond this — `StoreLegacyAlias` (`MC2Strings.cpp:616-627`) overwrites its map entry silently, with no `Trace()` call. The override's correctness instead follows from two structural facts you can verify directly in the source rather than from a runtime log: mod FITs always load strictly after the full base-directory scan (`Load()`, `MC2Strings.cpp:865-876`), and `StoreLegacyAlias` always overwrites unconditionally regardless of source — so whichever `LegacyTextAlias{legacyId=32063,...}` block loads *last* wins, and the mod's is always last.

3. **See it in-game.** Row 63 isn't equipped on any mech by default (see above), so the only first-party UI surface guaranteed to call `cLoadString(32063, ...)` is wherever the stock game itself would have shown "undefined" for this slot — e.g. an in-game component/logistics screen that lists master components by ID rather than by mech inventory, if your build exposes one. If you don't have an easy screen to check, checks 1+2 plus the code-level reasoning above are the doc's supported verification path; equipping the weapon on a mech (§7, out of scope here) is the most certain way to see it rendered.

Checks 1 and 2 are independent of any specific mission or mech and don't require finding row 63 mounted on anything.

## 6. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| No `[mod]` lines at all in stdout | Mod not active | Confirm `mods/weapon-add/mod.json` exists and `MC2_ACTIVE_MOD=weapon-add` (must match `mod.json`'s `id`, or the folder name if `mod.json` is missing) |
| `[mod]` lines appear but `compbas.csv` doesn't resolve to `mod:weapon-add` | Stale mod index cache | `$env:MC2_REBUILD_MOD_CACHE = "1"` then relaunch — see [Chapter 0's stale-cache trap](../getting-started.md#the-stale-cache-trap) |
| Game crashes or components look wildly wrong on load | Row count mismatch, or a row corrupted while editing | Your `compbas.csv` must have exactly 1 header line + 255 data rows, comma-delimited. Diff your file against the one shipped in this folder — only row 63 (line 65) should differ |
| Row still shows "undefined" even though check 2's trace shows `2 LegacyTextAlias` loaded from your file | You shipped only `String{}` blocks (no `LegacyTextAlias{}` redirect) — the stock `mechlopedia_text_bridge.fit` alias for this legacyId wins every time, silently, regardless of your `String{}`'s `source` field | Add a `LegacyTextAlias { legacyId = <N>; textKey = "<your String key>" }` block for every legacyId you're renaming — see [§2](#2-the-display-name-wall--and-the-data-only-way-through-it). This is required for the ~125 stock `undefined` rows; it is *not* optional even with a non-`mc2res` `source` |
| `mc2-strings.log` shows `duplicate legacyId 32063 ... keeping key=...` instead of anything about your key | Two `String{}` blocks with the same `legacyId` and neither is `source = "mc2res"` — `StoreStringRecord` (`MC2Strings.cpp:587-594`) keeps whichever loaded first in that case. This is expected/harmless for row 63 specifically (the base game's own `mechlopedia_text_bridge.fit` also registers a non-`mc2res` `String{}` for `32063`) — it's the `LegacyTextAlias` redirect above, not this line, that actually decides what's shown | No action needed if the alias redirect (previous row) is in place; the `keyByLegacyId` entry this line refers to is never reached once the alias resolves |
| Weapon still shows "undefined" in-game after checks 1+2 both pass | You're looking at a UI surface that doesn't call `cLoadString(32063, ...)` at all, or a cached/previous session's data | Restart `mc2.exe`; `MC2Strings::Load()` is cached for the life of the process (`MC2Strings.cpp:818-822`, `state.attempted` guard) |
| Not sure your FIT file's syntax parsed at all | Malformed block (stray character before `{`, unbalanced quotes) | Set `MC2_LOG_STRINGS=1` and check stdout/`mc2-strings.log` trace counts (`loaded %ld String, ... from %s`, `MC2Strings.cpp:721-723`) for your file's path — a `0 String` / `0 LegacyTextAlias` count means the block wasn't recognized |

## 7. Related mechanisms (not covered here)

- **Weapon → visual effect binding** (`Special FX ID` → `effects.csv`) — this mod reuses an existing effect (ID `12`, the stock `Laser`'s trail/flare/hit effect) rather than adding a new one. See [Part 1 of `gosfx-modder-dropin-path.md`](../../../gosfx-modder-dropin-path.md#part-1--binding-works-today-csv-only) for the full chain if you want a custom effect instead.
- **Editing an existing weapon's stats** (rather than filling a free row) — see the sibling [`weapon-edit` example](../weapon-edit/README.md), including the full `compbas.csv` column reference table.
- **Equipping the new weapon on a mech** — requires appending a 97-row variant inventory block to a chassis CSV (e.g. `data/objects/atlas.csv`), cross-referenced by `masterID` at `code/mech.cpp:1545` (`26 + 97*variantNum + curItem`, column 5). See the [Mechanism Map's Mechs section](../mechanism-map.md#mechs). Not covered in this doc.
- **The branch-only `MC2_EQUIPMENT_ADD` registry** that removes the fixed-255-row limitation — see [§4](#4-coming-soon-the-branch-only-equipment-add-registry) above and the [Mechanism Map's "Branch-only" section](../mechanism-map.md#branch-only-not-present-on-this-checkout).

## Validator output (run against this folder's payload)

```
$ py -3 tools/mc2mod/mc2mod.py pack docs/modding/examples/weapon-add/mod --out <tmp>
pack: <tmp>/weapon-add-1.0.0.mc2mod
  id=weapon-add  version=1.0.0  files=3  sha256=eb5d931a2263d13d5bdaab4cc9db38141a5035a3a8547cf5078469f0d07817e1

$ py -3 tools/mc2mod/mc2mod.py verify-lite <tmp>/weapon-add-1.0.0.mc2mod
verify-lite PASS: id=weapon-add  version=1.0.0  files=3  pkg_sha256=eb5d931a2263d13d5bdaab4cc9db38141a5035a3a8547cf5078469f0d07817e1
```

Note: the top-level package `sha256` embeds each file entry's own per-file hash inside the zip's `package.json`, but the zip container itself carries per-file OS timestamps, so the outer `sha256`/`pkg_sha256` will differ run-to-run even with byte-identical payload content — treat it as evidence the validator ran and passed, not as a fixed value to diff against.

Also confirmed with `tools/mod_install/resolver.py` (the offline replica of `mclib/file.cpp`'s resolution ladder) against a synthetic deploy tree with this mod installed and `active_mod="weapon-add"`:

```
data/objects/compbas.csv                    -> layer 'mod:weapon-add'
data/defs/text/en_us/weapon_add_strings.fit -> layer 'mod:weapon-add'
data/objects/effects.csv (not shipped)      -> layer 'MISS' (expected — falls through to base game in a real deploy)
```

Both payload files resolve to the mod layer, exactly as the whole-file-shadow mechanism promises.
