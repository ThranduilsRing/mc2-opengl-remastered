# Sampler / texture-unit occupancy — SHADER-SAMPLER-BINDING-MANIFEST-1

**Arc:** VULKAN-CONTRACT-MANIFEST-ARC · **Slice:** SHADER-SAMPLER-BINDING-MANIFEST-1
**Generated/checked by:** `scripts/check-sampler-bindings.py` (registered in `scripts/check-contracts.sh` as `sampler_bindings`)
**Machine-readable companion:** `sampler-unit-occupancy.json` (regenerate: `py -3 scripts/check-sampler-bindings.py --json docs/render-backend-seams/sampler-unit-occupancy.json`)
**Built against:** nifty-mendeleev HEAD (re-grep line numbers before trusting — cheap to drift)

## Why this exists

`check-binding-slots.py` covers the SSBO/UBO buffer-binding-base namespace but
explicitly excludes the `GL_TEXTURE_*` (sampler) namespace. That was the single
largest descriptor blind spot for Vulkan readiness: a Vulkan descriptor set needs
every `(set, binding)` for combined image-samplers, and today MC2's sampler units
are scattered C++ literals coupled to GLSL only by hand-comment. This manifest
makes the texture-unit namespace **visible and lockstep-checked**, the same way the
buffer slot occupancy did for buffers.

## The model: sampler units are multiplexed per pass (NOT a flat namespace)

A unit number is semantic **only inside a program**. Confirmed multiplexing:

| Unit | Carried by (program → sampler) — non-exhaustive |
|---|---|
| **0** | colormap / atlas / `u_tex` (terrain, static-prop, mech legacy); `sceneTex`/`sceneDepthTex`/`depthTex`/`uSrc`/`ssaoTex` (post-fx); `uAtlas` (particle/vfx/tube); `u_atlas` (veg); `u_hdri` (skybox) |
| **1** | `sceneNormalTex` (post-fx); `u_pbrNormalTex` (mech); `ormTexArr` (static-prop); detail `tex2` (water); `u_sceneDepth` (particle) |
| **2** | `shadowMap` static (terrain); `u_pbrOrmTex` (mech); `u_objectIdTex`/`u_waterReflRT` |
| **3** | `dynamicShadowArray`/`dynamicShadowMap` (post-fx shadow, mode-alternate); `u_hdri` (water fast); `u_pbrPaintNormalTex` (mech); `tex3` cement (terrain indirect) |
| **4** | `dynamicFullMapShadow`; `u_pbrPaintOrmTex` (mech); `u_transitionMaskArray` (terrain indirect/mask) |
| **5** | `matNormalArray`/`matNormal0` (terrain); `mineSpriteArray` |
| **6,7,8,12** | `matNormal1..4` (terrain splat normals — `kTerrainMatNormalUnits[5]={5,6,7,8,12}`) |
| **9** | static shadow map (terrain/chunk, `kTerrainTexUnitStaticShadow`) |
| **10** | dynamic shadow array/map (terrain/chunk) |
| **11** | terrain height tex (legacy/indirect) **and** chunk transition-mask array (`kChunkTexUnitTransitionMask`) |
| **13** | dynamic full-map shadow (terrain/chunk) |

Because a unit means different things in different programs, the checker FAILs
only on **intra-program** contradictions, not cross-program reuse.

## How units are assigned (two binder families)

1. **Name-resolvable** — `Program::setInt("name", unit)` / `GLProgram::setSamplerUnit("name", unit)`.
   The sampler name is a string literal → joins to the GLSL `uniform sampler* name;`
   decl. This is the entire post-process subsystem (HZB/SSAO/screen-shadow/cloud/
   shoreline/fog/skybox/composite) plus `shadowDebug`. ~22 binds. **This is the
   lockstep surface the checker enforces.**
2. **Literal loc-cache** — `glUniform1i(s_locColormap, 0)` etc., where the unit is
   a bare literal and the sampler name lives in a cached `GLint` location variable
   (`s_loc*`, `tl.tex1`, `kTerrainTexUnit*`). The name is **not** statically
   recoverable from the call, so these are counted (237 `glUniform1i` sites,
   201 `glActiveTexture` sites) but appear as `UNKNOWN binder` rows in the manifest.
   This is terrain / mech batcher / static-prop batcher / bridges.

A Vulkan port should migrate family (2) toward family (1)'s named, greppable form
(or a generated binding constant) so every combined image-sampler has an explicit,
checkable `(program, name, unit, target)`.

## Checker contract

`scripts/check-sampler-bindings.py` — exit 0 unless a FAIL. WARN never fails the build.

**FAIL conditions:**
- **comment-vs-code drift** — a *non-multiplexed* sampler (one GLSL file, one C++
  receiver, one unit) whose own GLSL `// unit N` comment disagrees with its C++
  unit. (Multiplexed names like `u_hdri` are WARN, not FAIL — we don't statically
  link a GLSL file to its C++ program.)
- **intra-program contradiction** — one program receiver binds the same sampler
  name to two different units.

**WARN conditions (informational, expected):**
- `UNKNOWN binder` — GLSL sampler with no name-resolvable C++ assignment (terrain/
  mech/static-prop literal loc-cache binds, or a dormant gated path like
  `building_pbr.*` under `MC2_BUILDING_PBR`).
