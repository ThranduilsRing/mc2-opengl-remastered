<!-- doc-status: stale | reviewed:2026-10-11 -->
# Terrain Visual Resample Plan (TERRAIN-RESAMPLE-PLAN-0)

Plan-only artifact. **No implementation in this slice.** Answers the
slice-5 design questions ahead of any TERRAIN-RESAMPLE-1 /
TERRAIN-NORMALS-FROM-HEIGHT-1 / TERRAIN-DISPLACE-VISUAL-1 work.

Pairs with [docs/terrain-rv-arc-recon.md](terrain-rv-arc-recon.md)
(Slice 1) and [docs/terrain-height-audit.md](terrain-height-audit.md)
(Slice 3).

## 0. Core constraint (load-bearing)

**Gameplay heightfield is authoritative.** `getTerrainElevation()`
(mclib/terrain.h:205) and `worldToTile/worldToCell` (mclib/terrain.h:
335-375) define the world. Render-side resample is **visual only** and
**must satisfy the zero-error sample-point invariant**: at every
original gameplay vertex, the resampled height MUST equal the source
`PostcompVertex.elevation`. Between samples, anything goes — but at
samples, drift is forbidden.

If this invariant ever fails:
- Mechs/units float or sink relative to ground (visible bug).
- Building footprints lift off / clip in (placement bug).
- Cursor pick / unit move / LOS / shadow projection may all skew.

Validator (§9): every implementation slice must include a parity probe.

## 1. Source resolution

From Slice 1:

- Per vertex: 128 world units (`worldUnitsPerVertex`).
- Per cell: 42.67 wu (3× subdivide; `MAPCELL_DIM = 3`).
- Typical grid: 120 × 120 (range 60–120 valid).
- Source normal: `PostcompVertex.vertexNormal` pre-baked per vertex.
- Source layout: flat `PostcompVertex[S²]`, 28 bytes per record.

Visual ceiling at source = 128 wu spatial frequency Nyquist on height,
plus per-material normal-maps + POM faking high-frequency detail.

## 2. Render mesh today

- Legacy: `Terrain::render()` per-tile 20×20 vertex meshes, dispatched
  by `gos_terrain_bridge_*` ([gameos_graphics.cpp:1957-2094](../GameOS/gameos/gameos_graphics.cpp)).
- GPU-driven: `gos_terrain_indirect.cpp` + `gpu_driven_terrain_solid.comp`
  patches.
- Tessellation: `gos_terrain.tesc` does distance-LOD inner/outer; TES
  already does texture-based displacement along interpolated normal
  via `tc_sampleDisplacement` ([gos_terrain.tese:87-88](../shaders/gos_terrain.tese)).

The TES displacement path is the natural insertion point — the shader
infrastructure for sampling a "displacement" texture already exists.

## 3. Resample location decision

Three options:

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **A. CPU pre-bake** — build a higher-res `PostcompVertex[]` once at mission load, send as new vertex stream | Simple; no shader changes; height parity easy at samples | Doubles/quadruples VBO size; loses dynamic LOD; double-data path is divergent from gameplay grid | **NO** — fights the existing tessellation path |
| **B. GPU height texture** — store source heightfield as a 2D R32F (or R16) texture, sample in TES; resample is bilinear/bicubic at sample time | Single source of truth; tess path unchanged; cheap; supports debug views easily | Needs a texture upload at mission load + per-frame binding; needs careful TES sampler setup; alpha-channel collision with existing displacement texture | **YES (recommended)** |
| **C. Shader-side analytic** — encode height into vertex attribute, resample in TES via Catmull-Rom of neighboring vertex attributes | No new texture | TES doesn't easily access neighbor vertex attributes; would need per-patch SSBO | **NO** — fights tess pipeline |

**Pick: B.** Single R32F (or R16_UNORM with per-mission scale/bias if
memory matters) height texture, sized to source grid (e.g. 128×128
for a 120×120 mission with edge padding for sampler border). TES
samples it as the height authority for visual displacement; legacy
displacement texture (alpha-channel mode in `gos_terrain.tese`) is
either retired or retained as a per-material micro-detail layer
behind a separate gate.

Memory cost: 120² × 4 bytes = 57.6 KB per mission. Trivial.

## 4. Interpolation choice

Phase order:

1. **Bilinear first.** Establishes the substrate, isolates bugs in
   binding + TES sampling. Visually no better than today (in fact
   maybe worse, since GPU bilinear ≠ CPU barycentric).
2. **Catmull-Rom (bicubic, B-spline-style clamp) second**, once
   bilinear is shipping and parity probes pass. Catmull-Rom is the
   default because (a) it passes through control points (preserves
   the zero-error invariant), (b) cheap to implement with 4 bilinear
   taps + cubic weights, (c) no overshoot if implemented with the
   non-overshooting variant (e.g. monotonic cubic for height).
3. **Bicubic / Mitchell** considered later only if Catmull-Rom shows
   visible ringing on the steepest tier1 missions.

