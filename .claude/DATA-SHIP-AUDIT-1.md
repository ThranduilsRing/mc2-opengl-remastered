# DATA-SHIP-AUDIT-1 — shipped-data wholesale audit

**Lane:** AUDIT (read-only vs installs; census script committed).
**Primary target:** `A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0` (current lane).
**Cross-check:** `A:/Games/mc2-opengl/mc2-win64-v0.4` (drift).
**Tool:** `tools/data_ship_census.py` (new, reusable; stdlib-only; census / FST parse /
loose-vs-FST shadow hash-compare / duplicate groups / stale-gen heuristics / lane drift).
**Date:** 2026-07-01.

---

## 1. Census headline (v0.5.0)

Install total **3,358 MB / 28,309 files**; `data/` = **2,810 MB / 27,868 files**.

| Extension | Files | MB | Notes |
|---|---:|---:|---|
| .ktx2 | 5,035 | 966 | 896 MB of it = seven 128 MB 16k BC6H skyboxes in `data/hdr/` |
| .tga  | 5,863 | 879 | 320 MB `data/textures`, 349 MB `data/tgl` (318 MB in `tgl/128/`), 89 MB inside art.fst… |
| .jpg  | 128 | 414 | 347 MB = 116 per-mission `.burnin.jpg` colormaps (this is the cooked form) |
| .fst  | 8 | 242 | textures 175, tgl 24.7, art 23.4, mission 18.8, + 4 small |
| .bik  | 215 | 165 | movies (ffmpeg path) |
| .wav  | 286 | 153 | SDL2_mixer path |
| .txm  | 12,420 | 59 | terrain composite cache (generated; `terrtxm.cpp` quickLoad) |
| .pak  | 40 | 52 | mission terrain + sound paks |

Top dirs: `data/textures` 1,032 MB · `data/hdr` 919 MB · `data/tgl` 418 MB ·
`data/sound` 201 MB · `data/movies` 168 MB · `data/art` 81 MB · `data/missions` 44 MB.
Non-payload clutter at install root: `importer/` 21 MB (PyInstaller import tool —
confirm intent), `crashes/` 7 MB, `debug_state/` 7 MB, `terrain_gen_out/` 4 MB, logs/bat scraps.

FST archives (all stamped LZ `0xFADDECAF`, but **payloads are zlib** — makefst
zlib-compresses everything; engine `LZDecomp` sniffs the zlib magic per payload,
`mclib/lzdecomp.cpp:454`; the LZW path is a fallback. The census tool initially
tripped on exactly this — archive stamp is NOT the payload codec):

| Archive | Entries | comp MB | real MB | Dominant content |
|---|---:|---:|---:|---|
| textures.fst | 1,396 | 174.7 | 267.4 | stock terrain .tga |
| tgl.fst | 4,212 | 23.7 | 66.9 | prop .tga/.tgl/.agl |
| art.fst | 666 | 23.2 | 91.0 | UI .tga |
| mission.fst | 1,346 | 18.4 | 33.3 | .pak/.abl/.fit/.ftw |
| misc/camera/effect/insignia | 101 | 0.4 | 1.8 | |

## 2. Loose-vs-FST shadow finding

Resolution order (mclib/file.cpp:69): **mod > base-loose > base-strip (numeric size
subdir stripped) > fastfile > CD** — loose always overrides FST; that override is the
load-bearing modding semantic and any packing plan must preserve it.

- 2,293 loose files share a normalized key with an FST entry.
- **1,977 (86%) are byte-identical to the FST copy — 43.2 MB of pure loose redundancy**
  (verified by per-payload decompress + sha256). By archive: tgl.fst 1,326 / 34.2 MB,
  textures.fst 595 / 7.9 MB, effect.fst 1 / 0.9 MB, insignia.fst 55 / 0.2 MB.
- 316 are genuine overrides (differ): art.fst 37 (UI reskins), textures.fst 174,
  tgl.fst 101, misc/mission 4 — the mechanism working as intended; keep.
- As a share of all loose data files: 7% shadow an FST entry.

Deleting the 1,977 identical shadows changes nothing at runtime (FST serves the same
bytes) and reclaims 43.2 MB / ~2k files.

## 3. Redundancy findings, ranked by reclaimable MB

