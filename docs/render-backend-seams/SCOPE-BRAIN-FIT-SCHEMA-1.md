<!-- doc-status: current | reviewed:2026-10-11 -->
# SCOPE-BRAIN-FIT-SCHEMA-1 — Brain FIT Schema, Key/Alias Registry, and Checker

**Slice:** BRAIN-FIT-SCHEMA-1  
**Arc:** Brain & AI 2.14 / TechScript (GDD deferred; this slice is DATA/SCHEMA/CHECKER ONLY)  
**Date:** 2026-06-23  
**Author:** Claude Code (recon + scope)  
**Status:** SCOPE — ready for implementation assignment

---

## Goal

Define a machine-checkable schema for a new `mission_ai.fit` file that carries **Brain records** for mission
units. The schema specifies per-unit Brain entries (switches, tactic weights, up to 3 OPORD slots), a
canonical key + author-alias registry, OPORD enum validation against real `tacordr.h` codes, pilot-stat
alias mapping, and a checker script contract that matches the existing `scripts/check-*.py` convention.

No runtime code is introduced. No relink is required. Missions with no `mission_ai.fit` remain valid.

---

## Non-goals (explicitly out of scope)

- Task scheduler / priority queue (TASK-SCHEDULER-CORE-1, separate slice)
- TechScript-Special / BrainSpecial dispatch VM (TECHSCRIPT-SPECIAL-DISPATCH-1, deferred behind ABL hardening)
- New AI behavior (HullDown, HitAndRun — follow-up slices)
- Save/load runtime state (BRAIN-SAVELOAD-1, separate slice)
- Editor brain panel (EDITOR-BRAIN-PANEL-1, separate slice)
- ABL changes of any kind
- Archetype `.fit` resolver runtime (BRAIN-ARCHETYPE-FIT-1, separate slice)
- Any new C++ class, header, or object file

---

## Grounded current state

### FitIniFile block/typed-read parser

- `mclib/inifile.h`, `mclib/inifile.cpp` — `FitIniFile` implements `seekBlock(name)`,
  `readIdLong`, `readIdFloat`, `readIdString` (confirmed present, used in >15 files).
- The parser is **already block-structured and typed-read**; a new `mission_ai.fit` is a new file
  consumed by the same API with no parser changes.
- `data/defs/text/en_us/editor/editor_techscript.fit` already uses the typed `String { key= text=
  legacyId= }` block syntax, confirming the convention is live.

### `loadBrainParameters` and brain memory cells

- `code/warrior.h:1419` — `long loadBrainParameters(FitIniFile* brainFile, long warriorId)`
- `code/warrior.cpp:7318-7378` — reads block `Warrior%d`, then sub-blocks `Warrior%dCell%d` per
  `NumCells`; each cell has `MemType` (0=long, 1=float) and `Value`. Also reads `NumStaticVars` +
  `Warrior%dStatic%d` sub-blocks with `type`/`Name`/`Value`.
- `code/mission.cpp:2937,3028` — calls `loadBrainParameters(missionFile, i)` for each warrior after
  the main mission `.fit` is opened. The **same `FitIniFile*`** is passed — i.e., brain blocks today
  live **inside** `mission.fit`. The new design moves them to a companion `mission_ai.fit`; the loader
  opens it separately only if it exists (legacy fallback).

### TacticalOrderCode enumeration

`code/tacordr.h:130-157` (confirmed by grep):