NEVER pure bicubic without clamping — it overshoots and breaks the
zero-error invariant at sample sites + introduces invisible-bump
artifacts.

## 5. Scale factor

Recommendation: **2× initially, gated; design for 4× as next step**.

- 2× = 1 inserted sample per source step. Smoothest with least cost;
  most diff vs today is "smoother slopes" not "new geometric detail".
- 4× later, once 2× is stable. Costs 16× the per-frame tess workload
  if we naively raise tess factor — DO NOT do that; let TES interpolate
  the already-2×-uploaded texture and keep tess factor unchanged.
  4× = a new render-height texture upload (4× width, 16× memory; still
  < 1 MB for tier1).
- 8× considered out of scope until 4× has shipped and shows clear
  visual win without breaking the gameplay sync.

## 6. Normals from resampled height

Phase order:

1. **Slice TERRAIN-NORMALS-FROM-HEIGHT-1 (separate from displace)**:
   compute screen-space normal in TES or FS from the resampled height
   texture via central-differences. Debug-only mode at first — light
   the terrain with the new normal under
   `MC2_TERRAIN_DEBUG_MODE = normal_from_height`.
2. **Compare** with current barycentric `PostcompVertex.vertexNormal`
   path. If the comparison shows the height-derived normal matches at
   sample sites and only differs between, ship it under
   `MC2_TERRAIN_NORMAL_FROM_HEIGHT=1` (default OFF).
3. **Default ON** only after baseline captures (Slice 4) + visual
   review approve.

Open question: tangent basis for material normal-maps. Current path
uses world-space tangent reconstruction; height-derived normals will
need a consistent tangent basis to keep `matNormal0..3` blending
correct. Plan to leverage screen-space derivatives of UV in FS
(`dFdx/dFdy`) — robust and well-tested elsewhere.

## 7. Preserving original gameplay height at samples

Two mechanisms layered:

1. **Interpolation choice (§4).** Catmull-Rom passes through control
   points → at original sample positions, sampled height equals source
   height by construction.
2. **Sampler / UV math.** The TES sampler MUST be configured so that
   sample (0,0) in the height texture corresponds to source vertex
   (0,0) in world space. Half-texel offsets are easy to get wrong.
   Mandatory unit test: bind the height texture in a fixture, sample
   at every source vertex position via `texelFetch`, assert
   `bit_equal(sampled, source)`.

If §6 ever drifts (after a future bilinear regression or a sampler-
config change), the parity probe (§9) catches it before commit.

## 8. Unit / building float-or-sink mitigation

Even with the zero-error invariant at samples, the world is
continuous-but-piecewise-linear today and would become
continuous-with-smoothed-curves under resample. Between samples,
visible ground would drift below or above where unit pathing sees the
ground.

Mitigations (layered):

1. **Snap-to-source at gameplay contact.** Units, buildings, and
   placement gizmos read `getTerrainElevation()` which is unchanged.
   They will always be drawn at the gameplay height. Mostly invisible
   between samples because the smooth visual surface is close to the
   piecewise-linear gameplay surface.
2. **Per-vertex zero error** (§7) makes the divergence zero AT the
   sample grid, which is also where buildings tend to be placed
   (snap-to-grid by editor convention).
3. **Visual-only displace, near-contact fade-out.** For units, fade the
   visual displacement to zero within N world units of any moving
   unit's footprint sphere. Cheap: TES (or VS) clamps displacement
   amplitude based on distance to nearest unit (uniform-uploaded list
   of nearby unit positions, or a coarse 2D mask texture). Default
   OFF initially; turn on if user reports float-or-sink artifacts.
4. **Audit grounding** in a follow-up TERRAIN-GROUNDING-AUDIT-1 slice
   that compares draw-time unit Y against `getTerrainElevation()` and
   reports max drift per tier1 mission, before and after resample.

## 9. Debug views required

Adds to the Slice 2 (TERRAIN-DEBUG-VIEWS-1) set:

| Mode | Shows | When useful |
|---|---|---|
| `height_grayscale` | resampled height as grayscale (normalized per-mission) | sanity-check texture upload |
| `height_diff_from_source` | (resampled - source-bilinear) × scale + 0.5 | confirm zero-error at samples + see where resample interpolates |
| `normal_from_height` | shade by central-diff normal of resampled height | normal-from-height slice |
| `normal_legacy_vs_new` | colorize ΔN angle between two normals | proves no surprise relighting |
| `displacement_vector` | vector field of (source vert → displaced vert) magnitudes | confirm visual displace bounds |

All read-only. None alters render path when default mode = 0.

## 10. Gate / env name proposals

Following the static-prop naming family:

