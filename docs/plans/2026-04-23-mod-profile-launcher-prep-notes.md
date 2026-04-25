# Mod Profile Launcher — Read-Only Prep Findings

Companion to `2026-04-23-mod-profile-launcher-design.md`. Captured from grep/read
only — no code changes. Informs v1 implementation choices.

## Path-layer findings

Source: [mclib/paths.cpp:24-54](../../mclib/paths.cpp).

- All content roots are **single flat `char[80]` globals** with compile-time
  defaults of `"data\\<subdir>\\"`. Separately addressable:
  `terrainPath`, `objectPath`, `missionPath`, `cameraPath`, `tilePath`,
  `moviePath`, `shapesPath`, `spritePath`, `artPath`, `soundPath`,
  `interfacePath`, `profilePath`, `warriorPath`, `fontPath`, `savePath`,
  `texturePath`, `tglPath`, `effectsPath`, `campaignPath`, plus CD mirrors.
- `savePath[256]` is declared with **no compile-time default** — it's set at
  runtime by whoever initializes profile state. That's the natural hook for
  per-profile saves (v1 requirement). `saveTempPath` has a default and is
  shorter (80), worth noting.
- Buffer width is 80 chars. `profiles/magic_corebrain_gamesys/data/missions/`
  is 45 chars — safe. Deep mod trees could hit the ceiling; profile IDs should
  be short (e.g. `magic_cv`, not `magic_unofficial_patch_carver_v`).
- Loose-file override (the mechanism in `memory/mc2_file_override_system.md`)
  goes through `File::open` in `mclib/file.cpp` / `ffile.cpp`. 14 files
  reference the FastFile fallback chain — needs one closer look during
  implementation, not prep.

**v1 implementation choice implied:** rewrite the path globals at profile
bind time rather than rewriting `File::open`. Zero callsite changes, matches
how stock MC2 already parameterises content location.

## ABL registration findings

Source: [code/ablmc2.cpp:7737-8001](../../code/ablmc2.cpp), the single giant
`ABLi_addFunction` block that wires every C++ ABL extern.

### Stock MC2 already owns the legacy core* namespace

Lines 7886-7900 register the stock `new*` and `core*` primitives:

```
newmoveto, newmovetoobject, newpower, newattack, newcapture, newscan,
newcontrol, coremoveto, coremovetoobject, corepower, coreattack,
corecapture, corescan, corecontrol, coreeject
```

These are **stock** — Magic did not invent them. Any Magic or user content
calling these routes through the stock engine, which is fine.

### Our MCO tier-2 stubs overlap Magic's documented corebrain API

Three direct collisions confirmed at:
- [code/ablmc2.cpp:7993](../../code/ablmc2.cpp) — `magicattack`
- [code/ablmc2.cpp:8000](../../code/ablmc2.cpp) — `coreguard`
- [code/ablmc2.cpp:8001](../../code/ablmc2.cpp) — `corepatrol`

Registered via the MCO stubs commit `db8c00a`
(`memory/omnitech_abl_missing_names.md`). Expected from Magic's
`Brain_Library.txt`: also `coreescort`, `coresentry`, `corerepair`,
`tdebugstring`. Need to grep the tier-2 set once we start the audit task.

### Signature mismatch risk

- Magic's `magicAttack(x)` takes a single int → matches our `"i"` arg string
  at line 7993.
- Magic's `coreGuard(guardPoint[3], scanCriteria)` takes a WorldPosition
  array + int. Our registration at line 8000 is `"?ii"` (wildcard + 2 ints).
  Magic's brain passing a real `WorldPosition` may or may not match the
  wildcard at runtime — the memory note
  `memory/carver5_mission_playable.md` says signature char `?` = ANYTHING,
  which is the correct wildcard.
- `corepatrol` at line 8001 is `"??i"` — two wildcards + int. Magic's is
  `(PatrolState, PatrolPath)`, 2 args → likely signature count mismatch
  (2 vs 3). **Highest-priority audit target.**

