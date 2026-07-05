# TERRAIN-MATERIAL-LIB-1 — Recon

**Branch:** claude/controlmap-sample-1  **Base:** 85182be1  **Worktree:** `A:/Games/mc2-controlmap-sample-1`
**Scope:** RECON ONLY. No source changes, no build, no launch. Design against BASE + the assumption that TERRAIN-CONTROLMAP-SAMPLE-1 lands (control map RGBA weights R=rock G=grass B=dirt A=concrete, sampler `u_controlMap` unit 12, gate `MC2_TERRAIN_CONTROLMAP`).

---

## Executive summary

The premise "material layers are hardwired shader slots + hardcoded tiling/tint constants" is **mostly FALSE** on the live chunk path today. The per-layer tuning constants (tiling, tint, normal boost, class thresholds, detail tiling/strength) are **already data-adjacent**: they live as `gosRenderer` C++ members with documented defaults, are uploaded per-draw as uniforms, are ImGui-editable, AND are already loadable from a per-mission **JSON** profile (`data/visual_tuning.json` via `visual_tuning_profile.cpp`'s `TinyJson` + `gos_Set*` setters). What is genuinely hardwired:

1. **Layer→texture binding is fixed** — 9 material layers (`MAT_LAYER_ROCK..CONC_WALL`) map to fixed array slots loaded from fixed filenames `mat0_normal.tga`..`mat8_normal.tga` (`terrtxm2.cpp:2456`). Layer *identity* (which .tga is "rock") is not data-defined.
2. **Two tint constants are frag literals** — `tintConcrete=vec3(0.55,0.53,0.50)` and `tintSnow=vec3(0.75,0.78,0.84)` (`terrain_lod_chunk.frag:715-716`) are NOT uniforms; every other tint IS.
3. **Roughness/AO don't exist as terrain inputs today** — the material array is RGBA normal (A = displacement/POM height). No roughness, no AO channel is sampled on the chunk path.

**Consequence:** TERRAIN-MATERIAL-LIB-1's cheapest, highest-value v1 is **NOT a new engine subsystem** — it is (a) generalize the existing `visual_tuning.json` reader into a `terrain_materials.json` schema that covers ALL per-layer params (not just the ~6 keys the tuning profile touches), (b) promote the two frag-literal tints (`tintConcrete`, `tintSnow`) to uniforms so the JSON is complete, (c) add per-layer roughness/AO as **uniform scalars first** (cheapest; textures deferred). Layer→texture remapping (data-defining WHICH .tga is which layer) is a v2 stretch. Gate `MC2_TERRAIN_MATERIAL_LIB` default OFF; byte-identity = the default JSON's values reproduce the current member/frag defaults EXACTLY, and gate-OFF skips the JSON load entirely (members keep their C++ initializers verbatim).

**Design in 3 bullets:**
1. **Reader-generalization, not greenfield.** `terrain_materials.json` (schema below) loaded at mission load through a new `terrainMaterials_apply()` modeled 1:1 on `visualTuning_applyProfile()` (`visual_tuning_profile.cpp:294`, fopen + TinyJson + `gos_Set*`). Every key maps to an existing `gos_SetTerrain*` setter; NEW setters only for the two promoted frag-literal tints + the new roughness/AO scalars. Gate `MC2_TERRAIN_MATERIAL_LIB` default OFF → reader not called → members = C++ defaults = byte-identical.
2. **Per-layer roughness/AO = uniform scalars v1.** Add `vec4 matRoughness` / `vec4 matAO` (rock/grass/dirt/concrete) uniforms; default (1,1,1,1) neutral so gate-OFF and default-JSON are identical (they must feed the lighting term such that 1.0 = current behavior — see byte-identity list). Optional per-layer roughness/AO TEXTURES are a v2 that reuses the matNormalArray slot pattern.
3. **No new binding for v1.** Everything rides existing per-draw uniform uploads (`gameos_graphics.cpp:6838-6862`) on the already-cached `TerrainUniformLocs`. Only add uniform *locations* + a couple `glUniform*` calls; no UBO, no new texture unit (unit 12 is the control-map slice's). A per-layer std140 UBO is a v3 refactor if the uniform count becomes unwieldy (it will not at 4 layers).

---

## Current material inventory (live chunk path) — file:line

| Input | Kind | Where set (C++) | Frag consumer | Default |
|---|---|---|---|---|
| matNormalArray (9 layers, RGBA, A=displacement) | sampler2DArray | loaded `mclib/terrtxm2.cpp:2456` from `mat0..mat8_normal.tga`; bound unit 5 | `frag:136` sampler; `:568,675,688,758` | slots 0-3 required, 4-8 optional (`:2477`); mat5=marble cliff, mat6=painted conc, mat7=asphalt, mat8=conc wall (`terrain_mat_layers.hglsl:5-14`) |
| Per-layer displacement (matN_displacement.tga) | packed into array .a | `terrtxm2.cpp:2460` | POM `frag:237-239` | slot 4+ optional |
| `matTiling` (rock,grass,dirt,concrete) | uniform vec4 | member `terrain_mat_tiling_` `gameos_graphics.cpp:2410`; upload `:6854` | `frag:144,567,684` | **{3.0, 2.0, 1.0, 6.0}** |
| `matTilingSnow` | uniform float | `terrain_mat_tiling_snow_` `:2411`; upload `:6855` | `frag:146` | **1.0** |
| `matNormalBoost` (r,g,d,c) | uniform vec4 | `terrain_mat_normal_boost_` `:2407`; upload `:6853` | `frag:145,689` | **{0.9, 0.5, 1.1, 2.5}** |
| `detailNormalTiling.x` | uniform vec4 (only .x used) | `terrain_detail_tiling_` `:2379`; packed `tiling[4]` `:6839`; upload `:6845` | `frag:147,567,684` | **1.0** (yzw=0) |
| `detailNormalStrength.x` | uniform vec4 | `terrain_detail_strength_` `:2380`; packed `:6841`; upload `:6846` | `frag:148,706` | **1.0** (.y/.z = anti-tile gate, 0 when OFF) |
| `tintRock` | uniform vec3 | `terrain_tint_rock_` `:2423`; upload `:6858` | `frag:151,717` | **{0.36, 0.37, 0.40}** |
| `tintGrass` | uniform vec3 | `terrain_tint_grass_` `:2424`; upload `:6859` | `frag:152,717` | **{0.35, 0.42, 0.25}** |
| `tintDirt` | uniform vec3 | `terrain_tint_dirt_` `:2425`; upload `:6860` | `frag:153,718` | **{0.48, 0.42, 0.33}** |
| **tintConcrete** | **FRAG LITERAL** (not a uniform) | — | `frag:715,718` | **vec3(0.55, 0.53, 0.50)** |
| **tintSnow** | **FRAG LITERAL** (not a uniform) | — | `frag:716,719` | **vec3(0.75, 0.78, 0.84)** |
| `tintStrengthScale` | uniform float | `terrain_tint_strength_scale_` `:2418`; upload `:6856` | `frag:154,722` | **1.0** |
| `snowBrightnessDampen` | uniform float | `terrain_snow_brightness_dampen_` `:2421` (env-overridable); upload `:6857` | `frag:155` | **0.78** |
| `terrainClassGrass` | uniform vec4 | `terrain_class_grass_` `:2416`; upload `:6861` | `frag:142,181-182` | **{-0.02, 0.06, 0.22, 0.40}** |
| `terrainClassDirt` | uniform vec4 | `terrain_class_dirt_` `:2417`; upload `:6862` | `frag:143,189-190` | **{-0.02, 0.06, 0.22, 0.45}** |
| `matNormalBoost.w` (painted-conc normal) | reuse of above | `:6853` | `frag:689` | 2.5 |
| MAT_WORLD_UNITS_PER_TILE | **frag const** (geometry-derived, NOT a tunable — do not expose) | — | `frag:176` | 768.0 |
| roughness / AO | **DO NOT EXIST** on chunk path today | — | — | — |

**Notes:** the identical uniform set is uploaded to the thin/legacy terrain path too (`:6975-6997`) — that path is DEAD in default config but shares the members, so promoting frag literals to uniforms benefits both without a second code site. Per-layer params exist for exactly **4 blend layers** (rock/grass/dirt/concrete) + snow; layers 5-8 (cliff/painted/asphalt/wall) are special-effect samples, not general blend layers, and are NOT per-layer-tunable today.

---

## Proposed `terrain_materials.json` schema

Precedent: `data/visual_tuning.json` (flat/2-level float JSON, `visual_tuning_profile.cpp:53 TinyJson`). Follow it — a **per-layer array of objects**, NOT a new binary/FitIni format (repo convention for renderer tuning is JSON; FitIni is for mission/unit data). Location `data/terrain_materials.json`, env override `MC2_TERRAIN_MATERIAL_LIB_FILE`.

```json
{
  "version": 1,
  "layers": [
    { "name": "rock",     "tiling": 3.0, "normalBoost": 0.9, "roughness": 1.0, "ao": 1.0,
      "tint": [0.36, 0.37, 0.40] },
    { "name": "grass",    "tiling": 2.0, "normalBoost": 0.5, "roughness": 1.0, "ao": 1.0,
      "tint": [0.35, 0.42, 0.25] },
    { "name": "dirt",     "tiling": 1.0, "normalBoost": 1.1, "roughness": 1.0, "ao": 1.0,
      "tint": [0.48, 0.42, 0.33] },
    { "name": "concrete", "tiling": 6.0, "normalBoost": 2.5, "roughness": 1.0, "ao": 1.0,
      "tint": [0.55, 0.53, 0.50] }
  ],
  "snow":   { "tiling": 1.0, "tint": [0.75, 0.78, 0.84], "brightnessDampen": 0.78 },
  "detail": { "tiling": 1.0, "strength": 1.0 },
  "classify": {
    "grass": [-0.02, 0.06, 0.22, 0.40],
    "dirt":  [-0.02, 0.06, 0.22, 0.45]
  },
  "tintStrengthScale": 1.0
}
```

- **`name`** is documentation + future layer→texture remap key (v2); v1 ignores it (fixed order rock/grass/dirt/concrete).
- **`roughness`/`ao`** = uniform scalars v1 (1.0 = neutral = current behavior). Optional `"roughnessTex": "mat0_roughness"` / `"aoTex"` reserved for v2 (deferred; loads into a new array like matNormalArray).
- **albedo/normal/height paths** deliberately omitted from v1 required set (fixed matN_*.tga filenames stay). Add optional `"normalTex"`/`"albedoTex"`/`"heightTex"` override keys in v2 when layer remap ships.
- Keep parser flat-ish: `TinyJson` handles nested objects two levels deep; a per-layer *array* is a small extension (parse `[` and index) OR flatten to `layer0_tiling` keys to reuse the parser verbatim (recommend flatten-keys v1 to avoid touching TinyJson; array is a v2 nicety).

---

## Load path

**Model 1:1 on `visualTuning_applyProfile()` (`visual_tuning_profile.cpp:294-386`):**
- New TU `GameOS/gameos/terrain_material_lib.cpp` (mirror visual_tuning_profile.cpp): `terrainMaterials_apply(const char* missionName)`.
- Gate check first: `if (!envFlagOn("MC2_TERRAIN_MATERIAL_LIB")) return;` → default OFF path never opens a file, members keep C++ defaults → byte-identical.
- Path: `MC2_TERRAIN_MATERIAL_LIB_FILE` env else `data/terrain_materials.json`; `fopen`/read/TinyJson (same as `:302-305`).
- Apply each key through existing `gos_SetTerrain*` setters (`setTerrainMatTiling`, `setTerrainTintRock/Grass/Dirt`, `setTerrainMatNormalBoost`, `gos_SetTerrainClassGrass/Dirt`, `setTerrainDetailParams`, `setTerrainTintStrengthScale`). NEW setters only for: `tintConcrete`, `tintSnow`, `snowTiling`(exists as `matTilingSnow`), `matRoughness[4]`, `matAO[4]`.
- **Call site:** same place `visualTuning_applyProfile()` is invoked at mission load (find its caller in `code/mission.cpp`/`terrain.cpp` load; co-locate the new call directly after it so JSON tuning applies before first frame). Both are idempotent member writes.

**Why not FitIni:** repo convention — renderer/material tuning uses JSON (`visual_tuning.json`, terrain_gen tool outputs). FitIni is mission/unit content and cannot write braces (known limitation, MEMORY). JSON is the established precedent and already has a working in-engine reader. Do NOT introduce a second config mechanism.

---

## Uniform/UBO plan

**v1 = extend the existing uniform set (NO UBO).** All per-layer params already ride `TerrainUniformLocs` uploaded at `gameos_graphics.cpp:6838-6862`. Add:
- New locations in `TerrainUniformLocs` + `ThinTerrainUniformLocs` (mirror existing pairs): `tintConcrete`, `tintSnow`, `matRoughness`, `matAO`.
- New frag uniforms: `uniform vec3 tintConcrete; uniform vec3 tintSnow; uniform vec4 matRoughness; uniform vec4 matAO;` — replace the two `const vec3` literals at `frag:715-716`.
- New members: `terrain_tint_concrete_[3]={0.55,0.53,0.50}`, `terrain_tint_snow_[3]={0.75,0.78,0.84}`, `terrain_mat_roughness_[4]={1,1,1,1}`, `terrain_mat_ao_[4]={1,1,1,1}` + `glGetUniformLocation` in the chunk + thin location setup blocks (near `:6854`), + upload calls next to `:6858`.

**Binding choice:** none needed for v1 — reuses program default-block uniforms. **A per-layer std140 UBO is over-engineering at 4 layers**; only revisit if roughness/AO textures (v2) push toward a bindless material table. If a UBO is ever wanted, the terrain-relevant UBO/SSBO bindings 23-26 are taken (control map is texture unit 12); pick the next free UBO binding per the binding ledger at that time — not v1.

**Roughness/AO wiring:** feed into the terrain lighting term. Since roughness/AO don't exist today, the byte-identity requirement is that `matRoughness=matAO=1.0` produce EXACTLY the current lighting. Simplest safe form: `weightedRoughness = dot(matWeights, matRoughness)` used only inside an `if (u_useMaterialLib != 0)` branch — else the current lighting math runs verbatim. Add `uniform int u_useMaterialLib` (default 0) so gate-OFF is a compiler-invariant passthrough, exactly like the control-map slice's `u_useControlMap`.

---

## Gate + byte-identity constant list

**Gate:** `MC2_TERRAIN_MATERIAL_LIB` default OFF. OFF ⇒ (a) `terrainMaterials_apply()` returns early (no file, members untouched), AND (b) driver uploads `u_useMaterialLib=0` so any NEW lighting math (roughness/AO) is never taken. Both layers of protection (member-default + uniform-branch) as the control-map slice does.

**The default JSON MUST mirror these EXACT constants (gate-ON, default JSON ≈ byte-identical):**

| Key | Value | Source |
|---|---|---|
| layer.rock.tiling / grass / dirt / concrete | 3.0 / 2.0 / 1.0 / 6.0 | `gameos_graphics.cpp:2410` |
| snow.tiling | 1.0 | `:2411` |
| layer.*.normalBoost (r/g/d/c) | 0.9 / 0.5 / 1.1 / 2.5 | `:2407` |
| detail.tiling / detail.strength | 1.0 / 1.0 | `:2379-2380` |
| tint.rock | 0.36, 0.37, 0.40 | `:2423` |
| tint.grass | 0.35, 0.42, 0.25 | `:2424` |
| tint.dirt | 0.48, 0.42, 0.33 | `:2425` |
| tint.concrete | 0.55, 0.53, 0.50 | `frag:715` (LITERAL → promote) |
| tint.snow | 0.75, 0.78, 0.84 | `frag:716` (LITERAL → promote) |
| tintStrengthScale | 1.0 | `:2418` |
| snow.brightnessDampen | 0.78 | `:2421` (also env `MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN`) |
| classify.grass | -0.02, 0.06, 0.22, 0.40 | `:2416` |
| classify.dirt | -0.02, 0.06, 0.22, 0.45 | `:2417` |
| layer.*.roughness / ao | 1.0 / 1.0 (all) | NEW neutral default |

**Landmine on identity:** `terrain_class_dirt`/`terrain_class_grass` are ALSO mutated at runtime by the Sand_M24 material-profile path (`g_terrainMaterialProfile`, `frag:187-188` widens dirt gate). The JSON must NOT clobber the mission-profile override — apply JSON at load, but the profile widening happens in-shader via `g_terrainMaterialProfile`, so they compose. Verify order: JSON sets base thresholds; shader profile-1 still widens. `visual_tuning.json` already writes these same class values (`visual_tuning_profile.cpp:46-49`) — reuse its ordering.

---

## Files to touch (FIX slice, not now)

- **NEW** `GameOS/gameos/terrain_material_lib.cpp` (+ `.h`) — reader, `terrainMaterials_apply()`, gate. Mirror `visual_tuning_profile.cpp`.
- `GameOS/gameos/gameos_graphics.cpp` — new members (`terrain_tint_concrete_`, `terrain_tint_snow_`, `terrain_mat_roughness_`, `terrain_mat_ao_`), new setters, new `TerrainUniformLocs`/`ThinTerrainUniformLocs` fields + `glGetUniformLocation` (near `:2564-2669`), new upload calls (near `:6858`, `:6994`), new `u_useMaterialLib` upload.
- `shaders/terrain_lod_chunk.frag` — replace `tintConcrete`/`tintSnow` literals (`:715-716`) with uniforms; add `matRoughness`/`matAO`/`u_useMaterialLib`; roughness/AO into lighting under the `u_useMaterialLib` branch.
- `shaders/gos_terrain.frag` — mirror the two promoted tint uniforms (dead path but shares members; keeps them in sync — LOW priority, do only if the shared upload path would otherwise pass an unbound uniform).
- `code/mission.cpp` (or wherever `visualTuning_applyProfile` is called) — add `terrainMaterials_apply(missionName)` call.
- `data/terrain_materials.json` — the default (byte-identity) file.
- CMake: add the new TU to the GameOS target.
- `docs/` — a `terrain-material-lib.md` (mirror `visual-tuning-profiles.md`); update binding/asset registry only if v2 textures land.

---

## Landmines

1. **Most "hardcoded constants" are already uniforms/members — don't rebuild what exists.** The slice's real work is JSON-schema + reader + promoting TWO frag literals + adding roughness/AO scalars, NOT a material subsystem. Over-scoping = wasted effort + identity risk.
2. **Two frag literals (`tintConcrete`, `tintSnow`) must become uniforms for the JSON to be complete** — but promoting a `const` to a `uniform` changes the compiled program; the default upload value MUST be the exact literal (0.55,0.53,0.50 / 0.75,0.78,0.84) or gate-OFF drifts. Both terrain programs (chunk + thin) sample them.
3. **Roughness/AO don't exist today** — any new lighting math is a behavior change by definition; it MUST sit behind `u_useMaterialLib != 0` with neutral (1.0) defaults, exactly like the control-map slice's `u_useControlMap` pattern, or byte-identity is lost.
4. **Sand_M24 profile mutates the class thresholds in-shader** (`g_terrainMaterialProfile`, `frag:187`) — JSON class values compose with, must not fight, the mission profile. Test mc2_24.
5. **Shared uniform upload feeds the DEAD thin/legacy terrain path too** (`:6975-6997`) — add the new fields to `ThinTerrainUniformLocs` as well or that path passes stale/unbound tint uniforms. Do NOT edit `gos_terrain_indirect.cpp` / `quad.cpp` (dead in default config).
6. **Interaction with TERRAIN-CONTROLMAP-SAMPLE-1 (concurrent):** control map replaces the *classifier* (which weights); this slice tunes *per-layer appearance* (how each weighted layer looks). They are orthogonal — control map writes `matWeights`, this slice scales `matTiling`/tint/roughness by those weights. Order: control-map selection (`frag:601-616`) runs BEFORE tint/detail (`:675,717`), so material-lib params consume whatever weights the control map produced. No conflict, but do NOT double-own `matWeights`. Treat `terrain_lod_chunk.frag` + `mclib/terrain.cpp` as read-only-at-base reference while that agent works; merge the two frags carefully (both add uniforms + branches near the same region).
7. **Cliff triplanar (`useTriplanarCliff`, `frag:747`) samples MAT_LAYER_MARBLE_CLIFF (slot 5)** with its own hardcoded `ts=256.0` world-repeat — that is a layer-5 special sample, out of scope for the 4-layer material lib. Do not fold slot 5-8 tiling into the JSON v1 (they're not general blend layers).
8. **`MAT_WORLD_UNITS_PER_TILE=768` is geometry-derived, NOT a tunable** (`frag:172-176` comment) — do NOT expose it in the JSON.
9. **TinyJson is minimal** (`visual_tuning_profile.cpp:53`) — two-level flat float objects. A per-layer array needs parser work; recommend flatten-key form (`rock_tiling`) v1 to reuse it verbatim, pretty array as v2.
10. **snowBrightnessDampen has an env override already** (`MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN`, `:2421`) — JSON vs env precedence must be decided (recommend env wins, as it does for visual_tuning).

## Hot-reload story (assess)

**Nice-to-have, LOW cost.** `visual_tuning_profile.cpp` already has a save path (`:401 fopen(...,"w")`) and applies via setters — a file-watch or a `MC2_TERRAIN_MATERIAL_LIB` re-apply key press would re-run `terrainMaterials_apply()` and the next frame's uniform upload picks up new member values automatically (no relink, no texture reload for scalar-only v1). **Cost is trivial for scalar params** (just re-call the reader). It becomes expensive only when v2 texture overrides land (need array rebuild). Recommend: v1 ships a manual re-apply hook (reuse the ImGui panel button pattern), full auto file-watch deferred.

## Editor implications

`EditRel` renders terrain via the same chunk draw and shares the `gosRenderer` members, so the JSON tuning applies to the editor too if `terrainMaterials_apply()` runs on editor mission load. The two new frag uniforms/`u_useMaterialLib` must be bound in the editor draw (they are, since it's the same program + upload path). Default OFF ⇒ editor identical. An editor ImGui "Terrain Materials" panel is a natural follow-on (the getters/setters already exist for most keys; `EditorInspector.cpp` already touches these). Do NOT add CPU fallbacks to editor TUs.

---

## Acceptance tests

- **Gate-OFF byte-identical:** build RelWithDebInfo, deploy exe+shaders lockstep, run canonical tier1 smoke (verbatim CLAUDE.md), `MC2_TERRAIN_MATERIAL_LIB` UNSET → exit 0, no new `crash_*`, no new GL errors, shaders compile (check console — hot-reload silent-fail). Identity rests on member C++ defaults untouched + `u_useMaterialLib=0` else-branch = current code, + the two promoted-literal uniforms uploading their exact literal values.
- **Gate-ON default JSON ≈ identical:** `MC2_TERRAIN_MATERIAL_LIB=1` with the byte-identity `data/terrain_materials.json` → visually indistinguishable from OFF on mc2_01 (green) + mc2_24 (sand/dirt, exercises class thresholds + Sand_M24 profile). Static-cam screenshots ON vs OFF. Per arc ruling: workbench/static-cam verify, no pixel-golden.
- **Modified JSON visibly changes one layer:** edit `layer.grass.tint` to e.g. red or `layer.rock.tiling` to 12 → screenshot shows only that layer changed, others stable. Confirms per-layer isolation.
- **Interaction:** with control-map slice landed, run `MC2_TERRAIN_CONTROLMAP=1 MC2_TERRAIN_MATERIAL_LIB=1` on mc2_24 → both compose (control map picks weights, material lib styles them), no crash, no unbound-uniform warning.
- **Roughness/AO neutral proof:** default (1.0) roughness/AO ON must equal OFF (isolates the new lighting term).

---

## Implementation note (post-ruling)

Shipped as flat float keys (`rock_tiling`, `rock_tint_r`, ...) reusing TinyJson's
`floatObj()` verbatim -- no parser changes needed. `data/terrain_materials.json`
has NO comment keys (the TinyJson `num()` call on a string value doesn't advance
`p` safely) -- precedence/schema documentation lives in
`GameOS/gameos/terrain_material_lib.h` instead. Precedence: engine default <
`data/terrain_materials.json` (only read when `MC2_TERRAIN_MATERIAL_LIB` is set)
< env var (env wins for `snowBrightnessDampen`, matching `visual_tuning.json`
convention) < ImGui (not added this slice). Applied AFTER
`visualTuning_applyProfile()` at the same mission-load call site
(`code/mission.cpp`), so it wins on any overlapping terrain-material key.

## Open questions (need user ruling)

1. **Scope of v1:** ship the tuning-only lib (schema + reader + 2 promoted literals + roughness/AO scalars, fixed layer→texture) FIRST, deferring data-defined layer→texture remap (WHICH .tga is "rock") to v2? Recommend yes — remap is the expensive part and rarely needed.
2. **Roughness/AO in v1 at all, or defer entirely?** They don't exist today, so they're pure new behavior. Option A: add neutral scalars now (schema-complete, gated). Option B: defer, ship tuning-only. Recommend A (cheap, schema-complete, fully gated).
3. **JSON layer form:** flatten-keys (`rock_tiling`, reuse TinyJson verbatim) vs a proper `layers[]` array (touch the parser)? Recommend flatten v1, array v2.
4. **JSON vs env precedence** for the keys that also have env overrides (`snowBrightnessDampen`): env wins (matches visual_tuning) or JSON wins? Recommend env wins.
5. **Relationship to `visual_tuning.json`:** these two files OVERLAP (visual_tuning already writes class thresholds + some terrain keys). Merge terrain_materials INTO visual_tuning (one file), or keep separate (material lib = per-layer authoring, visual_tuning = global post/lighting)? Recommend separate files, but the reader must define precedence if both set the same key (recommend material lib applied AFTER visual_tuning so it wins for terrain material keys).
6. **Snow as a 5th layer?** Snow has tiling + tint + dampen but is HSV-derived, not weight-selected. Keep it a special sub-object (as schema'd) rather than a peer layer? Recommend yes (special).
