# TERRAIN-MATERIAL-TEXTURE-REMAP-1 — RECON (material-lib v2, layer→texture remap)

RECON ONLY. No source changes, no build, no launch. Worktree `A:/Games/mc2-controlmap-sample-1` @ HEAD `7cf11031` (TERRAIN-CONTROLMAP-ALBEDO-1).

North star: UE-style splat where each terrain material layer (rock/grass/dirt/concrete/snow, cliff mat5) samples a REAL 2048² PBR texture set (albedo+normal+roughness+height) instead of the current colormap-tint-only albedo. Reference: alpine grass/rock/snow.

---

## 1. Terrain_004 pack inventory — VERDICT: WRONG PACK TYPE

`C:/Users/Joe/Downloads/GameAsset/Terrain/Terrain_004/`:

| File | Size | What it is |
|---|---|---|
| `png_contents/Terrain004_2K_Color.png` | 2048², **16-bit RGB** (colortype 2) | Macro albedo for ONE baked terrain |
| `png_contents/Terrain004_2K_Height.png`→ actually `Details/Flow/Protrusion/Soil.png` | 2048², **16-bit grayscale** (colortype 0) | Gaea data channels, NOT tangent-space maps |
| `Terrain004_2K.exr` | 16 MB | HDR macro data (height/composite) |
| `Terrain004_2K.obj` | **519 MB** | Displaced terrain MESH |
| `Terrain004_2K_Color.r32` / `_Height.r32` | 50/16 MB | Raw float32 macro color + heightfield |
| license/readme | **NONE present** | ⚠ no license file — provenance unknown (looks QuadSpinner Gaea / "Terrain_004" numbered pack) |

**This is a QuadSpinner-Gaea-style MACRO TERRAIN heightfield pack, not a tileable per-material PBR set.** The maps (`Details`, `Flow`, `Protrusion`, `Soil`) are Gaea texture-export channels for a *single* sculpted terrain; they do NOT tile and are NOT albedo/normal/roughness for rock-vs-grass-vs-dirt. It is the wrong input for a per-layer splat remap.

**The RIGHT inputs are the SIBLING packs already downloaded** in `…/GameAsset/Terrain/` — these ARE AmbientCG/PolyHaven-format tileable per-layer PBR (Color/NormalGL/Roughness/AO/Displacement), one per material:
- Grass → `Grass001_2K-PNG.zip`, `aerial_grass_rock_2k.blend.zip`, `forest_ground_04_4k.blend.zip`
- Rock/cliff → `rocks_ground_02/03/05/06`, `dry_riverbed_rock`, `marble_cliff_01_2k.gltf.zip` (mat5 cliff already targets marble)
- Dirt → `dirt_aerial_03`, `rocky_trail`, `ganges_river_pebbles`
- Snow → `snow_field_aerial_2k.blend.zip`
- Concrete/road → `Road004`, `gravel_road`, `clean_asphalt`, `cracked_concrete`, `painted_concrete_02` (asphalt/gravel already wired — §5)

RULING NEEDED: retask this slice to consume the sibling AmbientCG packs (real per-layer PBR); keep Terrain_004 only as a possible macro-albedo/height source for one showcase mission (gaea_peaks), not as the material library. `cook_pbr_maps.py` already ingests exactly the AmbientCG `_Color/_NormalGL/_Roughness/_AO` layout (§3) — the tooling is pre-built for the siblings, not for Terrain_004.

---

## 2. Current texture plumbing (cited)

### 2a. Albedo — colormap-only, NO per-layer albedo array
- Per-layer albedo does **not exist**. Base albedo = the per-mission burn-in colormap `u_colormap` (`shaders/terrain_lod_chunk.frag:446-456`, 9-tap disc blur), then repainted per-layer only by **tint scalars** (`matTint*` uniforms) weighted by HSV classification. `mclib/terrtxm2.cpp` colormap path (`~:1211` accepts width 1024/2048; BC7 KTX2 sidecar via `MC2_COLORMAP_KTX2`, `terrtxm2.cpp:199-218`).
- TERRAIN-CONTROLMAP-ALBEDO-1 (`terrain_material_lib.cpp:119-128`, gate `MC2_TERRAIN_CONTROLMAP_ALBEDO`) lets authored control-map weights pull the tint harder (`controlAlbedoStrength`, default 0.7) — still tint, not a texture.
- **This is the gap v2 fills: introduce a per-layer albedo array so layers show real material color, not tinted colormap.**