- `cross-pass reuse` — a unit carried by >1 sampler across programs (the multiplex).
- `mode-alternate` — `dynamicShadowArray`/`dynamicShadowMap` share unit 3 on
  mutually-exclusive CSM-vs-single-map branches (`screenShadowProg_`); intentional.
- `shadowDebug` array/map share unit 0 (`shadowDebugProg_`); intentional.
- `multiplexed comment/code mismatch` — a multiplexed name whose comment disagrees
  with one of its C++ units; needs per-program manual verification, not auto-FAIL.

Current tree: **PASS, 0 fail, 45 warn** (49 GLSL samplers, 24 name-resolvable C++ binds, 243 `glUniform1i` / 228 `glActiveTexture` literal sites).

## Vulkan descriptor-contract cross-reference (SAMPLER-UNIT-MANIFEST-1)

`RenderCore/vulkan_contract.h` enumerates exactly **4** `CombinedImageSampler`
rows — the FBO-backed images the enum registry OWNS and that are sampled
downstream. Each GLSL sampler NAME that reads one of those owned resources is
tagged in the manifest (`samplers[name].resourceId`, and every `occupancy` row)
so a combined image-sampler's future `(set, binding)` descriptor grounds against
a KNOWN `RenderResourceId`, not just a bare unit literal:

| RenderResourceId | VkDescriptorClass | GLSL sampler names (this tree) |
|---|---|---|
| **MainColor** (scene HDR, sceneFBO attach0) | CombinedImageSampler | `sceneTex`, `u_sceneColor` |
| **MainDepth** (scene depth) | CombinedImageSampler | `sceneDepthTex`, `depthTex`, `u_sceneDepth`, `u_sceneDepthTex` |
| **MainNormal** (GBuffer1 normal attach1) | CombinedImageSampler | `sceneNormalTex`, `u_sceneNormalTex` |
| **ShadowDynamicMap** (dynamic shadow depth) | CombinedImageSampler | `dynamicShadowArray`, `dynamicShadowMap`, `dynamicFullMapShadow` |

**11 of 49** GLSL samplers link to an enum-owned resource. The other 38 are
tagged `descriptorClass: "CombinedImageSampler?"` with `resourceId: null` — they
WILL be combined image-samplers post-port, but the resource they sample (asset
atlases, splat-material normal arrays, PBR maps, building/decal textures) is not
yet enum-backed in the registry. That `?` suffix is the honest coverage marker:
the sampler descriptor dimension is *visible* for all 49, *grounded to a Vulkan
resource* for 11.

**Self-checking:** `scan_vulkan_cis()` re-parses the 4 CIS rows out of
`vulkan_contract.h` every run. If the map (`VULKAN_SAMPLER_RESOURCE`) references a
resourceId that is no longer a CombinedImageSampler row → **FAIL** (stale
cross-ref). If the header's CIS set changes membership → WARN (review the map for
new/removed sampled resources). This keeps the manifest↔contract link from
drifting silently, the same self-demoting discipline as the hard-anchor table.

## Known follow-ups (recorded, not fixed by this slice)

- **`building_pbr.frag` samplers** (`tex1`/`u_normalTex`/`u_ormTex`) have no
  discoverable binder — gated path (`MC2_BUILDING_PBR`, default-OFF). Confirm the
  binder TU or mark the path dead. (UNKNOWN row.)
- **`gos_terrain.frag:95` stale comment** — `u_transitionMaskArray` comment says
  "unit 4" but the chunk program binds unit 11 (`kChunkTexUnitTransitionMask`),
  indirect/mask bind unit 4. Comment is per-program-ambiguous; multiplexed, so WARN.
- **Duplicated unit constant** — `kTerrainMatNormalUnits[5]={5,6,7,8,12}` is hand-
  copied in both `gameos_graphics.cpp` and `gos_mech_batcher.cpp` (and `kChunkTexUnit*`
  re-declares 9/10/13/5 in `gos_terrain_lod_chunk.cpp`). Hand-lockstep surface —
  candidate for a single shared header constant.
- **2D_ARRAY tex-unit leak residual** (from GLSTATE-TEXUNIT-LEAK-GUARDS-1, AMD-tested-
  only) — the array-target samplers (`u_texArr`/`ormTexArr` 0/1, `matNormalArray`
  5/11/4, `dynamicShadowArray` 10, `mineSpriteArray` 5). Batchers carry save/restore
  epilogues; post-process shadow-array binds do NOT restore array units (only the 2D
  composite was fixed at `6de2cbb0`). The manifest records target per sampler so a
  future array-aware guard slice can key off it.
- **`gos_grass.geom` `tex1`** — DEAD shader (no loader; only GosVegetation cards live).

## Vulkan payoff

This manifest is the seed of the descriptor-set/binding table Vulkan demands
explicitly. Migrating the literal loc-cache binders (family 2) to named binds and
adding a generated `sampler_units.hglsl`/C++ constant pair (a future
SAMPLER-MANIFEST-GENERATE slice) would close the texture-unit dimension the same
way `view_uniforms.hglsl`/`material_gpu.hglsl` closed their buffer ABIs.
