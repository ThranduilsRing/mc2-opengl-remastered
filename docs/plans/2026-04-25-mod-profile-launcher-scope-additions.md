# Mod Profile Launcher — v1 Scope Additions

Status: design. Companion to `2026-04-23-mod-profile-launcher-design.md`
and `2026-04-23-mod-profile-launcher-prep-notes.md`. Read those first.

## Why this exists

Three concerns surfaced after the original design that belong in v1, not
v2:

1. **ABL / ABX policy.** Commit `5b5b248` proved native externs registered
   via `ABLi_addFunction` silently shadow same-named functions inside
   compiled `.abx` libraries — last-registered-wins, no warning. Stock
   ships `corebrain.abx` with `magicAttack` / `coreGuard` / `corePatrol` /
   `coreWait`; Magic and MCO ship variants that may or may not redefine
   the same names. The launcher must let a profile choose which native
   stubs are skipped at boot so the active `.abx` can own the symbol.
   See `docs/observations/2026-04-25-abl-library-shadow-rule.md`.

2. **Custom campaign override.** Some profiles want to ship more than one
   campaign (e.g. Magic's profile with both stock Carver V and Magic-tuned
   Carver V). Without an explicit slot model, the launcher would force
   one profile per campaign and block legitimate "same content rules,
   different mission set" use cases.

3. **Graphics profile metadata.** Exodus needs water flipbooks; burn-in
   campaigns conflict with our post-process shadow pass; texture-heavy
   profiles need the 3000-handle cap. These are *expectations* the
   profile declares so the launcher can validate and warn — not renderer
   features the launcher implements. Keeping the declaration in v1
   prevents profile authors from inventing parallel side-channels.

## Profiles are the compatibility boundary