| # | Finding | MB | Action class |
|---:|---|---:|---|
| 1 | **16k HDRI tier**: 7 × 128 MB BC6H skyboxes = 32% of `data/` | ~840 | Ship 4k tier by default (existing `DaySkyHDRI063B_4K.ktx2` is 8.4 MB → seven 4k ≈ 60 MB); 16k as optional download pack. Packaging decision, zero engine. |
| 2 | **.burnin.tga without .jpg twin** (terrain004 117.2, newmap 12.3, gaea_peaks 6.8, genmap 3.0) | ~135 | Convert to `.burnin.jpg` q90 like the other 116 (loader: `terrtxm2.cpp tryLoadBurninJpg`, File::open, `MC2_BURNIN_NO_JPG` killswitch). Conversion, not deletion. |
| 3 | **`data/tgl/128/*.tga` with `.ktx2` sidecar**: 1,112 loose / 72.7 MB (includes all 610 /128 shadow files) + 659 FST twins (11.8 MB comp) | ~85 | Purpose-built path already exists: delete loose + `tools/fst_repack_drop.py` on tgl.fst; engine ROUTE-2 (`MC2_TEXMGR_KTX_PRIMARY`, default 1) decodes the BC7 sidecar on .tga miss. |
| 4 | **Identical loose shadows** outside #3: 1,367 files | 9.3 (43.2 total incl. #3 overlap) | Delete loose; FST serves identical bytes. |
| 5 | **`data/tgl/128/_tga_slim_backup/`** — backup dir from the slim arc shipped inside the install | 34 | Delete dir. |
| 6 | **Cruft suffixes**: `*.old` 10 (terrain004.pak.old…), `.orig_normal` 12, `.bak` 3, `.orig` ×33 1.9, `thumbs.db` 4.6 | ~32 | Delete. |
| 7 | **Duplicate content groups** (2,317 groups, 158.7 MB gross waste; overlaps rows 5/6): `snow.tga` byte-identical to `mat4_normal.tga` (16 MB — **likely a placement bug**: snow albedo is a normal map; cf. the 0.4 cement-pad albedo issue in docs/asset-pipeline.md), 531+180+178 identical tiny `.txm` (35.7 MB), 47 identical `x_pavedroad_bridge*.tga` frames (2.9 MB), 30 identical `*.detail.tga` (1.8 MB), `genmap.burnin.tga` duplicated in `terrain_gen_out/` (3 MB) | ~40 net | Investigate snow.tga (bug, not dedupe); the rest are reference-shaped (same content, many names) — dedupe needs ref-rewrite, low value; leave except the obvious `terrain_gen_out` copy. |
| 8 | **Dev dirs in ship lane**: crashes/debug_state/terrain_gen_out/logs (+ `importer/` if unintended) | ~20–40 | Lane hygiene; exclude from release payload. |
| 9 | **Cook candidates** (later, needs loader check): `mat0..8_normal.tga` 9 × 12–16 MB = 134 MB uncompressed 2048² normals; 1,161 `/128` upscale .tga with no ktx2 sidecar (245 MB) | ~250 as BC7 | Cook-to-KTX2 arc, not deletion. |

Sidecar sprawl (fine, censused): 5 `.beauty` dirs, 20.8 MB (terrain004 15.3 — mostly
`visual_height_4x.r32`). **Authoring surface — must stay loose.**

**v0.4 drift cross-check:** v0.4 = 8,213 MB total (data/ 6,301 MB): +3.1 GB .ktx2
(4× cook tiers), +1.27 GB mech .glb experiments (62 files), +1.18 GB `mods/`
(11k files, MCO setup), 191 numeric-subdir strip-twins (661 MB, vs 8 in v0.5),
20 MB `.orig_bak` etc. v0.5.0 already shed ~5 GB of that; same shadow ratio
(2,291 matched / 237 overrides). Conclusion: stale generations accumulate per lane;
the census script should run as a release gate.

## 4. Packing strategy

### (a) FST status quo — what it gives / costs
Gives: elfHash per-archive lookup (`ffile.cpp openFast`, linear scan with hash
short-circuit over ≤4,212 entries — fine at current scale); zlib payloads (LZW is
legacy fallback only); **loose-override modding for free** (loose checked first);
mature tooling (`makefst.exe` + `pak.exe` ship in the install; `fst_listing.py`,
`fst_repack_drop.py`, `mc2x_import/fst.py` in repo); archives registered data-driven
via `system.cfg [FastFiles]` — adding an archive is a config edit, no engine change.
Costs: u32 offsets (4 GB archive cap — fine, largest is 175 MB); 250-char member
names; **no store mode** (`readFast` unconditionally decompresses — the comp==real
raw path is commented out "ALL files are now zLib compressed NO EXCEPTIONS", so
pre-compressed payloads must at minimum be zlib-framed; zlib level-0/1 ≈ store with
memcpy-speed inflate); whole-payload inflate per open (no streaming). No existing
decompress-cost measurements: the 2026-06-21 startup/load arc (SAVE-LOAD-FAST,
STARTUP-INIT-ASYNC, PARALLEL-VARIANTS, SMART-LOAD) targeted logistics/CSV/save-scan,
not FST I/O — measure before optimizing.

