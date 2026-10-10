<!-- doc-status: stale | reviewed:2026-10-11 -->
# Static-Prop Material ORM + Normal-Map Recon — `STATICPROP-MATERIAL-ORM-NORMAL-RECON-1`

**Status:** recon / design only. No shader, C++, asset-format, ABI, cook, or behavior
changes. Branch `claude/staticprop-material-orm-normal-recon-1` (off `claude/nifty-mendeleev`).

**One-line conclusion:** Worth doing now — but **split ORM from normal mapping**. The
MaterialGpu ABI, KTX loader, manifest validator, and RGBA8 cook are *already done*; ORM
(roughness/metallic/AO) is a visually-sound, contained addition. Normal mapping is
meaningful (diffuse is **not** baked) but carries a real split-granularity trap and a
tangent prerequisite — it belongs in its own larger arc, after ORM.

---

## Goal

Add real runtime **authored material slots** to the in-game static-prop path: baseColor,
normal, ORM (R=AO, G=roughness, B=metallic), optional emissive — and make the runtime
`static_prop` shader actually consume them. Separate from the asset-viewer Local PBR
Material Ball (that is a viewer concern; this is in-game runtime).

## Non-goals / hard constraints

No implementation. No shader edits, no C++ runtime edits, no asset-format change, no
MaterialGpu ABI change, no cook-output change, no behavior change, no `git add -A`.

---

## 1. Current static-prop material contract

### Vertex inputs (`shaders/static_prop.vert:23-30`)
```
location 0  vec3  a_position
location 1  vec3  a_normal
location 2  vec2  a_uv
location 3  uint  a_localVertexID
location 4  uint  a_aRGBLight     // per-vertex light/window magic-color tag
```
**No tangent. Location 5 is free.** CPU stride = 40 B, inline layout
(`GameOS/gameos/gos_static_prop_batcher_internal.h:256-263`), attrib setup `:1887-1900`.

### MaterialGpu (`RenderCore/MaterialGpu.h:88-117`) — **the slots already exist**
```cpp
struct alignas(4) MaterialGpu {   // 32 bytes, std430-locked, static_assert'd
    uint32_t albedoTex;             //  0
    uint32_t normalTex;             //  4
    uint32_t metallicRoughnessTex;  //  8  (R=AO G=roughness B=metalness)
    uint32_t emissiveTex;           // 12
    uint32_t flags;                 // 16  (kNormalMap=1<<1, kMetallicRoughness=1<<2, kEmissive=1<<3, kWindow=1<<5)
    float    baseColorFactor;       // 20
    float    metallicFactor;        // 24
    float    roughnessFactor;       // 28
};
```
GLSL mirror `shaders/include/material_gpu.hglsl:47-54`; mirror gate
`scripts/check-material-gpu-mirror.sh`; reflect invariants `tools/shader_reflect/reflect.py`
(MaterialGpu offsets hard-pinned via `shaders/fixtures/material_gpu_contract.frag`).
`kMaterialTexAbsent = 0xFFFFFFFF` (`MaterialGpu.h:83`).

### What is populated / ignored (`gos_static_prop_batcher.cpp:3144-3152`)
```cpp
m.albedoTex = layer;                       // bucket-relative array layer  (POPULATED)
m.normalTex = m.metallicRoughnessTex = m.emissiveTex = kMaterialTexAbsent;  // IGNORED
m.flags = 0;
m.baseColorFactor = 1.0f; m.metallicFactor = 0.0f; m.roughnessFactor = 1.0f;
```
The shader **never samples** normalTex/metallicRoughnessTex/emissiveTex. Only `albedoTex`
(array layer) and the scalar `metallic/roughnessFactor` are read.

### Exact current lighting model (corrects the brief's "baked" premise)
Direct diffuse is **computed live, per-vertex** — not baked:
1. `get_base_light()` decodes the `a_aRGBLight` magic-color/window tag → `base_light`
   (`lighting.hglsl:60-160`). This is the only "baked-ish" term; ≈0 for stock daytime props.
2. `calc_light(lightDataIndex, worldNormal, worldPos, base_light)` — full 6-light-type
   dispatch, **`dot(worldNormal, lightDir)` computed live** (`lighting.hglsl:228,239,251,269`).
   Up to 16 lights. **Gouraud (per-vertex)**, interpolated to `v_argb`.
3. Hemisphere ambient (`static_prop.vert:350-355`, default strength 0).
4. SH-L2 IBL `evalShL2(worldNormal)` (`static_prop.vert:362-365`, default 0).
5. Window nodes (`kFlagIsWindow`) bypass lighting entirely (`vert:336-338`).

