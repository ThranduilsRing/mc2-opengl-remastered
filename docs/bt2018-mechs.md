# BT2018 mechs as first-class MC2 chassis

Plan: `E:/mc2-archive-2026-10/plans/mech-import-plan-2026-10-22.md` (packets P1-P15).
Owner decisions (2026-10-22): MC2 combat behaviour with explicit pack-owned rack sizes;
BT identity, locations and mounts with a versioned MC2 balance profile; Marauder first,
then the 51 families (8 extra model entries are skins); a personal-use mod built from
the owner's files, with tools and recipes kept apart from the extracted assets.

## What exists (lane `claude/bt2018-mechs`)

| Piece | Where |
|---|---|
| Recipe (BT ChassisDef/MechDef/MovementCapDef schema + MC2 layout) | `tools/bt2018_mechs/recipes/<family>.json` |
| Weapon mapping table, balance profile | `tools/bt2018_mechs/data/weapon_map_v1.json`, `tools/bt2018_mechs/data/balance_profile_v1.json` |
| Compiler (recipe -> profile -> compatibility files) | `tools/bt2018_mechs/compile.py` |
| Pack builder (writes `mods/bt2018_mechs/`) | `tools/bt2018_mechs/build_pack.py` |
| Package chassis rules (pure) + registry (engine) | `code/package_chassis.h`, `code/package_chassis_registry.cpp` |
| Tests | `scripts/tests/test_bt2018_mechs.py`, `tests/unit/test_package_chassis.cpp` |

Build the pack into an install (never into stock data):

    py -3 tools/bt2018_mechs/build_pack.py --install <install> --bundles <BT assetbundles directory> --texture-source <dev install>/data/tgl
    py -3 tools/bt2018_mechs/build_pack.py --install <install> --bundles <BT assetbundles directory> --check     # reproducibility

`--bundles` is required: it points to the owner's BattleTech Unity assetbundles
used to bake each chassis' native animation clips. Install the pinned UnityPy
dependency from `tools/bt2018_import/requirements.txt` before building.

Mount it as an add-on: `MC2_MOD_DEPS=bt2018_mechs` (stock campaign + pack).

### Pack contents (generated; the engine reads only these)

- `mod.json` with `"capabilities": ["imported_mechs", "package_chassis", "equipment_add"]`.
  A capability turns the matching engine path ON for that package only; the old env gates
  stay as kill switches (`MC2_ASSIMP_MECH_IMPORT=0`, `MC2_MECH_IMPORT_GPU=0`,
  `MC2_MECH_IMPORT_ANIMATE=0`, `MC2_EQUIPMENT_ADD=0`).
- `data/defs/chassis/bt2018/<family>/chassisdef.fit`: the object type (same blocks as an
  Object2.pak packet) plus `[Chassis]`, `[RegionN]`, `[HardpointN]`, `[MountTags]`.
  The registry allocates object-type numbers after the last Object2.pak packet; they are
  runtime-only, saves must carry `ChassisId` (P8).
- `data/objects/bt2018_<family>.csv`: legacy chassis CSV (23+97n layout) generated from the
  profile; weapon rows carry their BT mount id in column 41.
- `data/tgl/bt2018_<family>.ini`: appearance. `[Import] Source` = pack model,
  `AnimHost` = host whose gesture timing is borrowed until P9 owns clip timing. No host
  LOD/shadow/arm meshes are named, so no host limbs can appear.
- `data/tgl/bt2018/<family>.glb` + `.package.json`, base textures.
- `data/defs/text/en_us/bt2018_mechs.fit` (legacy string ids 61800-61899),
  `data/defs/equipment/bt2018/<item>/equipmentdef.fit` for referenced pack racks.
- `content/<family>.profile.json` (raw BT + converted values), `content/report.json`
  (hashes of every generated file, copied asset and untouched stock reference).

### Rules the engine enforces for a package chassis

One validator (`mc2pkg::checkAdd`) for the mech lab, auto-place and deployment: a grid item
sits wholly inside one location region; each weapon needs a free mount of its type in that
location (AntiPersonnel-capable items may use AP mounts); total equipment mass stays within
`Payload`. Deployment hands the mech-lab mount assignment to `BattleMech` so each weapon
fires from its mount's socket (`weapon_<location>[n]` -> joint via the package node manifest).

### Icons (P6)

