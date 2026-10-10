<!-- doc-status: stale | reviewed:2026-10-11 -->
# Static-Prop Lighting Audit (V-LIGHTING-STATIC-0)

Read-only audit of the current `StaticPropOpaque` lighting model. Captures
where every lighting input comes from and what the shader does with it, so
future PBR / IBL / normal-map slices have a single starting reference.

Snapshot of HEAD `9a9d6eb0` on branch `claude/nifty-mendeleev`. This doc is
**descriptive, not authoritative** — every claim cites `file:line`. If a
cite drifts, re-derive from source, then update the cite here.

Scope: `StaticPropOpaque` and `StaticPropAlphaTest` lanes only. Mech, terrain,
shadow, water, VFX paths are out of scope. No shader behavior described here
was changed by this slice.

---

## 1. Overview

Today the static-prop opaque lane does **per-vertex Gouraud lighting** with
hot-color magic tag handling, then **modulates** the lit vertex color by an
albedo texture sample in the fragment shader. There is **no PBR, no
metallic/roughness, no normal map sampling, no IBL, no specular, no AO, no
tonemapping**.

Shading equation (effective, per-fragment):

```
litRgb       = v_argb.rgb      // per-vertex lit Gouraud value (VS output)
tex_color    = texture(...)    // albedo only (no normal/MR/emissive sampled)
c.rgb        = tex_color.rgb * litRgb
c.rgb       += v_highlight.rgb * v_highlight.a   // additive selection tint
c.rgb        = mix(v_fog.rgb, c.rgb, u_fogValue) // linear fog
FragColor    = c                                  // no gamma, no tonemap
```

Source: `shaders/static_prop.frag:190-201`.

---

## 2. Shader chain

| Stage     | File                                 | Includes                                           |
|-----------|--------------------------------------|----------------------------------------------------|
| Vertex    | `shaders/static_prop.vert`           | `shaders/include/lighting.hglsl` (line 21)         |
| Fragment  | `shaders/static_prop.frag`           | `shaders/include/render_contract.hglsl` (line 9)   |
|           |                                      | `shaders/include/material_gpu.hglsl` (line 59, coalesce only) |
| Transitive (via lighting.hglsl) | `shaders/include/lighting.hglsl` | `shaders/include/scene.hglsl` (line 1) |

`MC2_STATIC_PROP_LIGHTING` is defined in `static_prop.vert:20` before the
`lighting.hglsl` include; this gates the BGR→RGB swizzle in `get_base_light()`
(`lighting.hglsl:145-149`) and the `calc_light` zero-light early-return
(`lighting.hglsl:209-214`).

---

## 3. Inputs

### 3.1 ViewUniforms (UBO binding=3)

Consumed by `static_prop.vert` only when `MC2_USE_VIEW_UNIFORMS` is defined
(`static_prop.vert:69-73`); otherwise legacy `uniform mat4 u_worldToClipGL`
is used. The block layout is:

```glsl
layout(binding = 3, std140) uniform ViewUniformsBlock {
    mat4 u_worldToClipGL;
    mat4 u_worldToViewGL;
    vec4 u_cameraWorldPos;
};
```

Source: `shaders/include/view_uniforms.hglsl:13-27`. Binding is the canonical
`RenderCore::kViewUniformsBinding = 3` (`docs/render-binding-registry.md:45`,
`RenderCore/ViewUniforms.h`).

Only `u_worldToClipGL` is consumed by static_prop.vert today
(`static_prop.vert:150`). `u_worldToViewGL` and `u_cameraWorldPos` are
declared but unused on this lane.

### 3.2 LightsData (SSBO binding=20)

The directional + ambient + point/spot lighting source. Migrated UBO→SSBO
in `[LIGHTSSBO v1]` to lift the 64-slot ceiling
(`shaders/include/lighting.hglsl:43-56`).

```glsl
layout (binding = 20, std430) buffer LightsData
{
    ObjectLights light[];
};
```

`ObjectLights` schema (`lighting.hglsl:35-41`):

```glsl
struct ObjectLights {
    mat4 light_to_world[16];
    vec4 light_dir[16];     // .w = light type
    vec4 light_color[16];
    vec4 light_falloff[16]; // x=close, y=far, z=oneOver
    ivec4 numLights;
};
```

C++ mirror is `TG_HWLightsData` and must stay byte-for-byte lockstep
(`lighting.hglsl:30-34`). Per-instance index into the array comes from
`Instance::lightDataIndex` written by the C++ batcher
(`static_prop.vert:37-41`, `static_prop.vert:221`).

Six light types supported (`lighting.hglsl:17-22`):

