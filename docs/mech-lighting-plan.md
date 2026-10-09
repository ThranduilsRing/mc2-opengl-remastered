# Mech Lighting Plan — First Safe Visual Improvement (MECH-LIGHTING-PLAN-0)

Planning doc only. **No implementation in this slice.** Decides the
sequence and gate for the first gated mech visual improvement, grounded in
the Batch-1 recon (`docs/mech-rv-arc-recon.md`) and a fresh read of the live
shaders at HEAD `a4e76ce6`.

Companion to `docs/terrain-rv-arc-recon.md` / the StaticProp visual arc
(V-AMBIENT-STATIC-1 → V-IBL-STATIC-1 → V-MATERIAL-PBR-*), which are the
reference patterns adapted here.

---

## What the live mech path actually does today

**Lighting is NOT off — it is already a per-vertex model, default ON.**
`MC2_GPU_MECH_LIGHTING` is `envFlagDefaultOn` (`gos_mech_batcher.cpp:71`), so
`u_lightingMode=1` by default and `mech.vert` runs `calc_light()` per vertex
(`mech.vert:159-170`):

- `calc_light(inst.lightDataIndex, worldNormal, worldMC2, base=vec3(0.35))`
  — sun + per-actor cached `ObjectLights` (LightsData UBO, binding 0),
  over a **flat ambient floor of 0.35** (`mech.vert:149`).
- `lightsOut` actors (destroyed/disabled/shutdown, renderFlags bit 1) get the
  ambient floor only.
- Result forwarded as `v_litColor`; `u_lightingMode=0` is a flat-white
  passthrough bisect lever (not the default).

**`mech.frag`** (`mech.frag:52-87`): `tex_color = textureLod(u_tex, v_uv, 0)`
(AMD LOD-0 workaround), alpha-test, `c = tex_color * v_litColor`, add
highlight, mix per-actor fog. Writes FragColor + GBuffer1 (geometric world
normal for screen-shadow eligibility) + optional objectId. Plus 9 raw
`u_debugMode` branches (MECH-DEBUG-VIEWS-1 surfaces 4 as registry views).

**Data available to a lighting improvement:**

| Data | Available? | Source |
|---|---|---|
| Albedo | YES | live `u_tex` (per-draw 2D, not array) |
| Geometric normal (per-vertex) | YES | `worldNormal` from `a_normal` x boneT (`mech.vert:130`) |
| Team color | YES, but **CPU-baked into the albedo texture** at load (`mech3d.cpp:1725`, dominant-channel classifier) — invisible to the shader |
| Sun + per-actor lights | YES | `calc_light` / LightsData UBO |
| Normal **map** (tangent-space) | NO | `a_tangentOct` declared but zero-filled/unused; no normal-map sampler |
| Roughness / metallic | NO | none |
| Damage mask | NO | none in GPU path |
| MaterialGpu sampling | NO | SSBO uploaded (binding 2) but shader never reads it; identity/compare only |
| View transport | legacy `u_worldToClipGL` (NOT ViewUniforms UBO) |

---

## Required plan answers

**Should MECH-VIEWUNIFORMS-1 happen before any visual work?** **Yes.** Mechs
are the last main opaque lane still on the legacy `u_worldToClipGL` uniform;
static_prop already consumes the ViewUniforms UBO (binding 3, F1-3B). Putting
mechs on the canonical view transport first (a) aligns the lane with the
Track-R spine before we start touching the shader for visual reasons, and
(b) means the first *visual* slice edits a shader that is already on the
modern transport, avoiding a second disruptive reflect-golden churn. It is a
**no-visual-change** migration with a proven reference pattern, so it is the
safe first code step.

**Is current mech lighting per-vertex calc_light()?** Yes — default ON, over a
flat 0.35 ambient floor (see above).

**What does mech.frag do today?** Albedo × per-vertex lit color + highlight +
fog mix; GBuffer1 normal; objectId; 9 debug modes. No ambient/IBL/PBR math in
the fragment — all lighting is vertex-side.

**What data exists for lighting?** Albedo only (team color already baked into
it), per-vertex **geometric** normal (no normal map), sun + per-actor lights.
No roughness/metallic, no IBL coefficients wired for mechs.

