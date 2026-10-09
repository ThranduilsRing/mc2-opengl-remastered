# Seam 2: single source of truth for C++/GLSL mirrored structs

## Decision: (b) C++ header is source; Python generator emits committed .hglsl plus C++ layout asserts
Rejected:
- (a) GLSL as source: C++ structs carry docs, alignas and trivially-copyable asserts. Generating C++ from GLSL loses the compiler's own offsetof cross-check and puts prose in generated files.
- (c) dual-language macro header: GLSL has no uint32_t, offsetof, static_assert, or float[16]-as-mat4. Macro shims hide the mat4 and std140 traps rather than checking them.
Why (b): C++ is the producer (memcpy of a POD). The generator computes std430/std140 offsets independently of the compiler and emits static_asserts on the C++ side, so the compiler layout and the GLSL are both validated against one rule set. This is check_ubo_layout.py's logic promoted from "compare" to "emit + compare".

## File layout
- `RenderCore/gpu_abi/GpuStaticPropInstance.h`: hand-written source struct. Annotation `// @gpu std430 glsl=GpuStaticPropInstance` above the struct, `// @mat4` on `float modelMatrix[16]`, `// @vec4` on `float x[4]`. Restricted subset: fixed-width scalars, float[N], no ctors/methods/bitfields. Ends with `#include "GpuStaticPropInstance.layout.gen.h"`.
- `RenderCore/gpu_abi/GpuStaticPropInstance.layout.gen.h`: committed, generated. sizeof and per-field offsetof static_asserts computed from std430 rules.
- `shaders/include/gpu_abi/GpuStaticPropInstance.gen.hglsl`: committed, generated. Emits the `struct` only; each shader keeps its own `layout(binding/set, std430) buffer` wrapper (GL and VK binding syntax differ). For UBOs it emits `#define NAME_MEMBERS ...` member-list macros, since blocks cannot be included as structs.
- `tools/gpu_abi_gen.py` plus `tools/gpu_abi_manifest.json`: generator and validator. tools/check_ubo_layout.py becomes a shim and its REGISTRY becomes manifest rows (legacy GLSL-vs-C++ compare kept for pairs not yet migrated).
- `scripts/check-gpu-abi.py`: regenerate in memory, diff against the committed files (drift = fail), then run the legacy compare. Register in check-contracts.sh beside spirv_artifacts.