Fragment (`static_prop.frag`): `c = albedo × v_argb.rgb` (`:311`) + highlight, then a
**sun-only Schlick specular** block (`:336-398`, gate `u_pbrV1Strength`, default 0) that
already runs **per-fragment on the surface normal** with `metallic/roughnessFactor` scalars
from MaterialGpu. MRT out: `FragColor`(0), `GBuffer1`(1, geometric normal), optional
objectId(2). `MC2_STATIC_PROP_PBR_V1` default-OFF.

---

## 2. Missing pieces (what it takes)

| Piece | Where | Class |
|---|---|---|
| MaterialGpu slots + flags + scalars | `RenderCore/MaterialGpu.h` | **DONE** |
| GLSL MaterialTable mirror + check + reflect goldens | `material_gpu.hglsl`, scripts | **DONE** |
| KTX2/BC7 loader exposes `isSrgb`/`vkFormat` | `RenderCore/KtxLoader.cpp:161-165` | **DONE** |
| Manifest validator: slot/colorSpace/tangent cross-check | `tools/validate_asset_manifest.py:79-310` | **DONE** |
| `mc2texcook` RGBA8 presets (normal/orm/emissive) | `tools/mc2texcook/mc2texcook.py:226-269` | **DONE** |
| BC7 cook hardcoded albedo/sRGB only | `tools/mc2texcook/batch_cook.py:52-97` | **NEEDS-PLUMBING** |
| Bucket array builder hardcodes sRGB BPTC | `gos_static_prop_batcher.cpp:2376,2795` | **CONFLICTS-WITH-BC7-BUCKET** |
| Parallel per-usage linear arrays (normal/ORM/emissive) | batcher `:2643-2699`, bind `:5609` | **NEEDS-PLUMBING** |
| Source feed: a 2nd/3rd texture per type | batcher `:2536` (`listOfTextures[textureSlot]` = albedo only) | **GREENFIELD** |
| Populate normal/MR/emissive indices + flags | batcher `:3144-3152` | **GREENFIELD** |
| Shader sample normal/ORM/emissive + fallbacks | `static_prop.frag:84,196,346-351` (one sampler) | **GREENFIELD** |
| Tangent attribute + TBN | vert/VBO (loc 5 free) | **GREENFIELD** (mech precedent) |
| Runtime cooked normal/ORM `.ktx2` + geo fixtures | none in tree | **GREENFIELD** |

---

## 3. Shader / ABI impact

- **MaterialGpu: zero ABI churn.** Struct, offsets, GLSL mirror, mirror-check, and
  `reflect.py` invariants all stay byte-identical — the slots already exist.
- **Goldens that change** only on a vertex-attribute add (tangent) or new SSBO surface:
  `tools/shader_reflect/expected/shaders__static_prop.{vert,frag}__*.json` (4 variants each).
  `tests/unit/test_rendercore.cpp` `ssboBindingsMask` asserts (`:411,480,494`) change only if
  the pass's binding mask changes.
- **Bindings**: vert uses SSBO 0/1/2/3/20; frag-coalesce uses SSBO 4 (`PerDrawEntry`,
  carries `materialIdx`) + 5 (`MaterialTable`) + `sampler2DArray u_texArr`. Free SSBO slots
  6–15,17–19,21+. Adding ORM/normal **needs new `sampler2DArray` uniforms** (one per usage
  array) — no new SSBO binding if the layer-index model is kept.
- **Feature variants** are prefix `#define`s prepended to `#version 430`
  (`gos_static_prop_batcher.cpp:896-1001`): `MC2_COALESCE`, `MC2_USE_VIEW_UNIFORMS` (default
  on), `MC2_OBJECT_ID_BUFFER`. New behavior must add a `#define` + the dual interlock below.

### Gate: `MC2_STATICPROP_MATERIAL_PBR_SLOTS` (default-OFF)
Follow the `MC2_STATIC_PROP_PBR_V1` pattern (`batcher:498-501`): compile-guard the new
shader block **and** force the strength/sample uniform to 0 at upload when the gate is off,
so gate-OFF output is byte-identical. Add the gate to the `run_smoke.py` env allowlist
(`:528-538`).

---

## 4. Texture / cook impact

- **Cannot share the albedo array.** A GL `TEXTURE_2D_ARRAY` has one internalformat; the
  builder hardcodes `GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM` (`batcher:2376,2795`). Normal/ORM
  are **linear**. → build **parallel per-usage arrays** with the format chosen from
  `KtxImage.isSrgb` (loader already returns it). The bucket machinery (dim partition, layer
  dedup, `glCompressedTexSubImage3D` upload) is fully reusable; only the per-array format
  choice and a second/third array+sampler are new.
- **Source feed is the real GREENFIELD gap.** Today `listOfTextures[textureSlot]` carries
  only albedo; nothing supplies normal/ORM/emissive per type. This needs either a material
  manifest consulted at `finalizeGeometry`, or an extension of the type→texture mapping.
