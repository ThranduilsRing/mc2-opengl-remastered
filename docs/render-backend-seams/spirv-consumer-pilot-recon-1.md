<!-- doc-status: stale | reviewed:2026-10-11 -->
# SPIRV-CONSUMER-PILOT-RECON-1

**Status:** RECON COMPLETE · read-only · doc-only commit to nifty (base `05740404`).
**Worktree:** `…/worktrees/nifty-mendeleev` @ `claude/nifty-mendeleev`. All `file:line` re-grepped this session.
**Question this slice answers:** *Can the engine consume a precompiled SPIR-V shader artifact without breaking hot-reload / the runtime-GLSL path?* (Not "can we enumerate variants?" — that's banked: [shader-permutation-inventory](shader-permutation-inventory.md) = GO.)

## VERDICT: **GO (consumer pilot is feasible)** — gated on one prerequisite

The capability is present and the seam is clean. A SPIR-V consumer can be added **alongside** the GLSL path with the GLSL hot-reload fallback preserved *by construction*. **One hard prerequisite blocks an actual pilot:** the `.spv` must come from a **non-auto-map offline compile** — reflect.py's `.spv` is reflection-only and NOT runtime-usable (it auto-maps bindings; the engine relies on explicit `layout(binding=)`). That offline compile is the next slice (`OFFLINE-SHADER-VARIANT-BUILD-1`). So: **GO to design the consumer; it cannot run until the offline bake exists.**

## Answers to the 7 recon questions

### 1. Does the target support `GL_ARB_gl_spirv` / `glSpecializeShader`? — CONDITIONAL YES (runtime-detect)
- Context **requested = GL 4.3 Core** (`gos_render.cpp:190-196`): `SDL_GL_CONTEXT_PROFILE_CORE`, major 4, minor 3. Comment: *"AMD RX 7900 XTX supports up to 4.6 core; 4.3 is the minimum feature level."*
- `GL_ARB_gl_spirv` + `glSpecializeShader` are **core in GL 4.6**, ARB extension below. So SPIR-V is **NOT guaranteed by the requested version** — must be runtime-detected.
- **Symbols are already reachable today, no loader work:** loader = GLEW (`gameosmain.cpp:1097-1098` `glewInit()`); `3rdparty/include/GL/glew.h` already declares `GL_SHADER_BINARY_FORMAT_SPIR_V`=0x9551 (:2536), `glSpecializeShader` (:2547), `glShaderBinary` (:3915), and the full `GL_ARB_gl_spirv` block + `GLEW_ARB_gl_spirv` var (:5073-5085).
- Detection is a one-liner mirroring existing pattern (`gameosmain.cpp:1113` `if (GLEW_ARB_parallel_shader_compile)`): `if (GLEW_ARB_gl_spirv || GLEW_VERSION_4_6)`. **Not currently queried** (0 hits). AMD + NVIDIA modern drivers expose it.

### 2. Can it load SPIR-V for one family while preserving GLSL fallback? — YES
- The compile chokepoint is `compile_shader()` (`shader_builder.cpp:379-398`): `glShaderSource(shader, count, strings, 0); glCompileShader(shader);` then `get_shader_error_status(GL_COMPILE_STATUS)`. Called from `makeShader` (:446-448).
- **Minimal seam** (inside that function, behind a runtime+artifact-present check):
  `glShaderBinary(1,&shader,GL_SHADER_BINARY_FORMAT_SPIR_V,blob,len); glSpecializeShader(shader,"main",0,nullptr,nullptr);` — same `GL_COMPILE_STATUS` check works unchanged.
- `makeProgram2` (link + reflect, :724-892) is **format-agnostic** — it attaches the GL shader id and reflects the linked program identically for SPIR-V-specialized shaders. No ABI change.
- State needed (mostly present): entry point = constant `"main"` (all shaders use implicit main); spec-const list = empty for now; `.spv` blob keyed off `glsl_shader::fname_` (stored, :486). Fallback = if no `.spv` / extension absent / specialize fails → fall through to the existing GLSL `glShaderSource` branch.

### 3. How does hot-reload work if GLSL source still exists? — GLSL recompile, fallback is automatic
- Trigger = **2 Hz file-mtime poll**, dev-only, default OFF: `gos_ShaderHotReloadEnabled()` env `MC2_SHADER_HOT_RELOAD` (`gameos_graphics.cpp:5624-5630`); `endFrame()` polls every 500 ms (:5669-5686) → `checkReload()` (:479-487) → `needsReload()` = `last_load_time_ < max-mtime` (`shader_builder.cpp:1269-1272`). No watcher, no keypress.
- `reload()` (`shader_builder.cpp:946-1039`) is **hardcoded to the GLSL path** — reads `fname_` and `glShaderSource`-compiles with `prefix_` (:964-972). It **ignores any `.spv`**. So the design is automatic: **SPIR-V loaded at startup; any hot-reload recompiles from live GLSL** (the existing runtime-GLSL path, untouched). Reload failure keeps the old program (silent, per CLAUDE.md).

### 4. Where would precompiled artifacts live? — `shaders/` (auto-deployed); no CMake bake step yet
- Shaders live in the `shaders/` tree. `deploy_payload.py` walks it **recursively and copies every file** (:363-378), so a `.spv` under `shaders/` (or `shaders/spv/`) is **auto-deployed**. Adding `.spv` to `SHADER_EXTS` (:56) would give it the byte-diff hard-fail guard the other shader files get.
- **No GLSL→SPIR-V compile step exists in CMake.** The only shader-tool target is `shader_reflect` CI (`CMakeLists.txt:596-613`, depends on glslang+spirv-cross) which reflects, does not emit runtime `.spv`. A real offline `glslang/glslc → .spv` codegen target is **net-new = `OFFLINE-SHADER-VARIANT-BUILD-1`** (the Vulkan SDK dep is already assumed present).

### 5. How are specialization constants applied? — none today; `#define` literals
- **Zero `layout(constant_id=N)` in the tree** (grep: 0 hits). The `SPECIALIZATION_CONSTANT`-class macros are injected `#define` literals.
- `MC2_SHADOW_CSM_MAX` **sizes arrays**: `shadow.hglsl:101-107` `uniform mat4 dynamicCascadeMatrices[MC2_SHADOW_CSM_MAX]`; clamp at :198. Injected via snprintf `GameOS/gameos/gameos_graphics_material_internal.h:379-381`.
- **To make it a real `glSpecializeShader` constant:** (a) `layout(constant_id=N) const int MC2_SHADOW_CSM_MAX = 3;` (spec-const-sized array is legal), (b) compile to SPIR-V once with default decoration, (c) at load pass constant index `N` + runtime `mc2ShadowCsmCount()` to `glSpecializeShader` before link, (d) stop injecting the `#define`. Clamp at :198 unchanged.
- `READBACK_SSBO_BINDING` is **not** a true spec-const — it's a descriptor binding index (→ fixed `layout(binding=)` in Vk), not a `glSpecializeShader` value.

### 6. Can reflect.py output become the source of `shaderVariantId` metadata? — YES for metadata; NO for the runtime `.spv`
- **Metadata: YES.** `normalize()` (`reflect.py:464-472`) already emits per variant `{shader, variant, defines, ubos, ssbos, outputs}`; goldens persist the define-set (`expected/shaders__mech.frag__objectid.json` → `"defines":["MC2_OBJECT_ID_BUFFER=1"]`). A stable `shaderVariantId = hash{shader-base + sorted-define-set}` — both inputs exist verbatim. This is the authoritative per-variant descriptor/interface record.
- **Runtime `.spv`: NO.** reflect.py compiles with `--auto-map-bindings --auto-map-locations` (:329-339) and **deletes the `.spv`** after reflecting (:819-823). Auto-mapped bindings need not match the engine's real slots; the engine relies on **explicit `layout(binding=)`** (binding-slot-occupancy: every SSBO `role="glBindBufferBase(literal)"`; gpu_cull.comp:131 `binding=9`). A runtime consumer needs a **separate non-auto-map compile** that preserves the explicit decorations.
- **Coverage caveat:** reflect.py `SHADER_VARIANTS` (:63-119) covers a *subset* of the full `OFFLINE_VARIANT` space (`KNOWN_VARIANT_MACROS = {MC2_COALESCE, MC2_OBJECT_ID_BUFFER}` only; ALPHA_TEST/IS_OVERLAY/MRT_ENABLED/PBR_SLOTS not enumerated). The metadata source must be extended to the full variant set before it can drive a complete bake.

### 7. Which pilot is safest? — **postprocess first (mechanism), then MechOpaque (keyed). Avoid static-prop depth.**
| Candidate | Variants | Binding/vertex surface | Isolation | First-pilot verdict |
|---|---|---|---|---|
| **postprocess (no-macro pass)** | 1 (`#version 430` only; pick the composite/copy pass, NOT shadow_screen which injects MC2_SHADOW_CSM) | fullscreen triangle (`gl_VertexID`, no VAO attribs), input textures only, no SSBO lockstep | **highest** — last pass; a failure is contained, never corrupts scene geometry | ✅ **safest mechanism pilot** (prove glShaderBinary+glSpecializeShader + GLSL fallback with one `.spv`) |
| **MechOpaque** | 4 (`OBJECT_ID × USE_VIEW_UNIFORMS`) | 1 fixed VAO (GpuMechVertex 48B); registered + reflected; already the PipelineKey pilot | medium | ✅ **second pilot** (first *keyed/registered* integration, ties to pipeline-key-schema) |
| static-prop depth | COALESCE × PBR_SLOTS | **coalesce needs `GL_ARB_shader_draw_parameters` + reflect.py `#version 460` rewrite** for the vert variant | medium | ❌ **avoid first** — the ARB-extension + version-460 SPIR-V bake is the messiest starting point |

**Recommendation:** prove the *consumer mechanism* on an isolated no-macro postprocess shader (one `.spv`, contained blast radius, exercises detect→bind-binary→specialize→link→fallback). Once green, do the first *registered/keyed* pilot on **MechOpaque** (already the `PIPELINE-KEY-SCHEMA-1` pilot). Defer static-prop depth.

## Net feasibility summary
| Capability | Verdict |
|---|---|
| Loader exposes `glSpecializeShader`/`glShaderBinary`/`SPIR_V` | **YES** (GLEW, no loader work) |
| Runtime extension detection available | **YES** (`GLEW_ARB_gl_spirv`/`VERSION_4_6`); not yet queried |
| Clean seam for a SPIR-V branch | **YES** (`compile_shader()` / `makeShader`) |
| GLSL hot-reload fallback preserved | **YES** (`reload()` is GLSL-only by construction) |
| Deploy picks up `.spv` | **YES** (recursive copy) |
| Runtime-usable `.spv` exists today | **NO** — reflect.py `.spv` is auto-mapped + deleted; needs non-auto-map offline bake |
| Spec constants wired | **NO** — 0 `constant_id`; `#define` literals today |

## Prerequisite + next
**Prerequisite for an actual pilot:** `OFFLINE-SHADER-VARIANT-BUILD-1` — a CMake `glslang → .spv` step that compiles **without** `--auto-map-bindings` (preserving explicit `layout(binding=)`), bakes one `.spv` per (shader, define-set) variant keyed by the `shaderVariantId` metadata, deploys under `shaders/spv/`. The runtime prefix/`#ifdef` matrix becomes a `.spv` artifact matrix.
**Then:** `SPIRV-CONSUMER-PILOT-BUILD-1` — wire the `compile_shader` SPIR-V branch + `GLEW_ARB_gl_spirv` gate (env-gated, default OFF) for the postprocess pilot, GLSL fallback intact.

## Exclusions honored
Recon only — no Vulkan code, no SPIR-V consumer wiring, no shader edits, no `makeShader`/`reload` changes, no offline-compile build step, no loader change, no spec-const GLSL migration. Foreign WIP (`mech3d.cpp`, `txmmgr.h`, `golden-sets.json`) untouched.
