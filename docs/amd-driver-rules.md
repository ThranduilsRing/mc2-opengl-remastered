# AMD Driver Rules (RX 7900 XTX, driver 26.3.1)

Discovered through extensive debugging. MUST be followed.

- **Attribute 0 must be active** -- AMD skips draws if vertex attrib 0 isn't enabled. Add layout(location = 0) with a dummy read.
- **Explicit gl_FragDepth** -- AMD optimizes away "empty" fragment shaders. Write gl_FragDepth = gl_FragCoord.z for depth-only passes.
- **Dummy color attachment on depth-only FBOs** -- AMD may not rasterize without a color attachment. Add a small R8 color texture.
- **No texture feedback loops** -- unbind shadow texture from sampler unit 9 before rendering to shadow FBO (same texture as depth attachment). Re-bind after.
- **Matrix transpose: GL_FALSE for direct upload** -- deferred system uses GL_TRUE. Shadow shader uses direct glUniformMatrix4fv(..., GL_FALSE, ...). Never mix.
- **Deferred vs direct uniforms** -- setFloat/setInt BEFORE apply(). Direct glUniform* AFTER apply() (which calls glUseProgram). drawIndexed() calls apply() internally.
- **material->end() deactivates shader** -- In multi-batch loops, end() calls glUseProgram(0). Must re-apply() and re-upload all direct uniforms each batch.
- **draw_screen() timing** -- Runs BEFORE gamecam.cpp sets camera/light values each frame. Shadow matrix uses previous frame's values. First ~240 frames have zero camera pos.
- **sampler2DArray crashes** -- NEVER use. Use individual sampler2D on units 5-8 instead.

## Tested-and-refuted claims

- **MRT location=1 corruption from non-terrain shaders.** A comment at `GameOS/gameos/gos_postprocess.cpp:519-520` warns *"AMD RX 7900 corrupts color output if non-terrain shaders write location=1."* **Tested 2026-04-27 via F3 canary** (`docs/superpowers/specs/render-contract-f3-canary-report.md`): added `layout (location=1) out vec4 GBuffer1` to `gos_tex_vertex_lighted.frag` and wrote `rc_gbuffer1_screenShadowEligible(normalize(Normal))`. **No corruption observed** across 5–6 missions including one full mission completion on AMD RX 7900 XTX, driver 26.3.1. The comment is treated as stale; non-terrain shaders may declare and write attachment 1 freely. The comment itself can be removed in a post-F3 cleanup.
