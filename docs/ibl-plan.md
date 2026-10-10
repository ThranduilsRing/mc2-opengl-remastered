<!-- doc-status: stale | reviewed:2026-10-11 -->
# IBL Plan (V-IBL-STATIC-0)

Track V Batch 2 slice 3/3. **Plan only** — no shader/code edits.
Closes Batch 2 ahead of any IBL implementation work.

Branch: `claude/nifty-mendeleev`. HEAD at planning: `b695c803`.

---

## 1. Context

Track V is the visual-quality arc that runs **after** the engine
closure audit (`b7987b70`, `docs/engine-closure-audit.md`). The
StaticPropOpaque lane is the reference green-lit closure axis
(audit §"Track V verdict: YELLOW_BUT_READY"), so it is the chosen
substrate for the first lighting upgrades.

Batch 2 sequence:

| Slice | Status | Anchor |
|---|---|---|
| V-LIGHTING-STATIC-0 (audit) | shipped | `af314d22` → `docs/static-prop-lighting-audit.md` |
| V-AMBIENT-STATIC-1 (hemisphere) | shipped, default OFF | `19e85517` → `shaders/static_prop.vert:115,271` |
| V-MATERIAL-DEBUG-1 (debug views) | shipped | `feca6efe` |
| **V-IBL-STATIC-0 (this plan)** | **DRAFT** | this file |

`V-AMBIENT-STATIC-1` is the precedent for gating: shader uniform
`uniform float u_ambientV1Strength;` (`shaders/static_prop.vert:115`),
default `0.0` (byte-identical OFF), driven to `1.0` when env
`MC2_STATIC_PROP_AMBIENT_V1=1`. Hemisphere term added at
`shaders/static_prop.vert:271`:
`ambient_v1 = mix(groundColor, skyColor, 0.5 + 0.5 * worldNormal.y)`.
IBL is the natural next step UP from the hemisphere term — same gate
shape, same upload site, more accurate ambient.

---

## 2. Current state

### 2.1 Sky / HDRI assets present

- `data/hdr/DaySkyHDRI063B_4K.exr` — 14,720,660 bytes (~14 MB),
  equirectangular `.exr`, 4K resolution implied by filename.
- `data/hdr/DaySkyHDRI063B_LICENSE.txt` — license sidecar.

No other `.hdr`, `.exr`, `.ktx2` cubemaps, or BRDF LUTs exist
anywhere in the worktree (verified via repo-wide glob: zero
`.hdr` / zero `.ktx2` / zero `*brdf*` results outside the
shader-reflect tooling tree).

### 2.2 Current skybox draw path

- Load: `GameOS/gameos/gos_hdri.cpp:111` `loadHdriTexture(const char*)`
  decodes the EXR via tinyexr, uploads as `GL_RGBA16F`
  (`gos_hdri.cpp:55`), `GL_CLAMP_TO_EDGE` + `GL_LINEAR` (no mips,
  `gos_hdri.cpp:57-60`).
- Init site: `GameOS/gameos/gos_postprocess_gates.cpp:108-135` — env gate
  `MC2_HDRI_SKY` (default ON unless `=0`), loads
  `data/hdr/DaySkyHDRI063B_4K.exr` (`GameOS/gameos/gos_postprocess_gates.cpp:112`),
  compiles `hdri_skybox.vert` + `hdri_skybox.frag`.
- Draw: `GameOS/gameos/gos_postprocess.cpp:981`
  `gosPostProcess::renderHdriSkybox(viewMat, projMat)`. Inverse-
  projects an NDC corner, transposes the upper 3x3 of view, samples
  `u_hdri` (`sampler2D`, equirect) at the resulting world direction.
- Shader: `shaders/hdri_skybox.frag` — equirect 2D sampler, NOT a
  cubemap. `atan(z,x)` + `asin(y)` UV math at `hdri_skybox.frag:30-33`.
  **Status: dirty in worktree** (pre-existing v-flip experiment; the
  IBL plan does NOT depend on this and MUST NOT entangle with it).

### 2.3 Current ambient model

Per `docs/static-prop-lighting-audit.md` (key findings echoed in
the `af314d22` commit body):

- Per-vertex Gouraud (math in `shaders/static_prop.vert`, color
  interpolated to fragment as `v_argb`; `.frag` only modulates).
