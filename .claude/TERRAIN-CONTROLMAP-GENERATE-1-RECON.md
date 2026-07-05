# TERRAIN-CONTROLMAP-GENERATE-1 (+SAMPLE-1) — Recon

**Branch:** claude/nifty-mendeleev  **HEAD:** 85182be1  **Worktree:** `.claude/worktrees/nifty-mendeleev`
**Scope:** RECON ONLY. Proposes an RGBA control/splat map path for LIVE chunk terrain, gated `MC2_TERRAIN_CONTROLMAP` default OFF.

---

## Executive summary

The premise "per-vertex TerrainType enum + slope heuristic drives material selection" is **only half true on the live chunk path** — and the wrong half is the load-bearing one:

- **Material layer weights (rock/grass/dirt/concrete/snow) are derived from the COLORMAP COLOUR in the frag**, not from `terrainType` and not from slope. See `shaders/terrain_lod_chunk.frag:167 chunkColorWeights(vec3 color)` and `:256 chunkWeights` — weights come from `color.g-color.r`, `color.r-color.g`, HSV-snow, water test. The colormap atlas (BC7 KTX2) is the de-facto material control map today.
- **`v_terrainType` (SSBO binding 24) drives ONLY concrete/cement** (`frag:614 pureConcrete = smoothstep(2.0,3.0,v_terrainType)`), plus a debug viz (bit 128). It is a `float` material index 0..3 computed on the CPU (`mclib/terrain.cpp:802-814 terrainTypeToMaterial`), NOT a raw enum, NOT slope.
- **The only slope-based material logic is `useRockSlopeBias` (`frag:599`) and `useTriplanarCliff` (`frag:712`) — both gated OFF by default** (byte-identical when off). The cliff *colour* darken (`frag:698-705`) always runs but is a post-tint recolour, not layer selection.

**Consequence for this slice:** a "control map" that replaces material selection must replace the **colormap-colour → weight classifier** (`chunkColorWeights`), optionally subsume the concrete bit currently in `v_terrainType`. A v1 parity control map is trivially generated at load by running the *same* `chunkColorWeights` math (or its CPU twin) over the colormap, OR by sampling the colormap unchanged. The genuinely safe v1 is: **runtime-generate an RGBA control raster at mission load from `blocks[]` + colormap, upload as a new texture, and have the frag branch (`u_useControlMap`) read weights from it instead of `chunkColorWeights(base)`** — with the branch defaulting to the existing color-classifier so gate-OFF is provably identical.

**Recommended design (3 bullets):**
1. **v1 = runtime-generated at load, no asset dependency.** New free texture unit **12** (RGBA8, sampler `u_controlMap`), generated CPU-side in `mclib/terrain.cpp` next to the existing `ttype`/`elev` upload (`:790-817`) by writing R=rock, G=grass, B=dirt, A=concrete weights per vertex (parity = run the CPU twin of `chunkColorWeights`, or seed from colormap). Frag: `if (u_useControlMap != 0) matWeights = <sampled>; else matWeights = chunkColorWeights(base);` — one uniform branch, default 0.
2. **Gate `MC2_TERRAIN_CONTROLMAP` default OFF**, read in `mclib/terrain.cpp` (same site as `MC2_TERRAIN_LOD_CHUNK`, `terrain.cpp:142`). When off: no texture created, `u_useControlMap` uploads 0, sampler bound to a 1x1 dummy or left unbound-but-never-sampled. **Byte-identical guaranteed by the uniform branch**, not by relying on "bound-but-unsampled".
3. **v2 = authored sidecar** (`data/missions/<stem>.beauty/control_map.png`, RGBA8, resolution = `realVerticesMapSide` texels, GL_LINEAR), loaded only when present (precedent: `.beauty/visual_height_4x.r32`, `terrain.cpp:819-824`). Same sampler/branch; v1 and v2 differ only in data source.

---

## Data flow today (live chunk path)

