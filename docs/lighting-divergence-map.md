> Native checker update (window 62 fixups): `inline_light_floor` pins an empty
> baseline of hardcoded numeric inline floors in the live Vulkan shader sweep.
> Adding `mix(0.25, 1.0, shadow)` or `(0.50 + 0.50 * NdotL)` fails the contract.
> Calls through `sunShadowFloor(...)` are shared helper policy, outside this
> deliberately narrow numeric-floor pattern. Historical GL rows below are archival.

<!-- doc-status: current | reviewed:2026-10-11 -->
# Lighting divergence map (ARCH-CONVERGE-S2)

> s68/P9: Retired GL sources are cited with Git object syntax (`8064c6930:path`) so the historical implementation remains inspectable.

Every cell below was read from the shader source or the C++ reader itself, at
the commit this file was written against. Nothing here is taken from the
feature registry, from `docs/tier1_env_vars.md`, or from a code comment
describing another file. Cells that could not be determined say **UNKNOWN**.

Pinned by `scripts/check-lighting-divergence.py` (contract row
`lighting_divergence`). If a row here changes, that check fails.

The GL-only rows below are historical snapshots after s68. All GL stage entry points are deleted. The current pin covers native scalar/fill floors, actual native light-kernel calls and the explicit zero-light return; retired GL rows were removed from the live baseline. Current water rendering uses `shaders/vulkan/water_geom.frag`; the deleted GL shader is named without a live-path claim.

---

## 1. The shape of the problem

There is no shading model. There is a shared *kernel* — `calc_light()` in
`shaders/include/lighting.hglsl:228-308`, a faithful 6-type GLSL port of the
CPU lighting in `mclib/tgl.cpp` — and then there is what each class actually
does, which ranges from "calls it" to "includes the header and then hand-rolls
a different loop three functions later".

Concretely, the live tree carries:

| thing | how many distinct answers |
|---|---|
| named scalar ambient floor | **5** (`0.35` ×3, `0.18` ×2) |
| hemisphere sky/ground fill pair | **3** (terrain, GL prop, VK prop) |
| unnamed inline floor (`mix(x,1,·)`, `(x + y··)`) | **5 distinct values** across 10 sites |
| definition of `calc_light` | **2** (GL header; VK mech re-port — forced by the VK toolchain) |
| light-type dispatch table | **2** (shared 6-type; prop 3-type, used identically by GL *and* VK props) |
| answer to "no lights on this actor" | **2** (`vec3(1,1,1)` vs `base_light`) |
| shadow-receive mechanism | **2** (in-shader `calcShadow`; deferred GBuffer1 tag) |
| fog mechanism | **4** (per-actor haze; `apply_fog` distance; `FogValue` varying; compute-baked `fogRGB`) |

---

## 2. The map

Abbreviations: **SSE** = `rc_gbuffer1_screenShadowEligible` (deferred
screen-space shadow pass applies it later); **SH** = `rc_gbuffer1_shadowHandled*`
(opts OUT of the deferred pass); **in-shader** = calls `calcShadow()` from
`36544d925:shaders/include/shadow.hglsl` directly.

### 2a. Object classes

