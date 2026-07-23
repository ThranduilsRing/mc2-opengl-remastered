# Game Data Provenance

Answers GitHub issue #32 ("Data Files: Patches Applied?"). Distinguishes what
is verifiable from the repository and its data trees from what requires
upstream confirmation. Nothing here is asserted beyond what was actually
checked; the "Remaining unknowns" section lists what was not.

Last researched: 2026-07-16 (issue-fix batch, branch `claude/github-issue-fixes-1`).

## TL;DR

- The shipped game data is built from **`alariq/mc2srcdata`** (the Linux
  port's curated asset tree, first published 2017), not lifted from a patched
  retail install.
- **`mc2_patch.exe` is not an official Microsoft patch.** It is a fan-made
  rebalance by the modder Magic (of MC2X). Its headline content markers were
  checked against the shipped data and **all read stock/unpatched**.
- Magic's rebalance content *does* exist in this repo — deliberately, as
  **opt-in mods** (`mods/magic-gamesys-tweaks`, `mods/magic-ballistic-weapons`)
  that are never applied to the stock data and are not part of the release
  zips.
- This project's own data changes are **additive** (new files, optional
  upscale overlays, texture caches); no stock gameplay file has been replaced
  in a shipped release. Known local working-tree edits that have NOT shipped
  are flagged below so they don't ship silently later.

## Source chain

1. **MechCommander 2 retail** (Microsoft/FASA Interactive, 2001). Microsoft
   later released the source and assets under the **Shared Source Limited
   Permissive License** (full text: [`EULA.txt`](../EULA.txt); summary table:
   [`LICENSES.md`](../LICENSES.md)), which grants reproduction and
   redistribution of derivative works — the legal basis for shipping the data
   at all ("No original MC2 install required" in the release README).
2. **[`alariq/mc2srcdata`](https://github.com/alariq/mc2srcdata)** — "data
   files for my Mech Commander 2 linux port" (author sebi / Sergii
   Biloshytskyi, initial commit 2017-09-11). A build-scriptable source-asset
   tree, not a raw game dump. It is acquired by plain `git clone` (not a
   submodule); the local clone lives beside the engine repo.
3. **`mc2srcdata/build_scripts/`** (GNU Make) builds the runtime data with the
   tools compiled alongside the engine — `aseconv`, `makefst`, `makersp`,
   `pak`, `text_tool` — producing the `.fst` archives (`mission.fst`,
   `art.fst`, `textures.fst`, …) plus the loose `data/movies`, `data/objects`,
   `data/sound` directories. **There is no patch-application step anywhere in
   this pipeline.**
4. **Deployed install** = engine build + step-3 output + this project's
   additive `data/` files (see below).
5. **Release zips** are packaged from the deployed install + `mc2srcdata` by
   [`scripts/build_release.sh`](../scripts/build_release.sh)
   (`mc2-gamedata.zip` = `data/` + `*.fst`; upscale overlays and movies ship
   as separate optional zips; `mods/` is not packaged).

## The `mc2_patch.exe` question — answered at the content level