```
MISSION LOAD (mclib/terrain.cpp:790-817)
  mapData->getBlocks() : PostcompVertex[]  (elevation, terrainType enum 0..20, textureData)
      |
      |-- elev[i]  = blks[i].elevation           --> gos_TerrainLodChunk_UploadHeightFull  --> SSBO binding 23
      |-- ttype[i] = terrainTypeToMaterial(...)  --> gos_TerrainLodChunk_UploadTerrainTypeFull --> SSBO binding 24
      |     (enum->float material 0=Rock 1=Grass 2=Dirt 3=Concrete; terrain.cpp:802)
      \-- (cement words, built later by gos_terrain_indirect PopulateRecipeCementWords)
            --> gos_TerrainLodChunk_UploadCementWordsFull --> SSBO binding 25

DRAW (GameOS/gameos/gos_terrain_lod_chunk.cpp, per block)
  VAO: localOffset ivec2 (loc0) + isSkirtFlag int (loc1); regular CPU grid, NOT tessellated
  VERT (terrain_lod_chunk.vert): Z from heights[] (binding 23); v_terrainType = terrainTypes[] (binding 24)
  FRAG (terrain_lod_chunk.frag):
     base = 9-tap blur of u_colormap (unit 0)                    [:398-407]
     cement override from cementWordsF[] (binding 25) + u_cementAtlas (unit 3)  [:409-549]
     >>> matWeights = chunkColorWeights(base)  <<<  MATERIAL SELECTION = COLORMAP COLOUR  [:590]
     useRockSlopeBias (OFF) : slope->rock bias                   [:599]
     pureConcrete = smoothstep(2,3, v_terrainType)               [:614]  (only terrainType use)
     detail normals from matNormalArray (unit 5) weighted by matWeights  [:640]
     tint / cliff colour / triplanar(OFF) / breakup / lighting / shadow
```

**Texture units actually bound by the chunk draw** (`gos_terrain_lod_chunk.cpp:138-200`):
`0` colormap · `3` cement atlas (`kChunkTexUnitCement`) · `5` matNormalArray · `9` static shadow · `10` dynamic shadow · `11` transition mask array · `13` dyn full map. **Free: 1, 2, 4, 6, 7, 8, 12.**

---

## Proposed design

**Gate:** `MC2_TERRAIN_CONTROLMAP` (default OFF), read in `mclib/terrain.cpp` at the loader (mirror `MC2_TERRAIN_LOD_CHUNK` read pattern). Optional companion `MC2_TERRAIN_CONTROLMAP_FILE` override (mirror `MC2_TERRAIN_VISUAL_HEIGHT_FILE`).

**Files to touch (fix slice, later — NOT now):**
- `mclib/terrain.cpp` (~:790-817 and the 2nd site ~:4199) — build + upload the control raster; gate read.
- `GameOS/gameos/gos_terrain_lod_chunk.cpp` — new `gos_TerrainLodChunk_UploadControlMap(const uint8_t* rgba, int side)`, create GL texture, bind at draw on unit 12, upload `u_useControlMap`, `u_controlMap` sampler loc (mirror `s_locColormap` handling at `:525/:880`).
- `GameOS/gameos/gos_terrain_lod_chunk.h` — new `TERRAIN_CONTROLMAP_*` constant if needed (texture, not SSBO).
- `shaders/terrain_lod_chunk.frag` — add `uniform sampler2D u_controlMap; uniform int u_useControlMap;` and the `if (u_useControlMap != 0) matWeights = sampleControl(); else matWeights = chunkColorWeights(base);` branch at `:590`; add a debug viz mode.
- `docs/render-binding-registry.md` — add texture-unit 12 row.
- Optional generation tool: extend `tools/terrain_beautify/mission_terrain_analyzer.py` (already reads `PostcompVertex` + colormap-equivalent masks) to emit `control_map.png` for v2 authoring/verification.

**Sampling site:** replace/guard the single call `chunkWeights(base, matWeights, snowWeight)` at `frag:590`. Control map supplies rock/grass/dirt/concrete in RGBA; snow can stay derived from `base` HSV (or ride a 5th channel later). Keep the post-weight pipeline (detail normals, tint, cliff) untouched so only *selection* changes.

