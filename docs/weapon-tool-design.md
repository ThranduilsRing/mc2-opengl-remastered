<!-- doc-status: current | reviewed:2026-10-11 -->
# Weapon Viewer / Modder Tool — Design

`tools/mc2weapon/` — view, edit, and create MC2 weapons (stats + FX), output as a
loose mod overlay that "just works." Mirrors the `tools/mc2fx` modder-tool pattern
but for the weapon data layer. **Viewer slice shipped; editor slices specced below.**

Recon backing this: the 3 scouts mapped in this session (weapon stats, pak/fit/fst
formats, mc2fx architecture). File:line evidence is in those scout findings; the
load-bearing facts are summarized here.

---

## Data model (where weapon data lives)

| Layer | File | Format | Editable today |
|---|---|---|---|
| **Gameplay stats** | `compbas.csv` | loose CSV (1 header + 255 rows); **masterID = row index** | packed in v0.4, but **loose `mods/<id>/data/objects/compbas.csv` overlay works** |
| **FX binding** | `effects.csv` | loose CSV (skip 1 header; row N = Special FX ID N) | **loose, editable** |
| **Range meters** | `gamesys.fit` `[WeaponRanges]` | text INI (global brackets × per-weapon short/med/long) | packed |
| **Bolt visual** | `object2.pak` packet (`[BoltProjectileData]`) | `.fit` text-INI inside a PacketFile; object# = packet index | packed (.pak read+write exists, see below) |
| **Display name** | string tables (`32000+masterID`) | `.tab`/MCL strings | not in compbas |

**compbas.csv weapon columns** (`MasterComponent::initEXCEL`, `mclib/cmponent.cpp`):
`masterID, Type, Name, crit-hits(=slots), Recycle(=cooldown s), Heat, Weight(tons),
Damage, BR, RP, Range(short|medium|long), [8 location flags], MissileType
(0=Energy/1=Ballistic/LRM/ST/SRM), Fields(1=streak/2=inferno/4=LBX/8=Artillery),
SpecialFXID, AmmoMasterID, ...`. Weapons = `Type ∈ {EnergyWeapon, BallisticWeapon,
MissileWeapon}` (37 in stock). Stats are raw floats; range is a 3-bucket tag whose
meters come from `gamesys.fit`. Names come from string IDs, not the CSV Name column
(decorative). `SpecialFXID` → `effects.csv` row → `{effectName, muzzle, hit, miss,
objNum}` → `gosFX::EffectLibrary::Find(name)` + `object2.pak` packet.

### Two reality-checks (corrected the original ask)
1. **Weapons have NO 3D mesh.** A weapon's in-world visual is a *procedural bolt*
   (tube/quad with a `TextureName` + gosFX particles), not a glb. The
   `model_override_registry` (glb upload) covers **staticProp/tree only**, and bolts
   aren't meshes. So for a weapon, "model" = **bolt texture + muzzle/hit/miss FX**, not
   a glb upload. (Glb upload is a *mech/prop* feature, separate subsystem.) The tool's
   "model" surface is therefore: pick/assign the bolt texture + the FX set.
2. **No .pak repack needed for stats/FX.** compbas.csv is packed in v0.4, but the game
   resolves a loose `mods/<id>/data/objects/compbas.csv` first (mods already ship one).
   So the editor **writes a loose CSV overlay** → `MC2_ACTIVE_MOD=<id>` → it just works.
   `.pak`/`.fit` editing is only needed for the bolt *visual* (texture/color/length), a
   later slice — and the formats DO support write (PacketFile create/writePacket; the
   Python `tools/terrain_gen/pak_exporter.py` is a RAW-packet patcher precedent).

---

## Architecture

Standalone **Python** CLI (`tools/mc2weapon/mc2weapon.py`), stdlib-only, consistent
with `pak_exporter.py` / `fst_listing.py` / `mc2mod.py`. Unlike `mc2fx` (C++, because
it reuses engine gosFX binary-spec classes), weapon stats are **CSV text** → no engine
closure needed. Header-name-driven column mapping → survives column drift.

```
tools/mc2weapon/
  mc2weapon.py      core CLI (viewer now; editor commands next)
  README.md         usage
  (later) dist/     launcher .bat mirroring mc2fx-console.bat
```

