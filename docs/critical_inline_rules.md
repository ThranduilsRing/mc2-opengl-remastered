<!-- doc-status: current | reviewed:2026-10-11 -->
# Critical inline rules (every session reads these)

> s68/P9: Retired GL sources are cited with Git object syntax (`8064c6930:path`) so the historical implementation remains inspectable.

Extracted from CLAUDE.md 2026-05-24. **Authoritative.** CLAUDE.md links here.

Rules are organized loosely by category but every rule applies whenever the
matching condition is met. Read them all before touching anything.

## Style

- **No emoji in any file, ever.** Unicode punctuation (em/en-dash, ellipsis) OK; pictographic emoji not. Full scope: `memory/feedback_no_emoji_in_files.md`.
- **No wall-clock time projections.** Describe complexity in code dimensions (subsystems, files, parity gates, soak windows), not time.

## Grounding (every cited fact)

- **Grep before citing file:line.** Every file:line in any output must be verified at write-time. Symbols stable; line numbers drift. Rationale: `memory/brainstorm_code_grounding_lesson.md`. Plan-stage: `.claude/skills/adversarial-plan-review.md`.
- **Negative claims need opposite-direction grep.** "X is NOT consumed by Y" requires grep'ing Y, not the obvious-named consumer. See `memory/feedback_data_flow_audit_asymmetry.md`.

## Build / deploy

- **Build:** ALWAYS `--config RelWithDebInfo`. Release crashes with `GL_INVALID_ENUM`.
- **Full relink before deploy** when load-bearing functions change: `rm build64/RelWithDebInfo/mc2.exe` (+ changed `.obj`) before `cmake --build`, or `--clean-first`. Incremental leaks stale linkage when inline funcs / templates / static state change. Class-layout changes: `memory/feedback_class_layout_change_needs_clean_first.md`.
- **Deploy:** NEVER `cp -r`. ALWAYS `cp -f` per file + `diff -q`. `cp -r` silently fails on Windows/MSYS2.
- **Deploy ONLY to the 5 canonical deploy folders** (`mc2-win64-v0.4` / `v0.4c` / `0.4c` / `abl-validate` / `v0.3` under `A:/Games/mc2-opengl`). `deploy_payload.py` ENFORCES this (`DEPLOY_ALLOWLIST` + `--allow-new-target` override). **NEVER `cp -r` a full install to a per-lane folder** (`mc2-<lane>-test`) — each is ~5GB and bloats `A:/Games/mc2-opengl` by tens of GB. Pick a FREE canonical folder (a sibling may hold the lease) + md5-verify after. New install / release cut = deliberate `--allow-new-target`.
- **Shaders deploy in lockstep with exe.** Any slice touching a shader MUST redeploy the shader tree, not just mc2.exe. See `memory/shader_exe_deploy_lockstep.md`.
- **Parallel build+smoke + deploy-dir/worktree map + verification tooling:** `docs/build-parallel-and-tooling.md`.
- **Git:** NEVER push to `alariq/mc2` origin. All work is local.

## GPU resource invariants (S2 NVIDIA hardening — enforced)

Four rules. No exceptions for new code.

- **New SSBO range bind:** use `gpuBindSsboRange(index, buf, offset, size, "tag")`. Raw `glBindBufferRange(GL_SHADER_STORAGE_BUFFER, ...)` is forbidden in new code. Enforces offset alignment (`GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT`) that NVIDIA rejects and AMD silently tolerates. `gos_gpu_sync.h`.
- **New render pass entry:** call `render_contract::assertPassContract(PassIdentity::X, "hint")` after the pass's fixed-function state block. Aborts under `MC2_RENDER_CONTRACT_ASSERT=1` on depth-func / blend mismatches. `mclib/render_contract.h`.
- **New compute/copy/readback edge:** add a named row to `barrierBitsFor` in `gos_gpu_sync.cpp`, then call `gpuSyncBarrier(producer, consumer, "tag")`. No raw `glMemoryBarrier` in new code. If the edge is already in the table, just call the wrapper. If it's not, add the edge first — unmapped edges fall back to `GL_ALL_BARRIER_BITS` with a loud stderr warning.
- **New shader variant:** must compile with `[SHADER WARN]` clean (zero lines logged from `glGetShaderInfoLog` / `glGetProgramInfoLog` on success). NVIDIA emits warnings for unused uniforms, precision mismatches, and deprecated builtins that AMD logs nothing for. Treat them as errors. `8064c6930:GameOS/gameos/utils/shader_builder.cpp`.

## Shaders / GL

