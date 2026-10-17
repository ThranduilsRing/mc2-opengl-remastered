# Writing shaders in Slang

Owner ruling 2026-10-17: new shaders are Slang. Existing GLSL migrates later (VK-path only; GL is
being deleted). Toolchain: `slangc` from the Vulkan SDK (same source as glslangValidator and
spirv-cross; `shader_common.find_tool`). Minimum version pinned in `tools/slang/SLANG_VERSION`
(2026.8); the version used is recorded in `spirv_package.json` under `tools.slangc`.

## Add a shader
1. Write `shaders/<name>.<stage>.slang` (stage = `vert`, `frag` or `comp`). One entry point per
   file, named `main`, with a `[shader("vertex"|"fragment"|"compute")]` attribute.
2. Add a pilot to `tools/shader_offline_build/pilots.json` pointing the stage at the `.slang`
   file (see `scripts/tests/fixtures/slang/pilots.json`). Optional keys: `entries`
   (`{"frag": "myEntry"}`), `spirv` (profile, default `spirv_1_0`).
3. Build: `py -3 tools/shader_offline_build/build_variants.py` (`--check` verifies without
   writing). Output: `shaders/spv/<base>.<stage>.<variant>.<id>.spv`, sidecar JSON (with
   `language: "slang"`), `spirv_index.json` and `spirv_package.json`, same layout as GLSL.
   `scripts/check-spirv-artifacts.py` (contract row `spirv_artifacts`) detects stale artifacts.

## Conventions shared with the GLSL
- Target is Vulkan SPIR-V 1.0, same as `glslangValidator -V` (`tools/check_vk_shaders.py`).
- Always give explicit `[[vk::binding(binding, set)]]` on every resource. Use set 0 unless the
  pipeline layout (`scripts/check-vk-pipeline-layout.py`, `docs/render-binding-registry.md`)
  says otherwise. Do not rely on slangc's inferred bindings.
- UBO = `ConstantBuffer<T>` (std140); SSBO = `StructuredBuffer` / `RWStructuredBuffer`
  (std430). Keep C++ mirrors in sync through `gpu_abi` (`tools/gpu_abi_gen.py`) like GLSL
  blocks: vec3 members pad to 16 bytes, so prefer float4 / explicit pad fields. Matrices:
  keep the engine's existing layout (slangc default is column-major; do not mix).
- Texture + sampler are separate objects (`Texture2D` + `SamplerState`), reflected as
  `texture2D` / `sampler` entries in the sidecar `samplers` list with `set` and `binding`.

## Limits (not supported yet)
- Stages: vert, frag, comp only. No geometry/tess/mesh/ray.
- No define-set variants: the `defines` list is recorded but not passed to slangc.
- Source hash covers the main `.slang` file only, not `import`ed modules (keep new shaders
  self-contained or touch the main file when a module changes).
- Not wired into the CMake SPIR-V bake (`GameOS/gameos/CMakeLists.txt`) or `check_vk_shaders.py`
  (which scans `shaders/vulkan/*.vert|frag`); nothing loads Slang artifacts at runtime yet.
- Slang bindings are not checked against the GL `binding-slot-occupancy.json`.

Tests: `py -3 -B -m pytest scripts/tests/test_slang_build.py -q -p no:cacheprovider`.
