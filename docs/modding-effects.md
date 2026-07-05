# Modding Effects (FX-DEFS Sidecar)

How to **retune or replace a weapon/explosion effect** without recompiling the engine and
without touching (or shipping) the whole 904-effect `mc2.fx` blob. This works through a
per-effect JSON sidecar — `data/effects/defs/<EffectName>.fxdef.json` — that overlays the
matching gosFX spec at load time. Companion to
[gosfx-modder-dropin-path.md](gosfx-modder-dropin-path.md) (the full effect-binding chain) and
[modding-animations.md](modding-animations.md) (the weapon-fire *gesture* half of the same
`fireWeapon` event — see [Graphics modder onboarding](#graphics-modder-onboarding) below).

> Status: **overlay tier (schema v1) ships now** — texture, blend mode, constant-curve color/
> alpha/scale/lifeSpan retuning, and disabling an effect. Authoring a **brand-new** effect name
> (not in the stock 904) still requires the whole-blob `mc2.fx` overlay (see
> [gosfx-modder-dropin-path.md](gosfx-modder-dropin-path.md) Part 2) — FX-DEFS only *overlays
> existing names*, it does not append new ones (see [Limits](#limits)).

---

## What an effect IS here (the 30-second model)

- Every visual effect in MC2 (muzzle flash, explosion, smoke trail, spark burst, …) is a
  **gosFX spec** — a named entry in `data/effects/mc2.fx` (904 stock specs; card/cardcloud/
  shardcloud/tube/shape/… classes).
- A spec is: a **texture** (the sprite/sheet it draws), a **blend mode** (`additive` glows and
  brightens; `alpha` composites normally), and a handful of **curves** — values sampled over the
  effect's normalized age `0..1` (its lifetime), e.g. alpha fading out, color shifting, scale
  growing. `m_lifeSpan` is the curve that sets how long (in seconds) the effect lives.
- A weapon or explosion is **bound** to an effect purely by name (2 CSV columns — see
  [gosfx-modder-dropin-path.md](gosfx-modder-dropin-path.md) Part 1). This doc is about editing
  the effect itself once you know its name.

## The `.fxdef.json` sidecar (what you author)

One file per effect you want to change, named after the effect:
```
data/effects/defs/<EffectName>.fxdef.json          # base tree (rare — usually you ship in a mod)
mods/<yourMod>/data/effects/defs/<EffectName>.fxdef.json   # normal case
```
`<EffectName>` must match the spec's name in `mc2.fx` **case-insensitively** — the same string
`effects.csv`/`EffectLibrary::Find` already use. Get existing names via `mc2fx dump mc2.fx`
(see [gosfx-modder-dropin-path.md](gosfx-modder-dropin-path.md)) or by asking your mod's
target CSV row.

### Schema v1 field reference

```json
{
  "effect": "Fireball",
  "disabled": false,
  "texture": "myExplosion.tga",
  "blend": "additive",
  "curves": {
    "alpha": 0.9,
    "red": 1.0,
    "green": 0.55,
    "blue": 0.15,
    "scale": 1.6,
    "lifeSpan": 1.2
  }
}
```

| Field | Type | Meaning |
|---|---|---|
| `effect` | string (required) | Name of the spec to overlay. Case-insensitive, must already exist in the loaded `mc2.fx` (this slice overlays, it does not add new names — see [Limits](#limits)). |
| `disabled` | bool | `true` forces the effect's lifetime to ~0 (a functional no-op) — use to silence an effect you don't want without editing binding CSVs. |
| `texture` | string | Relative texture/TGL name. Rebound through the same texture-pool lookup the stock loader itself uses, so any texture resolvable via the normal `data/tgl/` search works. Must be a plain relative name — no absolute paths, no `..`. |
| `blend` | `"additive"` \| `"alpha"` | Overrides the spec's blend mode. Additive = glow/brighten (fire, sparks, energy); alpha = normal transparency compositing (smoke, dust). |
| `curves` | object | Sparse **constant** overrides. Keys: `alpha`, `red`, `green`, `blue`, `scale`, `lifeSpan` (case-insensitive). Each value replaces the whole age-curve with one flat number — this is the same "constant tier" `tools/mc2fx build` already edits offline, just applied in-engine and per-effect. Curve-key authoring (fade-in/out over age) is not yet exposed through this sidecar; use `tools/mc2fx` for that today. **`scale` only applies to Singleton-family classes (Card/Shape)** — `CardCloud`/`ParticleCloud`-family effects (most explosions/smoke, including stock `Fireball`) have no single per-particle scale curve in v1 and log `curve field 'scale' unsupported for this effect's class` if you try; `alpha`/`red`/`green`/`blue` work on both families. |

Unknown top-level keys are tolerated (logged, not fatal) — this is forward-compat with the
richer schema v2 (native layered emitters) planned for a later slice; `flipbook`/`erosion`/
`distortion`/`light` keys are reserved for upcoming slices and currently parsed-and-ignored.

### What happens if you get it wrong

Nothing crashes. Every problem is **validated before you launch** (see [fxlint](#validating-your-defs-fxlint)
below) and, if something still slips through, the runtime loader logs it to stderr and falls
back to the stock spec value for that field — never silently, never fatal:
```
[FXDEF] curve field 'alpah' unsupported for this effect's class: Fireball
[FXDEF] effect name not found in loaded SpecLibrary (typo, or not in mc2.fx catalog): Fireball2
```

---

## Worked example: replacing the stock `Fireball` explosion

This is the exact recipe used by `mods/my-explosions/` in this repo — clone it directly.

1. **Find the effect name.** `Fireball` is a stock explosion spec (verify with
   `mc2fx dump data/effects/mc2.fx` if you have the tool built, or just trust this doc — it's
   confirmed present in the shipped v0.4 blob).

2. **Author the def.** `mods/my-explosions/data/effects/defs/Fireball.fxdef.json`:
   ```json
   {
     "effect": "Fireball",
     "blend": "additive",
     "curves": {
       "red": 1.0,
       "green": 0.55,
       "blue": 0.15,
       "alpha": 0.9
     }
   }
   ```
   This retunes Fireball to a hotter orange with more alpha presence, without touching a single
   byte of `mc2.fx`. (Fireball is a `CardCloud`-family spec, so `scale` isn't available in v1 —
   see the field reference table above; `red`/`green`/`blue`/`alpha` work on every class.)

3. **(Optional) author a flipbook texture.** The flipbook/erosion authoring path (`tools/fx_cook`)
   is a later slice (#4, EXPLOSION-FLIPBOOK-1) — v1 sidecar only retunes color/blend/scale/
   lifetime and can rebind to a different *static* texture via `"texture"`.

4. **`mod.json`** (minimal):
   ```json
   { "schema": "mc2-mod/1", "id": "my-explosions", "name": "My Explosions", "version": "1.0.0", "dependencies": [] }
   ```

5. **Lint before you launch:**
   ```
   py -3 tools/fxlint/fxlint.py mods/my-explosions/
   ```
   Exit 0 = clean. Folder mode walks the mod root for `data/effects/defs/` automatically.

6. **Preview it** (see [Preview loop](#preview-loop) below):
   ```
   set MC2_FX_DEFS=1
   set MC2_ACTIVE_MOD=my-explosions
   set MC2_FX_FORCE_SPAWN=1
   set MC2_LOG=1
   mc2.exe -mission mc2_24
   ```
   Watch stdout for `[FXDEF] applied overlay to 'Fireball' (from mods/my-explosions/data/effects/defs/Fireball.fxdef.json)`
   and eyeball the fireball at the force-spawn point.

That's the whole loop — no rebuild, no engine change, no whole-blob `mc2.fx` copy.

---

## Validating your defs (`fxlint`)

`tools/fxlint/fxlint.py` validates the SAME `.fxdef.json` files the engine reads (never a
parallel hand-maintained facts file). Findings are modder-first, one per line,
`<path>:<line>: <CODE> <message>`:

```
tools/fxlint/fxlint.py <file.fxdef.json>          # lint one file
tools/fxlint/fxlint.py data/effects/defs/          # lint a defs/ dir (non-recursive)
tools/fxlint/fxlint.py mods/my-explosions/         # folder mode: finds data/effects/defs/ under a mod root
tools/fxlint/fxlint.py mods/my-explosions/ --catalog catalog.json   # + cross-ref effect names against a real mc2.fx catalog (from `mc2fx dump`)
```

Exit 0 = no errors (warnings, e.g. an unrecognized-but-tolerated top-level key, don't block).
Exit 1 = at least one error.

### Error catalog

| Code | Meaning |
|---|---|
| `FXD_PARSE_ERROR` | Malformed JSON (trailing comma, unterminated string, …). |
| `FXD_ROOT_NOT_OBJECT` | Top-level value isn't a JSON object. |
| `FXD_MISSING_EFFECT_NAME` | `"effect"` missing or empty. |
| `FXD_EFFECT_NAME_UNRESOLVED` | (only with `--catalog`) the name isn't in the real `mc2.fx` catalog — likely a typo or a not-yet-added effect. |
| `FXD_BAD_TYPE` | A field has the wrong JSON type (e.g. `"disabled": "yes"`). |
| `FXD_BAD_BLEND` | `"blend"` isn't `"additive"` or `"alpha"`. |
| `FXD_TEXTURE_UNSAFE_PATH` | `"texture"` looks absolute or contains `..` (path traversal). |
| `FXD_UNKNOWN_CURVE` | A key under `"curves"` isn't one of `alpha/red/green/blue/scale/lifeSpan` — includes a "did you mean" nudge for common typos (`alpah` → `alpha`). |
| `FXD_CURVE_BAD_VALUE` | A curve value isn't numeric. |
| `FXD_UNKNOWN_TOP_KEY` | (warning, not error) an unrecognized top-level key — tolerated at runtime, but probably a typo worth checking. |
| `FXD_FLIPBOOK_NONSQUARE` | (warning) `"flipbook"` cols≠rows — valid, but double-check the atlas layout. |

### The intentional-failure example

`tests/fxlint/fixtures/sample_bad_mod/` ships one file per mistake — every code above except
`FXD_ROOT_NOT_OBJECT`/`FXD_BAD_TYPE` has a dedicated broken fixture (`sample_bad_mod/data/effects/defs/*.fxdef.json`).
Run `fxlint.py tests/fxlint/fixtures/sample_bad_mod/` to see all seven findings fire at once —
useful both as a "what does a broken def look like" reference and as the regression test
(`tests/fxlint/test_fxlint.py`). `sample_good_mod/` is the clean counterpart (0 errors).

---

## Preview loop

Canonical loop (documented fully once FX-PREVIEW-LOOP-1 lands the targeted `MC2_FX_FORCE_SPAWN=<EffectName>`
mode — today's baseline already works, just less targeted):

1. Edit `.fxdef.json` (and/or its texture) in your mod.
2. `fxlint.py mods/<yourMod>/` — fix anything red before launching.
3. Launch with `MC2_FX_DEFS=1 MC2_ACTIVE_MOD=<yourMod> MC2_FX_FORCE_SPAWN=1 MC2_LOG=1`, a static-cam
   bookmark (`tests/visual/bookmarks/mc2_01_werewolf.json`) or `-mission mc2_24`.
4. The effect fires at a fixed point a few seconds in — screenshot or eyeball it.
5. Check stdout for `[FXDEF] applied overlay to '<name>' ...` — confirms your def was read and
   matched (if you see `effect name not found`, check spelling/case against the real catalog).
6. Repeat from step 1. No rebuild, no relaunch-avoiding hot-reload yet (mtime-watch is a
   possible follow-up, not shipped in this slice) — but the full loop is well under 60 seconds.

---

## Graphics modder onboarding

If you're touching both **effects** and **animations** for the same weapon (the common case —
recoil gesture + muzzle flash fire on the same `fireWeapon` event), read both docs:

- **Effects** (this doc): `.fxdef.json` overlay, texture/blend/curve retuning, `fxlint`.
- **Animations**: [modding-animations.md](modding-animations.md) — replace/remap gesture clips
  (`Walk`/`Run`/hit-reactions/…) via `anims.json` or direct `.ase`/`.agl` drop-in.
- **Textures/meshes**: the main [modding-guide.md](modding-guide.md) §5 (texture upscaling,
  `data/tgl` `Source=` overrides) covers the non-FX asset paths.
- **Binding**: both effects and animations key off the SAME weapon-fire event
  (`code/mech.cpp fireWeapon`) but through independent tables — effect via
  `compbas.csv`/`effects.csv`, gesture via the mech's built-in gesture set (not weapon-specific).
  Changing one never requires changing the other.

---

## Limits (what doesn't work yet)

- **No net-new effect names.** `.fxdef.json` overlays a name that already exists in the loaded
  `mc2.fx` (SpecLibrary's array is frozen after Load, per
  [gosfx-modder-dropin-path.md](gosfx-modder-dropin-path.md)). To add a brand-new effect name,
  you still need the whole-blob overlay (`mods/<id>/data/effects/mc2.fx`, built via
  `tools/mc2fx new`/`clone`). Closing this gap is schema v2 (`FX-DEFS-NATIVE-1`, native layered
  emitters that don't reference a gosFX spec at all).
- **Curve authoring is constant-only.** You can retune a flat value, not author a fade-in/out
  keyframe curve, through the sidecar. Use `tools/mc2fx` for richer curve edits offline, or wait
  for a schema extension.
- **`fxlint` checks schema shape, not per-class field support.** It confirms `"scale"` is a
  recognized curve *key*, but not every effect's C++ class actually exposes a scale curve (see
  the field reference above) — that check would require loading the real `SpecLibrary`, out of
  scope for the stdlib-only offline linter. An unsupported-for-this-class field is caught at
  **runtime** instead (`[FXDEF] curve field 'X' unsupported for this effect's class`), applied
  loudly-never-silently, same as every other soft failure in this system.
- **No flipbook/erosion/distortion/light content yet.** Those keys are schema-reserved and
  parsed (so your file doesn't break when those slices land) but not yet consumed by the
  renderer — see the roadmap in `.claude/VFX-MODERNIZATION-PROPOSAL-1.md` §7 slices #4-#6.
- **Hot-reload is relaunch-only.** Edit → relaunch, not edit → see-it-live. A cheap mtime-watch
  hot-reload is a possible follow-up (`MC2_FX_DEFS_WATCH`), not shipped here.
