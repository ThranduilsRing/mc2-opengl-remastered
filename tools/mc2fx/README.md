<!-- doc-status: current | reviewed:2026-10-11 -->
# MC2 gosFX Effect Tools (`mc2fx`)

Inspect, edit, author, and preview the game's particle/effect definitions in
`data/effects/mc2.fx` — the single binary blob holding all ~904 visual effects
(weapon trails, explosions, smoke, jump-jets, muzzle flashes, etc.). Use these to
**replace or retune effect animations** for a mod, with no engine rebuild.

## What's here
| File | What |
|---|---|
| `mc2fx.exe` | command-line inspector/editor/author |
| `mc2fx_preview.exe` | GUI — lists effects and graphs their animation curves |
| `mc2fx-console.bat` | **double-click** → opens a console at the game root with the tools ready |
| `mc2fx-preview.bat` | **double-click** → launches the GUI on `data\effects\mc2.fx` |

`mc2fx.exe` is a command-line program: double-clicking it just flashes a usage
message and closes. Use **`mc2fx-console.bat`** instead — it opens a console
parked at the install root (where `data\` lives) with `mc2fx` on the PATH.

## Quick start
1. Double-click `mc2fx-console.bat`.
2. Try: `mc2fx dump --full data\effects\mc2.fx out.json` then open `out.json`.
3. Double-click `mc2fx-preview.bat` to *see* an effect's curves.

## Commands
```
mc2fx dump        <in.fx> [out.json]               catalog: index / effectID / classID / name
mc2fx dump --full <in.fx> [out.json]               + every animation curve's values
mc2fx rebuild     <in.fx> <out.fx>                 load->save round-trip (sanity check)
mc2fx build       <base.fx> <patch.json> <out.fx>  apply curve overrides to named effects
mc2fx clone       <base.fx> <src> <newName> <out.fx>   duplicate + rename an effect
mc2fx new         <base.fx> <type> <newName> <out.fx>   blank effect of a given type
mc2fx_preview     <in.fx>                          GUI curve viewer
```

## The modder workflow — replace an animation
```bat
:: 1. duplicate an existing effect so you start from something that works
mc2fx clone data\effects\mc2.fx PPC_Trail Plasma_Trail work.fx

:: 2. look at its curves, decide what to change
mc2fx-preview            (or: mc2fx dump --full work.fx peek.json)

:: 3. retune values with a patch (see schema below)
mc2fx build work.fx patch.json final.fx

:: 4. ship it as a mod overlay (the game prefers mods\<id>\data\... when MC2_ACTIVE_MOD is set)
copy final.fx mods\MyMod\data\effects\mc2.fx
```
Then **tie the effect to a weapon** (these are separate data files, edited by hand):
- `data\objects\compbas.csv` — the weapon's **`Special FX ID`** column = an effect id.
- `data\objects\effects.csv` — row `[effect id]` maps that id to the **effect names**
  (`effectName` / `hitEffectName` / `missEffectName`) the game looks up in `mc2.fx`.
- So: point the weapon at an id whose names in `effects.csv` are your new effect's name.

## Patch file format (`build`)
```json
{
  "edits": [
    { "effect": "Plasma_Trail", "set": { "m_lifeSpan": { "type": "constant", "value": 2.5 } } },
    { "effect": "smoke",        "set": { "m_scale":    { "type": "constant", "value": 1.4 } } }
  ]
}
```
- `effect` = the effect's name (case-insensitive). Not found → warned, skipped.
- `set` = curve fields to overwrite. Only listed fields change; everything else
  in the effect is preserved.
- Common fields: `m_lifeSpan` (how long it lives), `m_scale`/`m_size` (size),
  `m_red`/`m_green`/`m_blue`/`m_alpha` or `m_pRed`.. (color/fade),
  `m_particlesPerSecond` (emission rate). Use `dump --full` to see what an
  effect actually has.

## `new` effect types
`ParticleCloud`-family billboards and a few others can be created blank:
`PointCloud`, `ShardCloud`, `CardCloud`, `EffectCloud`, `Card`, `Tube`,
`DebrisCloud`, `PointLight`. (Abstract base types are rejected — they have no
loader.) A blank effect has default curves and no texture, so it's mostly a
starting skeleton; `clone` is usually the better start.

## Current limits
- `build` edits **constant-value** curves only (no multi-keyframe curve authoring,
  no per-particle seed sub-curves, no texture/blend-state or emitter-physics edits yet).
- Authoring appends to the **end** of the catalog; no insert / delete / in-place
  rename of existing effects.
- Saved blobs reload correctly and are catalog-identical, but are **not** byte-for-byte
  identical to the original (a harmless sub-1/255 color round-trip drift on mesh
  effects). Fine for modding; just don't expect `rebuild` to report "IDENTICAL".
- `mc2fx_preview` graphs the animation **curves**; it does not run the live particle
  simulation (that needs the full game renderer).

## Notes
- The tools are self-contained but expect to run with the game's `data\` folder as
  the working directory — the launchers handle that. Running `mc2fx.exe` from
  elsewhere is fine as long as you pass a full path to the `.fx` file.
- `mc2fx_preview.exe` needs `SDL2.dll` and `glew32.dll` (already in the game root);
  launch it via `mc2fx-preview.bat` so it finds them.
- Always keep a backup of the stock `data\effects\mc2.fx`, or work on copies and
  deploy through a mod folder rather than overwriting the original.