### (b) Pack loose modern assets into our own FSTs — what breaks
**Hard blocker for the big categories: the modern loaders bypass `File::open`.**
- `.ktx2` (966 MB): `RenderCore/KtxLoader.cpp:65` raw `std::fopen`.
- `.wav` (153 MB): `SDL_LoadWAV`/`Mix_LoadWAV` by path (`gameos_sound.cpp:247/267`).
- `.bik` (165 MB): ffmpeg `avformat` by path.
- `.glb`: Assimp by path.
FST-packable **today** (File::open confirmed): `.txm` cache (12,420 files / 59 MB —
biggest file-count win), `.burnin.jpg` (loader deliberately uses File::open for mod
overlay, `terrtxm2.cpp:110`), and all legacy .tga/.fit/.pak/.abl/.csv/.tgl/.agl.
**Must never pack** (authoring/mod/hot-iteration surfaces): `shaders/` (deploys in
lockstep with exe, hot-reload), `data/missions/*.beauty/` + in-authoring mission
files (editor writes them), `model_overrides/*.json` manifest, `data/terrain_materials.json`,
`data/visual_tuning.json` (live-tuned), `*.mcasset.json` sidecars, `options.cfg`/prefs,
`mods/` trees, generated-map burnins while the terrain_gen loop is active.

### (c) Better-than-FST options
| Option | Engine cost | Modder cost | Verdict |
|---|---|---|---|
| **Keep FST + zlib-0 for pre-compressed payloads** | none (LZDecomp already sniffs zlib; level-0 inflate ≈ memcpy) | none (makefst tweak / py packer) | **Do this** when packing ktx2-class data |
| FST + zstd | small: add zstd magic sniff branch in `LZDecomp` (same trick as the existing zlib sniff) + makefst variant | must use our packer (already true) | Defer until a measured decompress bottleneck exists |
| zip-store container | new archive backend behind the `FastFileFind` seam in the File::open fallback chain | any zip tool (best modder story) | Not worth it now: FST already delivers loose-override + tooling; revisit only if modders ask for standard tooling |
| Route `KtxLoader` through `File::open` | one seam (fopen → File; `FitIniFile::open(buffer,len)` in-memory groundwork exists, file.cpp:1355) | none | The actual unlock for packing the 966 MB ktx2 category; pairs with zlib-0 |

The real load-perf lever is likely **fewer file opens** (12k+ .txm probes), not
compression choice — measure with a harness before any format work.

## 5. Recommendation — staged plan

1. **DATA-SHIP-DEDUPE-1** (free ~120 MB, zero behavior change): delete 1,977
   verified-identical loose shadows (43.2 MB, census JSON is the manifest),
   `_tga_slim_backup/` (34 MB), cruft suffixes + thumbs.db (~32 MB),
   `terrain_gen_out/genmap.burnin.tga` dup (3 MB); exclude dev dirs from the release
   payload. Gate: re-run census (0 identical shadows) + tier1 smoke.
2. **SNOW-TGA-TRUTH-1** (RESOLVED — dead file, not a rendering bug): `data/textures/snow.tga`
   is byte-identical to `mat4_normal.tga` (sha256 `2a890ae2…`), but unlike the 0.4
   cement-pad-albedo-was-normal-map bug, this file is **not live** — grepped the
   whole tree (C++, shaders, JSON manifests, `.ini`/`.tgl`, `textures.fst` listing)
   for the literal string `snow.tga`: zero hits. Terrain snow color comes from an
   HSV-derived tint (`tintSnow` uniform, `shaders/gos_terrain.frag`/`terrain_lod_chunk.frag`,
   `gos_SetTerrainTintSnow`), not a sampled albedo texture; the only file the loader
   touches by name is `mat4_normal.tga` (`mclib/terrtxm2.cpp:2457`, `normalNames[4]`),
   loaded into `matNormalArray` layer `MAT_LAYER_SNOW` — that IS correct and distinct
   from mat0-3 (verified unique sha256 per slot). `snow.tga` is also absent from
   `textures.fst` (loose-only, no FST override to worry about) and the modern PBR
   cook already produced the real albedo at `data/terrain_layers/snow_albedo.ktx2`
   (source `snow_field_aerial_col_2k.jpg` per `tools/mc2texcook/terrain_layer_manifest.json`),
   which the tint path doesn't even need. **Verdict: delete-candidate, no engine/asset
   change required.** Likely a leftover from the classic DirectX-era splat set
   (`data/textures/64/snow 01.tga`, `snow 02.tga` — also unreferenced by any current
   loader, not investigated further here). Safe to fold into DATA-SHIP-DEDUPE-1's
   cruft-deletion pass; not a visual-correctness fix.
3. **BURNIN-JPG-CONVERT-1** (~135 MB): cook the 4 remaining `.burnin.tga` to q90
   `.burnin.jpg`; keep `MC2_BURNIN_NO_JPG` escape hatch; per-map visual check.