| Type id | Name                          | GPU support in calc_light()                 |
|--------:|-------------------------------|---------------------------------------------|
| 0       | `TG_LIGHT_AMBIENT`            | accumulated into `ambient`                  |
| 1       | `TG_LIGHT_INFINITE`           | `clamp(dot(N, -L), 0, 1) * color`           |
| 2       | `TG_LIGHT_INFINITEWITHFALLOFF`| `INFINITE` × linear distance falloff        |
| 3       | `TG_LIGHT_POINT`              | dir = `lightPos - vertPos`, with falloff    |
| 4       | `TG_LIGHT_SPOT`               | uses `light_dir` as cone axis (CPU mirror caveat at `lighting.hglsl:256-262`) |
| 5       | `TG_LIGHT_TERRAIN`            | **GPU ignores** (CPU pre-bakes into listOfColors specular; spec R2, `lighting.hglsl:273-281`) |

### 3.3 MaterialGpu (SSBO binding=5, coalesce variant only)

Used in `static_prop.frag` to select the texture array layer for the albedo
sample. Default-ON behavior since `ae2152cd`
(`HANDOFF_2026_05_26_material_gpu_static_prop_complete.md`).

```glsl
layout(std430, binding = 5) readonly buffer MaterialTable {
    MaterialGpu materials[];
} materialTable_;
uniform int u_materialGpuSample;  // 0 = legacy texArrayLayer, 1 = material table
```

Source: `shaders/static_prop.frag:53-63`. Kill switch
`MC2_MATERIAL_GPU_SAMPLE=0` falls back to the per-draw `texArrayLayer`
(`docs/tier1_env_vars.md:86-92`, `GameOS/gameos/gos_static_prop_batcher_internal.h:386-391`).

MaterialGpu fields actually **consumed** by the .frag today:

| Field         | Used? | Cite                              |
|---------------|-------|-----------------------------------|
| `albedoTex`   | YES   | `static_prop.frag:126`            |
| `normalTex`   | NO    | (no normal-map sampling)          |
| `metallicRoughnessTex` | NO | (no PBR)                       |
| `emissiveTex` | NO    | (no emissive add)                 |
| `flags`       | NO (in .frag) — alpha-test bit comes from per-draw `materialFlags`, not `MaterialGpu::flags`. `materialFlags` is filled from a parallel CPU computation. |   | |
| `baseColorFactor` | NO |                                |
| `metallicFactor`  | NO |                                |
| `roughnessFactor` | NO |                                |

Effective use of MaterialGpu today is **albedo layer selection only**. The
inspector's "Material (RW handle lookup)" panel already displays the full
`MaterialGpu` record so the absent vs present state of `normalTex` /
`metallicRoughnessTex` / `emissiveTex` is visible
(`GuiRuntime/EditorInspector.cpp:1043-1062`).

### 3.4 Vertex attributes (`static_prop.vert:23-30`)

| Loc | Name           | Type | Notes                                          |
|----:|----------------|------|------------------------------------------------|
| 0   | `a_position`   | vec3 | model-space, Stuff axis (.x=left, .y=elev, .z=fwd) |
| 1   | `a_normal`     | vec3 | model-space; transformed by `mat3(M)` row-vector form (`static_prop.vert:216`) |
| 2   | `a_uv`         | vec2 |                                                |
| 3   | `a_localVertexID` | uint | debug + diagnostics                          |
| 4   | `a_aRGBLight`  | uint | per-vertex hot-color tag (B,G,R,A bytes)        |

### 3.5 Per-instance and per-type SSBOs

`static_prop.vert:55-57`:

- `binding=0` Instances (model matrix, typeID, flags, highlight, fog,
  `lightDataIndex`)
- `binding=1` Colors (legacy debug input — RAlt+9 mode 4 still reads it)
- `binding=2` PerType (hot-pink/yellow/green magic colors per type)
- `binding=3` ParityOut (write-only diag buffer; opt-in via env)

These are inputs to lighting math but are not "light sources" — they are
data plumbing.

---

## 4. Lighting math (verbatim quotes)

### 4.1 Base light decode (vertex shader)

`static_prop.vert:194-201`:

```glsl
const uint kFlagIsLightsOut = (1u << 0);
bool lightsOut = (inst.flags & kFlagIsLightsOut) != 0u;
vec3 base_light = get_base_light(
    perVertexARGB,
    false, 0.0, false, lightsOut,
    ptd.hotPinkRGB.rgb,
    ptd.hotYellowRGB.rgb,
    ptd.hotGreenRGB.rgb);
```

`isNight` and `nightFactor` are **stubbed to false / 0.0** pending eye-state
UBO wiring (`static_prop.vert:172-175`). Hot-pink "lit window" magic
(`lighting.hglsl:80-94`) therefore **paints dark grey 0x2F2F2F** instead of
glowing at night today.

### 4.2 Window node short-circuit

`static_prop.vert:239-246`:

```glsl
const uint kFlagIsWindow = (1u << 1);
vec3 lit;
if ((inst.flags & kFlagIsWindow) != 0u) {
    lit = base_light;          // hot-color magic only, no sun/ambient
} else {
    lit = calc_light(int(inst.lightDataIndex), worldNormal, worldPos, base_light);
}
```

### 4.3 Highlight add + alpha pin