| Gate | Default | Effect |
|---|---|---|
| `MC2_TERRAIN_DEBUG_MODE` | 0 | Already proposed in Slice 1; sliced separately in TERRAIN-DEBUG-VIEWS-1 |
| `MC2_TERRAIN_HEIGHT_TEX` | 0 | Upload + bind the render-height texture (no consumption) — first beachhead |
| `MC2_TERRAIN_HEIGHT_TEX_SAMPLE` | 0 | Consume the texture in TES (bilinear) — replaces analytic disp path |
| `MC2_TERRAIN_HEIGHT_TEX_FILTER` | bilinear | string env: `bilinear` / `catmull_rom`; future-proof for §4 phase 2 |
| `MC2_TERRAIN_NORMAL_FROM_HEIGHT` | 0 | Shade using height-derived normal in FS |
| `MC2_TERRAIN_DISPLACE_VISUAL` | 0 | Enable visible-only displace using resampled height; off by default |
| `MC2_TERRAIN_DISPLACE_NEAR_UNIT_FADE` | 0 | Enable §8.3 fade-out near units (only meaningful with DISPLACE_VISUAL=1) |

Each gate registered in `env_registry.txt` (or `tier1_env_vars.md`)
and verified by `check-env-registry.sh`.

## 11. Validation / capture story

Per future slice:

| Slice | Validation gates |
|---|---|
| TERRAIN-RESAMPLE-1 (upload only) | tier1 5/5 PASS; capture_baseline run with `MC2_TERRAIN_HEIGHT_TEX=1` shows byte-identical pixels vs default (proves no consumption); a CPU↔GPU parity probe (texelFetch at vertex positions returns source value) |
| TERRAIN-NORMALS-FROM-HEIGHT-1 | tier1 5/5 PASS; capture for each debug mode; shader_reflect goldens refreshed |
| TERRAIN-DISPLACE-VISUAL-1 | tier1 5/5 PASS with gate OFF (no regression); tier1 5/5 with gate ON (smoke survives); capture both states + diff via existing capture infra; sample-site zero-error probe re-run after change |
| TERRAIN-GROUNDING-AUDIT-1 | report only; no shipping invariant beyond drift bound |

Parity probe shape (debug-only build flag): once per mission load,
walk every source vertex, sample the height texture, assert equality
within `1e-5 × max|elevation|`. Log failures with `(i, j, source, sampled, delta)`.
Fail loud in tier1 smoke.

## 12. Out of scope (explicit anti-goals)

- Changing gameplay height anywhere.
- Changing pathfinding, collision, AI sensing, unit placement, LOS,
  cursor pick, or shadow projection logic.
- Per-tile terrain object identity / picking ID.
- Mech / static-prop / VFX changes.
- Modifying `PostcompVertex` layout or `MapData::save()` (no on-disk
  format changes).
- New shadow technique / CSM work.
- New compute pass / render-graph rewrite. The resample plan
  intentionally re-uses the existing tessellated terrain path.
- KTX / cooker pipeline expansion.
- Default-ON flips without explicit user approval after capture
  review.

## 13. Recommended slice order

1. **TERRAIN-DEBUG-VIEWS-1** (Slice 2) — substrate for visual review.
   Owned by the other session if they want; otherwise next.
2. **TERRAIN-HEIGHT-AUDIT-0 SCRIPT** — script-author follow-up to
   Slice 3 recon, paired with an engine-side dump validator.
3. **TERRAIN-BASELINE-0** (Slice 4) — must run AFTER Slice 2 to
   capture the debug modes, AFTER §10 envs added to `CAPTURE_ENV_KEYS`.
4. **TERRAIN-RESAMPLE-1** — upload-only. Gate `MC2_TERRAIN_HEIGHT_TEX`.
5. **TERRAIN-NORMALS-FROM-HEIGHT-1** — debug mode visual only.
6. **TERRAIN-DISPLACE-VISUAL-1** — visible displace, gated OFF.
7. **TERRAIN-GROUNDING-AUDIT-1** — drift report.
8. **TERRAIN-IBL/LIGHTING-1** — lighting work only after geometric
   basis is sane (per orchestrator spec).

Each carries its own diff-ready halt + reviewer dispatch + commit.

## 14. Open questions for the user

1. R32F vs R16_UNORM for the render-height texture — happy with R32F
   (57.6 KB / mission) for simplicity, or do we want the smaller
   format from day one?
2. Is the legacy `gos_terrain.tese` alpha-channel displacement path a
   per-material micro-detail layer (retain behind gate) or should
   TERRAIN-RESAMPLE-1 replace it outright?
3. Is the §8.3 near-unit fade design acceptable, or do you want a
   different mitigation if float-or-sink artifacts appear?
4. Confirm the env-name family (`MC2_TERRAIN_*`) before
   TERRAIN-DEBUG-VIEWS-1 starts — once landed, renaming is churn.

---

**Status:** plan-only artifact. No code, no shader, no shipping
visual change. Slice 5 of approved batch (TERRAIN-RESAMPLE-PLAN-0).
Implementation slices (TERRAIN-RESAMPLE-1 onwards) require explicit
user approval per orchestrator spec.