- **BC7 cook for non-albedo** (`batch_cook.py --bc7`) is hardcoded to `R8G8B8A8_SRGB` +
  `--assign-tf srgb`; needs a slot parameter to emit **linear** BC7 (`--assign-tf linear`,
  UNORM) for normal/ORM. RGBA8 cook already handles all presets.

### Color-space policy (from `validate_asset_manifest.py:79-85`, the authority)
| slot | colorspace | vkFormat (BC7) | fallback when absent |
|---|---|---|---|
| baseColor | **sRGB** | 146 | albedo array layer (current) |
| normal | **linear** | 145 | flat normal `(0,0,1)` |
| ORM | **linear** | 145 | AO=1, roughness=`roughnessFactor`, metallic=`metallicFactor` |
| emissive | **sRGB** | 146 | black (no add) |

Absent handling mirrors the existing albedo `kMaterialTexAbsent` guard
(`static_prop.frag:189-193`) per slot.

---

## 5. Tangent strategy

**STOP-CONDITION CHECK: contained additive change, NOT a mesh-format rewrite.** The mech
path is the exact precedent: `GpuMechVertex.tangentOct[2]` (octahedral int16) at
**location 5**, zero-filled for stock meshes (`gos_mech_batcher.cpp:1070-1072,1194`;
`mech.vert:40`; design `docs/plans/2026-04-19-mech-normal-maps.md`).

- Static-prop source (`TG_TypeVertex`, `mclib/tgl.h:37-42`) has **no tangent**; ASE/FIT
  carries none.
- **Generate at load in the batcher's `registerType` expand loop**
  (`gos_static_prop_batcher.cpp:1746-1785`) from position+UV derivatives. This keeps
  `TG_TypeVertex` and the on-disk `.tgl` binary cache untouched (avoids the
  `LoadBinaryCopy` `sizeof` bump at `tgl.cpp:525` — the one format hazard).
- Add `a_tangent` at location 5 (+ `glVertexAttribPointer` after `:1900`, + shader input).
  Use **`vec4` tangent with `.w` handedness** for mirrored UVs (octahedral, like mech, loses
  handedness — acceptable for an MVP but vec4 is safer for props).
- Stock/legacy props degrade gracefully: generate-for-all (trivial cost; triangle-soup
  layout needs no cross-vertex averaging) or zero-fill + skip TBN per material.

---

## 6. Lighting-model interaction — the load-bearing risk

**ORM is sound now. Normal mapping has a split-granularity trap.**

- **Roughness/metallic map:** drop-in — the specular block already runs per-fragment on the
  surface normal and already reads the scalar factors (`frag:346-358`). The map multiplies
  the scalars. ✓ visually consistent.
- **AO (R of ORM):** must multiply **only the indirect/ambient terms** (hemisphere ambient +
  SH-L2 IBL), **never** `calc_light` direct diffuse (double-darkens N·L creases) nor raw
  albedo. Today those ambient terms are vertex-stage; AO is per-fragment, so AO forces them
  to be re-applied/​moved per-fragment, or AO has nowhere to land. Contained but real.
- **Normal map:** direct diffuse is per-**vertex** live `N·L`; a normal map perturbs
  per-**fragment**. Feeding the mapped normal only into the per-fragment specular + GBuffer
  (leaving diffuse on the coarse vertex normal) produces a **visible split**: highlights and
  shadow-receiver normals track bumps while diffuse stays flat. Doing it *right* requires
  moving `calc_light` diffuse to the fragment stage — a much larger change than the existing
  specular-only PBR-3 move. Window/magic-color nodes (`kFlagIsWindow`) must keep bypassing.

→ **Recommendation: ship ORM first (no normal map); defer normal mapping to its own arc.**

Pre-existing inconsistency to reconcile: frag fallback `roughness=0.6` (`frag:346`) vs table
default `roughnessFactor=1.0` (`batcher:3152`).

---

## 7. Fallback / default / backward compatibility

- **Gate OFF:** byte-identical to today (dual compile+runtime interlock).
- **Gate ON, no new maps:** `kMaterialTexAbsent` per slot → flat normal / AO=1 /
  scalar-only / emissive=0; albedo-preserving. Stock + old mods render unchanged (runtime
  synthesizes the material table live at `finalizeGeometry`; no manifest versioning needed —
  `batcher:3144-3152`).
- **Old mods:** identical flow; albedo-only, all extra slots absent.
- **QA:** primary soak `mc2_10` (urban/buildings/windows); also `mc2_01` (foliage),
  `mc2_17`, `mc2_24`. Harness: `scripts/capture_baseline.py` (deterministic camera,
  PNG+JSON, `--verify` byte-diff) + `mc2-render-state` MCP. Debug views exist
  (`MC2_STATIC_PROP_DEBUG_MATERIAL`: albedo/normal/roughness/metallic). Add AO + sampled-
  normal modes when wiring. Add `staticPropAoStrength`/`staticPropPbrStrength` to
  `data/visual_tuning.json` (mirrors `staticPropIblStrength`).

