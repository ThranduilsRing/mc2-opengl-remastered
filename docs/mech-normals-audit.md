<!-- doc-status: stale | reviewed:2026-10-11 -->
# MECH-NORMALS-AUDIT-0

Why the Mad Cat looks "too smooth" and the mech normal debug view shows a
"rainbow pattern that doesn't follow geometry." Recon/diagnostic only — no code
changed in this slice. Decides where the fix belongs.

Two read-only recon passes (source/cook/load + shader/skinning). HEAD `64b6d40d`.

## Verdict (one line)

The mech normals are corrupted at **mesh load**: `TG_TypeShape::LoadTGShapeFromASE`
averages all per-face vertex normals that share a vertex *index*, ignoring
3DS-Max smoothing groups — collapsing hard-edge normal splits into a single
blended (often near-garbage) normal. The shader faithfully renders the bad
normals. Recommended first fix = **C/D, mech-local** (recompute/split mech
normals in the GPU-mech build path), NOT the shared TGL loader, NOT the shader.

## Evidence

### Source / load (primary)
- `GpuMechVertex.normal` is a full `float[3]` (not packed), `gos_mech_batcher.h:15`.
  `tangentOct` is zero-filled for stock ASE (no normal-map data).
- The batcher fills it by `memcpy` from `TG_TypeVertex::normal`
  (`gos_mech_batcher.cpp:~821`), expanding to **triangle soup** (one vertex per
  triangle-corner, no dedup/weld, no meshoptimizer).
- `TG_TypeVertex` DOES carry a `normal` (`mclib/tgl.h:34-41`). It is loaded from
  the ASE's `*MESH_FACENORMAL` + `*MESH_VERTEXNORMAL` records, NOT computed from
  geometry.
- **The bug:** `LoadTGShapeFromASE` (`mclib/tgl.cpp:~1349-1424`) accumulates every
  `*MESH_VERTEXNORMAL` for a given vertex **index** into
  `listOfTypeVertices[idx].normal`, counts them, then divides + normalizes — a
  naive arithmetic average across ALL faces touching that index, with **no
  smoothing-group awareness**. The loader's own comment (`tgl.cpp:952-958`)
  acknowledges Max writes a separate normal per face-instance and "averages
  together" — that averaging is exactly what destroys hard edges.
- Empirical: in `mc2srcdata/tgl/madcat.ase`, a single shared body vertex carries
  near-orthogonal normals across its faces (e.g. `(0,0,-1)`, `(0,-1,0)`,
  `(-0.99,0.13,0)`); their average is a tiny vector that renormalizes to a
  direction unrelated to any real surface.
- The Mad Cat uses this stock ASE path exclusively. Track D (Assimp/GLB,
  `aiProcess_GenSmoothNormals`) is gated off (`ENABLE_ASSIMP_IMPORTER` not built)
  and does not affect it.

### Shader / skinning (secondary)
- `mech.vert:148` transforms the normal with `mat3(boneT)` — the raw upper-3×3,
  **not** the inverse-transpose normal matrix. Correct only if `boneT` is
  orthonormal (pure rotation). If any mech bone carries non-uniform scale/shear,
  normals are skewed. Mech bones are believed rigid (rotation+translation), so
  this is a **secondary** factor — verify before relying on it.
- Axis swap `(-x, z, y)` is applied consistently to position (`:147`) and normal
  (`:149`). Normalize is applied in the VS (`:149`) and re-applied per-fragment
  in the FS for both the debug view (`mech.frag:79`) and the GBuffer1 write
  (`mech.frag:87`). No missing-normalize bug.
- The debug view (`u_debugMode==4`, `mech.frag:79`) shows the **smooth-interpolated
  skinned world normal** (`v_normal` has no `flat` qualifier). A smooth gradient
  ("rainbow") across faces is the EXPECTED look for smooth/averaged normals — it
  is not itself a shader bug. The pathology is that the underlying averaged
  normals are wrong, so the gradient sweeps in directions that don't track the
  surface.