### Precedence question (the one v1 actually needs to answer)

Magic's `corebrain.abx` declares these names as ABL *library* functions
(authored in ABL, compiled to bytecode). Our engine declares them as ABL
*extern* functions (C++ callbacks). When a brain calls `magicAttack`, the
ABL name resolver has to pick one. Which wins:

- **If externs always win**: Magic's corebrain will be silently shadowed,
  and brains calling `magicAttack` etc. get our stub behavior. Behavior
  regression is likely. This is the "we need override machinery" path.
- **If `.abx` library wins**: our stubs are dead code under Magic's profile
  but harmless. No override machinery needed. This is the path the updated
  design hopes for.
- **If it's a link-order race** or **a "last registered wins" model**:
  deterministic but fragile; we'd want to pick a convention.

This is answered by reading the `ABLi_addFunction` and library-load code
in `mclib/ablenv.cpp` / `mclib/ablrtn.cpp` (both already identified), plus
a one-time boot test with instrumentation. **v1 audit, not prep.**

## Content inventory snapshot (from the archive)

| Archive dir | Files | Destination path global |
| --- | --- | --- |
| `1. Brain Library/corebrain/corebrain.abx` | 1 | `missionPath` |
| `1. Brain Library/Sample_brains/*.abl` | 8 | `warriorPath` |
| `2. Gamesys/gamesys.fit` | 1 | `missionPath` |
| `4. Weapons/Art-weapons/*.tga` | 18 | `artPath` |
| `4. Weapons/weapons_mod/*.csv` | 2 | `objectPath` |
| `5. Mechs/mechs-CSV/*.csv` | 32 | `objectPath` |
| `7. Carver V/Carver5missions&purchase/*.fit` | 54 | `missionPath` |
| `7. Carver V/Carver5campaignwarriors/*.abl` | ~80 | `warriorPath` |
| `9. Exodus/.../Data/art/*.fit` | 1 | `artPath` |
| `9. Exodus/.../Data/campaign/exodus.fit` | 1 | `campaignPath` |
| `9. Exodus/.../Data/missions/**` | ~100 .fit/.pak + warriors | `missionPath` + `warriorPath` |
| `9. Exodus/.../Data/movies/*.bik` | 8 | `moviePath` |
| `9. Exodus/.../Data/textures/*.{tga,jpg,lst}` | hundreds | `texturePath` |

Maps cleanly onto the path-global-rewrite implementation choice.

## Things that remain unknown after prep

Defer to v1 implementation discovery, do not block planning on these:

1. Does `File::open` walk multiple loose roots today, or assume one? (Grep
   inside `mclib/file.cpp` at impl time.)
2. Where exactly `savePath` gets assigned — probably `mechcmd2.cpp`
   startup.
3. Whether `FastFile` handles per-profile `.fst` packs or whether only
   loose files override.
4. ABL name-resolution precedence (per the precedence question above).
5. How `profilePath` and `warriorPath` currently interact — they share the
   same default subdir (`data/missions/profiles/`) which suggests legacy
   coupling worth understanding before adding more indirection.

## Revised v1 risk ranking

1. **HIGH**: ABL precedence between C++ externs and `.abx` library
   functions. Determines whether Magic's smart AI actually runs.
2. **MEDIUM**: `corepatrol` signature arity mismatch (our `"??i"` vs
   Magic's 2-arg call). Could manifest as brain load failure or silent
   arg corruption.
3. **MEDIUM**: Per-profile save isolation — needs a real decision on save
   directory naming, migration path for existing saves, and whether the
   launcher enforces profile binding on load.
4. **LOW**: Path buffer width (80 chars). Only matters if someone nests
   profiles deeply; enforce short IDs.
5. **LOW**: FastFile per-profile `.fst` support. Not needed for v1 — loose
   files are sufficient for Magic's archive and Exodus.

No blockers identified for starting v1 implementation work.