### 2b. Normal + displacement — 9-layer sampler2DArray (EXISTS)
- Loader: `mclib/terrtxm2.cpp:2456-2571`. Names `mat<N>_normal.tga` / `mat<N>_displacement.tga`, N=0..8. **mat0-4 required, mat5-8 optional** (missing 5+ = skipped slot, `:2476-2477`). `.tga`, `UNC_TRUE`, all layers must share one `arrayWidth` (`:2491-2492` — square, equal for all).
- Layer semantics (shader `MAT_LAYER_*`): 0=ROCK 1=GRASS 2=DIRT 3=CONCRETE 4=SNOW; 5=cliff/marble (triplanar, `cliffTriplanarStrength`), 6-8 spare.
- GL array build: `gameos_graphics.cpp:2108-2175` — `GL_TEXTURE_2D_ARRAY`, `GL_RGBA8`, 9 layers, `glTexImage3D` + explicit mip chain (`GL_LINEAR_MIPMAP_LINEAR`, `GL_REPEAT`). Individual-texture fallback via `gos_CreateTerrainNormalTexture` (`:9657`, `GL_RGBA8`, **no mips**, `GL_LINEAR`).
- Shader sampling: `matNormalArray` (`terrain_lod_chunk.frag:155`), `.rgb`=tangent normal, **`.a`=displacement/height** (used for POM/elevation `:274-276`; `matNormal2` alpha retained on CPU `terrtxm2.cpp:2566-2571`). Per-layer `matTiling`/`matNormalBoost`/`matRoughness`/`matAO` uniforms (`:163-201`).
- Detail normal + detail displacement: separate `detail_normal.tga` / `detail_displacement.tga` (`terrtxm2.cpp:2367`, `~:2411`), global tiling.
- Texunit map (`gameos_graphics.cpp:140`): normal array → unit 5 (`kTerrainTexUnitNormalArray`); colormap, cementAtlas, controlMap, transitionMaskArray, overlaySidecar each own units.

### 2c. Material-lib JSON today (scalars only)
`data/terrain_materials.json` (46 keys): per-layer `*_tiling`, `*_normalBoost`, `*_tint_{r,g,b}`, `*_roughness`, `*_ao`, `detail_*`, `tintStrengthScale`, `snow_brightnessDampen`, `controlAlbedoStrength`, `classify_*`. Reader `terrain_material_lib.cpp:130-245` (TinyJson, flat float obj; **arrays/objects skipped** `:82-95` — a `"layers":[…]` block is silently ignored today, so v2 must extend the reader). Gate `MC2_TERRAIN_MATERIAL_LIB` default OFF; missing file / omitted key = byte-identical shipped defaults.

---

## 3. v2 JSON schema + loader design

### 3a. v2 schema (additive; scalars stay flat, textures nested under `layers`)
```jsonc
{
  // ...all existing flat scalar keys unchanged (byte-identity floor)...
  "textureRoot": "data/terrain/materials/",     // relative to CWD
  "layers": {
    "rock":     { "albedo": "rocks_ground_02_albedo.ktx2", "normal": "rocks_ground_02_normal.ktx2",
                  "orm": "rocks_ground_02_orm.ktx2", "height": "rocks_ground_02_height.ktx2" },
    "grass":    { "albedo": "grass001_albedo.ktx2", "normal": "grass001_normal.ktx2", "orm": "…", "height": "…" },
    "dirt":     { … },
    "concrete": { … },
    "snow":     { "albedo": "snow_field_albedo.ktx2", … },
    "cliff":    { … }   // mat5 marble/triplanar
  }
}
```
- Each layer: optional `albedo`/`normal`/`orm`(AO+Rough+Metal packed, matches cook_pbr_maps)/`height`. Any absent field → fall back to stock (colormap-tint for albedo; existing `mat<N>_normal.tga` for normal). Reuse the KTX2 `.a`-in-normal displacement OR a dedicated height layer.
- Precedence: env `MC2_TERRAIN_MATERIAL_TEXTURES` gates the whole texture path; per-field absence falls to stock; scalar keys keep existing env>JSON>default precedence.