| Class | Shader | Ambient floor | Output clamp | Light types | Night | Fog | Shadow receive | Light data fed from |
|---|---|---|---|---|---|---|---|---|
| **Mech (GL)** | `8064c6930:shaders/mech.vert` / `mech.frag` | `kAmbientFloor = 0.35`, flat, seeds `calc_light`'s `base_light` (`mech.vert:204,218`) | `clamp(litRGB + highlight, 0, 1)` (`mech.vert:223`) | full 6-type via shared kernel (`mech.vert:219` → `lighting.hglsl:228`) | via `lightsOut` render-flag bit 1 → floor only (`mech.vert:210-216`) | per-actor haze `mix(c, v_fogRGB.rgb, v_fogRGB.a)` (`mech.frag:404`) | **SSE** (`mech.frag:428`) | `mechShape->CacheGpuLightData()` (`mclib/mech3d_update.cpp:441`) → `inst.lightDataIndex` |
| **Mech (VK tier1)** — INTENTIONAL parity port (VK-PARITY-MECH-1) | `shaders/vulkan/mech_tier1.vert` / `.frag` | `kAmbientFloor = 0.35` (`:166`); per-fragment hemisphere fill + Blinn sun sheen + imported gamma/AO/normal map from the SHARED include `shaders/include/mech_shading.hglsl` (same body as GL `mech.frag`), knobs from the same producer `gpuMechShadingParams()` | same shape (`:173`) | full 6-type — but from its **own** `calc_light` definition (`:95`), a verbatim re-port; object night light pools via the SHARED `shaders/include/object_light_pool.hglsl` (pool array = CPU copy of GL binding 28, VK set0 binding 5) | as GL | per-actor haze, as GL | **SSE** (a=0, `Nshade`) | same ObjectLights row, copied into a VK SSBO |
| **Ground vehicle** | `8064c6930:shaders/gos_tex_vertex_lighted.vert` / `.frag` | `max(base_light, vec3(0.35))` — a true **floor**, not a replacement (`:169-172`), gated `MC2_VEHICLE_AMBIENT_FLOOR` **default-ON** (`mclib/txmmgr_render_phases.cpp:185-190`) | `clamp(lit, 0, 1)` under the same gate (`:202`) | full 6-type via shared kernel (`:191`/`:193`) | inherits `get_base_light` with night args **stubbed to `false,0.0`** (`:162`) | `apply_fog(c, WorldPos, CameraPos)` (`gos_tex_vertex_lighted.frag:78`) | **SSE** (`.frag:84`) | `gvShape->CacheGpuLightDataAllLeaves()` (`mclib/gvactor_render.cpp:128`) → `light_offset_.x` |
| **Static prop (GL)** — buildings, trees, generic | `8064c6930:shaders/static_prop.vert` / `static_prop.frag` | **no scalar floor.** Its floor is `ambientTerm` (sum of `TG_LIGHT_AMBIENT` rows, `:722-723`) + optional hemisphere `kAmbientV1Sky/Ground` (`:833-834`, strength default **0**) + optional SH-IBL (`:845`) | `clamp(lit + highlight, 0, 1)` (`:912`) | **3 types only**: AMBIENT + first INFINITE/INFINITEWITHFALLOFF. Point/spot deliberately omitted (`:713`). Sun **direction** is not the light's — it is `u_terrainSunMC2` (`:701-712,732`) | real: `u_isNight`/`u_nightFactor` drive the hot-colour emission delta (`:591-607`) | `mix(v_fog.rgb, c.rgb, u_fogValue)` (`static_prop.frag:611`) | **SSE** (+ `_noSelfShadow`, `_alphaTest` variants) | `GatherGpuObjectLightDataOnly()` / `mc2CacheOrBakeStaticGpuLight` (`GameOS/gameos/gos_static_prop_batcher_submit.cpp:261`, `mclib/bdactor_lightbake.cpp:186`) |
| **Static prop (VK)** | `shaders/vulkan/color_prop_tier1.vert` | real `ambientTerm` scan + SH-IBL when `lightingReady` (`:277-291`); own hemisphere pair (plus, since 2026-10-14, a verbatim port of GL's `kAmbientV1Sky/Ground` fill under the StaticPropAmbient option: INTENTIONAL, parity row 22) `kAmbientSky = (0.00,0.02,0.08)` / `kAmbientGround = (0.14,0.13,0.11)` (`:245-246`) is the **fail-soft path only** (`:259-261`) | `clamp(..., 0, 1)` (`:315`) | AMBIENT + INFINITE + INFINITEWITHFALLOFF — **same matrix as GL props**, but reached via bare integers `lt == 1 \|\| lt == 2` (`:284`) | real `uFrame.isNight/nightFactor` and the per-type emission table (set 0 binding 7), as GL (`:104-192`) | UNKNOWN | UNKNOWN | ObjectLights row, `lightingReady` flag (`:32`, `:277`) |

The live guard also pins the existing `moverShadow` floors in `screen_shadow_csm.frag`: 0.4 for props and 0.55 for mechs.

NIGHT-CONSISTENCY (INTENTIONAL, 2026-10-23): every VK sun-shadow floor (ground static and mover, object static and mover, water static and mover) now goes through `sunShadowFloor(dayFloor)` in `shaders/vulkan/include/mover_shadow.glsl`, which rises toward 1.0 by the night fade `moverFrame.light.x` (0.85 x `mc2NightLightFactor()`), so the inline-floor scanner sees only the GL `shadow.hglsl` sites. The day floors (0.4, 0.55 mechs) are unchanged.

The Vulkan terrain path uses mission environment ambient plus sun and shared shadows.
The legacy ambient floor and pad-glow strength knobs were retired after their VK shader terms were removed.

### 2b. World classes

| Class | Shader | Ambient floor | Output clamp | Light types | Night | Fog | Shadow receive | Light data fed from |
|---|---|---|---|---|---|---|---|---|
| **Terrain (LOD chunk)** — default-ON (`mclib/terrain.cpp:144-156`) | `8064c6930:shaders/terrain_lod_chunk.vert` / `.frag` | `u_terrainAmbientFloor`, default **0.35**, bounded [0,2] (`GameOS/gameos/gos_terrain_lod_chunk.cpp:2883-2890`). **Albedo-scaled**: `baseColor * ambCol * floor * ambShadow * dayGate * (1-flatLitShare)` (`.frag:2210`) | none on the ambient term | terrain light pipeline (`gos_terrain_lighting.comp`), **not** ObjectLights | gated OFF at night by `u_terrainAmbientDayGate` from authored sun pitch (`.frag:2193-2198`) | compute-baked per-vertex `fogRGB` (`shaders/include/terrain_lighting_shared.hglsl:23`, `gos_terrain_lighting.comp:266`) | **in-shader** `calcShadow` (`.frag:1960`) + **SH** tag | `GpuTerrainLight[]` SSBO + `Camera::getLightRed/Green/Blue` (`mclib/camera.cpp:4392-4446`) |
| **Terrain (VK tier1)** | `shaders/vulkan/terrain_tier1.frag`, shared `include/ground_lighting.glsl` | mission environment ambient only; legacy floor retired | none | shared terrain light pools | mission environment | baked colormap | static/dynamic sun shadows | `TerrainFrameUBO`; retired floor slots are zero |
| **Terrain (legacy)** — `MC2_TERRAIN_LOD_CHUNK=0`, vestigial | `8064c6930:shaders/gos_terrain.frag` | **no ambient term** | none | as above | UNKNOWN | as above | **in-shader** `calcShadow` (`:958`) | as above |
| **Terrain overlay / decals** | `terrain_overlay.frag`, `decal.frag` | none (overlay and decal) | none | terrain sun dir only | UNKNOWN | UNKNOWN | **in-shader** `calcShadow` (`terrain_overlay.frag:388`, `decal.frag:82`) + **SH** | `terrainLightDir` uniform |
| **Water** | `gos_terrain_water_mdi.frag` (former GL source, removed in s63) | `SKY_AMBIENT = 0.18`, applied **twice**: as a floor `max(vertexLight, 0.18)` and again additively `+ waterCol*0.18*0.5` (`:101,274-275`) | none | none — consumes the terrain vertex light | UNKNOWN | `mix(fog_color.rgb, col, FogValue)` (`:395`) | **SSE** | terrain vertex light packed in `Color.bgra` (`:273`) |
| **Vegetation cards** | `gos_vegetation_card.vert` / `.frag` | inline `(0.60 + 0.38 * NdotL)` (`.frag:70`) — a 0.60 diffuse floor | `min(vec3(1.0), col)` (`.frag:75`) | one hardcoded up-normal `dot(vec3(0,0,1), u_terrainLightDir)` (`.frag:68`) | none | none | **SH** — opts out (`.frag:79`) | `u_terrainLightDir` uniform only |
| **Native vegetation cards** | `shaders/vulkan/vfx_vegetation.frag` | shared `include/ground_lighting.glsl` compose with terrain: sun band, hemisphere ambient, night pools, pad glow and mover contact | shared post exposure | macro root normal bent toward card tip; bounded 0.08 transmission | terrain night gate | mission macro/cement tint | root static + existing mover receiver, min visibility applied once; GBuffer alpha 1 prevents a second screen-shadow multiply | **INTENTIONAL P3**: replaces the fixed diffuse floor/desaturation/root lift; consumes existing resources, no new light system. Greenfield mask/MoverShadowFrame remain shadow-lane dependencies. |

### 2c. Unlit classes (verified genuinely unlit, not "lighting UNKNOWN")

| Class | Shader | Evidence |
|---|---|---|
| **VFX mesh** | `8064c6930:shaders/vfx_mesh.vert` | 40-line file, read in full: position, UV, `u_instanceColor`. No light input of any kind. |
| **Particles** | `particle_billboard.vert` + `include/particles.hglsl` | zero matches for `light`/`ambient` in the include. |
| **HUD / 2D** | `gos_vertex.vert`, `gos_text.vert`, `gos_tex_vertex.vert` | pass-through vertex colour. |
| **`gos_vertex_lighted.vert`** | — | **naming trap**: despite the name, the 22-line file does *no* lighting — it passes `color` straight through. Only `gos_TEX_vertex_lighted` is really lit. |

---

## 3. INTENTIONAL vs ACCIDENTAL

The question that decides whether to spend money.

### INTENTIONAL — a real difference in what the class needs

| # | Divergence | Evidence it was chosen |
|---|---|---|
| 1 | **Static props omit point/spot lights.** | Stated at the site, with a reason: `static_prop.vert:713` — *"Point/spot lights are omitted (negligible for buildings); noted."* Buildings are large, static, and mostly lit by the sun + a per-actor ambient; per-vertex point falloff on a 4-vertex wall face is close to a no-op. Reinforced by `[T3.2]` at `:677-684`, where spotlight *cone billboards* were deliberately deleted in favour of real `TG_Light` registrations. |
| 2 | **The 0.35 floor is duplicated as a literal rather than hoisted into `lighting.hglsl`.** | Intentional — a decision was made and written down at `gos_tex_vertex_lighted.vert:165-168`. **But its stated reason does not survive checking, and this is the one place in this document where a defended divergence has a bad defence.** The comment says `lighting.hglsl` is "included by shaders that must NOT get a floor (terrain, particles)". It is not: the header's nine includers are `mech.vert`, `static_prop.vert`, `gos_tex_vertex_lighted.{vert,frag}`, `building_pbr.{vert,frag}`, `lightgrid_build.comp`, and the two VK object shaders — **no terrain shader and no particle shader includes it.** Independently, a `const float` in a header applies a floor to nobody; only a shader that *writes* the identifier gets one. So the blast-radius argument is wrong twice over. See §4.1 Move 1 — the real argument against hoisting is coupling, not include scope. |
| 3 | **Vehicles use `max(base_light, 0.35)` where mechs use a bare `vec3(0.35)`.** | `gos_tex_vertex_lighted.vert:97-100`: mechs have no per-vertex seed to lose; vehicles do (`get_base_light` implements the hot-colour window conventions), so `max()` is a floor that can only raise. Backed by a measured census of all 8 stock vehicle `.tgl` binaries at `:102-117`. |
| 4 | **Terrain's ambient is albedo-scaled and day-gated; the object floors are flat and unconditional.** | `terrain_lod_chunk.frag:2166-2184` rejects the flat form by name and by measurement (a flat add inflated the user-ratified concrete pads +21-26%). The day gate exists because every night mission in shipping data has an authored sun pitch that zeroes it (`:2193-2198`, proved offline by `tools/terrain_daylight_harness`). Terrain also genuinely differs: its albedo already carries baked mission light, objects' does not. |
| 5 | **Static props use `u_terrainSunMC2` instead of the ObjectLights sun direction.** | `static_prop.vert:701-703`: `calc_light`'s frozen `worldLights[0]` sun was ~90° off for this path. Substituting the terrain sun is a fix, not drift. |
| 6 | **Legacy vegetation and grass use crude floors.** Native P3 cards receive root-static and mover shadows in the shared ground compose and mark shadow handled to prevent a second deferred multiply. | Cards are two-sided billboards with no meaningful normal; `gos_vegetation_card.frag:79` sets `shadowHandled` on purpose. A physically-correct model on a crossed quad produces the "dark face rotates with the camera" artefact that `static_prop.vert:686-689` documents and works around with `abs(NdotL)`. |
| 7 | **Legacy terrain (`gos_terrain.frag`) never got the ambient floor.** | `mclib/terrain.cpp:150-152` prints `[8Z_VESTIGIAL]` when the opt-out is taken and states the chunk-OFF path "has no production renderer". Not shipping the fix to a path with no renderer is a choice, not an omission. |

### ACCIDENTAL — drift nobody chose

Only three survived adversarial review. Five candidates were refuted — see below;
that section is the more useful half of this document.

| # | Divergence | Evidence it is drift |
|---|---|---|
| C | **`building_pbr.frag` lights its specular from a hardcoded direction** `normalize(vec3(-0.35, 0.65, 0.55))` (`:81`). | No comment defends it; the same file correctly consumes the real ObjectLights sun for its diffuse term via `VertexLight`. A fixed light vector in a game with authored per-mission sun pitch is placeholder code. Contrast `vulkan/color_prop_tier1.vert:267-270`, where a hardcoded `kSunColor` in the *same* role was found by measurement and removed — so this pattern is known to this codebase as a defect. |
| D | **`building_pbr.vert:64` still feeds `calc_light` STUFF-frame position and normal.** | This is the *exact* bug `VEHICLE-NIGHT-LIT-1` fixed, and the fix's own comment (`gos_tex_vertex_lighted.vert:36-58`) enumerates the consumers it audited: `mech.vert`, `static_prop.vert`, and this shader. `building_pbr.vert` is a near-copy of `gos_tex_vertex_lighted.vert` and was missed. Gated OFF by default, so it ships no pixels — but it is a latent copy of a fixed bug. |
| H | **`gos_vertex_lighted.vert` is named "lighted" and does no lighting.** | Pure naming drift. Cheap to fix, zero risk, but it is exactly the kind of thing that makes the next reader's mental model wrong. |

### REFUTED on review — candidates that looked accidental and are not

This section exists because five of my eight initial ACCIDENTAL calls were
wrong, and two of them were wrong because **my own checker misread the code**.

- **A. "VK static props use different hemisphere ambient values from GL."**
  REFUTED — INTENTIONAL, and measured. `vulkan/color_prop_tier1.vert:240-244`
  records `VK-FLIP-FIELD-REPORT-2 WASHOUT FIX`: round-1 tuning measured
  **+45/255 mean over GL** on the mc2_01 hangar roof at the `airport_bldgs_mid`
  oracle pose by byte-for-byte pixel compare, and the sky tint was reduced by
  exactly that delta while the ground tint was left alone because shadowed faces
  were *not* over-lit in the same measurement. The values differ from GL's
  because this hemisphere is a **stand-in for three terms the VK island cannot
  feed**, not a copy of GL's hemisphere — and `:259-261` shows it is now only the
  fail-soft path when `lightingReady == 0`. My "the comment and the code
  disagree" reading was wrong: the comment describes the *starting point*, the
  constants record the *measured endpoint*.

- **B. "VK static props dispatch AMBIENT only."** REFUTED — **my checker was
  wrong.** `vulkan/color_prop_tier1.vert:284` dispatches INFINITE and
  INFINITEWITHFALLOFF as bare integers (`lt == 1 || lt == 2`) and names only
  `TG_LIGHT_AMBIENT`. A named-constants-only scan reports "AMBIENT only", which
  is false. VK and GL static props have the **same** light-type matrix. Fixed in
  the checker (`RE_LIGHT_TYPE_INT`) before pinning, so the false row was never
  baselined.

- **E. "`shaders/vulkan/mech_tier1.vert:75` re-defines `calc_light` instead of including
  the shared header."** REFUTED as *drift*, retained as a *risk*. The GL include
  mechanism (`#include <include/...>`) is resolved by the GL shader loader; the
  Vulkan pipeline compiles through a different toolchain. This is a forced
  duplication, not a careless one. It stays worth knowing about — `lighting.hglsl:209-220`
  already has to warn three files not to double-correct the same decode — but
  "someone should have used the shared header" is not available as a fix.

- **F. "Water applies `SKY_AMBIENT` twice."** REFUTED as accidental. Reading the
  expression rather than the count: `waterCol * max(vertexLight, 0.18)` is a
  *floor on the incoming vertex light*, and `+ waterCol * 0.18 * 0.5` is a
  separate *additive sky term*. Those are two different physical roles that
  happen to share a constant. Sharing the constant is arguably sloppy; applying
  both is not a mistake.

- **G. "Terrain's shadow floor is 0.55 where everything else is 0.4."** REFUTED.
  `terrain_lod_chunk.frag:2207-2209` gives the reason at the site — ambient is
  only *partly* occluded by the sun-shadow term because a shadowed patch still
  sees sky. That is a correct statement about ambient vs direct occlusion, and
  the 0.4 sites are attenuating a different quantity. Different numbers for
  different terms is not divergence.

- **`gos_tex_vertex_lighted.frag:60` calls `calc_light` a second time on the
  un-fixed Stuff frame.** REFUTED — I first read this as a live double-apply. The
  call sits in the `#else` of `ENABLE_VERTEX_LIGHTING`, which `lighting.hglsl:8`
  hard-defines to `1`. **Dead code, not a bug.** Pinned anyway, because reviving
  it would be a silent regression.

- **`mech.vert` dispatching only 2 light types** looked like a narrowed matrix.
  It is not: that loop (`:283-292`) is the *specular sun-finder*; mech lighting
  comes from the shared 6-type kernel one call earlier.

- **`mech.vert` defining `MC2_STATIC_PROP_LIGHTING`** looked like copy-paste. The
  comment at `:9-14` shows it is deliberate and reasoned — it selects
  `calc_light`'s no-lights early-return, and the macro's *other* effect (a BGR
  swizzle) is inert here because `mech.vert` never calls `get_base_light`. Bad
  name, correct behaviour.

---

## 4. Recommendation

### 4.1 Lighting: **partially centralise — two narrow moves, and explicitly not a unified shading model.**

**Do not build the UE5 answer.** UE5 has one shading model with per-material
overrides because its inputs are uniform: every surface has a normal, a
material, and a scene light set, and lighting is deferred to screen space where
class no longer exists. MC2 fails that premise in ways that are not fixable
without replacing the renderer:

- MC2's lighting is **per-vertex on the CPU's data model** — `TG_HWLightsData`
  rows are gathered per *actor*, not per pixel, and the light budget is 32 per
  object (`lighting.hglsl:33`) with a 5-way ABI lockstep guarding it. A unified
  model wants per-pixel light lists; this engine has per-actor light rows.
- Terrain's albedo **already contains baked mission light**
  (`terrain_lod_chunk.frag:2150-2153`). It is not albedo in the UE5 sense. Any
  unified `baseColor * irradiance` model double-counts it on terrain and
  under-counts it everywhere else — this is precisely why the terrain floor had
  to be albedo-scaled and the object floors flat.
- Two classes (vegetation, grass) are billboards with **no meaningful normal**.
  A unified model has nothing correct to feed them.
- A previous **StaticProp↔Mech reorder attempt PARITY-FAILED** and is now
  permanently gated OFF. That is direct evidence that this codebase's classes
  are not as interchangeable as they look.

So: the honest answer to *"is centralising even worse than what we have?"* is
**yes for the shading model, no for two specific things**.

**Move 1 — centralise the CONSTANTS, not the code.** Put the genuinely shared
numbers — the `0.35` object floor (3 sites) and the `0.4` shadow floor (5 sites)
— in a single header of `#define`s that each shader `#include`s and *uses where
it already has a floor*. No shader changes which terms it computes; no shader
gains or loses a floor. This is a pure textual dedup and a **byte-identical
no-op** at every one of the 8 sites.

Deliberately **not** in scope: the hemisphere sky/ground pairs. Adversarial
review established that GL and VK props differ there for a measured reason
(refuted divergence A), and terrain's differ from both because terrain albedo is
pre-burned. Those are four different quantities that happen to share a shape.
Hoisting them would be exactly the cargo-culting this lane was asked to avoid.

The counter-argument on record is **factually wrong**, and should not be what
decides this either way. `gos_tex_vertex_lighted.vert:165-168` refused to hoist
because `lighting.hglsl` is "included by shaders that must NOT get a floor
(terrain, particles)". Checked against the live include graph: the header's nine
includers are the two mech shaders, the two prop shaders, the two vehicle
shaders, the two `building_pbr` shaders, and `lightgrid_build.comp` — **no
terrain shader and no particle shader includes it.** Independently, a `#define`
or `const float` in a header hands a floor to nobody; only a shader that *writes*
the identifier gets one. Both halves of the stated reason fail.

That makes Move 1 more *available* than the tree's own comment suggests. It does
not make it more *valuable* — the real objection is coupling, immediately below.

**Move 2 — finish or delete `building_pbr.*`** (divergences C and D). It is the
one genuinely unfinished lighting path left: a hardcoded specular light vector
and a copy of a frame bug that was fixed everywhere else. It is default-OFF
(`mclib/bdactor.cpp:242-243`), so the cheap option — delete it, or gate it
loudly as experimental — costs zero pixels. Leaving a half-built lighting path
in-tree is what generates the next false "buildings are unlit" report.

**Do NOT do:** unify `static_prop.vert`'s hand-rolled loop back into
`calc_light`. It hand-rolls precisely to substitute `u_terrainSunMC2` for a
known-bad light direction (divergence 5). Folding it back means either
re-introducing the bug or adding a `u_useTerrainSun` branch to the shared kernel
— which is a per-class override inside the shared path, i.e. the same divergence
with more indirection and a wider blast radius.

**Cost line — Move 1 (constants header):**
- Files: 1 new header + 8 shaders touched (`mech.vert`, `vulkan/mech_tier1.vert`,
  `gos_tex_vertex_lighted.vert`, `include/shadow.hglsl`, `shadow_screen.frag`,
  and the three VK shadow re-ports). No C++, no engine code.
- Visual-regression risk: **low.** Every site becomes textually different and
  numerically identical. The real risk is not pixels, it is the VK toolchain:
  `vulkan/mech_tier1.vert` cannot use the GL `#include` mechanism (that is *why*
  it re-defines `calc_light`), so it either gets the constants by a different
  route or stays out of scope — leaving 6 sites, not 8.
- Re-verify: tier1 5/5; GL byte-identity capture on mc2_01 (day) and mc2_10
  (night) — must be XOR=0 at every site, since a no-op that moves a pixel means
  a transcription error. Roughly half a session.
- **Honest value assessment: this is small, and it is the riskiest thing in
  this document.** It removes a duplicated literal. It does not remove a
  divergence — `check-lighting-divergence.py` already makes the duplication
  safe by failing when the copies drift apart.

  **The argument against my own recommendation**, which the reader should weigh
  before spending anything: a shared `#define` creates a coupling that does not
  exist today. Right now, an edit to `mech.vert`'s 0.35 moves mechs. After
  Move 1, an edit to `MC2_OBJECT_AMBIENT_FLOOR` moves mechs *and* vehicles *and*
  the VK mech path in one keystroke, and the person making it may only have been
  looking at one of them. Duplication is how these three classes stayed
  independently tunable through VEHICLE-AMBIENT-FLOOR-1, which needed the
  vehicle floor to be a `max()` where the mech floor is an assignment — a
  distinction a shared constant does not capture and would quietly invite
  someone to erase.

  The existing refusal to hoist (`gos_tex_vertex_lighted.vert:165-168`) was
  written by someone who had just been burned by this. If the budget is tight,
  **skip Move 1**: the check alone captures most of the benefit at none of the
  coupling cost.

The building-PBR shaders and their gate were retired in merge window 37.

### 4.2 Batching: **no. Leave it. Neither can absorb the other.**

Verified structure:

- **Props** — `GpuStaticPropRegistry` (`GameOS/gameos/gos_static_prop_registry.cpp`):
  *recipes* (frozen snapshots — `frozenRecordsArmed()` at `:858`, golden store
  rebuilt only when dirty at `:849-855`) plus *instances* injected via
  `submitCachedInstance()` (`:1303`), which deliberately does not tick the
  submission counters (`gos_static_prop_registry.h:184`). Population type is set
  once per recipe by `setRecipePopulation()` (`:1930`) and starts `0xFF` =
  unset (`:649`).
- **Mechs** — `GpuMechBatcher::submitActor()`
  (`GameOS/gameos/gos_mech_batcher_flush.cpp:78`), per-actor, per-frame, staging a
  bone palette and capturing a texture handle at submit time
  (`gos_mech_batcher.h:90,102`).

The registry's whole performance argument is that a prop's geometry and
transform are **immutable after registration**, so a frozen record set can be
built once and the GPU can own culling. A mech's bone palette changes every
frame by definition. Feeding mechs through the registry means invalidating the
frozen store every frame, which deletes the reason the registry exists. Feeding
props through `submitActor` means paying per-actor submission cost for ~10³
static objects that never move — the exact cost the registry was built to
remove.

They are not two implementations of one idea. They are the two correct answers
to "is this thing's transform static?".

**Cost line:** unification is **not recommended at any price**; the estimate is
offered only to size the "no". It would touch both batchers, the frozen-record
substrate, the light-index broadcast path, and the counter-honesty checks —
15-25 files, high visual-regression risk (every prop and every mech), and the
one prior attempt to reorder these two paths relative to each other
PARITY-FAILED and is permanently gated OFF. The only shared-infrastructure win
available is the *light row* plumbing, and that is already shared — both classes
land in the same `TG_HWLightsData` table via `addLightDataStructure`.

---

## 5. Honest limitations of this map

- The **Vulkan column is thin.** VK cells marked UNKNOWN were not read this lane.
  The VK backend lives on `claude/vk-interop-bridge-1`; only its shaders are in
  this tree.
- The checker pins **named constants and two inline forms**. Floors written in
  other shapes (e.g. a `clamp(x, 0.1, 1.0)` floor as the retired grass shader used, or a floor
  applied C++-side before upload) are **not** pinned. `check-lighting-divergence.py`
  will not catch those.
- The `no_lights_semantics_fork` category detects `#define
  MC2_STATIC_PROP_LIGHTING`. `shaders/vulkan/mech_tier1.vert:97` takes the same
  fork by **hardcoding** it (`if (ld.numLights.x == 0) return base_light;`) with
  no macro, so that shader's fork is invisible to the category even though it is
  correctly pinned via `calc_light_definition`. A third shader adopting the
  hardcoded form would not be caught.
- Adjacent-check gap, noted not fixed: `scripts/check-light-loop-numlights-bound.py`
  declares a 3-file scope (`lighting.hglsl`, `mech.vert`, `static_prop.vert`).
  `vulkan/mech_tier1.vert:100` and `vulkan/color_prop_tier1.vert:279` also loop
  per-light. Both are correctly bounded by `min(numLights.x, MAX)` **today**, so
  nothing is broken — but they are outside that tripwire's scope.
- Fog and night columns are less thoroughly sourced than ambient/clamp/light-type.
  Where they say UNKNOWN, that is literal.
- **This lane changed no pixels.** It produces a map, a guard and a
  recommendation. Nothing here is visible on a default launch.
P8 shadow consolidation (INTENTIONAL): the two ground receiver floors remain 0.4 in `ground_shadow.glsl`. The unified object receiver retains a 0.4 static/prop floor and a 0.55 mover floor for mechs. The scanner names these live sites, replacing the deleted cascade and static screen shader sites. Vegetation receives the same static mask and mover visibility with a 0.4 floor.

# R2 update, 2026-10-22

INTENTIONAL: native props use the mission environment SH once, with baked vertex
emission and the existing clamp retained. ObjectLights ambient and fixed prop
hemispheres no longer stack. Native mechs use a mission-colored hemisphere in
vertex lighting; both the constant vertex floor and fragment hemisphere are
removed. The shared light kernel's MC2_ENVIRONMENT_AMBIENT compile policy omits
ObjectLights ambient for that receiver. Legacy callers keep their existing policy.
Terrain burnin already contains ambient; its extra cool floor is removed.
Night SH is an analytic hemisphere from mission ambient at quarter strength,
not the DaySkyHDRI coefficients. The census now pins zero fixed fill colors;
its remaining scalar-floor witness is water's unchanged SKY_AMBIENT.

**2026-11-02 (light-contract2):** `color_prop_tier1.vert` no longer scans ObjectLights or dispatches on light type; the prop sun term is the camera contract `contractSun(S, A, F, I)` (mclib/prop_light_contract.h), so its `light_type_dispatch` row is retired. INTENTIONAL.