```
TACTICAL_ORDER_NONE
TACTICAL_ORDER_WAIT
TACTICAL_ORDER_MOVETO_POINT
TACTICAL_ORDER_MOVETO_OBJECT
TACTICAL_ORDER_JUMPTO_POINT
TACTICAL_ORDER_JUMPTO_OBJECT
TACTICAL_ORDER_TRAVERSE_PATH
TACTICAL_ORDER_PATROL_PATH
TACTICAL_ORDER_ESCORT
TACTICAL_ORDER_FOLLOW
TACTICAL_ORDER_GUARD
TACTICAL_ORDER_STOP
TACTICAL_ORDER_POWERUP
TACTICAL_ORDER_POWERDOWN
TACTICAL_ORDER_WAYPOINTS_DONE
TACTICAL_ORDER_EJECT
TACTICAL_ORDER_ATTACK_OBJECT
TACTICAL_ORDER_ATTACK_POINT
TACTICAL_ORDER_HOLD_FIRE
TACTICAL_ORDER_WITHDRAW
TACTICAL_ORDER_SCRAMBLE
TACTICAL_ORDER_CAPTURE
TACTICAL_ORDER_REFIT
TACTICAL_ORDER_GETFIXED
TACTICAL_ORDER_LOAD_INTO_CARRIER
TACTICAL_ORDER_DEPLOY_ELEMENTALS
TACTICAL_ORDER_RECOVER
```

OPORD schema enum maps to these directly. Sentry/Ambush/Scout are **not** single `TacticalOrderCode`
values — they are compositions (Sentry = WAIT+GUARD, Ambush = HOLD_FIRE+ATTACK_OBJECT, Scout =
MOVETO+sensor sweep); they are valid OPORD `type=` string values in the schema but resolve to a
tac-order sequence, not a single code.

### Pilot stats and trait fields

`code/warrior.h:184-187,687-692` (confirmed):

| Engine field | Spec alias | Type |
|---|---|---|
| `skills[MWS_GUNNERY]` | `Gunnery` | char (skill score) |
| `skills[MWS_PILOTING]` | `Piloting` | char |
| `skills[MWS_SENSORS]` | `Sensors` | char |
| `aggressiveness` | `Aggressiveness` | char |
| `courage` | `Courage` | char |
| `professionalism` | `Leadership` (GDD alias) | char |
| `decorum` | `Discipline` (GDD alias) | char |

Ranks: `WARRIOR_RANK_GREEN=0`, `REGULAR=1`, `VETERAN=2`, `ELITE=3`, `ACE=4` (warrior.h:199-203).

### Checker script convention

`scripts/` contains 40+ `check-*.py` and `check-*.sh` files. Python checkers follow:
- Shebang `#!/usr/bin/env python3`
- Docstring at top: one-line summary, background, exit-code contract
- `argparse` with `--root <repo>` (default `.`), `--quiet`, optional `--json <out>`
- `FAIL`/`WARN`/`PASS` prefixed print lines
- Exit 0 = no FAILs; exit 1 = at least one FAIL
- Registered in `scripts/check-contracts.sh` (the aggregator)

---

## Proposed schema

### File location and naming

```
<mission_dir>/
  mc2_01.fit            # existing mission fit (unchanged)
  mc2_01_ai.fit         # NEW — Brain records for this mission; absent = legacy ABL fallback
```

Naming convention: `<mission_stem>_ai.fit`. The loader attempts `<stem>_ai.fit`; if absent, skips
silently (legacy ABL path unchanged).

### Top-level structure

```fit
// mission_ai.fit — Brain records for <mission_stem>
// Format: typed-block FIT, same parser as FitIniFile (mclib/inifile.cpp)

SchemaVersion {
    version = 1
}

Brain {
    unitRef = "Warrior0"          // required; must be unique in file; maps to Warrior%d
    archetype = "house_davion"    // optional; names an archetype .fit preset

    // --- Switches (boolean, 0/1) ---
    // canonical key = snake_case; aliases accepted (see Key Registry)
    combat.engage_range_check  = 1
    doctrine.hold_position     = 0
    sensor.passive_mode        = 0
    clan_honor.zellbrigen      = 0

    // --- Thresholds (float 0.0–1.0) ---
    threshold.withdraw_health  = 0.25
    threshold.pursuit_range    = 500.0

    // --- Tactic weights (float >= 0.0; need not sum to 1.0; checker normalizes for display) ---
    tactic.IndirectFire        = 0.3
    tactic.HullDown            = 0.5
    tactic.FightingWithdraw    = 0.2
    tactic.Pursue              = 0.1
    tactic.HitAndRun           = 0.0

    // --- OPORD slots (Primary / Secondary / Tertiary) ---
    // 1–3 OPORDs per Brain block.
    OPORD {
        slot       = "Primary"
        type       = "Patrol"         // validated against OPORD enum (see below)
        waypointGroup = "patrol_wp_A" // optional object reference
        looping    = 1
    }
    OPORD {
        slot       = "Secondary"
        type       = "Guard"
        waypointGroup = ""
        looping    = 0
    }

    // --- Fallback policy (after Tertiary OPORD completes) ---
    fallback_policy = "HoldPosition"  // HoldPosition | HoldFire | Withdraw | LoopPrimary | RequestOrders
}
```

