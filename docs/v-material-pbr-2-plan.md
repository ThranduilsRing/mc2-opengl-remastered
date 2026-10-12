<!-- doc-status: current | reviewed:2026-10-11 -->
# V-MATERIAL-PBR-2 — Cheap Specular for StaticPropOpaque (PLAN)

**Status:** DRAFT — plan-only. Awaiting reviewer.
**Owner slice:** V-MATERIAL-PBR-2 (first real consumer of `metallicFactor` +
`roughnessFactor` shipped dead-data in V-MATERIAL-PBR-1 `eb7bebdb`).
**Pre-req HEAD:** `5bfd15d8` (V-IBL-STATIC-2 per-mission SH selection).

---

## §1. Context

`MaterialGpu.metallicFactor` (byte offset 24) and `roughnessFactor` (byte
offset 28) are surfaced through the static-prop pipeline today, but are
**dead-data** for lighting math. V-MATERIAL-PBR-1 only exposed them through
the V-MATERIAL-DEBUG-1 inspector (modes 5/6 = grayscale view) and the
inventory snapshot. No fragment or vertex actually consumes them for
shading. V-MATERIAL-PBR-2 is the first slice that does.

Visible target: metallic-ish architecture (window frames, glass trims,
metal panels on building shapes that already carry hot-pink window magic).
Glass/window magic is preserved unconditionally — the window-flag branch
in `static_prop.vert` (line 308) early-outs to `base_light` and is
**explicitly skipped** by this slice.

PBR contract guarantee for static props post V-MATERIAL-PBR-1:
`metallic ∈ [0,1]`, `roughness ∈ [0,1]`, with cook defaults
`metallic = 0.0` (dielectric) and `roughness = 1.0` (fully matte). For
default cook assets this slice produces a **null term** mathematically
(metallic=0 zeroes the metal-Fresnel bias, roughness=1 zeroes the
smoothness exponent), so even at full gate-ON the visible delta is
limited until per-material values are authored.

## §2. Current lighting model (audit)

Per docs/static-prop-lighting-audit.md (archived 2026-10-11, E:/mc2-archive-2026-10/docs/cull-wave1-docs-2026-10-11.tar.gz) (`af314d22`) and direct re-read
of `shaders/static_prop.vert` and `shaders/static_prop.frag`:

- **Lighting is per-vertex (Gouraud).** The vertex shader builds `lit`
  (`base_light` + `calc_light` directional + V-AMBIENT-STATIC-1 hemisphere
  + V-IBL-STATIC-1 SH-L2 ambient + highlight) and packs it into
  `v_argb.rgb`. The fragment shader multiplies `tex_color * v_argb.rgb`
  and adds fog/highlight. There is **no per-fragment lighting today**.
- **View vector is NOT plumbed to the fragment shader.** `cameraWorldPos`
  is in `ViewUniforms` (binding=3, F1-3 era) and is reachable in the
  vertex shader under `MC2_USE_VIEW_UNIFORMS`, which is **default-on** at
  process start (per HANDOFF `cf5f67bc`, kill-switch
  `MC2_VIEW_UNIFORMS=0`). Legacy fallback path still exists and exposes
  only `u_worldToClipGL` — no camera position.
- **No specular term exists at all today.** Neither directional specular
  nor environment specular nor any Fresnel.

## §3. Recommended approach

**Option B from recon (Schlick-Fresnel + directional-only specular).**
No environment-specular hack via SH-along-reflection. No prefiltered
cubemap. No BRDF LUT.

Math (per vertex, inside the `else` (non-window) branch of
`static_prop.vert`, after existing `lit` is computed):

