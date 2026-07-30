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

## Which copy of the mission data is authoritative (STOCK-DATA-TRUTH-1)

Everything above is about *where the data came from*. This section answers the
operational question that offline tooling actually asks: **given several trees
on disk that all look like mission data, which one does the engine load?**

**Answer: the deployed `mission.fst`, definitionally.** `mclib/file.cpp`
(`fileExists()` / `File::open()`) resolves in one order:

> active mod overlay → loose file on disk → FastFile (`.fst`)

For every `mc2_*` map, in every deploy lane, the first two rungs miss — no
loose file in any lane shadows an FST member (the loose files are ABL/brain
test fixtures and the custom `gaea_peaks_01` map, none of which are FST
members). So the bytes always come from `mission.fst`.

**A source tree is never authoritative on its own.** It is usable only by
*proving* it equals the authority. That proof is
[`tools/mc2data/stock_missions.md5`](../tools/mc2data/stock_missions.md5) — a
committed md5 of every `data/missions/*` member of the shipped FST, generated
by `scripts/check-stock-mission-data.py --write`.
[`tools/mc2data/stock_missions.py`](../tools/mc2data/stock_missions.py) is the
resolver every offline tool uses; it refuses to return data it cannot verify.

Measured 2026-07-30, whole-archive (not a sampled subset):

| tree | vs the shipped `mission.fst` |
|---|---|
| `mission.fst` in lanes 0.4 / 0.4c / 0.4d-rc1 / 0.5.1 | **byte-identical**, md5 `1e7935dc33fe…`, all 1346 members |
| `A:/Games/mc2-opengl/mc2srcdata-fresh/missions/` | **all 1344 members match**, 0 differ, 0 absent |
| `A:/Games/mc2-srcdata/missions/` (working clone) | differs on `mc2_01` + `mc2_13` (`.pak` and `.fit`) — the uncommitted edits flagged above, independently confirmed |
| `A:/Games/Carver5-feasibility/data/missions/` | **differs on ALL 24 campaign maps**, `.pak` and `.fit` |

### What actually differs in the Carver5 tree — decoded, not just hashed

"Different md5" is not the same claim as "different terrain", so the grids were
decoded field by field across all 24 maps:

- **`elevation` is BIT-IDENTICAL on all 24** (max |delta| = 0.0). Carver5 stores
  the same heightfield uncompressed (packet type `RAW` vs the shipped `ZLIB`) —
  that accounts for most of the size gap (mc2_17: 2.9 MB vs 394 KB).
- `[Water] Elevation` in the `.fit` is identical on all 24.
- `textureData` (base TXM + hi16 overlay handle) differs — 137…14 121 vertices
  per map. `terrainType` differs on 11 maps, up to 14 076 vertices. The packed
  `water` alpha byte and vertex `normal` differ on all 24. Terrain object
  records differ on mc2_04/05/06/11 (up to 14 objects). Every `.fit` differs
  (camera params, `ObjectNumber` lists, `NewScale`).

So a **height-only** analysis reached the same answer either way — the shoreline
lane's NO-GO stands on its own. But anything reading overlays, terrain type,
normals, or the object list (`cook_markings.py`, `overlay_extract.py`,
control-map classification, scatter exclusion, smooth-pass footprint
protection) **was** reading different data, and nothing in the tooling could
tell which. That is the defect: not "the numbers were wrong" but "nobody could
tell whether they were."

Why this section exists: twelve offline tools defaulted their mission-data
path to `A:/Games/Carver5-feasibility/data/missions` and called it "stock
mission data". Everything run with default flags was therefore grading terrain
the engine never loads — silently, because the files exist, parse, and produce
plausible numbers. A shoreline lane came within one meta-review of publishing
a go/no-go computed on it
(`.claude/SHORELINE-V0-FALSIFY-1-STATUS.md` §0a). Note that *requiring* the
flag would not have prevented it: the wrong path arrived in a brief, and a
required flag would have had the lane type the same wrong path and get the
same silence. **Verification is the fix; requiredness is not.**

`scripts/check-stock-mission-data.py` keeps it from returning: a structural
`ast` scan that fails on any hard-coded absolute mission-data path in
executable code anywhere in `tools/` or `scripts/` — not a denylist of one bad
string, so the same defect cannot come back under a different wrong path.
Registered in `check-contracts.sh` as `stock_mission_data` +
`stock_mission_data_selftest`.

**Not settled by this**: the *derived* beauty sidecars
(`data/missions/<stem>.beauty/shoreline_mask.png` etc). Those are cook outputs,
not FST members — `check-beauty-sidecars.py` owns their cooked-vs-installed
drift. See the note under "Remaining unknowns".

**Adjacent, latent, unfixed**: the same "a source tree is assumed
authoritative" shape exists for other corpora, with no verification attached —
`tools/blender_pipeline/{srcdata,tex_hd}.py` and `tools/mc2texcook/
ase_geo_audit.py` (`mc2srcdata/tgl` vs `tgl.fst`),
`tools/terrain_beautify/test_cook_markings.py` (`mc2srcdata/textures/
64Overlays` vs `textures.fst`), and `tools/mc2weapon/mc2weapon.py`
(`objects/compbas.csv`, which searches the editable clone *before* the deploy).
All four were measured 2026-07-30 and are currently **correct** (64Overlays
475/475 match `textures.fst`; `tgl` 1432/1432 of the members that exist as
source files match `tgl.fst`) — so they are a drift risk, not a live wrong
answer, and were left alone rather than half-fixed.

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