Notes:
- Multiple `Brain { }` blocks in one file — each must have a distinct `unitRef`.
- Keys outside the defined registry are a WARN (not FAIL) to support forward compat.
- OPORD sub-blocks: 1 required (Primary), Tertiary optional. Slot values must be
  "Primary" / "Secondary" / "Tertiary" — duplicates in the same Brain = FAIL.
- `archetype=` names an external `.fit` file; the checker validates the name is
  non-empty if present, but does NOT resolve the archetype file (resolver is
  BRAIN-ARCHETYPE-FIT-1, deferred).

---

## Key and alias registry

### Switch key format

Canonical: `<group>.<name>` (all lowercase snake_case).  
Groups: `combat`, `doctrine`, `sensor`, `survival`, `targeting`, `movement`, `faction`, `clan_honor`.

### Author-facing aliases (normalization table)

The checker and future loader normalize these on read; aliases produce WARN in strict mode.

| Canonical key | Accepted aliases |
|---|---|
| `combat.engage_range_check` | `EngageRangeCheck`, `engage_range` |
| `doctrine.hold_position` | `HoldPosition`, `hold_pos` |
| `doctrine.fire_at_will` | `FireAtWill`, `fire_will` |
| `sensor.passive_mode` | `PassiveMode`, `passive` |
| `sensor.enhanced_scan` | `EnhancedScan`, `sensor_boost` |
| `survival.eject_threshold` | `EjectThreshold`, `eject` |
| `targeting.prefer_mechs` | `PreferMechs`, `mech_priority` |
| `movement.jump_preferred` | `JumpPreferred`, `prefer_jump` |
| `clan_honor.zellbrigen` | `Zellbrigen` |
| `clan_honor.dezgra_response` | `DezgraResponse`, `dezgra` |

### Pilot-stat alias mapping (for tactic-weight modulator fields, future use)

| Engine field | Canonical schema name | GDD alias accepted |
|---|---|---|
| `skills[MWS_GUNNERY]` | `stat.gunnery` | `Gunnery` |
| `skills[MWS_PILOTING]` | `stat.piloting` | `Piloting` |
| `skills[MWS_SENSORS]` | `stat.sensors` | `Sensors` |
| `aggressiveness` | `stat.aggressiveness` | `Aggressiveness` |
| `courage` | `stat.courage` | `Courage` |
| `professionalism` | `stat.leadership` | `Leadership` |
| `decorum` | `stat.discipline` | `Discipline` |

Rank tier names: `Green`, `Regular`, `Veteran`, `Elite`, `Ace` (map to
`WARRIOR_RANK_GREEN`…`ACE` in warrior.h:199-203).

---

## OPORD enum validation

Valid `type=` values and their `TacticalOrderCode` resolution:

