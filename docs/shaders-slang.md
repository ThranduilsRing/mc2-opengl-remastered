# Writing shaders in Slang

Owner ruling 2026-10-17: new shaders are Slang. Existing GLSL migrates later (VK-path only; GL is
being deleted). Toolchain: `slangc` from the Vulkan SDK (same source as glslangValidator and
spirv-cross; `shader_common.find_tool`). Minimum version pinned in `tools/slang/SLANG_VERSION`
(2026.8); the version used is recorded in `spirv_package.json` under `tools.slangc`.

## Add a shader
1. Write `shaders/vulkan/<name>.<stage>.slang` (stage = `vert`, `frag` or `comp`). The CMake
   bake and `tools/check_vk_shaders.py` discover these files automatically and compile `main`,
   with a `[shader("vertex"|"fragment"|"compute")]` attribute. CMake emits the runtime path
   `build*/shaders/vulkan/<name>.<stage>.spv`; a GLSL/Slang output collision is an error.
   Both slangc and spirv-cross are required when Slang sources exist; the version pin applies.
2. Add a pilot to `tools/shader_offline_build/pilots.json` pointing the stage at the `.slang`
   file (see `scripts/tests/fixtures/slang/pilots.json`). Optional keys: `entries`
   (`{"frag": "myEntry"}`), `spirv` (profile, default `spirv_1_0`).
   Stages can share one source file with different entries. For multiple entries of the same
   stage, use separate program records pointing at that file (unique program names).
   Variant `defines` are passed as `-DNAME` / `-DNAME=value`, without shell expansion.
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
  blocks. Prefer float4 / explicit pad fields and verify reflected layout; do not assume
  float3 has a 16-byte size. Compilation explicitly requests column-major matrices.
- Texture + sampler are separate objects (`Texture2D` + `SamplerState`), reflected as
  `texture2D` / `sampler` entries in the sidecar `samplers` list with `set` and `binding`.
- Bake, variant build and artifact checks reject implicit or duplicate descriptors and check
  set-0 UBO/SSBO slots against `binding-slot-occupancy.json`. Slots are multiplexed per pass;
  a registered slot alone does not establish pipeline compatibility. Other sets retain their
  Vulkan identity. `repo_query.py binding --conflicts` includes Slang uses (nonzero sets use
  namespaces such as `vk_set1_ubo`); register consumers when adding engine resources.
- Bake and variant builds compare reflected structs named for existing `gpu_abi` C++/GLSL
  types against their authoritative headers: member names/types, offsets, array dimensions
  and strides, and matrix layout. Reuse those names for shared types; private renamed copies
  cannot be recognized. New shared types still need annotated C++ headers in `gpu_abi_gen.py`.
- Source hashes include the transitive local `import` and literal `#include` closure, with
  portable relative paths. Resolve modules relative to the importing file or entry file;
  dotted imports map to directories. Cycles are safe; unresolved imports fail. Conditional
  imports are tracked conservatively, even in inactive branches. CMake conservatively
  depends on shader modules/includes and ABI headers, so edits cause a fresh bake.

## Limits (not supported yet)
- Stages: vert, frag, comp only. No geometry/tess/mesh/ray.
- CMake and the directory compile gate use `main`; named entries/variants use the pilot
  builder. No macro-expanded includes, external module search paths or precompiled modules.
- Adding a shader still requires a consuming engine pipeline with the matching descriptor
  layout; this tooling does not create pipelines or migrate existing GLSL.

Tests: `py -3 -B -m pytest scripts/tests/test_slang_build.py --capture=sys -p no:tmpdir -p no:cacheprovider -q`.

## First engine Slang shaders: the native compute producers (step 3.4P)
`shaders/vulkan/terrain_lighting.comp.slang`, `water_cull_pack.comp.slang`, `water_draw_args.comp.slang` are consumed by
`GameOS/gameos/vk_native_producers.cpp` (pipelines through `vk_compute_pipeline.h`, RAII on `vk_owned.h`). Notes from writing them:
- CMake bakes `*.comp.slang` with no pilot entry; `pilots.json` is only for the runtime-variant GLSL builder.
- Set-0 UBO/SSBO slots must already exist in `binding-slot-occupancy.json` (the bake rejects an unregistered slot, e.g. UBO 4). The three shaders
  reuse registered slots (UBO 2/3, SSBO 0-4) instead of registering new ones; a shader needing a new slot must regenerate the occupancy file.
- The ABI check only covers structs named like existing `gpu_abi` types; the producer structs (`WaterRecipe`, `WaterThinRecord`, `LightingParams`)
  have no `gpu_abi` entry, so their C++ mirrors are guarded by `static_assert(sizeof)` and the startup self-test / parity frames, not by the bake.
- A matrix uniform is passed as four `float4` columns (the raw 16 floats of the GL `GL_FALSE` upload) and multiplied by hand; this avoids any
  row/column-major question between Slang's `mul` and the engine's matrices.
- `uint`/`int` uniforms are fine in a `ConstantBuffer` (the GL `uniform uint` crash does not exist here); pack them as `int4` to keep std140 trivial.