`static_prop.vert:257`:

```glsl
lit = clamp(lit + inst.aRGBHighlight.rgb, 0.0, 1.0);
```

`static_prop.vert:264`:

```glsl
v_argb = vec4(lit, 1.0);
```

Alpha is **always** 1.0 — the raw `a_aRGBLight` alpha byte is never
propagated; CPU `mclib/tgl_transform.cpp:479` hardcodes the same.

### 4.4 Fragment composition

`static_prop.frag:190-201`:

```glsl
vec3 litRgb = v_argb.rgb;
if ((materialFlags & ALPHA_TEST_BIT) != 0) {
    // Tree cards / leaves cap dark side at ~50% for readability.
    litRgb = max(litRgb, vec3(0.5));
}
vec4 c = tex_color * vec4(litRgb, v_argb.a);
c.rgb += v_highlight.rgb * v_highlight.a;
c.rgb = mix(v_fog.rgb, c.rgb, u_fogValue);
FragColor = c;
```

Note: `tex_color * litRgb` is a **straight modulate** — no separation between
diffuse and specular, no energy conservation, no PBR. The alpha-test
50%-floor at `static_prop.frag:193-194` is a tree-card readability hack, not
a lighting model decision.

`GBuffer1` write `static_prop.frag:202`:

```glsl
GBuffer1 = rc_gbuffer1_screenShadowEligible(normalize(v_normal));
```

— normal × 0.5 + 0.5 packed; `.a=0` signals screen-space post-shadow
applies (`render_contract.hglsl:27-29`). Post-process shadow modulates the
output in a later pass; static_prop.frag itself does **not** sample a shadow
map or apply self-shadow.

### 4.5 Tonemap / sRGB

**None.** `static_prop.frag` writes linear lit-modulate values directly to
`FragColor` (`static_prop.frag:201`). Whatever gamma response the user sees
comes from the swapchain/display path, not from this shader.

---

## 5. What is NOT done today

The following are absent on the StaticPropOpaque lane:

- **Normal maps** — `MaterialGpu::normalTex` is declared and uploaded but
  never sampled in `static_prop.frag`. Normal is purely vertex-interpolated
  (`static_prop.vert:216`, `static_prop.frag:21`).
- **PBR (metallic/roughness)** — `metallicRoughnessTex`, `metallicFactor`,
  `roughnessFactor` are declared and uploaded but never sampled
  (`material_gpu.hglsl:46-55`).
- **Emissive** — `emissiveTex` not sampled.
- **IBL (image-based lighting)** — no environment cube sample anywhere on
  this lane.
- **Specular** — `TG_LIGHT_TERRAIN` specular pre-bake is **ignored** on GPU
  (`lighting.hglsl:273-281`); no other specular term is computed.
- **AO (ambient occlusion)** — no AO term, no SSAO sample.
- **Tonemapping / sRGB encode** — output is whatever the modulate produces.
- **Self-shadow inside the .frag** — relies on the post-process pass via
  `GBuffer1.a=0` contract.
- **Day/night `isNight`/`nightFactor` wiring** — stubbed to false/0
  (`static_prop.vert:172-175`). Lit-window hot-color magic therefore goes
  dark instead of glowing.

---

## 6. Track V leverage points (where the smallest safe next step lives)

In ascending order of risk/effort:

1. **Inspector visibility additions** (this slice). Read-only labels so the
   user can confirm at-a-glance what lighting model is in force without
   reading the shader.
2. **Wire eye-state `isNight` / `nightFactor`** — small SSBO/UBO field add,
   unblocks lit-window glow without touching the lighting equation.
3. **Sample `MaterialGpu::emissiveTex` when present** — additive contribution
   only, gated by `kMatFlagEmissive`. Safer than normal maps because it
   doesn't perturb the existing lit-modulate.
4. **Normal map sampling** — requires TBN basis at the vertex (not currently
   carried). Larger slice; defer.
5. **PBR migration** — full split of diffuse/specular, energy conservation,
   IBL probe. Multi-arc.

Each step above should land as a separate slice with its own baseline
capture (`scripts/capture_baseline.py`) and inspector-row visibility.

---

## 7. Cross-references

- Binding registry: `docs/render-binding-registry.md` (HEAD `4b6cf25a`).
- Render-pass contract: `docs/renderpass-contract-spec.md` (HEAD `2b5024c9`).
- MaterialGpu inventory inspector: V-MATERIAL-STATIC-0, HEAD `9a9d6eb0`.
- MaterialGpu default-ON ship: `HANDOFF_2026_05_26_material_gpu_static_prop_complete.md` (`ae2152cd`).
- LightsData SSBO migration: `[LIGHTSSBO v1]` in
  `GameOS/include/gameos.hpp:2823-2827`.
- ViewUniforms F1-3 default-ON: `HANDOFF_2026_05_27_f1_phase3_complete.md`.
- Render-contract GBuffer1 semantics: `docs/render-contract.md`.