`tools/bt2018_mechs/render_icons.py` renders, from the same converted model, the HUD
force-group strip `data/art/<stem>_hud_<W>x<H>.tga` (25x30, 32x38, 40x48; row 0 front,
row 1 rear) in the exact region palette `code/mechicon.cpp` recolours (damageColors[0] with
the body-region index added to red: 0 LL, 1 RL, 2 LA, 3 RA, 4 RT, 5 LT, 6 CT, 7 Head), plus
a lit bay portrait. `[Chassis] HudIcon` names the strip; `MechIcon::hudIconSourceFor` blits
it at column 0 for that chassis only. Bay/mech-lab list icons still borrow the host's until
the logistics icon loader takes a package source the same way.

### Animation states (P9)

`mclib/mech_clip_state.h` maps the simulation's gesture state to the render clip and is a
one-way read (render poses never feed movement, firing or collision). Get-up plays
`atlas_downedGetup`; falls, hits, get-ups, deaths and jump start/land play once and hold;
prone holds `atlas_downedIdle` / `atlas_deathIdle`; limps use the directional limp clips;
a jump is start -> airborne idle -> land. Locomotion clips play at
`actual speed / planted-foot speed`, the latter measured per clip at import from the
`j_LFoot`/`j_RFoot` tracks (`[MECH_IMPORT] stride ...` log lines), which removes foot
sliding at any speed inside the 0.35x-2x clamp.

The BT prefab's enabled GrounderIK/LimbIK pass is also needed after clip retargeting:
rotation-only playback otherwise drives the Marauder's soles below its bind-pose base.
The imported GPU pose solves the two legs against their actual sole geometry, retaining
the reverse knee bend, bone lengths and swing clearance. Pelvis/torso motion and the
simulation position are untouched. The walk clip itself pitches the torso about 7-8
degrees; the converted torso agrees with the source clip within 0.1 degree. This
authored lean must not be cancelled with a whole-mech rotation or height offset.

## What mechs need from the content pipeline (claude/content-pipeline)

The bt2018 lane does NOT build a cooker or a material path. Where it needed one early it
left a thin, marked adapter for the pipeline to replace:

- `mclib/msl_load.cpp` "P2 BOUNDARY ADAPTER": Assimp and the `<stem>.package.json` sidecar
  get the mounted mod's absolute path from `LookupModAbsolutePath`. Replace with the
  provider-aware resource read (relative GLB/texture/sidecar references, provider-aware
  cache keys).
- Base textures are copied as the earlier cook left them (alb TGA, AO/normal KTX2).

Per mech asset the planned content-pipeline IR should carry, and the runtime should
consume:

1. **Identity**: `replaces: "chassis:bt2018:<family>"` (qualified chassis id, not a host
   mech name). Skins (`marauder_BHA`, `marauder_BlackWidow`) are condition/material
   variants of the same asset, not new chassis.
2. **Per-part material slots**: body, weapons, cockpit/glass, special; keep each part's
   own slot instead of the current merge onto one slot; damage replacements per slot.
   Material class explicit; no sheen on unauthored surfaces (`mtls`/`mtl` = metallic +
   smoothness, `spc` needs the reviewed specular conversion; Shadow Hawk and Victor use
   `base01-nrm`).
3. **Skeleton/clip refs**: `SkeletonRef` with joints, the 64 `atlas_*` clips, and
   `clip_events` (`foot_down_l/r`, `land`, `fire_la/ra/torso`, `getup_done`,
   `death_settle`) so P9/P10 drive effects from the clip clock.
4. **Sockets**: one `muzzle` socket per hardpoint socket name the chassisdef uses
   (`weapon_leftarm`, `weapon_leftarm2`, ... ) with a local matrix on its joint, plus
   `foot` (`dust_lfoot/rfoot`), `jumpjet`, `smoke`, `eject` (`cockpit`). Joint centres
   are not muzzles.
5. **Parts / damage regions**: one `Part` per BT location (`head`, `left_arm`,
   `right_arm`, `left_torso`, `right_torso`, `center_torso`, `left_leg`, `right_leg`)
   listing intact nodes, `_dmg`/`_explode` replacement nodes and the sockets it owns, so
   a destroyed location hides its geometry and disables its mounts (P13).
6. **LOD chains**: package-owned LOD1+ and a shadow stand-in; today only LOD0 is imported.
7. **Scale**: authored height relationship (Flea vs Marauder vs Atlas) instead of the
   largest-extent normalisation.
