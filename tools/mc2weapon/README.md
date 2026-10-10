<!-- doc-status: current | reviewed:2026-10-11 -->
# mc2weapon

MechCommander 2 weapon viewer / modder tool. Reads every component from
`compbas.csv`, resolves each weapon's FX binding via `effects.csv`, and presents a
unified weapon catalog. Editor commands (edit stats/FX → loose mod overlay, create new
weapons) are specced in [docs/weapon-tool-design.md](../../docs/weapon-tool-design.md).

Python 3, stdlib only. No build, no engine. Weapon stats are CSV text — edits ship as a
loose `mods/<id>/data/objects/compbas.csv` overlay that the game resolves first (no
`.pak` repack needed).

## Usage

```sh
# List all stock weapons with stats + resolved FX (auto-detects the CSVs)
py -3 mc2weapon.py list

# Include non-weapon components; or emit JSON
py -3 mc2weapon.py list --all
py -3 mc2weapon.py list --json

# One weapon's full stats + FX (by masterID or name)
py -3 mc2weapon.py show 145            # PPC
py -3 mc2weapon.py show "Gauss Rifle"
py -3 mc2weapon.py show 104 --json

# Point at specific CSVs (otherwise auto-detected from source/deploy/mods)
py -3 mc2weapon.py --compbas path/to/compbas.csv --effects path/to/effects.csv list
```

## Editing (writes a loose mod overlay — no .pak repack)

Edits never touch the base game. They write a full `mods/<id>/data/objects/compbas.csv`
(only the edited cells differ) plus a `mod.json`. Launch the game with
`MC2_ACTIVE_MOD=<id>` and it resolves the overlay first. Every write is validated.

```sh
# The FX palette you can assign (effects.csv rows)
py -3 mc2weapon.py list-fx

# Edit stats -> overlay (cumulative; re-running edits the same overlay)
py -3 mc2weapon.py set 145 damage=15 heat=8 --mod my-weapons
py -3 mc2weapon.py set "Gauss Rifle" recycle=4 range=medium --mod my-weapons

# Assign FX by id or trail-effect name (validated against effects.csv)
py -3 mc2weapon.py set-fx 145 gauss_trail --mod my-weapons
py -3 mc2weapon.py set-fx 104 10 --mod my-weapons

# Create a weapon in an unused masterID slot (defaults fill unspecified stats)
py -3 mc2weapon.py new 5 "Plasma Cannon" --type EnergyWeapon \
    damage=12 heat=9 recycle=5 range=long fxid=4 --mod my-weapons

# The "just works" gate — validate a compbas (yours or the base)
py -3 mc2weapon.py validate mods/my-weapons/data/objects/compbas.csv

# Then: copy mods/my-weapons/ into the game dir and run MC2_ACTIVE_MOD=my-weapons
```

Editable fields: `damage heat recycle range tons slots missileType fields fxid
ammoMasterId name type`. `--mod-root <dir>` chooses where mod folders live
(default `mods`). `--force` overrides the "slot already a weapon" guard on `new`.

## Not yet (see design doc)

- Bolt *visual* edits (texture/color/length) live in `object2.pak` `.fit` packets — a
  later slice (PacketFile write).
- Range-in-meters is global (`gamesys.fit`), not per-weapon; only the bracket
  (short/medium/long) is editable here.
- A GUI (ImGui) workbench with FX curve preview.

## What it shows

- **Stats** (from `compbas.csv`): damage, heat, recycle (cooldown s), range bracket
  (short/medium/long), tonnage, slots (crit-hits), BR, RP, missile type, fields.
- **FX** (from `compbas` Special FX ID → `effects.csv` row): trail / muzzle-flash /
  hit / miss effect names + object number.

## Notes / reality-checks

- **masterID = the weapon's identity** (its row index in compbas.csv). Mech loadouts
  reference weapons by masterID.
- **Range in meters** is global (`gamesys.fit [WeaponRanges]` × the per-weapon
  short/med/long tag); compbas only stores the bracket.
- **Weapons have no 3D mesh.** A weapon's visual is a procedural bolt (texture + gosFX
  particles), so "model" = bolt texture + FX, not a glb upload. (Glb model upload is a
  mech/prop feature via the model-override registry, a separate subsystem.)