- Directional light via `LightsData` SSBO binding=20.
- Hemisphere term (V-AMBIENT-STATIC-1) layered into the non-window
  branch under `u_ambientV1Strength`.
- **No IBL, no SH, no cubemap sampling anywhere in the lighting
  path.** Skybox sampling is background-only (depth-masked,
  `gos_postprocess.cpp:1043` `glDepthMask(GL_FALSE)`); the sky
  texture is not read by any surface shader.

### 2.4 Existing cooker and KTX2 loader

- Cooker `tools/mc2texcook/mc2texcook.py` produces **RGBA8 2D
  textures only** (`mc2texcook.py:8-15`). Presets:
  `albedo|normal|orm|emissive|mask`. **No cubemap support, no HDR
  format support** (RGBA8 hard-coded via VK_FORMAT_R8G8B8A8_*).
- Loader `RenderCore/KtxLoader.h` is **Phase 0**: RGBA8 only, mip
  0 only (`KtxLoader.h:17` `// RGBA8, mip 0 only (level 0)`), 2D
  textures only. No cubemap face handling, no mip-chain ingest.

This is the canonical GAP: **neither side of the cook→load pipeline
currently supports cubemaps or HDR-precision sample data.**

### 2.5 BRDF LUT

Not present. Zero hits for `brdf|BRDF|lut` in lighting-relevant
paths.

---

## 3. Chosen first IBL shape

### 3.1 Recommendation: **Option B — SH coefficients (L2, 9 vec3)**

Rationale (cite-anchored):

1. **No cooker gap to close.** L2 SH coefficients are 9 `vec3`
   floats, computed once offline from the existing `.exr`
   (Python + numpy). They ship as a small JSON or binary blob
   alongside the mission load — no KTX2-cubemap path needed.
   The current cooker (`mc2texcook.py:8-15`) does not write
   cubemaps and KtxLoader (`KtxLoader.h:17`) does not read them
   — Option A (cubemap) would force extending **both** sides
   before any visual change is observable.
2. **Smallest shader delta.** Adds `uniform vec3 u_iblShCoeffs[9];`
   and an `evaluateSh2(N)` helper. No new sampler binding (no
   binding-registry churn, no shader-reflect golden regen for
   sampler set/binding indices — only the uniform vector signature
   changes).
3. **Mirrors the V-AMBIENT-STATIC-1 gate exactly.** Same upload
   path (uniform float / vec3 array per flush in `gos_static_prop_
   batcher.cpp`), same default-OFF semantics (zero coefficients =
   byte-identical), same env-var pattern
   (`MC2_STATIC_PROP_IBL_V1`).
4. **Compatible asset:** `data/hdr/DaySkyHDRI063B_4K.exr` is an
   equirectangular sky — exactly the input form an offline SH
   projector consumes (sample direction → texel → accumulate
   `Y_l^m(N) * radiance * solidAngle`).

### 3.2 Why NOT Option A (irradiance cubemap) as first step

- Requires extending `mc2texcook.py` to emit KTX2 cubemap faces.
- Requires extending `KtxLoader.h` to parse `faceCount=6` and mip
  arrays (currently `mip 0 only`, `KtxLoader.h:17`).
- Adds a new sampler binding (`samplerCube`) → shader-reflect
  golden regen on every static-prop shader variant.
- All-or-nothing first cut: nothing renders until both extensions
  land.

Option A becomes V-IBL-STATIC-2 once the cooker/loader can carry
cubemaps; the shader code path then drops the SH coefficients and
switches to `texture(u_iblIrradiance, N)`.

### 3.3 Why NOT Option C (prefiltered specular + BRDF LUT)

- Requires a metallic / roughness material channel.
  `RenderCore/MaterialGpu` currently carries albedo + layer only
  (per `engine-closure-audit.md` + the MaterialGpu arc handoffs).
  Plumbing those channels is its own arc (call it `MATERIAL-PBR-1`)
  and must precede prefiltered specular.
- Requires a 256x256 RG16F BRDF LUT — does not exist
  (§2.5) and KtxLoader cannot ingest RG16F (RGBA8 only).