| Schema type string | TacticalOrderCode(s) | Notes |
|---|---|---|
| `Patrol` | `TACTICAL_ORDER_PATROL_PATH` | Direct |
| `Guard` | `TACTICAL_ORDER_GUARD` | Direct |
| `MoveTo` | `TACTICAL_ORDER_MOVETO_POINT` / `MOVETO_OBJECT` | Direct; waypoint disambiguates |
| `Escort` | `TACTICAL_ORDER_ESCORT` | Direct |
| `Attack` | `TACTICAL_ORDER_ATTACK_OBJECT` / `ATTACK_POINT` | Direct |
| `Withdraw` | `TACTICAL_ORDER_WITHDRAW` | Direct |
| `Capture` | `TACTICAL_ORDER_CAPTURE` | Direct |
| `Refit` | `TACTICAL_ORDER_REFIT` | Direct |
| `Follow` | `TACTICAL_ORDER_FOLLOW` | Direct |
| `HoldFire` | `TACTICAL_ORDER_HOLD_FIRE` | Direct |
| `Sentry` | WAIT + GUARD | Composition; not a single code |
| `Ambush` | HOLD_FIRE + ATTACK_OBJECT | Composition |
| `Scout` | MOVETO + sensor sweep | Composition |

Any `type=` value not in this table = FAIL by checker.

---

## Tactic-weight field validation

- Key must match `tactic.<Name>` where `<Name>` is in the registered tactic vocabulary.
- Value must be a float >= 0.0.
- Registered tactic names (initial set):
  `IndirectFire`, `HullDown`, `FightingWithdraw`, `Pursue`, `HitAndRun`,
  `StopAndFire`, `Flank`, `Joust`, `Turret`.
- Unknown tactic key = WARN (not FAIL); allows forward-compat additions.
- Checker emits normalized % display for each tactic (for human review) but does NOT
  require weights to sum to 1.0.

---

## Checker script contract

**File:** `scripts/check-brain-fit-schema.py`  
**Registration:** add to `scripts/check-contracts.sh` aggregator (cheap / read-only / idempotent).

### Inputs

```
py -3 scripts/check-brain-fit-schema.py [--root <worktree-root>] [--fixtures <dir>] [--quiet] [--json <out.json>]
```

- `--root`: defaults to `.` (worktree root). Script looks for `data/missions/**/*_ai.fit`
  and `scripts/fixtures/brain_fit/` fixture files.
- `--fixtures`: override fixture directory (default `scripts/fixtures/brain_fit/`).
- `--quiet`: suppress per-key output; print only PASS/FAIL summary.
- `--json`: write structured results to JSON file.

### Pass / fail rules

| Rule | Severity | Description |
|---|---|---|
| R1 | FAIL | `unitRef` is missing or empty in a Brain block |
| R2 | FAIL | Duplicate `unitRef` within the same `mission_ai.fit` |
| R3 | FAIL | `OPORD.type` value not in OPORD enum table |
| R4 | FAIL | Duplicate OPORD `slot` value within the same Brain block |
| R5 | FAIL | `OPORD.slot` value not in {Primary, Secondary, Tertiary} |
| R6 | FAIL | `tactic.<Name>` value is not a non-negative float |
| R7 | FAIL | `SchemaVersion.version` present but not integer |
| W1 | WARN | Unknown switch key not in registry (forward-compat; not FAIL) |
| W2 | WARN | Author alias used instead of canonical key |
| W3 | WARN | Unknown tactic name (forward-compat) |
| W4 | WARN | `archetype=` present but non-empty string is not checked (resolver deferred) |
| W5 | WARN | `mission_ai.fit` found with no `SchemaVersion` block |

### Exit codes

- `0` — no FAILs (WARNs allowed)
- `1` — at least one FAIL rule triggered
- `2` — file parse error (malformed FIT syntax, not a schema rule violation)

### Output format (matches existing checkers)

```
PASS  brain_fit  mc2_01_ai.fit  2 Brain blocks validated
WARN  brain_fit  mc2_01_ai.fit  Brain@Warrior1: W2 alias 'HoldPosition' → 'doctrine.hold_position'
FAIL  brain_fit  mc2_99_ai.fit  Brain@Warrior0: R3 unknown OPORD type 'Blitz'
```