Auto-detects `compbas.csv` + `effects.csv` from candidate locations; `--compbas`/
`--effects` override.

---

## Command surface

### Shipped (viewer)
- `list [--all] [--json]` — every weapon (or `--all` components) with damage/heat/
  recycle/range/tons/slots + resolved FX trail name.
- `show <masterID|name> [--json]` — one component's full stats + resolved FX
  (trail/muzzle/hit/miss/objNum).

### Specced (editor — next slices, all writing a loose overlay, never mutating base)
- `set <id> <field>=<val> ...` — change stats; validate; write
  `mods/<id>/data/objects/compbas.csv` overlay (full 256-row CSV, only the edited
  cells differ from base). Fields: damage, heat, recycle, range, tons, slots,
  missileType, fields, fxid, ammoMasterId.
- `set-fx <id> <fxid|effectName>` — assign FX; validate the FX id exists in effects.csv
  AND its effectName exists in the `mc2.fx` catalog (cross-check via `mc2fx dump`).
- `list-fx [--json]` — dump effects.csv rows (the assignable FX palette).
- `new <id> <name> --type <Energy|Ballistic|Missile> [field=val ...]` — create a weapon
  in an unused masterID slot (0..254). Inherits sensible defaults; requires damage/heat/
  recycle/range/fxid. Mech loadouts reference it by masterID.
- `validate <overlay-compbas.csv>` — the "just works" gate: every weapon row has a valid
  Type, numeric stats in range, a known Range bracket, a Special FX ID present in
  effects.csv (and ideally a known effectName), AmmoMasterID consistency. Exit 0 = safe
  to ship.
- `pack <id>` — assemble the mod folder (`mods/<id>/`, `mod.json`, overlaid compbas.csv
  + effects.csv) ready for `MC2_ACTIVE_MOD`. (Reuse `tools/mc2mod` packaging.)

### Later (bolt-visual + GUI)
- Bolt texture/color/length edit → rewrite the `[BoltProjectileData]` `.fit` packet in
  `object2.pak` (PacketFile write; preserve checksum + seek-table type bits + packet
  index = object#). Gauss/AC use BoltProjectileData; PPC/laser use BeamProjectileData.
- Range-in-meters: surface the `gamesys.fit [WeaponRanges]` brackets read-only (global;
  editing them is a global balance change, not per-weapon).
- Optional ImGui GUI mirroring `tools/mc2fx_preview` (SDL2+ImGui), embedding the mc2fx
  curve grapher to preview a weapon's assigned FX.

---

## Validation rules (the "just works" contract)

A weapon row is **valid** iff: `Type ∈ {EnergyWeapon, BallisticWeapon, MissileWeapon}`;
`damage/heat/recycle/tons ≥ 0` numeric; `slots` a positive int ≤ body capacity;
`Range ∈ {short, medium, long}` (or `0`); `SpecialFXID` is a row that exists in
effects.csv; `MissileType`/`Fields` are valid enum/bitfield ints; `AmmoMasterID` either
0 or a real component id. `new` additionally: masterID unused, name non-empty. The
editor refuses to write an overlay that fails validation (or `--force` with a warning).

---

## Roadmap (smallest valuable first)
1. **Viewer** — `list`/`show` (SHIPPED).
2. **list-fx + validate** — read-only FX palette + the validation gate (SHIPPED).
3. **set / set-fx** — edit stats/FX → loose overlay; validate before write (SHIPPED).
4. **new** — create weapons in an unused masterID slot; auto-writes mod.json (SHIPPED).
5. **Bolt visual** — `.fit`-in-`.pak` texture/color edit (PacketFile write). NEXT.
6. **GUI** — ImGui workbench + FX curve preview.

**End-to-end "just works" verified:** a `set 104 damage=99` + `set-fx 145 gauss_trail`
overlay written to `<deploy>/mods/wtest/`, launched with `MC2_ACTIVE_MOD=wtest` →
mc2_01 smoke PASS, Δdestroys=0, no crash. The loose compbas overlay loads in-engine
with no .pak repack, exactly as designed. `pack` folded into the editor (mod.json is
auto-created on first write).

Each editor slice gates on: `validate` passing on its own output, and an interactive
load test (`MC2_ACTIVE_MOD=<id>` → mission loads, weapon fires with new stats/FX).
