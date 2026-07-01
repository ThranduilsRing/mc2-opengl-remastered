# MOD-SYSTEM-RECON-1 — existing mod system, for EXAMPLE checkbox mods

Recon before EXAMPLE-MODS-1. ★Use the EXISTING mod.json + loader; example mods are CHECKBOX add-ons. Line refs from canonical tree (re-grep before editing).

## mod.json (source-dir manifest — NOT the packed package.json)
`mods/<id>/mod.json`, read by launcher (`tools/mc2_launcher/mc2_launcher.cpp:1330-1345`) + packer (`tools/mc2mod/mc2mod.py:145-159`). Fields to author:
- `schema` = `"mc2-mod/1"` · `id` = lowercase token `^[a-z0-9][a-z0-9._-]*$` (becomes install dir name) · `name` (display) · `version` (dotted) · **`type` = `"assets"` (→ stackable CHECKBOX add-on; `"campaign"`→campaign list; `"dependency"`→base radio)** · `description` (launcher pane) · `dependencies:[]` · optional `radioGroup`.
- (`package.json` inside a `.mc2mod` zip is packer-generated `mc2-package/1` w/ hashes — do NOT hand-author.)

## Loader + overlay (the mechanism)
- **Env-driven, set by launcher** (not runtime FS scan): `MC2_ACTIVE_MOD` = campaign folder; `MC2_MOD_DEPS` = comma/semicolon add-on+dep layers, highest-priority-first. Unset → base game, zero overhead. `mclib/file.cpp:62-201,633,669,720`.
- **Precedence** (`file.cpp:69`): active mod > dep N..0 > loose `data/` > size-subdir > FastFile `.fst` > CD. First-wins merge into `g_modIndex` (`:461`).
- **Overlay = whole-file shadow** keyed by `data/…`-relative fwd-slash path (`IndexModData` `:214`, relBase `"data/"` `:755`). `TryModOpen` O(1) hash before loose/FastFile (`:1011`). Gate `ShouldSearchMods` requires `data/` prefix, no abs/`..` (`:195`). Overridable: `.fit`, `.csv` (compbas/effects), `.tga`, `.abl/.abx`, and **our `data/defs/**/*.json`**.
- Cache: `.modindex-cache` mtime-invalidated; rebuild `MC2_REBUILD_MOD_CACHE=1`. Diag `MC2_LOG_FILE_RESOLVE=1|2`.
- The `model/anim/ui_override_registry` are a SEPARATE additive mechanism (model one is recon-only, not wired); the LIVE overlay mods use is the `g_modIndex` file-shadow.

## Checkbox / launcher
`mc2_launcher.cpp:1283-1420`: scans install `mods/`, each folder with `data/` → `ModEntry`. `type:"assets"` (or non-campaign folder w/ `data/`) → stackable **checkbox** in the scrollable add-on ListView (`LVS_EX_CHECKBOXES`). On Launch: checked rows → append `folderName` to `MC2_MOD_DEPS`; campaign → `MC2_ACTIVE_MOD`; spawn mc2.exe. **A checked mod reaches the engine purely as its folder name in `MC2_MOD_DEPS`.**

## ★ Our generated defs work as mod overlays — ZERO new plumbing
Source-flip loaders read `data/defs/{units,equipment}/…` (data-relative). A mod placing files at `mods/<id>/data/defs/units/foo.json` is indexed under key `data/defs/units/foo.json` and shadows/adds at mod precedence. **Whole-file shadow, NOT def-merge** → use mod-NAMESPACED filenames so new `qualified_id`s ADD keys (coexist) rather than clobber stock files; our `qualified_id=modId:name` precedence disambiguates at the def-registry layer. Only reuse a stock filename to truly replace it.

## ★ Dormant / default-off stock content
Components = rows in `data/objects/compbas.csv` (`logisticsdata.cpp:222` → MasterComponent). **No single "disabled" column** — "dormant" = present-as-data but not surfaced for PURCHASE (gated by purchase/RP catalog layer, `logisticscomponent.cpp:150-181` reads id/stats/icon only). The E2 registry `enabled` flag is def-registry side, NOT consumed by purchase yet. To enable dormant content a mod either: (a) LEGACY — override `data/objects/compbas.csv` / the purchase/availability `.fit` to re-add/re-surface rows (proven by `mods/magic-ballistic-weapons`); or (b) CLEAN (follow-on) — flip the E2 `enabled` flag in an overriding equipment def AND wire enabled→purchase (not done yet). So `core_unlocks` v0 rides the legacy compbas/purchase override; the clean enabled-flag path is a future engine wire.

## Sample mods to model on (shipped)
`mods/magic-ballistic-weapons/` (`type:assets`, overrides `data/objects/{compbas,effects}.csv`+`.tga` — BEST model for a def/CSV-overlay checkbox mod) · `mods/magic-gamesys-tweaks/` (`type:assets`, overrides `gamesys.fit`) · `mods/modern-tree-pack-v1/` (minimal). Install at `<deploy>/mods/<id>/`.

## Top gotchas
1. **Whole-file shadow, not merge** — same filename as stock = total replace; use namespaced filenames to ADD.
2. **`type:"assets"` required** or the launcher may misbucket a `data/`-bearing folder as a campaign.
3. **Guards:** dep layers can't override core `data/missions/*.abx` ABL (force-dropped `:784`); `mc2mod install` refuses canonical deploy roots (drop folder into install `mods/` directly for testing); dotfiles excluded from packaging/indexing.