**What `mc2_patch.exe` actually is:** despite circulating as "the official
patch" (e.g. on sarna.net), a 2026 analysis
([MHLoppy, "What does the 'official' MechCommander 2 patch actually change?"](https://mhloppy.com/2026/04/mechcommander-2-patch-changes/))
identifies it as a **fan-made rebalance by the modder Magic**, touching ~936
files: MC1-style weapon stats, weapon ranges 60/140/200 → 80/160/240-250,
mission payouts cut to ~10%, roughly half the campaign pilot roster removed
(including Hacksaw), all-mechs-get-sensors, and a rewritten `corebrain.abx`
AI. Microsoft never shipped an official content patch for MC2.

**Checked markers in the data we actually ship** (checked in `mc2srcdata` at
`19e8be1`, the tree that built v0.5.0's `mission.fst` on 2026-04-21):

| Marker | Patched value | Shipped value | Verdict |
|---|---|---|---|
| `missions/gamesys.fit` `WeaponRange` | 80/160/240-250 | `60, 140, 200` (line 14) | stock |
| `missions/gamesys.fit` shot modifiers | Magic tweaks these | `HeadShot 0.08 / LegShot 0.20 / ArmShot 0.15` (lines 521-523) | stock |
| Pilot roster | Hacksaw removed | Hacksaw present in `mc2_01.fit` and other missions | stock |
| `corebrain.abx` | Magic rewrite | alariq's initial 2017 import + his own `tacOrderParams` 15→19 engine-compat fix (commit `b5dd88d`, commented `//sebi:`) | stock + documented compat fix |

**Conclusion: the shipped stock data does NOT contain `mc2_patch.exe`
content at any of the checked markers.** The honest caveat: the patch touches
~936 files and we verified the headline markers, not a byte-level diff of
every file — see "Remaining unknowns".

**Where Magic's content DOES appear (and why someone might spot it):** the
repo ships it as clearly-labelled **opt-in mods** —
`mods/magic-gamesys-tweaks/` (Magic's own README: ranges 0-80/50-160/80-240,
difficulty changes) and `mods/magic-ballistic-weapons/` (MC Gold-style weapon
stats, new equipment). They live under the explicit mod pathway, are loaded
only when enabled, and are not part of any release zip
(`build_release.sh` packages engine/gamedata/movies/burnins/art/tgl only).

## alariq's own deviations from raw retail (all in `mc2srcdata` git history)

- `b5dd88d` (2017) — "ORIG FIX: mission resource files": `tacOrderParams`
  array 15→19 in `corebrain.abx`, `orders.abx`, `pbrain.abl`, `unipbrain.abl`
  to match the port's `execGetTacOrder` C++ side. Engine-compat, not a
  gameplay rebalance.
- `76722ab` (2017) — "recoded some wav files, to be valid".
- `bc51a96` — added 1920-res UI layouts.
- Build-script fixes (`bf8a08e` etc.).

Whether alariq's 2017 import came from a retail CD or the free 2006
Microsoft download is not documented upstream — but the two are content-
equivalent at every marker checked above.

## This project's modifications on top of alariq (as of v0.5.0)

**Committed to the local `mc2srcdata` clone:**
- `19e8be1` (2026-05-06) — 4× art upscales trimmed to UI/portrait-only (92
  files kept in `art_4x_gpu/`), all 1112 `tgl_4x_gpu` object-texture upscales
  removed from the loose-file override (archived in
  `tgl_4x_gpu_archive.zip`). Additive sidecars only.

**Shipped in releases, all additive (new files — no stock file replaced):**
- Engine-repo `data/`: `defs/` (UI defs), `materials/`, `model_overrides/`,
  `hdr/`, `cdag/`, `terrain_materials.json`, `visual_tuning.json`,
  `vegetation/` — modern-engine data that has no stock counterpart.
- TechBrain exports: loose `data/missions/*_ai.fit` + declarative
  `data/missions/warriors/*.abl` brains (new files consulted only by the
  Brain framework).
- Derived caches: `data/textures/*.ktx2/*.txm` compressed-texture caches,
  per-mission `.burnin` and terrain sidecars.
- Optional overlay zips: 4× burnins, 4× UI/art. Invariant: a stock install
  must remain playable without any of them
  (`memory/stock_install_must_remain_playable.md`).

**v0.5.0 FST build dates** (from the release lane): `art/camera/effect/
insignia/misc/mission/textures.fst` built 2026-04-21 from `mc2srcdata@19e8be1`;
`tgl.fst` rebuilt 2026-06-12 (mech-asset work era — exact delta vs a pure
alariq build was not recorded; see unknowns).

**Local working-tree edits that have NOT shipped** (flagged so they don't
ship silently): the local `mc2srcdata` clone carries uncommitted edits to
`missions/mc2_01.fit/.pak` and `missions/mc2_13.fit/.pak` (camera parameters,
`ObjectProfile` swaps, terrain pak growth — editor/terrain-work era,
2026-06/07). v0.5.0's `mission.fst` (built 2026-04-21) predates them. Any
future FST rebuild MUST either commit-and-document or revert these first.

## Relevance to the AI documentation in `/docs/`

The AI behavior documented in this repo (corebrain FSM, `calcTacOrder`
goal-plan, warrior decision trees) describes the **stock** MC2 AI as imported
by alariq (plus his `tacOrderParams` compat fix) — NOT Magic's rewritten
patch AI. Triage work on issues #9/#10/#17 (2026-07) additionally verified
the port's decision core byte-identical against the original 2001 source
release, which is only meaningful because the data is stock.

## How to verify yourself

```bash
# 1. Weapon ranges (stock = 60, 140, 200):
grep -n "WeaponRange" mc2srcdata/missions/gamesys.fit
# 2. Called-shot modifiers (stock = 0.08 / 0.20 / 0.15):
grep -n "ShotModifier" mc2srcdata/missions/gamesys.fit
# 3. Pilot roster (patch removes Hacksaw; stock contains him):
grep -rli "hacksaw" mc2srcdata/missions/ | head
# 4. Everything this project changed vs alariq:
git -C mc2srcdata log --stat origin/master..HEAD ; git -C mc2srcdata status
```

## Remaining unknowns (honest list)

- **No byte-level diff of all ~936 patch-touched files** has been performed —
  only the headline markers above. A full diff would require obtaining
  `mc2_patch.exe`, extracting it against a retail install, and hashing both
  trees; nobody has done this here.
- **The exact `tgl.fst` (2026-06-12 rebuild) delta** vs a pure-alariq build
  was not recorded at the time.
- **v0.3-era zip contents were not re-audited** for this document (v0.3 is
  superseded); the packaging recipe and data sources are the same chain, but
  if the reporter saw patch-flavored files in v0.3 specifically, a listing of
  which files would let us trace them (most likely candidates: the `mods/`
  folder of a local install, which is not part of the distributed zips).
- **alariq's original import source** (retail CD vs 2006 free release) is
  undocumented upstream.
