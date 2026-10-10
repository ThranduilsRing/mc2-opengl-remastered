<!-- doc-status: current | reviewed:2026-10-11 -->
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
- **sampler2DArray** -- Historical AMD caution. Synthetic RGBA8 `sampler2DArray` canary passed on RX 7900 XTX (driver 26.3.1) in both menu and mid-mission states (Canary A, see `docs/superpowers/explorations/2026-04-27-amd-sampler2darray-canary.md`). Do **not** use for production terrain colormaps until Canary B verifies real terrain texture / mip / sampler parity (real-tile-into-layer-0 visual A/B vs. legacy `sampler2D` path). Until that gate is cleared, keep individual `sampler2D` per material on units 5-8 for terrain. The original "NEVER use" wording is superseded by Canary A.

## C1b indirect draw barrier canary (Track C slice C1b, 2026-05-07)

**Q12 contract — first `GL_COMMAND_BARRIER_BIT` in the engine.**

### What was verified

C1b introduces the first `GL_COMMAND_BARRIER_BIT` usage in the codebase. The dispatch sequence is:

1. `glClearNamedBufferSubData` — reset per-bucket counts + actorVisBits + blockVisBits
2. `glDispatchCompute` (C1b cull shader) — writes `bucketCountData[]` + `visibleIds[]` + `actorVisBits[]`
3. `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)` — counts ready for patch dispatch
4. `glDispatchCompute` (patch shader) — copies `bucketCountData[b]` → `cmds[b].instanceCount`
5. `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT)` — indirect commands ready for draw
6. `glDispatchCompute` (rollup shader) — OR per-actor visibility into `blockVisBits[]`
7. `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)` — block visibility ready for C3 consumers

### Initial testing notes (2026-05-07)

**TODO:** Capture one frame at wolfman zoom on AMD RX 7900 XTX 26.3.1 using RenderDoc or apitrace.
Verify:
- `cmds[bucket].instanceCount` matches `bucketCountData[bucket]` after the patch barrier (step 5)
- The indirect draw consumes the correct `instanceCount` values per type
- No TDR / GPU hang from the barrier sequence
- `[GPU_CULL v1] event=indirect_draw overflow=0` across mc2_01

**Important:** In the current C1b shadow mode implementation, the batcher OVERWRITES the GPU-written `instanceCount` with CPU-computed values (via `glBufferSubData` in flush()) AFTER the patch dispatch. This means:
- The patch dispatch runs and writes GPU-culled counts (as a validation step)
- The batcher then overwrites them with CPU-agreed counts (visual correctness)
- The effective draw is CPU-authoritative until C1c enables GPU render authority

This two-phase write means the AMD barrier canary verifies only that the patch dispatch COMPLETED before the draw (not that its values are used for visibility decisions). Full barrier authority test requires C1c.

### Prohibitions verified (Q12 contract)

- `GL_ATOMIC_COUNTER_BARRIER_BIT` is NOT used — `atomicAdd` operates on SSBO-resident `uint`, not ACBOs. Using `GL_ATOMIC_COUNTER_BARRIER_BIT` here would be a cargo-cult error.
- No CPU readback (`glGetBufferSubData`) on the indirect command buffer mid-frame.
- Counters are SSBO `uint` + `atomicAdd` only.

## Tested-and-refuted claims

- **MRT location=1 corruption from non-terrain shaders.** A comment at `GameOS/gameos/gos_postprocess.cpp:519-520` warns *"AMD RX 7900 corrupts color output if non-terrain shaders write location=1."* **Tested 2026-04-27 via F3 canary** (`docs/superpowers/specs/render-contract-f3-canary-report.md`): added `layout (location=1) out vec4 GBuffer1` to `gos_tex_vertex_lighted.frag` and wrote `rc_gbuffer1_screenShadowEligible(normalize(Normal))`. **No corruption observed** across 5–6 missions including one full mission completion on AMD RX 7900 XTX, driver 26.3.1. The comment is treated as stale; non-terrain shaders may declare and write attachment 1 freely. The comment itself can be removed in a post-F3 cleanup.