```glsl
// V-MATERIAL-PBR-2: directional-only Schlick specular.
// Inputs:
//   N = normalize(worldNormal)        (Stuff-space Y-up, already used)
//   V = normalize(u_cameraWorldPos.xyz - worldPos)  -- world-space view
//   L = normalized sun direction      (from lighting UBO entry 0; see §5)
//   sunColor = sun radiance           (from lighting UBO entry 0)
//   albedo = albedoFromVertexARGB OR (1,1,1) at this slice (see Risk §7.a)
//   metallic, roughness  (from MaterialGpu[materialIdx])

vec3 F0 = mix(vec3(0.04), albedo, metallic);
vec3 H  = normalize(L + V);                       // half-vector
float NdotV = max(dot(N, V), 0.0);
float NdotH = max(dot(N, H), 0.0);
float NdotL = max(dot(N, L), 0.0);

// Schlick Fresnel along V (no GGX normalization — cheap).
vec3 F = F0 + (vec3(1.0) - F0) * pow(1.0 - NdotV, 5.0);

// Smoothness exponent. roughness=1 -> exponent ~1 -> very soft / no
// highlight. roughness=0 -> exponent ~512 -> sharp highlight.
float smoothness  = clamp(1.0 - roughness, 0.0, 1.0);
float specPower   = mix(1.0, 512.0, smoothness * smoothness);
float specLobe    = pow(NdotH, specPower) * NdotL;

vec3 specular = sunColor * F * specLobe;

// Diffuse modulation: metallic surfaces have no Lambertian diffuse.
// At this slice, the existing `lit` already contains the full diffuse
// term -- we cannot retroactively kill the metallic-diffuse contribution
// at v_argb pack time without re-computing it. **Compromise:**
// dim `lit` by (1 - metallic * u_pbrV1Strength) so default metallic=0
// is a no-op AND the dim only activates with the gate.
lit *= mix(1.0, 1.0 - metallic, u_pbrV1Strength);
lit += specular * u_pbrV1Strength;
```

**Per-vertex, NOT per-fragment**, for this slice:
- Matches the existing Gouraud pipeline (no new varyings needed for
  worldPos / interpolated normal).
- Avoids adding `cameraWorldPos` to the fragment shader (the legacy
  non-MC2_USE_VIEW_UNIFORMS branch has no UBO at all in the fragment
  side).
- Specular highlights WILL look blocky on large polys — accepted as a
  known limitation; documented in §7 and addressed in a follow-up
  slice (V-MATERIAL-PBR-3 per-fragment lift) if the visual cost is
  judged too high.

## §4. Gating

Following the V-IBL-STATIC-1 + V-AMBIENT-STATIC-1 precedent verbatim:

- **Env var:** `MC2_STATIC_PROP_PBR_V1`. Resolved once at process start
  to `s_pbrV1Enabled`. Authoritative on/off gate.
- **Runtime strength:** `g_pbrV1Strength` (float, file-scope, external
  linkage so EditorInspector can hold a slider). Range [0.0, 3.0].
- **Default:** env unset → `s_pbrV1Enabled = false` → per-frame upload
  `u_pbrV1Strength = 0.0f` → **mathematical default-OFF byte-identity**:
  - `lit *= mix(1.0, 1.0 - metallic, 0.0) == lit * 1.0` — bitwise no-op.
  - `lit += specular * 0.0` — bitwise no-op (multiply by literal 0).
  - **Default tier1 output is byte-identical to `5bfd15d8`.**
- **Kill-switch:** `MC2_STATIC_PROP_PBR_V1=0` ⇔ unset. Both produce
  identical output. Document this equivalence in the env-registry entry.
- **ImGui slider:** EditorInspector adds a slider mirroring the
  V-IBL-STATIC-1 row, label `"PBR specular v1 strength"`, tooltip
  references the env gate.

Shader composition (`static_prop.vert` else-branch only):

```glsl
// EARLY-CHEAP-GUARD pattern (matches V-IBL-STATIC-1):
if (u_pbrV1Strength > 0.0) {
    // ... §3 math ...
}
```

The guard means strength=0 pays a single uniform load + a compare —
**no Fresnel pow(), no normalize, no mat math**. This is the proof
of cheap default-OFF, stronger than relying on multiply-by-zero.

## §5. Files to touch (estimate)

| # | File | Delta | Purpose |
|---|------|-------|---------|
| 1 | `docs/v-material-pbr-2-plan.md` | NEW (this doc) | tracked plan |
| 2 | `shaders/static_prop.vert` | +~30 lines | PBR math block + uniform decl |
| 3 | `RenderCore/RendererFeatureRegistry.h` | +1 enum + 2 string rows | feature registry entry |
| 4 | `GameOS/gameos/gos_static_prop_batcher.cpp` | +~25 lines | env gate, uniform location, per-frame upload (legacy + coalesce paths), `g_pbrV1Strength` global |
| 5 | `GuiRuntime/EditorInspector.cpp` | +~10 lines | ImGui slider row |

**No new files. No MaterialGpu schema change. No ViewUniforms change**
(cameraWorldPos already present). No KTX / cook expansion. No new
cubemap / BRDF LUT path.

