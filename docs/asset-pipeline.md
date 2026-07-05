# MC2 Asset Pipeline & Inventory

> **This is the canonical asset reference.** It catalogs every asset type — where
> it lives, how it loads, its resolution / vertex budget, whether it has been
> upscaled/cooked, and who *owns* it on the **render** side vs the **gamedata**
> side. It also documents the upscale → cook → deploy pipeline.
>
> **Last verified:** 2026-06-17 (against `claude/nifty-mendeleev`, deploy rc1; merge `fdb7c470`).
> **Maintenance:** see [§7 Keeping this current](#7-keeping-this-current). Update this
> doc whenever you add/remove/re-cook/upscale an asset set, change a loader, or change
> the cook/upscale pipeline. `scripts/check-asset-pipeline-doc.sh` flags drift.

---

## 1. Master inventory

| Type → subtype | Location / format | Loader (file:line) | Size / res | Upscaled? | Render owner | Gamedata owner |
|---|---|---|---|---|---|---|
| **Terrain — colormap/splat** | deploy `data/textures/*.ktx2`+`.txm` (~7,822 root); src `.burnin.tga` 5120² | `mclib/terrtxm2.cpp:1613-1695` (ktx2 sidecar probe) → `gos_terrain_indirect.cpp:867` BC7 upload | 5120² atlas (4× from 1280²) | **Yes** (4× ESRGAN; `release_assets/0.3/mc2-burnins-4x-pt1/2.zip`, 2.3 GB) | `gos_terrain_indirect.cpp` (atlas bake + indirect draw) | `terrtxm2.cpp TerrainColorMap::init`; mission `.pak` |
| **Terrain — detail/normal** | `data/textures/{32,64,128,256}/detail_*.tga` (~20) | `terrtxm2.cpp:1814` → `shaders/gos_terrain.frag:44-48` (POM) | 64²→128²/256² | Yes | `gameos_graphics.cpp` terrain shader | `txmmgr.cpp` / TerrainColorMap |
| **Terrain — water** | `data/textures/{tier}/water_ocean.tga` + 64-frame anim | `terrtxm2.cpp:1836-1895` → `gameos_graphics.cpp:2617` | 64²→256² | Base yes | water fast-path (`gameos_graphics.cpp`) | TerrainColorMap; mission `.pak` |
| **Terrain — masks/overlays** | `data/textures/{tier}mask/` (126×4), `{tier}Overlays/` (475×4) | indirect cement atlas `gos_terrain_indirect.cpp:4140` | 64²→256² | Yes | indirect overlay packer | `mapdata.cpp`/`quad.cpp` |
| **Terrain — heightmap** | runtime GL tex (R32F) rebuilt from quad verts; src `mc2srcdata/terrain/*.tga` | `gos_terrain_height_tex.cpp:38` | mission grid (≈128²/256²) | n/a (procedural) | terrain vert shader (sampler unit 11) | `terrain.cpp`/`quad.cpp` (mission `.pak`) |
| **Static prop — geometry** (buildings ~2760, trees ~65, turrets ~111) | src `mc2srcdata/tgl/*.ase` (2,947); compiled `.tgl` → **`tgl.fst`** (33 MB) | `mclib/msl.cpp:563 LoadTGMultiShapeFromASE`; `bdactor.cpp:217/3391` | verts/tris in `.tgl` (`TG_TypeVertex` 28B `tgl.h:35`; `TG_TypeTriangle` 64B `tgl.h:121`) | n/a (geometry) | **`GpuStaticPropBatcher`** (`gos_static_prop_batcher.cpp:143`) | `BldgAppearanceType`/`TreeAppearanceType` (`bdactor.cpp`) |
| **Static prop — textures** | deploy `data/tgl/{128,256,512,1024}/*.ktx2` (BC7) | `gos_static_prop_batcher.cpp:830/2435` (path) → `RenderCore/KtxLoader` | 128 base; **256/512/1024 tiers** (cook 2026-06-02) | **Yes** (src 512/1024 in `mc2-tgl.zip`) | GpuStaticPropBatcher | BldgAppearance |
| **Mech — geometry** (13 src) | `mc2srcdata/tgl/*.ini`+`.ase`; only madcat deployed | `mclib/mech3d.cpp:239-387 init`; `gos_mech_batcher`; `GameAdapters/MechRenderAdapter.cpp:48` | verts via ASE `NUM_VERTEX` (`tgl.cpp:886`); 3 LODs, 25 anims | partial (4× archives) | `mech3d.cpp` (engine appearance) + `gos_mech_batcher` | `code/mech.cpp` BattleMech (game AI) |
| **Mech — GLB import (Flea)** | rc1 `data/tgl/Flea.glb` + `Flea.mcasset.json`; src `mc2srcdata/tgl/flea.ase` | `mclib/msl.cpp LoadFromFile` → `[Import] Source=Flea` in mech ini; `mech3d.cpp` LOD0 override; kill switch `MC2_ASSIMP_IMPORT=0` | GLB LOD0 (Assimp import); texture `data/tgl/128/fireantrgb.ktx2` (BC7, 512×512, AI-upscaled) | Yes (AI-upscaled src) | `mech3d.cpp` via `TG_TypeMultiShape` Assimp path | `code/mech.cpp` BattleMech |
| **Building — GLB import (HangarGLB)** | rc1 `data/tgl/HangarGLB.glb` + `HangarGLB.mcasset.json`; src `mc2srcdata/tgl/hangar.ase` | `bdactor.cpp` `[Import] Source=HangarGLB`; LOD0 probe; positive load confirmed mc2_01 via ASSIMP_TRACE; kill switch `MC2_ASSIMP_IMPORT=0` | GLB LOD0; texture `data/tgl/128/a_hangar.ktx2` | No | `GpuStaticPropBatcher` / `BldgAppearanceType` | `bdactor.cpp BldgAppearanceType` |
| **Mech — textures** | `mc2srcdata/textures/*.txm` (paint-hash) → `data/tgl/{tier}/*.ktx2` | `mech3d.cpp:1941 resetPaintScheme` → `txmmgr.cpp` | 128–1024 bucket; 27-bit paint instance | Yes (`art_4x_gpu`/BC7) | `txmmgr.cpp`; `RenderCore::MechVisualState` | BattleMech |
| **Vehicles** (7 src) | `mc2srcdata/tgl/*.ini`+`.ase`, `objects/*.fit`; **not deployed in v0.4** | `mclib/gvactor.cpp:151 GVAppearanceType::init` (mirrors mech) | 3 LODs, 10 anims | — | `GVAppearance` (`gvactor.cpp`) | `code/gvehicl.cpp GroundVehicle` |
| **VFX — GPU particles** | procedural SSBO (no disk tex); 64B `GpuParticle` | `gos_particle_bridge.cpp`; `mclib/particles/batcher.cpp`; `shaders/particle_billboard.{vert,frag}` | 64B/particle; atlas frames | n/a | `gos_particle_bridge_flush` | `code/weaponbolt.cpp`; `particles/spawn_*` |
| **VFX — gosFX cards/clouds** | `data/effects/mc2.fx` (~900 KB #XFG); MLR tex pool | `mclib/gosfx/cardcloud.cpp:18`; `mlr/mlrcardcloud.cpp` (CPU legacy) | world-unit cards + UV curves | no | `particle_billboard` (GPU) / MLRCardCloud (CPU) | `weaponbolt` fire/hit/miss IDs |
| **VFX — GPU trails** | procedural; `mclib/particles/gpu_trail.{h,cpp}` | `weaponbolt.cpp:1660 GpuTrailEmitter::Spawn` | density/m; MLR tex | no | particle bridge | weaponbolt (missile smoke/PPC) |
| **VFX — burn-ins** | `.burnin.tga/.jpg` per mission; (also terrain colormap) | `mclib/burnin_jpeg_decode.cpp:25`; `terrtxm2.cpp:94` | 5120² | Yes (4× release zips) | terrain splat | mission load |
| **UI/HUD art** | `data/art/` (~301 MB), `art/gui/` (531) `.tga`/`.png`; `art.fst` (24 MB) | `ui_editor/UiEditorImageCache.cpp:18`; `GuiRuntime/GuiRuntime.cpp`; legacy `code/controlgui.cpp` | menus 1024² TGA; HUD 256²/128² PNG | **Yes** (`mc2-art.zip` 1024/512) | GuiRuntime (ImGui) + legacy UI | `missiongui`/`campaigngui` |
| **Fonts** | `data/defs/ui/fonts/*.fit` → `.d3f` v1/v4 (8-bit alpha) | `GameOS/gameos/gos_font.cpp:11/234` | atlas ≤4096²; ~256 glyphs | no (rasterized) | `gos_font.cpp` | UI text |
| **Movies** | `data/movies/*.BIK` (185, 100 MB); `mc2-movies.zip` | `code/controlgui.cpp:152 playMovie`; ffmpeg headers | ~640×480/800×600 Bink | no | `MC2MoviePtr` player | mission scripting |
| **Audio** | `data/sound/` (314, 200 MB) WAV + `betty.pak` voice | `GameOS/gameos/gameos_sound.cpp` (SDL2_mixer); `code/gamesound.h` | PCM WAV; SFX IDs `sounds.h` | n/a | `gameos_sound.cpp` (SDL2; no GPU) | `GameSoundSystem` |
| **Mission/game data** | `mission.fst` (19 MB) `.fit`; `*.pak` terrain; `mc2-gamedata.zip` | `code/mission.cpp:1739 init`; `ObjectManager::loadTerrainObjects` | text INI / binary pak | n/a | — | `code/mission.cpp`, `logistics.cpp` |
| **Save data** | `data/missions/save.fit` (text FIT) | `code/logistics.cpp:208/216` | INI | n/a | — | `LogisticsData` |

**Non-mech GLB probe (bdactor/genactor/gvactor):** as of merge `fdb7c470`, `bdactor.cpp`, `genactor.cpp`, and `gvactor.cpp` all support `[Import] Source=<stem>` in the actor `.ini` to override LOD0 with a GLB loaded via Assimp. Only LOD0 is replaced; LOD1/LOD2 fall back to the legacy `.tgl` path. Kill switch: `MC2_ASSIMP_IMPORT=0` (default on in rc1). HangarGLB is the first production building to use this path; mech Flea uses the equivalent path in `mech3d.cpp`.

**CorrugatedSteel006A PBR material pack:** `gameassets/materials/CorrugatedSteel006A/`. Three BC7 KTX2 maps cooked from CC0 (ambientCG) sources: `albedo_srgb.ktx2`, `normal_linear.ktx2`, `orm_linear.ktx2`. Manifest: `manifest.json`. Not yet wired to engine — pending P1-G (building MaterialGpu SSBO + `building.frag` PBR). Cook pipeline: `tools/mc2texcook/cook_pbr_maps.py`. Hangar material classification: `docs/hangar.material_suggestions.json` (slot 0 → corrugated_steel_painted, needs_review=true).

**Cement-pad albedo issue (terrain overlays):** in **0.4 / 0.4c** the deploy `64/` folder ships `mat6_normal.tga` where the cement-overlay tile (`MAT_LAYER_PAINTED_CONC`, drawn by `terrain_overlay.frag`) reads its albedo, so the pad sampled a *normal map* as color → dark slab. The renderer stopgap (`ead760df`) samples the real cement diffuse atlas in `gos_terrain.frag` + `terrain_lod_chunk.frag`. The **proper fix** — swapping the literal `.tga` in `64/` and color-matching it (asset cook) — is already in **rc1** but NOT 0.4. See `docs/known_issues.md` (Water / terrain rendering).

### 1a. Geometry budgets (vertex/triangle, from `.ase`)

Audited 2026-06-02 via `tools/mc2texcook/ase_geo_audit.py` over `mc2srcdata/tgl/*.ase`
(2,947 meshes, summing `*MESH_NUMVERTEX`/`*MESH_NUMFACES` across each file's GEOMOBJECTs).
**Total: 288,708 verts / 366,484 tris.** MC2-era low-poly throughout.

| Category | files | total verts | total tris | median V/T | max V/T |
|---|---|---|---|---|---|
| building/other | 2399 | 233,235 | 294,059 | 76 / 82 | 979 / 1493 |
| mech | 200 | 31,214 | 44,601 | 186 / 268 | 241 / 332 |
| turret/sensor | 112 | 9,617 | 10,247 | 69 / 73 | 222 / 253 |
| tree/foliage | 122 | 7,644 | 8,326 | 21 / 15 | 479 / 274 |
| vehicle | 114 | 6,998 | 9,251 | 49 / 68 | 392 / 602 |

Notes:
- **`mech` file count = animation frames**, not distinct mechs: each mech ships ~25
  gesture `.ase` of the *same* base mesh. Base mech mesh ≈ **208–215 verts / ~314 tris**
  (Atlas, Urbanmech). So the real mech vertex budget is per-mech ~314 tris, not the 44k sum.
- **`building/other` includes `mcl_mc_*`** — Mechlopedia/mechlab *component* models
  (weapons: gauss/laser/SRM/etc, ~300–626 tris each), miscategorized as buildings by the
  filename heuristic. The single heaviest mesh is `bloodaspsttowk` (1493 tris). Genuine
  building props cluster near the 76v/82t median.
- Counts are source `.ase` (pre-LOD-merge); damage/`l1`/`x` variants are separate files.
- Re-run: `py -3 tools/mc2texcook/ase_geo_audit.py [--dir ...] [--top N]`.

## 2. Archive / packaging formats

- **FST FastFiles** (`mclib/fastfile.cpp`, `ffile.cpp`; ELF-hash directory). Deployed (~1.4 GB): `tgl.fst` (props 33M), `textures.fst` (176M), `art.fst` (24M), `mission.fst` (19M), `effect.fst`, `insignia.fst`, `misc.fst`, `camera.fst`. Built by `data_tools/makefst.cpp`.
- **PAK** (`data_tools/pak.cpp`) — terrain/object binary; per-mission `*.pak`.
- **FIT** (`mclib/inifile.h FitIniFile`) — text INI: missions, UI layouts, saves.

## 3. Ownership split (render vs gamedata)

| Domain | Render-side owner | Gamedata-side owner |
|---|---|---|
| Static props | `GpuStaticPropBatcher` (`gos_static_prop_batcher.cpp`) | `BldgAppearance`/`TreeAppearance` (`bdactor.cpp`) |
| Mechs | `mech3d.cpp` + `gos_mech_batcher` + `MechRenderAdapter` | `code/mech.cpp BattleMech` |
| Vehicles | `GVAppearance` (`gvactor.cpp`) | `code/gvehicl.cpp GroundVehicle` |
| Terrain | `gos_terrain_indirect.cpp` + `gameos_graphics.cpp` | `terrain.cpp`/`terrtxm2.cpp` + mission `.pak` |
| VFX | `gos_particle_bridge` / `particle_billboard` / MLR | `code/weaponbolt.cpp` + `gosfx` specs |
| Textures (all) | `mclib/txmmgr.cpp` + `RenderCore/KtxLoader` | (asset-specific appearance) |
| Text | `gos_font.cpp` | UI code |
| Audio | `gameos_sound.cpp` (SDL2) | `GameSoundSystem` |

## 4. Texture runtime path (disk → GPU)

- `mclib/txmmgr.cpp:3426-3521` — if `MC2_TEXMGR_COMPRESSED_UPLOAD` and a same-stem
  `.ktx2` sidecar exists: `RenderCore::ktxLoadRgba8` → validate BC7 (vkFormat 145/146)
  → `gos_NewCompressedTexture2D` (`GL_COMPRESSED_[SRGB_ALPHA|RGBA]_BPTC_UNORM`, mip 0)
  → else RGBA8 fallback.
- **KTX2 support** (`RenderCore/KtxLoader.cpp`): RGBA8 (37/43) + BC7 (145/146), stored
  (no Basis/supercompression). Constraints: typeSize=1, depth=0, faceCount=1, level≥1.

## 5. Upscale → cook → deploy pipeline

**Upscale** (4×, source TGA → upscaled TGA/PNG), root scripts:
- `upscale_gpu.py` — Real-ESRGAN (realesrgan-ncnn-vulkan), GPU; art/ & tgl/.
- `upscale_pytorch.py` — ESRGAN/SwinIR/HAT (ROCm/CUDA).
- `upscale_stablesr.py`, `upscale_ui_textures.py`, `upscale_textures.py` — variants.
- `deploy_256.py`, `deploy_colormaps.py` — upscaled → TGA mip chain / colormap deploy.

**Asset modernization toolchain** (ASE → GLB roundtrip + validation, added 2026-06-17 merge `fdb7c470`):
- `tools/ase_to_glb.py` — convert `mc2srcdata/tgl/*.ase` to GLB via Assimp; produces `.glb` + `.mcasset.json` sidecar.
- `tools/validate_glb.py` — validate GLB output (vertex count, UV presence, material slots).
- `tools/asset_baseline.py` — snapshot-based asset regression baseline tool.
- `tools/classify_materials.py` — classify material slots from GLB against PBR material library; outputs `docs/<stem>.material_suggestions.json`.

**Cook** (TGA/PNG → KTX2, BC7 via KTX-Software CLI `A:/Games/mc2-tools/ktx/ktx.exe`):
- `tools/mc2texcook/mc2texcook.py` — single-file; presets albedo(sRGB), normal/orm/mask/emissive(UNORM).
- `tools/mc2texcook/batch_cook.py` — batch tree cook (`--src --dst --preset --bc7 [--no-mips]`); cooks at **native** source res (no resize).
- `tools/mc2texcook/cook_pbr_maps.py` — PBR material pack cook (albedo sRGB + normal UNORM + ORM UNORM → BC7 KTX2); target layout `gameassets/materials/<MaterialName>/`. Used for CorrugatedSteel006A.
- `tools/mc2texcook/cook_tgl_tiers.py` — **multi-resolution tier cook** from the release `mc2-tgl.zip` → BC7 KTX2 into `data/tgl/{256,512,1024}`. Caps the longest edge per tier; **never upscales** (a 512 source in the 1024 tier stays 512). Example:
  ```
  py -3 tools/mc2texcook/cook_tgl_tiers.py \
      --zip A:/Games/mc2-opengl-src/release_assets/0.3/mc2-tgl.zip \
      --deploy A:/Games/mc2-opengl/mc2-win64-v0.4/data/tgl --tiers 256,512,1024 [--test]
  ```

**Tier convention:** numeric sibling folders (`64/128/256/512/1024`) under a texture
set. The asset viewer's tier switcher (and any tiered loader) treats these as the
resolution ladder; only tiers present on disk are offered.

## 6. Upscale / cook status (as of last-verified)

| Set | Upscaled sources | Cooked tiers deployed |
|---|---|---|
| Terrain colormaps | ✅ 4× burn-ins (release zips) | ✅ BC7 atlas (108→27 MB) |
| **Static-prop tgl textures** | ✅ 512/1024 in `mc2-tgl.zip` | 128 base; **256/512/1024 cooked 2026-06-02** |
| Mech textures | ✅ 4× archives | partial (bucketed 128–1024) |
| **Flea.glb mech** | AI-upscaled (`fireantrgb.ktx2`) from release_assets zip | ✅ BC7 512×512 deployed rc1 `data/tgl/128/` |
| **HangarGLB.glb building** | Source `hangar.ase` | ✅ `a_hangar.ktx2` BC7 deployed rc1 `data/tgl/128/` |
| **CorrugatedSteel006A PBR** | CC0 ambientCG sources | ✅ albedo/normal/orm BC7 KTX2 in `gameassets/materials/`; NOT yet engine-wired |
| Terrain detail/overlay | ✅ | 64/128/256 |
| **Terrain overlay tiles (roads/runways/crossings/bridges)** | ✅ 256² UltraSharpV2 via `tools/overlay_upscale/overlay_tile_upscale.py` (469 tiles, 17 families; cementpattern/scorch excluded) | ✅ `data/textures/256Overlays/` in 0.5-testing, read by gated loader `MC2_OVERLAY_TILE_HIRES` (OVERLAY-TILE-HIRES-1); legacy `64Overlays` untouched, per-file fallback |
| UI/menu art | ✅ 1024/512 (`mc2-art.zip`) | loose TGA/PNG (no BC7 tier yet) |
| VFX / fonts / heightmap / movies / audio | n/a | n/a |

**Originals are 128².** Deployed `tgl/128` cooks were made from the 128 originals;
the 512/1024 `.tga` in the release zips are prior **upscale results** (packaged into
`mc2-tgl.zip`, not loose in the working tree).

### Decision: in-game static-prop normal mapping SHELVED (2026-06-02)

Engine-session finding: moving prop diffuse per-fragment showed **on ≈ off** (no visible
gain) under MC2's lighting model — so a normal map feeding diffuse would also show ~nothing.
Specular-only normal mapping (Option A) is real but subtle and only on shiny sun-facing
faces — low ROI on these matte stone props. **Conclusion: do NOT invest in in-game
static-prop normal/ORM map cooking or shader work.** Normal/ORM map **authoring + preview
lives in the asset viewer** (Materials mode: normal slot + tangent-space mapping, validated
by `--smoke-tangent`). This is why deployed `tgl/128` is albedo-only — that's correct, not a
gap. Revisit only if the lighting model changes (e.g. stronger speculars / HDR).

## 6a. Mech HUD icon atlases (`mcui_med4` / `mcui_low4` / `mcui_high7`)

In-mission HUD + force-group mech icons. Loader: `code/mechicon.cpp`
(`MechIcon::initTextures` → `File::open("data/art/mcui_med4.tga")`, loose-first).
**Distinct subsystem from the mech-bay roster atlas** `mcui_gn_mechicons.tga`
(`code/logisticsmechicon.cpp`) — do not confuse them.

Resolution → file → cell (`setIconVariables`, screenWidth switch):

| screenWidth | file | unitIconX × unitIconY |
|---|---|---|
| 800 | `mcui_med4.tga` | 32 × 38 |
| 640 | `mcui_low4.tga` | 25 × 30 |
| else | `mcui_high7.tga` | 40 × 48 |

Layout = **N columns × 2 rows**: row 0 = front view, row 1 (`+unitIconY`) = back view.
Blit indexes by **column only**: `srcX = iconPictureIndex × unitIconX`. The atlas MUST be
uniform pitch = `unitIconX`, with the mech's cell at column = its `iconPictureIndex`.

**Stock (FST) atlas only has 33 mechs (cols 0–32).** MC2X/CVE-G mechs use indices >32
(e.g. Turkina = 76) → off the right edge → `AssetScale` clamps to width 0 → blank →
engine default-fallback icon. Fix = deploy a wider atlas with the mod roster.

**Authoritative CVE-G atlases live in `MC2X-CVE-G/art.fst`** (zlib, ver `CADDECAF`),
85 mechs each, correctly uniform-gridded:

| file | dims | cols @ cell |
|---|---|---|
| `mcui_med4.tga` | 2720 × 76 | 85 @ 32px |
| `mcui_low4.tga` | 2125 × 60 | 85 @ 25px |
| `mcui_high7.tga` | 3400 × 96 | 85 @ 40px |

Deploy = extract those three from `MC2X-CVE-G/art.fst` → loose into v0.4 `data/art/`.
**Then set `data/art/asset_sizes.csv` nominal = actual** (`mcui_med4 → 2720,76` etc.) so
`AssetScale::factorFor` returns 1 — these are direct-indexed sprite sheets, NOT upscales;
a non-1 factor multiplies `srcX` and reads OOB (crash in `MechIcon::setDrawBack`).

⚠️ Do **not** use `mc2-win64-0.4c/mods/mc2x-compat/data/art/mcui_med4.tga` (4256×76): it is
non-uniform (32px stock head + 25.6px CVE tail) — unreadable by any single `unitIconX`.

## 7. Keeping this current

Update this document when you:
- add / remove / rename an asset set, or change where it deploys;
- re-cook or upscale a set (record the new tiers in §1 + §6);
- change a loader, archive format, or the cook/upscale scripts (§4/§5);
- change render-side or gamedata-side ownership (§3).

`scripts/check-asset-pipeline-doc.sh` is an advisory pre-commit check: it warns when a
commit touches asset directories, cook/upscale scripts, or known asset loaders without
also updating this file. Wire it into `.git/hooks/pre-commit` (or the project hook) to
enforce. Bump the **Last verified** date in the header when you re-audit.
