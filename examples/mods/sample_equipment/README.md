# sample_equipment — a new EquipmentDefinition (live ADD)

Adds `sample:hot_medium_laser`, a stock Medium Laser (`core:laser`, legacy
masterId 156) cloned with a hotter/harder profile (damage 2.5→3.2, heat 3.0→4.5).

As of **EQUIP-ADD-FILL-1 (S2)** this is a LIVE add, not just an offline example:
the def claims the UNDEFINED stock compbas ordinal **40** via an explicit
`LegacyMasterId`, and the gated engine fill pass materializes it at load as a
real `MasterComponent` row (in-sim consumer) **and** a mechlab
`LogisticsComponent` (logistics consumer) from one synthesized compbas line.
`Enabled=TRUE` then rides the S1 enable wire (EQUIP-ENABLE-WIRE-1) so the weapon
is purchasable with no `purchase.fit` edit.

## Files

* `data/defs/equipment/sample/hot_medium_laser/equipmentdef.fit` — the
  EquipmentDefinition (same block/token format the source-flip overlay reads;
  format authority: `tools/unit_converter/equipment_emit.py`), extended with the
  ADD fields: explicit `LegacyMasterId` on a free ordinal, `Enabled=TRUE`,
  `[Presentation] Icon` (MANDATORY — the mechlab consumer drops icon-less rows),
  `Picture`/`SpecialEffect`/`IconX`/`IconY`.
* `modlint/hot_medium_laser.facts.json` — the facts the offline validator checks,
  including the ADD facts (`add_fill`, `has_display_name`, `has_icon`,
  `ammo_master_id`).

## Proves

* a namespaced **qualified id** (`sample:…`) so the def ADDS rather than clobbers
  the stock laser;
* **explicit** `LegacyMasterId` on a FREE ordinal (40) — no auto-assign, ids stay
  in 0..254 (255 = 0xff sentinel; no E4 widen);
* the **two-consumer** materialization (MasterComponent + LogisticsComponent from
  the same synthesized line);
* **modlint-clean**: `mc2_modlint --equipment modlint/hot_medium_laser.facts.json`
  → exit 0.

## Required env gates (what makes the weapon purchasable live)

Exactly TWO gates make the ADD materialize — both default OFF, so gates-off is
byte-identical stock:

| gate | value | why |
|---|---|---|
| `MC2_UNIT_PROFILE_DATA` | `1` | family gate for all unit/equipment modding |
| `MC2_EQUIPMENT_ADD` | `1` | arms the EQUIP-ADD-FILL-1 pass that fills the free ordinal |

**You do NOT need `MC2_EQUIPMENT_DEFINITION_SOURCE=generated` for the ADD path.**
That gate controls the *overlay* (re-stat an EXISTING stock weapon by editing its
green stats in place). The ADD-fill pass (`applyEquipmentAddFillIfEnabled`,
`code/equipment_registry_engine.cpp`) reads the `equipmentdef.fit` sidecars
directly via the mod index and is gated ONLY by the two vars above —
independent of `MC2_EQUIPMENT_DEFINITION_SOURCE`. (An earlier planning note in
`.claude/ADD-PATH-RECON-1.md` S2 said "requires SOURCE=generated"; that was the
pre-ship assumption that the ADD would live inside the SOURCE-gated overlay pass.
It does not — the shipped code and `tests/unit/` pin the two-gate contract.)

## Activate the mod (pick ONE path)

`MC2_ACTIVE_MOD` and `MC2_MOD_DEPS` are **alternative** activation paths — you
do not set both:

* **Launcher / `MC2_ACTIVE_MOD=sample_equipment`** — the ordinary path: copy
  `examples/mods/sample_equipment/` into your install's `mods/` folder and tick
  it (or name it directly).
* **`MC2_MOD_DEPS=<abs path to the mod folder>`** — the direct path-list form
  (highest-priority-first), handy for quick local testing without the launcher.

## Run it

```
set MC2_UNIT_PROFILE_DATA=1
set MC2_EQUIPMENT_ADD=1
set MC2_ACTIVE_MOD=sample_equipment
```

Load any mission, then open the mechlab: console shows
`[EQUIP-ADD] filled ordinal=40 id=sample:hot_medium_laser ...` and
`[EQUIP-ADD] mechlab row appended ...`; the S1 wire logs
`[EQUIP-ENABLE-WIRE-1] enabled sample:hot_medium_laser (masterID=40) ...` and the
weapon appears purchasable as **Hot Medium Laser**.

Known S3 gap: the in-mission info window derives its label from
`IDS_COMP_ABBR0+masterID` independently, so THAT one surface still shows a
missing-string name for new ids (EQUIP-UI-NAME-REGISTRY-1 will route it through
the registry).

## Verify offline

```
mc2_modlint --equipment examples/mods/sample_equipment/modlint/hot_medium_laser.facts.json
```