Sun direction + sunColor: read from `LightsData` UBO (binding 0)
already consumed by `calc_light`. Need to confirm in §6 whether
`get_base_light` / `calc_light` already exposes the directional
sun-light reachable at vertex shader scope — if not, we either
- (a) re-fetch from the UBO inline in the PBR block, or
- (b) extend `calc_light` to write the directional `L` and `sunColor`
  to `out` parameters.

**(a) is preferred** for blast-radius minimization. The UBO is already
bound and indexed by `inst.lightDataIndex`. Inspect
`shaders/include/lighting.hglsl` for the field layout during
implementation; if the directional entry is at a stable slot (e.g.
`light[lightDataIndex].lights[0]` is sun-typed by convention), inline
the read. If not stable, defer the slice and lock convention first.

## §6. Validation strategy

| Probe | Expectation | Tier |
|-------|-------------|------|
| Default-OFF tier1 (env unset) | `5/5 PASS`, `ok=1`, screenshots byte-identical to `5bfd15d8` golden | inner-loop |
| Kill-switch (`MC2_STATIC_PROP_PBR_V1=0`) | Identical to env-unset | one mission |
| Gate-ON (`=1`) tier1 | `5/5 PASS`, no crash, no GL_ERROR, slider at default 0.5 | inner-loop |
| Strength override (`=1`, slider=0.0) | byte-identical to gate-OFF (proves `u_pbrV1Strength > 0.0` short-circuit) | one mission |
| Strength override (`=1`, slider=3.0) | visibly brighter highlights on default-roughness=1 props (specPower=1 case) | manual visual |
| Debug-mode 5 (roughness view) | unchanged (debug-modes 5/6 are V-MATERIAL-PBR-1 grayscale, not gated by this slice) | inspection |
| SHADER-REFLECT-HYGIENE-6 follow-up | Run `tools/shader_reflect/reflect.py`; commit golden drift in same PR as shader edit | shipping gate |
| `MC2_VIEW_UNIFORMS=0` regression | Slice must compile and not crash on legacy projection path. If `u_cameraWorldPos` is unavailable in that path, the `s_pbrV1Enabled` per-frame upload must force `0.0f` when ViewUniforms is off | safety |

## §7. Risks

### §7.a Albedo at vertex stage
The PBR math wants `albedo` for `F0 = mix(0.04, albedo, metallic)`.
The vertex shader does **not have the sampled albedo** — that lives in
the fragment shader as `tex_color`. For this slice we **approximate**
`F0` with the dielectric base `vec3(0.04)` only (i.e. ignore the
albedo-tint on the metal Fresnel) by hard-coding `F0 = vec3(0.04)`
and dropping the `mix(...metallic)` blend. That makes pure metals
look slightly grey instead of metallic-colored, but it's a known
limitation of the per-vertex approach; the per-fragment lift in
V-MATERIAL-PBR-3 reintroduces albedo-tinted F0 once `tex_color` is
in scope. **Alternative:** sample albedo at the vertex stage from a
fixed UV (e.g. UV(0,0)) — rejected as cost > benefit at this slice.

### §7.b Per-vertex specular blockiness
Gouraud-interpolated specular highlights look blocky on large polys.
This is the dominant visual artifact of the per-vertex choice.
Mitigation: documented limitation; users who want correct highlights
roll forward to V-MATERIAL-PBR-3 (per-fragment).

### §7.c Energy conservation
We are using cheap Schlick + a `pow(NdotH, k)` highlight without
GGX normalization. At low roughness on a directional light this can
exceed `sunColor` energy at the specular peak. Mitigation: the
`u_pbrV1Strength` slider lets the user dial it down; default 0.5
is conservative; clamp `lit` after add via the existing
`clamp(lit + inst.aRGBHighlight.rgb, 0.0, 1.0)` at line 349 catches
runaway brightness.

### §7.d Sun-direction discoverability
If `lighting.hglsl`'s UBO layout does not expose the sun direction
at a known stable index, we must lock a convention before shipping.
Drop-slice: stop, document, lift to V-MATERIAL-PBR-2-PRE.

### §7.e ViewUniforms off-path
If user runs with `MC2_VIEW_UNIFORMS=0` (legacy uniform projection),
`u_cameraWorldPos` is undefined and the shader won't compile (or will
silently take a stale value). Force-upload `0.0f` to
`u_pbrV1Strength` when `MC2_VIEW_UNIFORMS=0` to **disable the slice
unconditionally on the legacy path**. Document the dependency:
"V-MATERIAL-PBR-2 requires MC2_VIEW_UNIFORMS=1 (default)."