**Is a hemisphere/IBL ambient term reasonable without normals?** A hemisphere
ambient term is reasonable **because a geometric normal IS available** — the
flat 0.35 floor can be replaced by a sky/ground hemisphere fill keyed on
`worldNormal.y` (up-ness). This adds soft directional ambient without needing
a normal map. Full SH-L2 IBL (as in V-IBL-STATIC-1) is **not** justified yet:
mechs have no per-mission SH set wired and the win over a simple hemisphere
term is small on flat-shaded, normal-map-less geometry. Start with hemisphere;
defer IBL.

**Should the first visual improvement be A/B/C/D?**
- **A. ViewUniforms migration** — do FIRST (no-visual prerequisite). = the
  recommended Batch-2 slice 3 (MECH-VIEWUNIFORMS-1-PRE).
- **B. Ambient/fill (hemisphere)** — the recommended first *visual* slice
  AFTER A, gated default-OFF. Replaces the flat 0.35 floor with a
  `mix(groundColor, skyColor, saturate(0.5+0.5*worldNormal.y)) * strength`
  fill term feeding `base` into `calc_light`. Uses only existing data.
- **C. Texture gamma/tint correction** — **rejected for now.** Team color is
  CPU-baked into the albedo; any global gamma/tint shifts every mech's paint
  and risks regressing the classifier's output. Touching it without a
  per-mech before/after capture matrix is a team-color-invention hazard.
- **D. Debug/capture only** — already delivered in Batch 1 + MECH-BASELINE-0;
  not a further slice.

**What gate/env name?**
- ViewUniforms: reuse the existing `MC2_VIEW_UNIFORMS` umbrella + a mech
  consumer that falls back to `u_worldToClipGL` when the UBO is absent
  (mirror static_prop's F1-3B fallback). No new default flip.
- First visual (hemisphere ambient): new gate `MC2_MECH_AMBIENT_V1`
  (default **OFF**) + `MC2_MECH_AMBIENT_V1_STRENGTH` (float), mirroring
  `MC2_STATIC_PROP_AMBIENT_V1`. Register both in `RendererFeatureRegistry.h`.

**What captures prove it?**
- ViewUniforms-pre: byte-identical proof — capture `mech_24`/`mech_17` before
  and after with the gate on; pixels must match (this is the static_prop
  F1-3C matrix-diff-probe discipline: `worldToClipGL` UBO bytes == legacy
  uniform). Tier1 5/5.
- Hemisphere ambient: `mech_24` + `mech_17` at gate OFF (baseline, =today) vs
  gate ON at a few strengths, via `capture_baseline.py --mech-debug-mode 0`
  (Final) and `--mech-debug-mode 3` (LightingOnly, isolates the lighting term
  from albedo). Sidecar already records the gate + mech inventory. Inspect
  shadowed/under-side faces (where the flat floor currently flattens detail).

**What is out of scope?**
- No MaterialGpu sampling for mechs (identity/compare only; needs a dedicated
  cook/array arc).
- No normal maps / tangent-space lighting (`a_tangentOct` stays zero-filled;
  no source data).
- No roughness/metallic/PBR specular (no data; would be inventing material
  params).
- No damage mask (no GPU data source).
- No team-mask shader path (team color is CPU-baked; do not invent a GPU mask).
- No mech shadow changes (separate lane).
- No gameplay/skinning/animation changes.
- No default flips in these slices.

---

## Recommended sequence

1. **MECH-VIEWUNIFORMS-1-PRE** (next; no-visual, gated/fallback) — mirror
   static_prop F1-3B. Reflect goldens regen required (Vulkan SDK dependency —
   see risk below).
2. **MECH-AMBIENT-1** (first visual; `MC2_MECH_AMBIENT_V1` reader default is ON) — only
   after capture matrix from step 1 confirms the migration is byte-identical,
   and only with separate visual approval.
3. Defer: MECH-IBL-1, any PBR/normal/damage work (no data source).

**Risk on step 1:** it is a real shader edit → `tools/shader_reflect` golden
drift. The Vulkan SDK (glslangValidator + spirv-cross) must be present to
regen goldens; memory notes it has been absent in some sessions
(`HANDOFF_2026_05_27_f1_3a` "shader_reflect Vulkan SDK absent"). Confirm SDK
availability before starting, else SHADER-REFLECT-HYGIENE will block.

**Verdict:** mechs are NOT ready for a visual lighting change in this batch.
The data supports exactly one conservative first visual slice (gated
hemisphere ambient), and it should land only AFTER the ViewUniforms migration
and a before/after capture matrix. The ViewUniforms-pre slice itself is safe
and is the correct Batch-2 slice 3.