- **Shader `#version`:** Never in shader files. Pass `"#version 430\n"` as prefix to `makeProgram()` (4.3 for SSBO / std430).
- **Uniform API:** `setFloat` / `setInt` BEFORE `apply()`, direct `glUniform*` AFTER. `apply()` flushes dirty uniforms.
- **GL_FALSE for terrainMVP:** direct-uploaded row-major matrices use `GL_FALSE`. Material cache uses `GL_TRUE`. The `gamecam.cpp` comment claiming `GL_TRUE` is wrong; do not "fix" it.
- **Explicit-program uniform upload:** any GOS API or helper that takes a `GLuint program` and calls `glGetUniformLocation(program, ...)` MUST upload via `glProgramUniformMatrix4fv(program, loc, ...)` (or its sibling for the type). `glUniformMatrix4fv(loc, ...)` uploads to the currently-bound program (`glUseProgram` state), NOT the named one — silent wrong-shader upload bug. Engine uses GL 4.5; the explicit-program family is unconditionally available. Full: `memory/glprogramuniform_vs_gluniform_explicit_program_trap.md`.
- **GLSL macros do NOT inherit C++ build flags.** `-DMY_FLAG` in `CMAKE_CXX_FLAGS` reaches only `.cpp` compilation. To gate a GLSL `#ifdef`, extend the `makeProgram()` prefix at C++ level: `prefix += "#define MY_FLAG 1\n"` inside `#ifdef MY_FLAG`. Verify by dumping compiled shader source before `glCompileShader`. Full: `memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md`.
- **Shader hot-reload fails silently:** bad compile = old shader stays active. Always check console after editing.
- **Resolution two-space contract:** `Environment.screenWidth/Height` is the LOGICAL 800x600 legacy-HUD canvas (clamped while `g_hudResClampEnabled`, default true — maintained at BOTH the `gameosmain.cpp` boot clamp AND the `gos_SetScreenMode` clamp; both must survive). `Environment.drawableWidth/Height` is the PHYSICAL GL drawable (written only via `graphics::get_drawable_size`). Modern render code (RenderCore/RenderWorld/GameAdapters/GuiRuntime/EditorBridge/HostServices/shaders) NEVER reads `Environment.screenWidth/Height` — use `Environment.drawableWidth/Height`, `gos_GetEnvironmentDrawableSize()`, or `gos_ComputeHudCanvasBox()`. The perspective frustum aspect equals the DRAWABLE aspect (unless `MC2_CAMERA_ASPECT_NATIVE=0` or inside the mech-preview FBO). Enforced: `scripts/check-res-aspect-contract.py` (zero-tolerance modern dirs + legacy-count ratchet + 4 anti-revert pins) and the `MC2_RES_ASSERT` runtime audit (`RenderCore/ResolutionContract.h`).

## Architecture / change discipline

- **Vulkan-prep:** new GPU-resource code uses explicit device-mediated binding (`device.bindVertexBuffer(vb)`, NOT `vb.bind()`); assume zero implicit cross-call GL state. PREP not a port. Full: `memory/vulkan_prep_explicit_device_discipline.md`.
- **Change discipline:** don't touch what you don't have to (every touch has blast radius); when you must, bring it to modern standard. Standalone cleanup slices need a blocking/debt justification. Full: `memory/minimal_touch_modern_when_touched.md`.
- **C++17 features:** project standard is C++17 since 2026-05-24 (commit `5c03835`). For which features are allowed / cautioned / avoided, see `docs/cxx17-coding-rules.md`. Rule: language upgrade != feature spree; minimal touch + measured wins.
- **Frame-currentness contract (review checklist, not a framework):** an optimization may skip expensive work, but it may NOT skip the cheap current-frame liveness stamp a downstream consumer needs to claim the object is current. When adding any per-frame `continue` / early-return that bypasses `update()`/`touch()`, ask: what stamp does this skip also drop (`cachedFrame_`, watch-ID currentness, animation advance), and who trusts it? The proven instance + counter pattern is R2B-STATIC-NATURAL-TOUCH-PRESERVE-1 (`07a1f8ac`); audits + guards in `docs/frame-contracts/`. AMD often tolerates a stale state NVIDIA drops — so surface the invalid contract with a consumer-boundary counter (e.g. `persistent_vanish`) even when the symptom is vendor-confined.

## Editor discipline

- **Editor is a GPU-only test bed.** `EditRel.exe` renders ONLY the 3D world. It has no menus, FMV cinematics, briefing maps, or mode-switch state machines — all legacy CPU paths in `mc2.exe` exist for those other modes, and the editor needs none. Adding a CPU fallback to editor TUs (or forcing the editor onto a legacy path via env gate) is wrong-direction and must be rejected at review time. Correct fix for "X breaks in editor" is to make X work on the GPU path the game uses. 2D UI overlays (selection brackets, brush gizmos, BuildingLink arrows, ImGui, MFC widgets) are NOT in scope of this ban — those are immediate-mode UI, same category as the game's HUD. Full rationale + historical context + env-gate polarity table: `memory/editor_is_gpu_only_testbed.md`.