---

## 8. Implementation slices (re-ordered: ORM-first)

1. **`STATICPROP-MATERIAL-SLOT-CONTRACT-0`** — docs/tests only. Pin slot semantics,
   color-space table, per-slot `kMaterialTexAbsent` fallbacks; reconcile the rough=0.6 vs
   1.0 default; add the `MC2_STATICPROP_MATERIAL_PBR_SLOTS` gate name + allowlist entry. No
   behavior.
2. **`STATICPROP-ORM-TEXTURE-ARRAYS-1`** — parallel **linear** BC7/RGBA8 array per usage
   (ORM first), format from `KtxImage.isSrgb`; slot-aware `batch_cook.py --bc7`; source feed
   for the ORM texture per type; populate `metallicRoughnessTex` + `kMetallicRoughness` flag.
3. **`STATICPROP-SHADER-ORM-GATED-1`** — gated shader: sample `metallicRoughnessTex` ×
   scalar factors; AO into ambient/IBL only; new `sampler2DArray`; regenerate frag goldens;
   debug AO view. Gate default-OFF, byte-identical when off.
4. **`STATICPROP-ORM-SOAK-1`** — `mc2_10` capture soak + `visual_tuning.json` knobs; decide
   default-ON for ORM.
5. **`STATICPROP-TANGENT-GEN-1`** *(separate normal arc, after ORM)* — runtime tangent gen
   in `registerType` (loc 5, vec4+handedness), zero-fill/skip for stock; no `.tgl` format
   bump.
6. **`STATICPROP-NORMALMAP-PERFRAGMENT-2`** *(big)* — normal array + TBN + **move
   `calc_light` diffuse per-fragment** to avoid split granularity; emissive add; soak.

(The brief's slices 2–4 map onto 5/6 + 2/3; the reorder front-loads the visually-sound,
contained ORM work and quarantines the large per-fragment-diffuse change.)

---

## 9. Risks

1. **Split normal granularity (highest).** Normal map on per-vertex diffuse is misleading
   unless diffuse moves per-fragment. Mitigation: ORM-first; normal map is its own arc with
   the diffuse move scoped in.
2. **BC7 bucket format conflict.** Builder is sRGB-hardcoded; linear arrays are a real fork.
   Mitigation: reuse machinery, parameterize format from `isSrgb`; contained to the batcher.
3. **Source feed is greenfield.** Nothing supplies non-albedo textures per type today.
   Mitigation: Slice 1 defines the manifest/type→texture contract before plumbing.
4. **`.tgl` binary-cache hazard.** Adding to `TG_TypeVertex` breaks cached binaries.
   Mitigation: generate tangents in the batcher expand loop; never touch `TG_TypeVertex`.
5. **Per-fragment diffuse move is a hot-path change** with perf + parity implications across
   16 lights. Mitigation: gate + soak + Tracy before any default-ON.
6. **Scope creep into mech/terrain materials.** Hold to static props.

## 10. Stop conditions

- Tangents require a broad mesh-format rewrite → **not triggered** (contained; mech precedent;
  batcher-local generation).
- Texture slot plumbing conflicts with the BC7 bucket → **partially triggered**: sRGB-
  hardcoded single-format array. Resolved by parallel linear arrays; do not force normal/ORM
  into the albedo array.
- MaterialGpu cannot support without ABI churn → **not triggered**: zero churn, slots exist.
- Visual model misleading due to lighting → **triggered for normal maps only** (split
  granularity). ORM is sound. Recommendation: ORM in-game now; normal map gated + its own arc;
  the asset-viewer Local PBR ball remains the place for free-standing material authoring.
- Feature too big for one Opus → **yes, split** (slices above; ORM arc vs normal arc).

## 11. Recommendation

**Do ORM now; do normal mapping later (its own arc); both gated.**

- **Now (worth it, low-risk):** ORM (roughness/metallic map + AO into ambient/IBL) behind
  `MC2_STATICPROP_MATERIAL_PBR_SLOTS`. The ABI/loader/validator/cook are done; specular
  already runs per-fragment on the surface normal; ORM is visually consistent. This is real,
  shippable material fidelity for static props with byte-identical stock/mod fallback.
- **Later (worth it, bigger):** normal mapping — needs tangents (contained) **and** moving
  direct diffuse per-fragment (large hot-path change) to avoid the split-granularity
  inconsistency. Sequence it after ORM proves the slot/array/cook plumbing.
- **Not now:** emissive can ride slice 6 or a small follow-on; it is additive and low-risk
  but unblocked by nothing urgent.
