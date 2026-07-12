# MECH-VIEWUNIFORMS-BINDING-DIAG-1 — Diagnosis

Diagnoses why the reverted MECH-VIEWUNIFORMS-1-PRE migration made mechs
disappear on the default UBO path while `MC2_VIEW_UNIFORMS=0` (legacy
`u_worldToClipGL` uniform) rendered them correctly.

**Verdict: the GL binding and the matrix are NOT the cause. The failure was
shader/program-side in the reverted mech.vert UBO consumer.** Fix options that
re-bind or re-order the UBO (A/B below) are ruled out by direct measurement.

## Method

A gated, read-only probe (`MC2_MECH_VIEWUNIFORMS_DIAG=1`, default OFF) was
added to `GpuMechBatcher::flush()` at the matrix-upload site
(`gos_mech_batcher.cpp`, right after the `gos_GetTerrainMVPMat4()` upload).
Per frame (rate-limited: first 5 + every 300th) it logs, AT MECH FLUSH TIME:

- `glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 3)` — the buffer bound at
  binding point 3.
- the binding-3 UBO's first `mat4` (read via `glGetBufferSubData`, generic
  `GL_UNIFORM_BUFFER` target saved/restored) — i.e. `ViewUniforms.worldToClipGL`.
- `gos_GetTerrainMVPMat4()` — the matrix the mech path actually uploads to the
  legacy uniform.
- `max_diff` between the two over all 16 floats.

Run on the current (legacy, mech-rendering) binary so the probe reports the
true frame state without the broken shader in the way.

## Result (mc2_24, MC2_MECH_VIEWUNIFORMS_DIAG=1, MC2_SNAPSHOT_MECH_EXTRACT=1)

```
[MECH_VU_DIAG v1] frame=1 submits=6 binding3_ubo=2391 max_diff_vs_terrainMVP=0.000000 ubo_row0=[2.706 -0.365 -0.000 -0.142] mvp_row0=[2.706 -0.365 -0.000 -0.142]
... frames 2–5 identical ...
```

- **binding3_ubo = 2391** (nonzero, stable) → the ViewUniforms UBO IS bound at
  binding point 3 at mech flush time, every frame.
- **max_diff = 0.000000** → its `worldToClipGL` is byte-identical to
  `gos_GetTerrainMVPMat4()`, which is exactly what the mech vertex path needs
  (the in-shader MC2→GL axis swap is applied to the world position; the matrix
  is the same one the legacy uniform receives, confirmed F1-3C-style here for
  the mech pass specifically).
- submits = 6 → mechs are submitted and the probe runs on the real draw path.

## Interpretation

Cross-referenced with the read-only frame-order recon (which found only
`view_uniforms_gl.cpp` ever writes `GL_UNIFORM_BUFFER` binding 3 — once per
frame at the top of `GameCamera::render()` — and nothing clobbers it before
`renderLists()`), this proves:

1. binding=3 is live at `GpuMechBatcher::flush()` (txmmgr.cpp:2176), exactly as
   it is for the static-prop flush one line earlier (txmmgr.cpp:2165).
2. The UBO content is the correct, current matrix for the mech pass.

Therefore, when the reverted mech.vert read `u_worldToClipGL` from the
anonymous `ViewUniformsBlock`, the value available to it was correct. The
mechs-off-screen symptom was **not** caused by a dead binding, a stale matrix,
a clobbered binding point, or a pass-order problem. The defect was in how the
reverted shader/program consumed that UBO.

### Ruled out

- **(A) Explicitly bind ViewUniforms UBO before mech flush** — useless;
  binding 3 is already bound with the correct buffer (2391).
- **(B) pass-local `ensureViewUniformsBound()`** — useless for the same reason.
- **Matrix staleness / wrong-pass capture** — `max_diff=0` disproves it.
- **binding-point clobber between upload and mech flush** — recon found no
  other binding-3 writer; probe confirms 2391 is still there.

### Still open (the real cause — shader/program-side)

The reverted consumer mirrored static_prop's pattern (`#include
view_uniforms.hglsl` under `#ifdef MC2_USE_VIEW_UNIFORMS`; legacy uniform under
`#ifndef`; batcher injects the define). reflect.py compiled the
`mech.vert [viewuniforms]` variant clean. Yet mechs vanished. With binding +
matrix proven correct, the remaining candidates are narrow and shader-side:

- the anonymous `ViewUniformsBlock` in the mech program resolved to a block
  binding other than 3 (driver default 0) — i.e. the `layout(binding=3)`
  qualifier did not take effect for the mech program as it does for
  static_prop, so the shader read a zero/garbage matrix despite UBO 2391 being
  bound at point 3;
- a redeclaration / name-resolution interaction with mech.vert's other
  includes (`lighting.hglsl`, scene block) that does not occur in
  static_prop.vert;
- the injected define / `#version` / include-order differing from static_prop
  in a way glslangValidator (used by reflect.py) tolerates but the engine's
  runtime GL compiler does not.

## Recommended next slice (the actual fix)

A focused, **gated** shader-consumer probe (do NOT flip default): re-apply the
mech.vert UBO consumer behind a NEW debug gate (e.g. `MC2_MECH_VIEWUNIFORMS=1`,
default OFF, independent of the global `MC2_VIEW_UNIFORMS`), and at program
link time log:

```
GLuint idx = glGetUniformBlockIndex(mechProgram, "ViewUniformsBlock");
GLint bind = -1;
if (idx != GL_INVALID_INDEX) glGetActiveUniformBlockiv(mechProgram, idx, GL_UNIFORM_BLOCK_BINDING, &bind);
```

Expected on a correct setup: `idx != GL_INVALID_INDEX` and `bind == 3`. If
`bind == 0` (or idx invalid), the fix is **option D** — call
`glUniformBlockBinding(mechProgram, idx, 3)` after link (or confirm the
anonymous-block `layout(binding=3)` is honored), NOT a bind/order change. Then
re-validate with a visual mech_24 capture (mechs must render on the gated path)
before any default flip.

## Constraints honored

Diagnostic only. Probe is gated default-OFF (zero effect on the default render
path — default mechs use the unchanged legacy `u_worldToClipGL` upload). No
shader change, no default flip, no gameplay/skinning/animation/shadow change.
New env var `MC2_MECH_VIEWUNIFORMS_DIAG` registered in `check-env-registry.sh`.