- Two-texture lookup (`prefilteredCube` + `brdfLut`) plus the
  Fresnel split-sum math is a much larger first-step than the
  audit's "smallest safe step" framing supports.

### 3.4 Shader-side sketch for V-IBL-STATIC-1

Inside the existing non-window branch in `shaders/static_prop.vert`
(near `:271`, where `ambient_v1` is already computed):

```glsl
// Added at the top of the non-window branch (after worldNormal):
uniform vec3  u_iblShCoeffs[9];   // L2 SH, 9 vec3
uniform float u_iblV1Strength;    // 0.0 = OFF (byte-identical)

vec3 evaluateShL2(vec3 N) {
    // Stupéfait/Ramamoorthi-Hanrahan basis. Real expansion;
    // exact constants left for implementation slice.
    // 1, Y, Z, X, XY, YZ, 3Z^2-1, XZ, X^2-Y^2.
    return u_iblShCoeffs[0]                            // band 0
         + u_iblShCoeffs[1] * N.y                      // band 1
         + u_iblShCoeffs[2] * N.z
         + u_iblShCoeffs[3] * N.x
         + u_iblShCoeffs[4] * (N.x * N.y)              // band 2
         + u_iblShCoeffs[5] * (N.y * N.z)
         + u_iblShCoeffs[6] * (3.0 * N.z * N.z - 1.0)
         + u_iblShCoeffs[7] * (N.x * N.z)
         + u_iblShCoeffs[8] * (N.x * N.x - N.y * N.y);
}

// Then, in the ambient assembly:
vec3 ambient_ibl_v1 = evaluateShL2(worldNormal) * u_iblV1Strength;
lit += ambient_ibl_v1;
```

Default `u_iblV1Strength = 0.0` → byte-identical OFF
(`lit += vec3(0)`). Mirrors the V-AMBIENT-STATIC-1 guarantee
exactly (`shaders/static_prop.vert:115,271`).

---

## 4. Asset / tool requirements

### 4.1 Offline SH projector

New small tool: `tools/ibl_project_sh/project_sh.py`.
Inputs: `data/hdr/DaySkyHDRI063B_4K.exr`.
Outputs: a tracked text file (e.g. `data/hdr/DaySkyHDRI063B.sh9.json`
or `.txt`) containing 27 floats (9 vec3).

Dependencies: numpy + a lightweight EXR reader. tinyexr already
ships in the engine TU (`gos_hdri.cpp:9-10`) — for the Python
offline tool, `OpenEXR` (PyPI) or `imageio` is preferred to keep
the cooker dependency surface unchanged. **No engine code change
to the cooker is required.**

Algorithm: for each texel of the equirect, compute the world
direction, evaluate the 9 SH basis functions, accumulate
`radiance * basis * solidAngle`. Final multiply by `4π / N_samples`
(or use the exact equirect dΩ = `sin(θ) dθ dφ`). Standard
Ramamoorthi-Hanrahan projection — no novel work.

### 4.2 Engine-side loader

A trivial parser: `RenderCore/IblShCoeffs.h` (declaration) plus
`mclib/ibl_sh_coeffs.cpp` (or similar) reads the 27-float blob
once at mission load. **No new asset binary format.** Use plain
text or JSON to keep the loader ≤30 lines.

### 4.3 Per-flush upload

In `gos_static_prop_batcher.cpp` (same site that uploads
`u_ambientV1Strength` per V-AMBIENT-STATIC-1), add a 9-vec3
upload and the strength scalar. Both paths (coalesce + legacy)
must mirror, matching the V-AMBIENT-STATIC-1 precedent.

### 4.4 What is NOT needed for V-IBL-STATIC-1

- No KTX2 cubemap cooker extension.
- No `KtxLoader` cubemap support.
- No BRDF LUT.
- No mip chain handling.
- No `samplerCube` binding-registry slot (per
  `docs/render-binding-registry.md`).
- No new RenderPassContract entry (per
  `RenderCore/RenderPassContract.h`) — IBL is a per-flush
  uniform delta inside the existing StaticPropOpaque pass.

---

## 5. Implementation phases

### V-IBL-STATIC-1 — SH ambient on static props (default OFF)