## Why committed, not build-time generated
GL resolves `#include` at runtime through the engine resolver (shader_common mirrors it) from the deployed shaders/ tree. VK uses glslc with GL_GOOGLE_include_directive, and GameOS/gameos/CMakeLists.txt:360-365 already DEPENDS-globs shaders/include/*.hglsl. A build-dir artifact would need a second include root at runtime plus deploy changes. Committed output gives one path for both backends. CMake gets only a `gpu_abi_check` custom target running --check; `--write` is manual.

## SPIR-V source-hash (check-spirv-artifacts)
The hash covers the flattened (#include-expanded) source. Replacing an inline struct with an include changes the flattened text of any baked pilot shader (pilots.json: postprocess, mech, ...). Those need a rebake via tools/shader_offline_build/build_variants.py in the same commit. Verify with grep of pilots.json that static_prop and shadow_prop are not pilots before slice 1. Generator output must be byte-deterministic (LF, sorted, no timestamps).

## Layout rules the generator enforces
- std430: scalar 4, vec2 8, vec3/vec4 16, mat4 16 (size 64). Struct align = max member; size rounded to align.
- std140: arrays of scalar/vec2/vec3 have stride 16, so REJECT float[N] scalar arrays in UBO structs (require vec4[N]). Struct align and size round up to 16.
- vec3 trap: reject `@vec3` unless explicit. Default is vec4 or three scalars. In std430 a vec3 followed by a float packs at +12; the generator computes this and C++ must declare the same.
- mat4: C++ float[16] is row-major, uploaded GL_FALSE, loaded as a column-major mat4 (shader uses row-vector `v * M`, see shadow_prop.vert). The generator only maps `@mat4`; never emit `row_major` silently.
- SSBO array stride equals sizeof; reject when the std430 size differs from sizeof(C++).
- Unknown type or annotation = exit 2 (matches the existing UNKNOWN-TYPE policy).

## Migration order (all mirror locations)
1. GpuStaticPropInstance 112B (slice 1). C++: GameOS/gameos/gos_static_prop_batcher.h:28. C++ mirrors: GameOS/gameos/vulkan_color_prop_island.cpp:205, GameOS/gameos/vulkan_shadow_island_live.cpp:205. GLSL: shaders/static_prop.vert:33, shaders/shadow_static_prop.vert:19, shaders/vulkan/color_prop_tier1.vert:100, shaders/vulkan/shadow_prop.vert:50. Consumers: GameAdapters/StaticPropRenderAdapter.cpp:39-47, gos_static_prop_batcher.cpp:9478-9566, tools/material_cook/reflect_expected_layout.md.
2. PerDrawEntry 32B: gos_static_prop_batcher.h:59; shaders/static_prop.frag:90; shaders/static_prop_depth.frag:41.
3. GpuMechInstance 64B: gos_mech_batcher.h (~35-56); shaders/mech.vert:49; shaders/vulkan/mech_tier1.vert:46; shaders/shadow_mech.vert.
4. MaterialGpu: shaders/include/material_gpu.hglsl:46 plus the RenderCore C++ struct. Flip to generated and retire check-material-gpu-mirror.sh.
5. Small std140 UBOs (adds the missing asserts, consistency-code M2): ShadowFrameUBO (vulkan_shadow_island_live.cpp:217, shadow_prop.vert), VfxVsParams (vulkan_vfx_island.cpp:111), WaterVsParams/WaterFsParams (vulkan_water_island.cpp:218,225), post-native EdgeFog/FogOob/Ssao*/ScreenFx*, SkyPC, OvlPC, LegacyFrameUBO, MechFrameUBO.
6. ColorFrameUBO (vulkan_color_prop_island.cpp:226, 256B; color_prop_tier1.vert and .frag).
7. TerrainFrameUBO (1024B; vulkan_terrain_island.cpp, terrain_tier1.frag, vulkan/include/terrain_frame_vk.glsl, skirt .vert/.frag copies). Last; per-field offsetof replaces the "off N" comments.
8. Lights: ObjLightsMirror (vulkan_color_prop_island.cpp:508) vs TG_HWLightsData / light_pools.hglsl; fold into check-light-abi-lockstep.py. Needs generator support for arrays of mat4/vec4, so deferred.
9. Stride constants (`vib.stride = 40`, kStaticPropVertexStride) go in the same headers (M3).

## Slice 1: GpuStaticPropInstance end to end
1. Create RenderCore/gpu_abi/GpuStaticPropInstance.h by moving the struct out of gos_static_prop_batcher.h (which includes it). Keep the global-namespace name so no call site changes.
2. Write tools/gpu_abi_gen.py (std430 only; scalars, vec4, mat4, arrays) and the manifest row. Run --write to produce the .layout.gen.h and .gen.hglsl.
3. Delete both *Mirror structs and use GpuStaticPropInstance.
4. In the 4 shaders replace the inline `struct Instance` with `#include "gpu_abi/GpuStaticPropInstance.gen.hglsl"` and change the buffer member type to GpuStaticPropInstance. Use the include syntax other shaders in the same family already use.
5. Add scripts/check-gpu-abi.py, the check-contracts.sh entry, and the check_ubo_layout.py shim.
Verification (the builder runs these; this plan ran none):
- Full build (SPIR-V recompiles via the DEPENDS glob).
- scripts/check-contracts.sh, including spirv_artifacts and check-gpu-abi.
- py -3 tools/check_ubo_layout.py.
- Negative test: change one offset in the header; expect check-gpu-abi and static_assert to fail.
- tier1 smoke with the shader-error gate: zero GLSL/glslc compile errors on both GL and VK prop and shadow paths.
- ab.py day and night captures vs baseline. Expect pixel-identical, since layout is unchanged and only the source moved.

## Risks
- GL runtime include resolution differs from glslc. A misplaced `#include` (before #version/extension, or inside a block) breaks one backend only, so smoke both.
- The flattened-source hash changes force a rebake for any pilot using migrated files.
- CRLF drift on Windows (autocrlf): generator writes LF; add .gitattributes eol=lf for the gpu_abi paths.
- The annotation parser is regex-lite. Keep the subset tiny and fail loudly. No alignas variants or [[no_unique_address]] in annotated structs.
- Name collision: GLSL `Instance` is renamed per shader. Ensure no other `Instance` struct remains in the same TU (mech shaders keep theirs until slice 3).
- Include cycle: the generated layout file must not include its source header.