**Why the uniform branch, not a permutation:** `#ifdef` in GLSL does NOT inherit C++ flags (CLAUDE.md rule) and would need a `makeProgram` prefix change → two program variants → hot-reload/perm risk. A `uniform int u_useControlMap` branch keeps ONE program; gate-OFF uploads 0 and the compiler-invariant `else` path is the current code verbatim → provable byte-identity.

---

## Binding table

### Texture units (chunk draw)
| Unit | Current use | Constant / site |
|---|---|---|
| 0 | colormap atlas | `:880 glUniform1i(s_locColormap,0)` |
| 3 | cement atlas | `kChunkTexUnitCement` `:194` |
| 5 | matNormalArray | `kChunkTexUnitMatNormalArray` `:144` |
| 9 | static shadow | `kChunkTexUnitStaticShadow` `:138` |
| 10 | dynamic shadow | `kChunkTexUnitDynamicShadow` `:139` |
| 11 | transition mask array | `kChunkTexUnitTransitionMask` `:200` |
| 13 | dyn full map | `kChunkTexUnitDynFullMap` `:140` |
| **12** | **PROPOSED `u_controlMap`** | free — recommend `kChunkTexUnitControlMap = 12` |
| free | 1, 2, 4, 6, 7, 8 | (also available) |

### SSBO bindings (terrain-relevant)
| Slot | Buffer | Path |
|---|---|---|
| 23 | TerrainHeightBuf | chunk (vert Z + frag normal) |
| 24 | TerrainTypeBuf (`v_terrainType`) | chunk (concrete only) |
| 25 | TerrainCementBufFrag | chunk cement |
| 26 | TerrainVisualHeightSsbo | 4x visual displace (gated) |

**Control map recommended as a TEXTURE (unit 12), not an SSBO** — it is a 2D raster sampled with GL_LINEAR (natural bilinear weight interpolation across cells; matches how colormap is sampled). An SSBO would force manual bilinear in-frag. Note MC2_TERRAIN_NORMALS_FROM_HEIGHT's R32F-on-unit-11 precedent is an SSBO-free raster bind — same pattern, but unit 11 is taken here, hence 12.

---

## Generation strategy

**v1 (parity, runtime-generated at load) — RECOMMENDED for first ship.**
- Location: `mclib/terrain.cpp` `:790-817`, alongside `elev`/`ttype`.
- Build `std::vector<uint8_t> rgba(side*side*4)` per vertex. Two parity options:
  - **(a) colour-classifier twin** — port `chunkColorWeights` to C++, run it on the per-vertex colormap colour, write the resulting rock/grass/dirt/concrete into RGBA. Frag sampling then reproduces today's weights (within LINEAR-vs-blur error — see risks).
  - **(b) seed-from-colormap-direct** — skip classification; the frag keeps classifying, control map only *overrides* where authored. (b) is the true byte-identical v1 (control map unused when gate off; when on with no override data, identical). Prefer (b) as the literal parity baseline; (a) is the "prove the classifier ports" step.
- No asset dependency, no format change, works on every stock mission immediately.

**v2 (authored) — follow-on.**
- Sidecar `data/missions/<stem>.beauty/control_map.png` (or `.controlmap`), loaded only if present (precedent `terrain.cpp:819` visual-height). Absent → v1 path. `MC2_TERRAIN_CONTROLMAP_FILE` override for iteration.
- Generation/verification tool: extend `mission_terrain_analyzer.py` (already parses `PostcompVertex`, overlay masks, world→grid). Emit the same RGBA the loader would build so the tool round-trips authoring against engine parity; `terrain_workbench.py` contact sheets verify.

**Verdict:** ship v1(b) first (literal parity, override-only), then v1(a) (classifier-in-control-map), then v2 authoring. Offline-only is wrong for v1 because it adds an asset dependency the parity gate does not need.

---

## Debug view plan

Existing chunk debug is a `u_diag` **bitmask** (`frag:46-54`) plus exact-value escapes (30/31/40) and a separate `u_lightingDebugView` enum (`frag:56-61`, values 40-46). `u_diag` bit 128 already visualizes `v_terrainType`; bit 64 the rock normal sample.