4. **TGL128-TGA-SLIM-1** (~85 MB): drop the 1,112 sidecarred `/128` .tga loose +
   `fst_repack_drop.py` the 659 FST twins; already designed for ROUTE-2 default.
5. **HDRI-TIER-SPLIT-1** (~840 MB off the default download): 4k skyboxes default,
   16k as optional pack (cook via shipped `tools/cook_bc6h_hdri.py`).
6. **TXM-COLDPACK-1** (file-count, then measure): makefst the 12,420 `.txm` into
   `txmcache.fst` + `system.cfg File8=`; harness-measure load delta (this doubles as
   the FST-perf measurement the format decision needs).
7. **FORMAT-DECISION** (only after 6's numbers): stay FST; if ktx2 packing is wanted,
   do the KtxLoader→File::open seam + zlib-0 packing; zstd/zip only with evidence.

Must-stay-loose list (final): `shaders/`, `.beauty/` dirs, editor-authored mission
files, `model_overrides/` manifest + GLBs, renderer JSON configs, `.mcasset.json`,
prefs/options cfg, `mods/` trees — plus (until the loader seam ships) all `.ktx2`,
`.wav`, `.bik`, `.glb`.

---

## 6. Post-dedupe regression re-check (2026-07-01, DEDUPE-BYPASS-RECHECK-1)

A "world looks low-res" report on 0.5-testing triggered a full audit of the
DATA-SHIP-DEDUPE-1 deletions (`.claude/dedupe-manifest-2026-07-01.json`,
commit 08a4cc04) against the bypass-loader hazard. **Verdict: the dedupe is
clean — zero files need restoring.** Evidence:

- Manifest = 2,472 deletions: 1,977 FST shadows (.tga 1,260 / .ini 716 / .fx 1),
  455 `_tga_slim_backup/` .tga, 40 cruft (.orig/.old/.bak/.db). **Zero
  bypass-loader extensions** (.ktx2/.wav/.bik/.glb/.fbx/.json) in any category.
- Lane populations of bypass types match this audit's pre-dedupe census
  exactly: .ktx2 5,035 / 966 MB · .wav 286 / 153 MB · .bik 215 / 165 MB ·
  .glb 4. Nothing outside the manifest was deleted (burnin colormaps intact:
  116 `.burnin.jpg` + 4 `.burnin.tga`; dev dirs untouched; all 8 FSTs present
  and registered in `system.cfg`).
- All 1,977 deleted shadow keys re-extracted from their FSTs and sha256+size
  verified against the manifest: **1,977/1,977 match** — every deleted file is
  byte-identically FST-servable.
- Reachability proven, not assumed: `LINUX_BUILD` is defined globally
  (CMakeLists.txt:221) so `PATH_SEPARATOR` is `/` and `FullPathFileName`
  lowercases — probe strings hash-match FST keys in both `File::open`
  (normalizes at file.cpp:986) and `fileExists` (no normalization of its own;
  works only because inputs are already normalized). Strip-layer hazard
  (file.cpp:1029 probes the parent dir BEFORE the fastfile) checked for all
  612 deleted keys with a numeric size subdir: zero parent-dir twins exist,
  so the FST is always reached. Loose-only probes audited: `msl.cpp:467`
  (`FILE_ON_DISK`, .glb/.fbx — none deleted) and `msl.cpp:849` (`== 1`,
  cache-warm touch only — benign).
- `.burnin.ktx2` specifically: the lane contains **zero** such files (before
  and after dedupe); the COLORMAP-BC7-KTX2-1 probe (terrtxm2.cpp:1677, raw
  `std::fopen`, **no FST fallback**) has never fired on this lane, and its
  absence costs VRAM only (silent same-resolution RGBA8 fallback) — it cannot
  cause a low-res world.
- Post-dedupe census gate GREEN: 0 identical shadows remain, all 316 genuine
  overrides preserved.

The low-res report is therefore **not** explained by DATA-SHIP-DEDUPE-1;
investigate elsewhere (options.cfg texture-detail drift, `MC2_TEXMGR_KTX_PRIMARY`
env, or the other commits on the lane).

**Permanent hardening:** `tools/data_ship_census.py` now classifies identical
shadows of bypass-loader types into a separate `identical_bypass_loader`
do-not-delete bucket (`BYPASS_LOADER_EXTS = {.ktx2, .wav, .bik, .glb, .fbx,
.json}`), so a manifest-driven dedupe can never delete a loose file whose
loader cannot read the FST. Any future loader that bypasses `File::open` must
add its extension to that set.

---
*Census tool: `tools/data_ship_census.py`. Raw JSON snapshots (v0.5.0 + v0.4 + drift)
were written to the session scratchpad; re-generate any time with:*
`py -3 tools/data_ship_census.py "<install>" [second_install] --json out.json`