### §7.f shader_reflect golden drift
New uniforms (`u_pbrV1Strength`) drift the goldens. Run
SHADER-REFLECT-HYGIENE-6 in the same PR. Plan adds 1 uniform; the
hygiene update is mechanical (`tools/shader_reflect/reflect.py` and
commit the JSON diff).

## §8. Anti-goals

- **No** prefiltered cubemap. Specular environment is V-MATERIAL-PBR-3+.
- **No** BRDF LUT.
- **No** environment-specular hack via "SH along reflection vector".
- **No** GGX / Cook-Torrance distribution. Schlick + pow(N·H, k) only.
- **No** normal-map sampling, no tangents, no anisotropy, no clearcoat,
  no sheen, no subsurface.
- **No** HDR post / tone-mapping change.
- **No** PBR on terrain, mech, shadow pass, or VFX. StaticPropOpaque ONLY.
- **No** MaterialGpu schema change (metallic/roughness already shipped
  in V-MATERIAL-PBR-1).
- **No** default visual change (gate default-OFF, mathematical proof of
  byte-identity).
- **No** new mandatory uniform that breaks legacy projection path.

## §9. Decision: implement-now vs defer

**Recommendation: DEFER** as a separate implementation slice after
this plan is reviewed. Rationale:

1. **Sun-direction discoverability risk (§7.d):** without confirming
   `lighting.hglsl`'s UBO layout exposes the directional sun at a
   stable index reachable at vertex scope, the slice may need a
   pre-cursor lift. File-count estimate above (5 files, ~65 src
   lines) is plausible BUT assumes (a) inline UBO read works; if it
   doesn't, an `out`-param plumbing pass into `calc_light` pushes
   shader edits into a 6th file (`lighting.hglsl`) and lifts the line
   count above the tiny-impl budget.
2. **Albedo approximation trade-off (§7.a):** the F0 = 0.04 fixed
   constant simplification is a judgment call worth advisor
   sign-off — it determines what V-MATERIAL-PBR-2 looks like on
   metallic=1 materials.
3. **Per-vertex vs per-fragment:** plan recommends per-vertex for
   blast-radius reasons but a per-fragment lift may be preferred by
   reviewer; both fit within similar file counts and the choice
   should be made before code lands.
4. **ViewUniforms off-path interlock (§7.e):** the
   "force-strength=0 when MC2_VIEW_UNIFORMS=0" safety mechanic
   touches the env-gate composition logic — worth one review pass
   before code.

If reviewer signs off on all four open questions, the implementation
fits in 5 files / ≤80 src lines and can land as a single tiny-impl
slice (V-MATERIAL-PBR-2-IMPL) with all six §6 probes green.

## §10. Cross-references

- V-IBL-STATIC-1 ship — `64e58c11` (SH ambient gating pattern,
  env+strength+slider+registry blueprint reused verbatim here)
- V-IBL-STATIC-1-TUNE — `99779f70` (default strength magnitude
  precedent; plan suggests 0.5 default for `g_pbrV1Strength`)
- V-MATERIAL-PBR-1 ship — `eb7bebdb` (metallic/roughness contract
  surfacing + debug modes 5/6; this slice is the first lighting
  consumer)
- V-IBL-STATIC-2 ship — `5bfd15d8` (per-mission SH set selection;
  HEAD this plan branches from)
- docs/static-prop-lighting-audit.md (archived 2026-10-11, E:/mc2-archive-2026-10/docs/cull-wave1-docs-2026-10-11.tar.gz) `af314d22` — audit of the
  per-vertex lighting flow this slice composes onto
- docs/ibl-plan.md (archived 2026-10-11, E:/mc2-archive-2026-10/docs/cull-wave1-docs-2026-10-11.tar.gz) `cfad795c` — broader IBL roadmap (V-MATERIAL-PBR-3
  belongs to the cubemap track tracked there)
- docs/v-ibl-static-1-plan.md (archived 2026-10-11, E:/mc2-archive-2026-10/docs/cull-wave1-docs-2026-10-11.tar.gz) `4c2bd769` — direct precedent doc for
  this plan's structure
- HANDOFF `cf5f67bc` — ViewUniforms default-on flip (cameraWorldPos
  availability proof)
- `shaders/include/view_uniforms.hglsl` — `u_cameraWorldPos` field
  the PBR view vector reads from