- `MC2_MECH_VIEWUNIFORMS` only changes the clip matrix; it does **not** touch the
  normal path. Normals are unaffected by the recent ViewUniforms slice.

## Required answers

- **Are source/cooked normals bad?** YES — primary cause. ASE per-face normals
  are over-averaged at load (smoothing groups ignored), producing blended/garbage
  per-vertex-index normals.
- **Is the shader/skinning transform bad?** Partially — `mat3(boneT)` is not the
  inverse-transpose; only matters if bones scale (likely not). Secondary, verify.
- **Is normal packing/unpacking bad?** NO — `normal` is a plain `float[3]`,
  copied verbatim; no pack/unpack involved.
- **Are duplicate vertices preventing smoothing?** NO — the opposite. There is no
  smoothing-group split; shared-index normals are *over*-merged across hard edges.
  (The GPU side is triangle soup, but the averaging already happened upstream by
  index in TGL.)
- **Should normals be recomputed during import/cook?** YES — the correct normals
  must respect smoothing groups (split the normal at hard edges, keep it shared
  across smooth faces), or be regenerated with an angle threshold. The ASE already
  contains the correct per-face normals; the loader throws that fidelity away.
- **Should meshoptimizer run after normal generation?** Not relevant here — no
  meshoptimizer in the mech path, and that is not the cause.

## Recommended first fix

**C/D, scoped MECH-LOCAL (do NOT touch the shared TGL loader yet):**

`LoadTGShapeFromASE` is shared by ALL TG shapes (mechs, buildings, props, trees) —
changing its averaging has a large blast radius and is out of scope here. The
GPU-mech build path (`gos_mech_batcher.cpp registerTypeLod`) already builds
triangle soup, so the fix can live there, mech-only:

1. **Confirm first (mandatory, cheap):** the per-triangle `faceNormal`
   (`listOfTypeTriangles[].faceNormal`, `mclib/tgl_parse.cpp:363`) is correct per-face.
   Temporarily write `tri.faceNormal` to all three corners in the batcher and
   capture the mech normal debug view. If it shows clean per-face facets (no
   rainbow garbage), the index-averaging diagnosis is confirmed.
2. **Then choose the ship fix (mech-only, gated default-OFF):**
   - **D — angle-threshold smooth normals:** in the batcher, recompute per-corner
     normals from the triangle soup by averaging adjacent face normals only where
     the angle between faces is below a threshold (e.g. 45–60°), preserving hard
     edges. This reproduces the original Max smoothing-group intent without
     touching TGL.
   - **C-lite — preserve ASE per-corner normals:** alternatively, plumb the ASE's
     per-face-per-vertex normals through to the batcher instead of TGL's averaged
     per-index normal (requires TGL to retain them — larger change).
   - Interim acceptable: ship the faceted `faceNormal` path gated default-OFF; it
     is geometrically correct (just hard-shaded) and unblocks lighting work.

**A (shader inverse-transpose)** is a cheap, low-risk hardening but is NOT the
primary cause; apply only after confirming bones carry scale.
**E (defer)** is not recommended — mech lighting (MECH-AMBIENT-1) should not
proceed on these normals.

## NEEDS-VERIFY (for the fix slice)
- Runtime dump of `TG_TypeVertex::normal` for the madcat body node (confirm
  averaged-garbage directions) — or the faceNormal-to-corners capture in step 1.
- Whether any mech bone `LinearMatrix4D` carries non-uniform scale (decides if
  fix A matters).
- Blast-radius check: confirm the chosen fix is mech-local and leaves static-prop/
  building normals (same TGL loader) untouched.

## Constraints honored
Recon/diagnostic only; no code changed, no build required. No default/visual,
gameplay, animation/skinning, shadow, PBR/ambient, texture-cook, or meshoptimizer
changes. The recommended fix is explicitly scoped mech-local and gated, deferring
any shared-TGL or cook change to separate approval.