---

## Sample fixtures

Fixtures live at `scripts/fixtures/brain_fit/` and are exercised by the checker via `--fixtures`.

### `valid_basic.fit` — should PASS

```fit
SchemaVersion { version = 1 }

Brain {
    unitRef = "Warrior0"
    combat.engage_range_check = 1
    doctrine.hold_position    = 0
    threshold.withdraw_health = 0.25
    tactic.IndirectFire = 0.4
    tactic.HullDown     = 0.6
    OPORD { slot = "Primary"   type = "Patrol" looping = 1 }
    OPORD { slot = "Secondary" type = "Guard"  looping = 0 }
    fallback_policy = "HoldPosition"
}
```

### `fail_bad_unitref.fit` — should FAIL R1

```fit
SchemaVersion { version = 1 }
Brain {
    unitRef = ""
    tactic.IndirectFire = 0.3
    OPORD { slot = "Primary" type = "Guard" }
}
```

### `fail_duplicate_unitref.fit` — should FAIL R2

```fit
SchemaVersion { version = 1 }
Brain { unitRef = "Warrior0" OPORD { slot = "Primary" type = "Guard" } }
Brain { unitRef = "Warrior0" OPORD { slot = "Primary" type = "Patrol" } }
```

### `fail_unknown_opord.fit` — should FAIL R3

```fit
SchemaVersion { version = 1 }
Brain {
    unitRef = "Warrior0"
    OPORD { slot = "Primary" type = "Blitz" }
}
```

### `fail_unknown_tactic.fit` — should FAIL R6 (bad value) / W3 (unknown name)

```fit
SchemaVersion { version = 1 }
Brain {
    unitRef = "Warrior0"
    tactic.FutureTactic = -0.5   // negative float → R6 FAIL; unknown name → W3 WARN
    OPORD { slot = "Primary" type = "Guard" }
}
```

### `warn_alias.fit` — should WARN W2 only, not FAIL

```fit
SchemaVersion { version = 1 }
Brain {
    unitRef = "Warrior0"
    HoldPosition = 0       // alias for doctrine.hold_position → W2
    OPORD { slot = "Primary" type = "Guard" }
}
```

### `legacy_no_ai_file` — absence of `_ai.fit` should PASS (legacy ABL fallback)

The checker silently skips missions that have a `.fit` but no `_ai.fit`. This is not an error.

---

## TechScript name collision (MUST resolve before any dispatch slice)

### The collision

`data/defs/text/en_us/editor/editor_techscript.fit` — confirmed present. This file defines
localized string keys for the editor's **mission-trigger condition/action FSM**: SetFlag, PlayBIK,
DisplayTextMessage, BooleanFlagIsSet, etc. (`editor_techscript.fit:10-30`). It is the MC2 legacy
*TechScript editor* — a trigger/consequence system for scripting mission events.

The Brain & AI 2.14 GDD's "TechScript Specials" is an **entirely different concept**: a C++
execution layer that routes a Brain to structured behavioral programs (tactical execution). These two
subsystems share the word "TechScript" but have zero code in common.

`editor/EditorResourceCatalog.cpp` also references "techscript" (the legacy editor surface).

### Risk

Any agent that greps for `TechScript` to find or patch the new Brain Specials layer will find the
legacy editor trigger system instead, and vice versa. This is a guaranteed confusion vector for
modders and code authors alike.

### Decision required (before TECHSCRIPT-SPECIAL-DISPATCH-1)

Two options:

| Option | What changes | Blast radius |
|---|---|---|
| A — Rename legacy surface | Rename `editor_techscript.fit` key namespace + catalog refs to e.g. `MissionTrigger` or `EditorTrigger` | Medium — touches FIT keys, catalog, string IDs; risks localization drift |
| B — Internal rename of NEW layer only | Keep legacy "TechScript" in editor; name the new Brain execution layer `BrainSpecial` internally (no user-visible name change to legacy editor) | **Low** — only new code uses the new name; zero change to existing editor files |

