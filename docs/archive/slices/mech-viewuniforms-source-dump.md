# MECH-VIEWUNIFORMS-SOURCE-DUMP-1 — Root Cause

## TL;DR

The mech ViewUniforms migration kept "failing" (mechs vanished, then the
runtime program appeared to lack `ViewUniformsBlock`) for one reason:
**the edited shaders were never deployed.** The deploy step used during the
investigation copied only `mc2.exe`; the engine loads `shaders/*` from the
**deploy directory** at runtime, so every shader edit ran against a stale,
day-old `shaders/mech.vert`. Once `shaders/` is synced to the deploy dir, the
gated mech ViewUniforms consumer works correctly with **no** code fix beyond
the consumer itself.

## How the source dump proved it

A gated dump (`MC2_MECH_SHADER_SOURCE_DUMP=1`) of the exact `{prefix, body}`
strings passed to `glShaderSource` for `mech.vert` showed:

- PREFIX (`strings[0]`) = `#version 430\n#define MC2_OBJECT_ID_BUFFER 1\n#define MC2_USE_VIEW_UNIFORMS 1\n` — the define **was** present.
- BODY (`strings[1]`) = the **original** mech.vert: legacy `uniform mat4 u_worldToClipGL;` **unguarded**, and **no** `#ifdef MC2_USE_VIEW_UNIFORMS`, no `view_uniforms.hglsl` include, no `ViewUniformsBlock`.

The body had none of the edits. Confirmed directly:

```
deploy  shaders/mech.vert : 0 matches for MC2_USE_VIEW_UNIFORMS  (mtime: day-old)
worktree shaders/mech.vert : 5 matches
```

The engine's `glsl_load` reads `shaders/mech.vert` relative to the process CWD
(the deploy dir). `cp mc2.exe` alone never updates it.

## Why each earlier symptom now makes sense

- **MECH-VIEWUNIFORMS-1-PRE "mechs vanished":** the new binary skipped the
  legacy `u_worldToClipGL` upload (believing the shader used the UBO), but the
  **stale** deployed shader still had the unguarded legacy uniform and needed
  that upload → the uniform stayed zero → every mech transformed off-screen.
  Not a binding bug — a **zero matrix**.
- **DIAG-1 "binding=3 live + matrix correct, mechs still gone":** true — the
  binary uploads the ViewUniforms UBO regardless — but the stale shader never
  consumed it.
- **BLOCKBINDING-1 "ViewUniformsBlock absent / bound to 0":** the stale shader
  had no block at all (`active_uniform_blocks=1` = `SceneData` only). The
  "bound to 0" hypothesis was an artifact of a block-less shader.

## After deploying `shaders/` (decisive re-test, mc2_24, MC2_MECH_VIEWUNIFORMS=1)

```
block[0] name='ViewUniformsBlock' binding=3
block[1] name='SceneData'         binding=1
vu_block_idx=0  pre_binding=3  post_binding=3  loc_u_worldToClipGL=-1  active_uniform_blocks=2
[VIEW_UNIFORMS v1] compare max_diff=0.000000 ok=1
```

- `ViewUniformsBlock` present at **binding 3** (legacy uniform gone → define effective).
- `pre_binding=3` **before** any explicit bind → the GLSL `layout(binding=3)`
  qualifier **is honored** (GL 4.2+ core; same as static_prop). **Option D
  (`glUniformBlockBinding`) was never necessary** and was dropped.
- The matrix the mech reads from the UBO is byte-identical (`max_diff=0`) to the
  correct MVP, so the gated path is a true no-visual-change migration.

## Durable lesson (process)

**A manual `cp mc2.exe` is an incomplete deploy for any shader-touching change.**
Shaders are runtime-loaded from the deploy dir; edits must be synced there
(`cp -r shaders/* <deploy>/shaders/`, or use the `/mc2-deploy` skill which
handles this). Validating a shader change after a binary-only deploy silently
tests the *old* shader — and (worse) a binary change that assumes the new
shader (e.g. skipping a uniform upload) will actively break against the stale
shader. Always confirm a shader edit reached the deploy dir before drawing
conclusions, and prefer visual/object evidence over smoke for shader changes.

## Outcome

Root cause = deploy gap. The gated mech ViewUniforms consumer
(`MC2_MECH_VIEWUNIFORMS=1`, default OFF) is correct and verified at GL-state
level once shaders are deployed; `glUniformBlockBinding` and the source-dump
scaffold were removed as unnecessary. The MC2_MECH_VIEWUNIFORMS_DIAG probe
(binding-3 + matrix at flush, block-presence at link) is retained for future
work.