A profile is the unit of "this content set was tested together." Campaign
slots are owned by a profile. v1 does **not** allow arbitrary cross-profile
mixing (Magic's balance + stock missions + Exodus warriors). That collision
surface is v2 territory.

## Schema additions to `profile.json`

```json
{
  "id": "magic_carver_v",
  "name": "Magic's Unofficial Patch — Carver V",
  "version": "1.45",
  "author": "Magic",
  "base": "stock",

  "abl": {
    "abx_libraries": [
      "missions/corebrain.abx"
    ],
    "native_extern_policy": {
      "magicattack": "disabled",
      "coreguard": "disabled",
      "corepatrol": "disabled",
      "corewait": "disabled"
    }
  },

  "campaigns": [
    {
      "id": "carver5_magic",
      "name": "Carver V — Magic Rebalance",
      "campaign_fit": null,
      "mission_root": "missions",
      "warrior_root": "missions/warriors",
      "movie_root": null,
      "texture_root": null,
      "default_mission": "mc2_01.fit",
      "default": true
    },
    {
      "id": "carver5_stock",
      "name": "Carver V — Stock Campaign",
      "campaign_fit": null,
      "mission_root": null,
      "warrior_root": null,
      "movie_root": null,
      "texture_root": null,
      "default_mission": "mc2_01.fit"
    }
  ],

  "graphics": {
    "preset": "profile-default",
    "water": {
      "mode": "auto",
      "flipbook_frames": 16
    },
    "post_shadow": {
      "enabled": true,
      "burnin_policy": "auto"
    },
    "textures": {
      "budget": 3000,
      "diagnostics": true
    }
  }
}
```

### Why no `abl.mode` field

The original advisor draft proposed `"mode": "stock" | "profile" |
"profile_with_native_extern_overrides"`. Dropped: behavior is fully
derivable from `abx_libraries` (empty = no profile libs loaded) and
`native_extern_policy` (empty = all native stubs registered as today).
A separate enum invites the three fields to disagree.

### Why `null` for stock-campaign roots, not `..` traversal

A campaign slot whose roots are `null` resolves through the profile's
`base` chain — the same mechanism that already serves stock content
when a profile inherits from `"stock"`. No `..` parent-traversal, no
`char[80]` buffer-width risk (the prep notes flagged 80 chars as the
ceiling), and no special case in the path resolver.

`campaign_fit: null` is also valid and is the expected value for any
slot that uses the legacy single-mission / stock campaign-FIT-less
flow. It does **not** mean "this slot has no campaign state"; it means
"the engine's existing default campaign load path applies."

## ABL / ABX engine wiring

At engine init, after profile resolution and before the existing
`ABLi_addFunction` block in `code/ablmc2.cpp`:

1. Load `abl.native_extern_policy` from the active profile.
2. For each entry marked `"disabled"`, skip its `ABLi_addFunction` call.
   The 4 stubs already gated under `#if 0` in `5b5b248` move to a
   profile-policy gate instead.
3. For each path in `abl.abx_libraries`, resolve through the profile +
   base chain and call `ABLi_loadLibrary` after the native registration
   phase completes.
4. Emit a `[ABL]` summary at boot:
   ```
   [ABL] profile=magic_carver_v
   [ABL] native extern skipped: magicattack reason=profile policy
   [ABL] native extern skipped: coreguard reason=profile policy
   [ABL] native extern skipped: corepatrol reason=profile policy
   [ABL] native extern skipped: corewait reason=profile policy
   [ABL] abx loaded: missions/corebrain.abx
   ```

The 7 tier-2 names from `db8c00a` that are confirmed absent from stock
`.abx` (`magicpatrol`, `magicguard`, `magicescort`, `setwillrequesthelp`,
`tdebugstring`, `isdeadorfled`, `printteamstatus`) stay registered
unconditionally. They are not policy-controlled in v1; if a profile's
`.abx` redefines one of them later, the policy mechanism is already in
place to opt out.

### ABL ordering invariant

`native_extern_policy` controls *native registration*, not arbitrary
symbol replacement after the fact. Disabled native externs are skipped.
Enabled native externs must remain the runtime winner unless the
implementation explicitly documents and logs ABX-last override behavior
for that specific symbol.

Concretely: because `ABLi_loadLibrary` runs after `ABLi_addFunction` and
both populate the same symbol table, a naive load order would let any
same-named ABX symbol silently override an enabled native extern. The
implementation must guard against this — either by detecting the
collision before the library load completes and refusing it, or by
restoring the native binding after `ABLi_loadLibrary` returns.

Validation step: at boot, the engine logs the resolved winner for every
same-named native/ABX symbol pair it discovers. Any winner that
contradicts the profile's stated policy aborts launch with a clear
error rather than silently mis-binding. This is the lever that turns
"silent AI paralysis" into "launcher refuses to boot."

## Campaign slot engine wiring

### Campaign binding invariant

Campaign selection is fixed for the lifetime of the engine process in
v1. The launcher passes `--profile` and `--campaign` before startup;
the engine binds the pair once at init. Path globals are rewritten at
mission-load time only because that is when the engine consumes them
— *not* because campaign switching is supported in-game.

### Path globals affected

Campaign binding rewrites this set:

- `campaignPath`
- `missionPath`
- `warriorPath`
- `moviePath`
- `texturePath`

Slots whose root field is `null` fall through to the profile's `base`
chain. The launcher exposes a campaign dropdown only when the active
profile declares more than one slot. CLI form:

```
mc2.exe --profile magic_carver_v --campaign carver5_magic
```

If `--campaign` is omitted, the slot marked `"default": true` is used.
Exactly one slot per profile may carry `"default": true`; launcher
validation rejects the profile otherwise.

## Save-game scoping

Saves are scoped to the active `(profile, campaign)` pair:

```
profiles/<profile_id>/saves/<campaign_id>/
```

Profiles without declared campaign slots are treated as having one
implicit slot: `default`. `savePath` (per the prep notes, the only
content-root global with no compile-time default) is rewritten at
mission load time once both profile and campaign are bound.

### Save-header guard

Each save written under v1 carries:

```
profile_id=<id>
campaign_id=<id>
```

On load, saves whose header does not match the active `(profile,
campaign)` pair are hidden from the normal load list. A "show
incompatible saves" debug toggle is v2 territory.

### Migration from earlier drafts

If `profiles/<id>/saves/` exists from an earlier launcher build:

- If the profile has exactly one campaign slot, the launcher offers to
  move existing saves into that slot's directory.
- If the profile has multiple campaign slots, the launcher leaves the
  saves in place and reports that manual migration is required. A
  `"default": true` slot is **not** sufficient justification to auto-
  move — the launcher cannot know which slot the legacy saves were
  authored under.

No silent moves.

## Graphics profile

Stored in `profile.json`, applied by a separate config layer at engine
init. The launcher does **not** implement renderer features here — only
declarations and diagnostics.

v1 fields and behavior:

| Field | v1 behavior |
| --- | --- |
| `preset` | One of `stock`, `profile-default`, `safe`, `high`. Maps to a fixed bundle of existing engine knobs (shadow res, grass on/off, post-shadow on/off). `profile-default` defers to per-field values. |
| `water.mode` | `stock` / `flipbook` / `auto`. `auto` scans the active campaign's texture root for `*.water0000.tga` patterns and logs whether flipbook playback is wired up (it currently is not — see `water_rendering_architecture.md`). Mismatched declaration produces a launcher warning, not an error. |
| `water.flipbook_frames` | Default 16. Reserved for the future water shader work; v1 only validates the count matches assets on disk. |
| `post_shadow.enabled` | Honored by the existing post-process shadow pass. |
| `post_shadow.burnin_policy` | `normal` / `reduce` / `disable_on_burnin` / `auto`. v1 implements `normal` and `auto` (auto = scan for `burnin.jpg` in the active campaign's texture root and warn about double-darkening risk; v1 does not auto-disable the pass, only logs the conflict). `reduce` and `disable_on_burnin` are accepted schema values but behave as `normal` + a launcher warning until renderer support lands. |
| `textures.budget` | Sanity-check target, default 3000 to match `MAX_MC2_GOS_TEXTURES`. Profile loaders that exceed budget log a warning. |
| `textures.diagnostics` | When true, emits `[TEXTURE_BUDGET]` line after mission load showing handles in use vs budget. |

The `auto` policies are the load-bearing v1 contribution: they convert
"silent renderer/content mismatch" into "log line at boot." That's the
whole v1 graphics ask.

## Launcher UI delta

```
Profile:  Magic's Unofficial Patch — Carver V   ▾
Campaign: Carver V — Magic Rebalance            ▾
Graphics: Profile Default                        ▾
[Launch]  [Validate]
```

- Campaign dropdown only renders if the active profile has >1 slot.
- Graphics dropdown lists the four presets; "Profile Default" maps to
  the profile's declared `graphics` block.
- `Validate` runs the audit (file presence, ABL policy resolves, no
  shadowed-symbol surprises, asset declarations match disk) without
  launching.

## Revised v1 scope

In:

- Profile selection (from original design).
- Profile inheritance via `base` (from original design).
- **ABL / ABX compatibility policy (per-symbol).**
- **Profile-owned campaign slots.**
- **Per-(profile, campaign) saves.**
- **Graphics profile metadata + diagnostics.**
- Launch-time diagnostic log proving per-profile / per-campaign
  resolution (extended to include `[ABL]` and `[TEXTURE_BUDGET]` lines).

Out:

- In-game profile or campaign switching (restart required).
- Arbitrary cross-profile content mixing.
- Remote profile install / repository.
- Renderer feature work (water flipbook playback, burn-in-aware shadow
  pass, texture streaming) — these declare their expectations in v1 and
  are implemented as separate efforts.
- User-editable `native_extern_policy` UI surface. Profile authors edit
  `profile.json` directly in v1; launcher exposes the policy read-only
  in the Validate report.

## Updated risk ranking

Promoted from prep-notes ranking:

1. **MEDIUM-HIGH (was HIGH):** ABL precedence — the mechanism is now
   proven and the per-symbol policy gives us a clean lever, but stays
   above MEDIUM until the validator actually enumerates `.abx` exports
   (disassemble or ASCII-token scrape per the observation doc) and
   cross-checks against the native registration table at boot. Failure
   mode remains silent AI paralysis, not a crash, which is why the
   prep-notes ranking treated it as the worst class. Drops to MEDIUM
   once the validator ships and the ordering invariant assertion above
   is wired up.
2. **MEDIUM:** Save-header migration. Existing user saves written
   before campaign slots existed need the migration path described
   above. Implementation must be conservative — single default slot
   only.
3. **MEDIUM:** `corepatrol` arity mismatch (prep notes #2). Unchanged.
   Per-symbol `native_extern_policy` makes the workaround obvious:
   disable our stub for any profile whose `.abx` redefines `corepatrol`,
   regardless of arity.
4. **LOW:** Path-globals buffer width (80 chars). Campaign slot roots
   add one more nesting level; enforce short profile + campaign IDs
   (validation gate).
5. **LOW:** Graphics declaration drift. Profile says
   `water.mode=flipbook`, engine doesn't support it, mission boots with
   stock water and a warning. Acceptable — declaration is aspirational.

## v1 exit criteria (revised)

Original criteria still hold, plus:

- `magic_carver_v` profile boots `mc2_01` with Magic's `corebrain.abx`
  loaded and the 4 shadowing stubs skipped per policy. Warriors call
  `magicAttack` and engage on contact (regression test against the
  passive-AI bug fixed in `5b5b248`; the stock-content equivalent —
  `stock` profile with policy unchanged — must also still pass that
  regression).
- Profile with two campaign slots can launch each slot, save in each,
  and the saves do not appear in the other slot's load list.
- Exodus profile validates with `water.mode=auto` and emits the
  expected "flipbooks declared, playback not implemented" warning
  without aborting launch.

## Deferred to v2 (unchanged + additions)

- GUI polish, profile thumbnails, dependency detection.
- Remote profile install.
- MCO / Wolfman as first-class profiles.
- **In-game `(profile, campaign)` switch.**
- **"Show incompatible saves" debug toggle.**
- **User-editable native_extern_policy via launcher UI** — keeps the
  v1 escape hatch usable by power users without exposing it as a
  footgun to casual users.
- **Cross-profile campaign mixing.**
- **Renderer feature work declared in graphics profile** (water
  flipbook playback, burn-in-aware post-shadow, etc.).