### Recommendation

**Option B — name the new Brain execution layer `BrainSpecial` internally.**

- Legacy editor TechScript (`editor_techscript.fit`, `EditorResourceCatalog.cpp`) remains
  untouched; no localization risk.
- New layer: C++ class `BrainSpecial`, registry function `registerBrainSpecial()`, dispatch header
  `brain_special.h`. Never named "TechScript" in source.
- User-facing GDD language ("TechScript Specials") may still appear in docs/GDD prose as a
  design-concept label, but the internal code symbol is `BrainSpecial`.

**Rule (mandatory for all future agents):** do not grep `TechScript` to locate Brain dispatch code.
Brain dispatch lives under `BrainSpecial*` symbols. `TechScript` in source = legacy editor
mission-trigger FSM ONLY.

---

## Acceptance criteria

(Verbatim; all must pass before slice is declared DONE.)

1. **valid fixture passes** — `check-brain-fit-schema.py --fixtures scripts/fixtures/brain_fit/` exits 0 on `valid_basic.fit`
2. **bad unitRef fails** — checker exits 1 on `fail_bad_unitref.fit` (R1)
3. **duplicate unitRef fails** — checker exits 1 on `fail_duplicate_unitref.fit` (R2)
4. **unknown OPORD fails** — checker exits 1 on `fail_unknown_opord.fit` (R3)
5. **unknown tactic key fails** — checker exits 1 on `fail_unknown_tactic.fit` (R6/W3 combo)
6. **alias normalization tested** — checker exits 0 on `warn_alias.fit` with at least one W2 in output
7. **legacy mission with no mission_ai.fit remains valid** — checker exits 0 when no `_ai.fit` files are present
8. **no relink** — confirmed below

---

## Relink determination

**No relink required.**

This slice introduces:
- A new Python checker script (`scripts/check-brain-fit-schema.py`) — no C++ touched
- New `.fit` fixture files under `scripts/fixtures/brain_fit/` — no C++ touched
- This scope document

No C++ header, source file, or class layout is modified. The existing `FitIniFile` parser
(`mclib/inifile.cpp`) is read at runtime as-is; no new entry point is added to it in this slice.
The `loadBrainParameters` function in `warrior.cpp` is not touched; the new loader that opens
`mission_ai.fit` separately is part of BRAIN-RUNTIME-1 (deferred).

Per build rules: relink is required only on load-bearing function or class-layout changes.
Neither applies here.

---

## Dependencies

- **None blocking this slice.** BRAIN-FIT-SCHEMA-1 is pure data/schema/checker.
- Does NOT depend on ABL hardening (borrow-scripting-1).
- Does NOT block on TASK-SCHEDULER-CORE-1 or BRAIN-RUNTIME-1.
- Unblocks: TACTIC-WEIGHT-SELECT-1, BRAIN-ARCHETYPE-FIT-1, BRAIN-RUNTIME-1,
  EDITOR-BRAIN-PANEL-1 (all consume this schema definition).
- Independent of ABL hardening arc; the two can proceed in parallel.

---

## Open items

1. **Fixture directory location** — `scripts/fixtures/brain_fit/` proposed; confirm with project
   conventions (some check-*.py scripts embed fixture data inline; either approach acceptable).
2. **FIT parser `seekBlock` behavior on missing file** — confirm that opening a non-existent
   `mission_ai.fit` returns a clean error code (not a fatal) before BRAIN-RUNTIME-1 wires the
   loader. (Recon: `FitIniFile` is already used with absence checks in `mission.cpp:2937`; the
   pattern exists.)
3. **OPORD composition semantics** — Sentry/Ambush/Scout map to multi-step tac-order sequences.
   The schema accepts them as valid `type=` strings; the runtime expansion is BRAIN-RUNTIME-1
   scope, not this slice.