**Propose:** `u_diag` bit **1024** = control-weight viz — output `vec4(matWeights.rgb,1)` (rock=R, grass=G, dirt=B) after the selection branch, so authored vs classified weights are directly comparable (flip the gate, compare screenshots). Wire the C++ side through the existing `MC2_TERRAIN_LOD_CHUNK_DIAG` env→`u_diag` path (`s_locDiag`). Keep it bitmask-additive (don't collide with 30/31/40 exact escapes). Concrete already visible via bit 128.

---

## Asset / sidecar format (v2)

- **Container:** PNG RGBA8 (tooling-friendly, lossless, authorable in any editor). Alt `.controlmap` raw R8G8B8A8 for zero-decode, but PNG preferred for hand-authoring.
- **Location:** `data/missions/<stem>.beauty/control_map.png` (co-located with existing `.beauty` sidecars — `visual_height_4x.r32` precedent).
- **Resolution:** `realVerticesMapSide × realVerticesMapSide` (one texel per terrain VERTEX, side ∈ {60,80,100,120}). Matches SSBO 23/24 layout and lets the loader index `blks[i]` 1:1. GL_LINEAR gives cross-cell blend equivalent to today's smoothstep transitions.
- **Channels:** R=rock, G=grass, B=dirt, A=concrete weight (0-255). Snow stays HSV-derived from colormap in v1/v2 (5th channel deferred — RGBA is full).
- **Seams at chunk borders:** control map is a single map-wide texture (like colormap/height), NOT per-chunk — so there are **no chunk-border seams** (chunk borders are a geometry/LOD concern only; the raster is continuous and sampled by world UV exactly like the colormap at `frag:392-393`). This is a key safety property: reuse the colormap's atlas-UV reconstruction.

---

## Landmines / do-not-touch

1. **Material selection is colormap-colour, NOT terrainType/slope.** Design against `chunkColorWeights` (`frag:167`), not against `v_terrainType`. Getting this wrong = re-implementing concrete and missing the actual classifier.
2. **`v_terrainType` is concrete-only + already a material index (0..3), not the raw enum.** `terrainTypeToMaterial` (`terrain.cpp:802`) collapses 21 enum values to 4. Don't feed raw enum.
3. **Cement/roads/runways are a SEPARATE bake** (SSBO 25 + cement atlas unit 3, `frag:409-549`) and OUT OF SCOPE. Do not route roads through the control map. Interaction risk: the control map's concrete channel and the cement override both write "concrete" — order matters. Keep cement override AFTER control-map selection (as `pureConcrete`/`cementHit` already run after `:590`). `gos_terrain.frag:449 transition NO-OP` is a landmine on the OTHER (indirect/legacy) frag — irrelevant here but don't confuse frags.
4. **Wrong-TU hazard (documented past waste):** `gos_terrain_indirect.cpp`, `gos_terrain_water_stream.cpp`, `mclib/quad.cpp` are DEAD in default config (MC2_TERRAIN_LOD_CHUNK ON). They also read `terrainType` (`indirect:707`, `water_stream:365`) — do NOT edit them for this slice. `gos_terrain.frag`/`.tese` = the indirect/legacy path, NOT the live chunk frag.
5. **BC7 colormap atlas interaction:** the colormap (unit 0) is the current classifier input; v1(a) must classify the SAME post-blur `base` (9-tap, `frag:394-407`) the frag uses, else weights drift. v1(b) sidesteps this by not classifying.
6. **`#version`/macro rule:** NO `#version` in `.frag`; passed via `makeProgram` prefix. GLSL `#ifdef` does NOT inherit C++ flags. → use `uniform int u_useControlMap` branch, NOT a preprocessor permutation.
7. **Hot-reload fails silently** — check console after shader edits (CLAUDE.md).
8. **Editor path:** `EditRel` renders terrain via the same chunk draw (`EditorCamera::render` → `flushDrawCommands`). New sampler/uniform must be bound in the editor draw too or the editor's program gets an unbound sampler. Safest: the uniform branch defaults OFF → editor identical unless gate set. Do NOT add CPU fallbacks to editor TUs (RenderWorld rule).
9. **Gate-OFF byte-identity:** do NOT rely on "a bound-but-never-sampled texture is free." Guarantee identity via the `u_useControlMap==0 → chunkColorWeights(base)` else-branch = current code verbatim. When off, ideally don't even create/bind the texture.
10. **Two upload sites:** `terrain.cpp:817` AND `:4199` both call the type upload — a control-map upload must be added at BOTH or one path silently lacks it.
11. **TerrainRuntime sidecar** (`mclib/terrain_runtime.{h,cpp}`) exists — control-map sampling belongs in the FRAG (a raster), not behind TerrainRuntime (a CPU provider). Don't rebuild TerrainRuntime; if v2 file loading wants a provider seam, that's a later refactor, not v1.

---

## Acceptance tests

### Gate-OFF (must be byte/behaviour identical)
- Build RelWithDebInfo, deploy in lockstep (exe + shaders).
- Run canonical tier1 smoke (verbatim from CLAUDE.md), `MC2_TERRAIN_CONTROLMAP` UNSET:
  `$env:MC2_DEBUG_STATE_DUMP="1"; $env:MC2_DIAGNOSTIC_TRACE_FILE="debug_state/diagnostic_trace.jsonl"; $env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE"; py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs`
- Expectation: exit 0, no new `crash_*`, all 5 missions (mc2_01/03/10/17/24) pass. No new GL errors in console. Shader compiles (check console — hot-reload silent-fail rule).
- Byte-identity claim rests on the `u_useControlMap==0` else-branch = verbatim current code + no texture created when gate off.

### Gate-ON (validation, no live pixel-golden required per user ruling)
- Mission: **mc2_24** (sandy/dirt profile, exercises the classifier widening `g_terrainMaterialProfile==1`) + one green map (mc2_01).
- Set `MC2_TERRAIN_CONTROLMAP=1`. v1(b): screen must be visually identical to gate-OFF (override-only, no data → passthrough). v1(a): weights should match the color classifier within LINEAR-vs-blur tolerance.
- Debug: `MC2_TERRAIN_LOD_CHUNK_DIAG=1024` (proposed) → control-weight viz; compare gate-ON vs gate-OFF weight maps side by side.
- Workbench: `tools/terrain_beautify/terrain_workbench.py` contact sheet for the target mission; compare generated `control_map.png` (from `mission_terrain_analyzer.py`) against the in-engine debug-viz screenshot.
- Static-camera screenshots at a fixed mission position, gate ON vs OFF, for the two missions.
- One tier1-mission smoke with gate ON (`--mission mc2_24`) → exit 0.

---

## Open questions (need user ruling)

1. **v1 parity flavour:** ship v1(b) override-only-passthrough (literal byte-identity, control map does nothing without authored override) as the FIRST milestone, or go straight to v1(a) classifier-in-control-map (proves the port but introduces LINEAR-vs-9tap-blur drift)? Recommend (b) first.
2. **Snow channel:** keep snow HSV-derived from colormap (RGBA = rock/grass/dirt/concrete), or reserve a channel now and drop concrete into `v_terrainType` (freeing A for snow)? Affects format finality.
3. **Concrete ownership:** should the control map's A(concrete) SUPERSEDE `v_terrainType`/cement selection, or stay purely additive with cement override winning? Recommend cement/`v_terrainType` remains authoritative for runways (it drives the atlas), control map only affects the non-cement material blend.
4. **Resolution:** vertex-resolution (side×side) is the natural parity choice; do we want to allow HIGHER-res authored control maps (e.g. 2x) for v2, which would break the 1:1 `blks[i]` index and require UV-space authoring? Recommend vertex-res for v1, revisit for v2.
5. **Texture unit 12 vs an SSBO:** confirm texture (bilinear, colormap-like) over SSBO (manual filtering). Recommend texture.