Scope:
- New env gate `MC2_STATIC_PROP_IBL_V1` (default OFF).
- New feature-registry entry (mirrors
  `RendererFeatureRegistry::StaticPropAmbientV1=19`; this becomes
  `StaticPropIblV1=20`, `COUNT 20→21`, per `19e85517` commit body).
- Offline SH projector (§4.1) producing
  `data/hdr/DaySkyHDRI063B.sh9.json` (single tracked file).
- Engine-side loader called once at mission load
  (or once at first flush — load location to be decided in
  V-IBL-STATIC-1's discuss-phase).
- Shader uniforms: `vec3 u_iblShCoeffs[9]` + `float u_iblV1Strength`
  (§3.4).
- CPU upload site mirrors `u_ambientV1Strength` in
  `gos_static_prop_batcher.cpp` (both coalesce + legacy paths).
- Tier1 5/5 default-OFF byte-identical proof; tier1 5/5 ON proof
  with visible ambient color variation.
- Baseline tracked-flag entry: add `MC2_STATIC_PROP_IBL_V1` to
  `scripts/capture_baseline.py:48` `TRACKED_FLAGS` tuple
  (currently includes `MC2_STATIC_PROP_AMBIENT_V1` at line 56).
- Shader-reflect goldens regen for `shaders/static_prop.vert`
  (uniform signature changes).

### V-IBL-STATIC-2 — irradiance cubemap (replaces SH)

Prerequisite slices (independent arcs, can interleave):
- `MC2-TEXCOOK-CUBE-1` — extend `mc2texcook.py` (§2.4) to emit
  KTX2 cubemaps from a single equirect input.
- `MC2-KTXLOADER-CUBE-1` — extend `KtxLoader` (§2.4) to ingest
  `faceCount=6` + mip chains.

Then: replace `u_iblShCoeffs[9]` with `samplerCube
u_iblIrradiance`, drop `evaluateShL2`, sample
`texture(u_iblIrradiance, worldNormal)`.

### V-IBL-STATIC-3 — prefiltered specular (deferred)

Hard prerequisite: `MATERIAL-PBR-1` (metallic/roughness channels
in `RenderCore/MaterialGpu`). Not in scope for the V-IBL arc as
currently framed.

### V-IBL-MISSION-1 — per-mission HDRI (deferred)

Per-mission `.exr` selection, projected to SH or cubemap at
mission load. Predicated on a stable V-IBL-STATIC-1 default-ON
flip.

---

## 6. Risks / blockers

1. **Cooker / loader cubemap gap** (§2.4) — blocks Option A.
   Plan sidesteps by choosing Option B (SH). No risk to
   V-IBL-STATIC-1.
2. **Skybox shader paused dirty state**
   (`shaders/hdri_skybox.frag`, working tree pre-existing
   modification). V-IBL-STATIC-1 touches only
   `shaders/static_prop.vert` — the IBL plan is independent of
   skybox shader cleanup. Risk = zero if the discipline holds.
3. **SH projection correctness vs reference.** Standard
   Ramamoorthi-Hanrahan; many reference impls (Filament, glTF
   sample viewer). Will use a known-good impl as cross-check.
4. **AMD driver uniform-array reliability.** Per
   `INDEX-SHADERS` entry `amd_tes_uniform_propagation_unreliable`
   (cited in 2026-05-22 LATE handoff), uniform arrays via
   `glUniform3fv(loc, 9, …)` on TES are unreliable. **Mitigation:**
   IBL is in the **vertex** shader (`static_prop.vert`), not TES.
   No tessellation pipeline involvement. Still: the impl phase
   must verify upload via a parity probe (mirroring the
   `worldToClipGL byte-identical` proof from F1-3C).
5. **Shader-reflect golden churn.** Each uniform signature change
   regenerates the JSON golden for `shaders__static_prop.vert__*.json`
   under `tools/shader_reflect/expected/`. Single regen cycle per
   slice — tractable.
6. **Energy conservation when both hemisphere v1 AND IBL v1 are
   ON.** Both terms add to `lit`. Plan: make the gates mutually
   exclusive at upload time (if `MC2_STATIC_PROP_IBL_V1=1`, force
   `u_ambientV1Strength=0`), OR document explicitly that they are
   additive and intended for A/B comparison only. Decision
   deferred to V-IBL-STATIC-1's discuss-phase.

---

## 7. Anti-goals for V-IBL-STATIC-1

- No PBR material contract (materialIdx still albedo + layer only).
- No metallic / roughness asset cook.
- No specular reflections.
- No per-mission dynamic IBL update; SH coeffs loaded once at
  mission start (or first flush) and held until mission unload.
- No HDR post-processing or tonemap changes.
- No skybox shader edits (skybox shader stays in its current
  paused-dirty state, owned by a separate arc).
- No cubemap cooker / loader work (deferred to V-IBL-STATIC-2).
- No BRDF LUT (deferred to V-IBL-STATIC-3).
- No mech / terrain / VFX integration — static-prop lane only.
- No default-ON flip in V-IBL-STATIC-1's first ship. Flip is its
  own slice once tier1 + soak signal is clean.

---

## 8. Validation strategy

### 8.1 Default OFF (byte-identical proof)

`u_iblV1Strength = 0.0` → `lit += evaluateShL2(N) * 0.0` →
`lit += vec3(0)`. Compiler MUST NOT speculatively execute the
SH evaluator (no side effects) but even if it does, the result
is dead-code on the addition.

Same proof structure as V-AMBIENT-STATIC-1 (`19e85517` commit
body: "0.0 default = mathematically byte-identical OFF (lit +=
vec3(0))").

Gate: tier1 5/5 PASS with `MC2_STATIC_PROP_IBL_V1` unset.

### 8.2 Gate ON (visible delta)

`MC2_STATIC_PROP_IBL_V1=1` → `u_iblV1Strength=1.0`, coefficients
loaded from `data/hdr/DaySkyHDRI063B.sh9.json`. Expected visual:
ambient color modulated by surface normal direction (warm tones on
upward-facing faces from the sky band, cooler on downward).

Gate: tier1 5/5 PASS, no crashes, no `glGetError` regressions,
debug-view comparison (V-MATERIAL-DEBUG-1, `feca6efe`) shows
non-zero ambient contribution channel.

### 8.3 Baseline capture

Per `docs/visual-baseline-howto.md`. Add `MC2_STATIC_PROP_IBL_V1`
to `scripts/capture_baseline.py:48` `TRACKED_FLAGS` — small,
mechanical edit, planned now but only applied during
V-IBL-STATIC-1.

### 8.4 SH projection cross-check

Project the same `.exr` with at least one second-source impl
(e.g. `cmgen --sh=3`) and diff the 27 floats. Acceptance:
RMS < 1e-3 between the two projections.

---

## 9. Cross-references

- `docs/static-prop-lighting-audit.md` (`af314d22`) — current
  lighting state, "what is NOT done" §
- `docs/engine-closure-audit.md` (`b7987b70`) — Track V verdict
  + StaticPropOpaque closure-axis state
- `docs/render-binding-registry.md` — SSBO/sampler binding slots
  (no new slot needed for V-IBL-STATIC-1)
- `docs/visual-baseline-howto.md` — baseline capture harness
- `RenderCore/RenderPassContract.h` — no new pass entry needed
  (uniform delta inside existing StaticPropOpaque)
- `RenderCore/KtxLoader.h:17` — RGBA8 mip-0-only loader
  (blocks Option A, irrelevant to Option B)
- `tools/mc2texcook/mc2texcook.py:8-15` — RGBA8 2D cooker
  (blocks Option A, irrelevant to Option B)
- `shaders/static_prop.vert:115,271` — V-AMBIENT-STATIC-1 gate
  precedent
- `GameOS/gameos/gos_hdri.cpp:12` — EXR loader (used by skybox;
  the offline SH projector consumes the same `.exr` file path
  but does NOT share code with this engine TU)
- `GameOS/gameos/gos_postprocess.cpp:981` — current sky draw
  site (independent of IBL surface integration)
- `scripts/capture_baseline.py:48` — `TRACKED_FLAGS` tuple
  (extension site for V-IBL-STATIC-1)
- V-AMBIENT-STATIC-1 (`19e85517`) — gating + upload precedent
- V-MATERIAL-DEBUG-1 (`feca6efe`) — debug-view harness for
  visual A/B
- INDEX-SHADERS `amd_tes_uniform_propagation_unreliable` —
  AMD uniform-array caveat (mitigated by being in `.vert`)