### 3b. Loader design (two new arrays, mirror existing normal-array build)
1. Extend TinyJson to descend into `layers` (add an object-walk; current `skipVal` at `terrain_material_lib.cpp:90` throws it away). Emit a `TerrainLayerTextures[6]` struct list (paths per layer).
2. New albedo array: build a 6-layer `GL_TEXTURE_2D_ARRAY` **sRGB** (`GL_SRGB8_ALPHA8`) mirroring the normal-array build at `gameos_graphics.cpp:2108-2175` (same `glTexImage3D`+mip+REPEAT). New texunit `kTerrainTexUnitAlbedoArray` + `matAlbedoArray` sampler.
3. Reuse the **existing** 9-layer normal array for `normal` (already there) — the remap only *replaces the source file* per layer (swap `mat<N>_normal.tga` → the pack's KTX2). Add an ORM array (linear) if roughness/AO textures wanted (else keep scalar roughness/AO).
4. Shader: in `terrain_lod_chunk.frag`, replace the `base = texture(u_colormap…)` tint composition (`:446-476`) with `sum_layer( weight * texture(matAlbedoArray, layer) )` **behind the gate** (branch on a `u_useMatAlbedo` int uniform; 0 → identical old path → byte-identity). Weights already computed by `chunkWeights()`/control-map.
5. Loader lives beside `terrtxm2.cpp` material-array load (`:2456`), applied at mission load like `terrainMaterials_apply`.

Do NOT rebuild the loader from scratch — clone the normal-array path; it already handles square-equal-width, mips, optional slots, format checks.

---

## 4. Cook pipeline — REUSE cook_pbr_maps.py

`tools/mc2texcook/cook_pbr_maps.py` already cooks AmbientCG-layout packs to game-ready BC7 KTX2:
- `{slug}_albedo.ktx2` (BC7 **sRGB** from `_Color.png`), `{slug}_normal.ktx2` (BC7 linear from `_NormalGL.png`), `{slug}_orm.ktx2` (BC7 linear, R=AO G=Rough B=Metal). Two-step `ktx create --encode uastc` → `ktx transcode --target bc7`, full mip chain. Ext tool `A:\Games\mc2-tools\ktx\ktx.exe`.
- The sibling packs (Grass001, rocks_ground_*, snow_field, Road004…) unzip to exactly `_Color/_NormalGL/_Roughness/_AO/_Displacement` PNGs → feed straight into cook_pbr_maps.py, one invocation per layer. **No new cook script needed** for the correct inputs.
- Terrain_004 does NOT fit cook_pbr_maps (macro heightfield, wrong channels) — another reason to retask to siblings.
- Direct stb load (TGA/PNG) is possible for a quick spike but ships uncompressed VRAM (§5) — cook to BC7 KTX2 for release. KTX2 loader precedent already exists (colormap `.burnin.ktx2` sidecar, `terrtxm2.cpp:1668-1690`; `cook_ktx2_textures.py`).

---

## 5. VRAM / perf budget

Per-layer 2048² costs (with full mip chain ≈ ×1.333):
- **Uncompressed RGBA8**: 2048²×4 = 16 MiB base → ~21.3 MiB w/ mips.
- **BC7** (¼ of RGBA8): 4 MiB base → ~5.3 MiB w/ mips.

For 6 layers × 3 maps (albedo+normal+orm):
- Uncompressed: 6×3×21.3 ≈ **384 MiB** (unacceptable).
- BC7: 6×3×5.3 ≈ **96 MiB** (fine on the 7900 XTX / 24 GB; ~acceptable on 8 GB targets alongside colormap+shadows).
- Minimal (albedo+normal only, drop ORM → scalar rough/AO): 6×2×5.3 ≈ **64 MiB**.

Mandate BC7 KTX2 for release; array texture (not 18 individual) keeps binding/texunit pressure flat (adds only the 1 new albedo-array unit + optional ORM unit). Existing normal array already budgeted. Cliff mat5 triplanar samples 3× per fragment — keep it on the existing path, don't add a triplanar albedo unless perf-checked.

---

## 6. Channel mapping, interactions, gate & acceptance

### Control-map → layer mapping (existing convention)
Control map RGBA weights: **R=rock G=grass B=dirt A=concrete**, snow derived from colormap HSV (`terrain_lod_chunk.frag:304-309`), cliff=mat5 slope-triplanar. Terrain_004 has no per-material split, so mapping siblings:
- R rock ← rocks_ground_02/05 · G grass ← Grass001/aerial_grass_rock · B dirt ← dirt_aerial_03/rocky_trail · A concrete ← cracked/painted_concrete · **snow** = keep HSV-derived 5th layer (mat4), source snow_field_aerial · **cliff mat5** = keep marble_cliff triplanar (already targeted; don't replace).
- Snow ruling: keep HSV-detected snow as its own array layer (mat4) — do NOT collapse into a channel; the classifier already gates it and users expect snow-on-height.

### POM synergy
POM recon in flight wants per-layer height. The pack layout carries height (KTX2 normal `.a` or dedicated `_height`/`_Displacement`). v2's `height` field feeds POM directly — coordinate so both read the same array/`.a` convention (`.a`=displacement already the norm, `terrain_lod_chunk.frag:274`).

### Gate + byte-identity + acceptance
- Gate `MC2_TERRAIN_MATERIAL_TEXTURES` **default OFF**. OFF → loader early-returns, no array built, shader `u_useMatAlbedo=0` → old colormap-tint path verbatim → **byte-identical to stock** (mirror `terrain_material_lib.cpp:120,136` no-op convention; verify no member writes when OFF).
- Independent of `MC2_TERRAIN_MATERIAL_LIB` (textures vs scalars are orthogonal gates, per existing controlAlbedo precedent).
- Acceptance: static-cam **gaea_peaks** with full stack (control-map albedo + material-lib scalars + texture remap + POM) — visual check alpine grass/rock/snow reads as real material, not tinted colormap; smoke tier1 unchanged with gate OFF (byte-identity); VRAM within BC7 budget (§5).

---

## Landmines
1. **Terrain_004 is the wrong pack** — macro Gaea heightfield, not tileable per-layer PBR. Real inputs = sibling AmbientCG packs already in `…/GameAsset/Terrain/`. Building the loader against Terrain_004 wastes the slice.
2. **No albedo array exists today** — albedo is colormap+tint only. v2 must *add* a new sRGB array + shader branch, not "swap files" (unlike the normal path which already exists).
3. **sRGB vs linear**: albedo array MUST be `GL_SRGB8_ALPHA8`; normal/ORM/height MUST be linear `GL_RGBA8`. Wrong colorspace = washed/dark terrain.
4. **Byte-identity**: shader must branch on `u_useMatAlbedo`; the old `base=texture(u_colormap…)` disc-blur path (`:446`) must be reachable unchanged when gate OFF. Any member write in the OFF path breaks stock parity.
5. **Snow (mat4) + cliff (mat5)** are special (HSV-derived / slope-triplanar) — don't force them into RGBA control channels.
6. **Array width equality** — all layers one square `arrayWidth` (`terrtxm2.cpp:2491`); mixing 2K grass with 4K rock will fail the load. Cook all to 2048².
7. **No license file** in Terrain_004 — provenance unverified; confirm redistribution rights before shipping any of these packs in the deploy payload.
8. **TinyJson skips nested objects** (`terrain_material_lib.cpp:90`) — a `layers` block is silently dropped today; v2 reader extension is mandatory or the schema is a no-op.

## Open rulings
- **R1 (blocking):** Retask slice to sibling AmbientCG packs (real per-layer PBR, cook_pbr_maps-ready) instead of Terrain_004? Recommend YES; keep Terrain_004 as optional macro-albedo/height for one gaea_peaks showcase only.
- **R2:** ORM textures (per-layer roughness/AO array) or keep scalar roughness/AO (saves ~32 MiB VRAM)? Recommend scalar for v2, ORM as v2.1.
- **R3:** Reuse existing normal array (swap source files) vs new normal array? Recommend reuse — remap the `mat<N>_normal` source to the pack's cooked KTX2.
- **R4:** Snow as HSV-derived layer4 (keep) vs authored 5th control channel? Recommend keep HSV.
- **R5:** License/provenance sign-off on the sibling packs before deploy-payload inclusion.
