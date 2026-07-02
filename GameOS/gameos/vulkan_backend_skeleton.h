// VULKAN-BACKEND-SKELETON-1
// First Vulkan bootstrap slice. Compile-gated by CMake MC2_VULKAN (default OFF).
// When OFF, this header/TU compiles to nothing and no Vulkan dep is pulled in.
//
// mc2_vulkan_probe(): fail-soft. Creates a VkInstance, enumerates physical
// devices, dumps capabilities, destroys the instance. Returns true on success,
// false (with a logged reason) on ANY failure. NO swapchain, NO surface, NO
// device creation, NO window takeover, NO game render path.
//
// Runtime gate: even in an MC2_VULKAN=ON build the probe only runs when the
// MC2_VULKAN_PROBE env var is set. It is NOT wired into the frame loop.

#ifndef MC2_VULKAN_BACKEND_SKELETON_H
#define MC2_VULKAN_BACKEND_SKELETON_H

#ifdef MC2_VULKAN

// VULKAN-VOLK-LOADER-1: route all Vulkan entry points through the volk
// meta-loader instead of a hard link to vulkan-1.dll. volk.h defines
// VK_NO_PROTOTYPES itself and includes the Vulkan headers, so we include it in
// place of <vulkan/vulkan.h>. No Vulkan symbol resolves until volkInitialize()
// (see the .cpp), which is the OpenGL-user-without-a-Vulkan-runtime fail-soft
// path: if the loader/DLL is missing, volkInitialize() fails and every probe
// returns false gracefully rather than failing to load the exe.
#include <volk.h>

// VULKAN-SHADER-TOOLCHAIN-1: fail-soft SPIR-V shader-module helpers.
//
// mc2_vulkan_load_spv(): reads a .spv file, vkCreateShaderModule. Returns
// VK_NULL_HANDLE on ANY error (missing file, bad size, create fail) -- logged.
// mc2_vulkan_free_shader(): vkDestroyShaderModule (no-op on VK_NULL_HANDLE).
// No pipeline, no render -- module create/destroy only.
VkShaderModule mc2_vulkan_load_spv(VkDevice device, const char* path);
void mc2_vulkan_free_shader(VkDevice device, VkShaderModule module);

// VULKAN-SHADER-TOOLCHAIN-1: probe extension. Creates a minimal headless
// VkDevice (no surface/swapchain), loads the compiled fullscreen.vert.spv +
// fullscreen.frag.spv into VkShaderModules, reports, destroys everything.
// Fail-soft: logs + returns false on any error. spvDir = directory holding the
// compiled .spv files (build output). Runs only from mc2_vulkan_probe_if_env
// when MC2_VULKAN_PROBE is set.
bool mc2_vulkan_probe_shaders(const char* spvDir);

// VULKAN-FULLSCREEN-TRIANGLE-1: headless offscreen render + readback + verify.
// Creates a headless VkDevice (no surface/swapchain), builds a render pass +
// graphics pipeline from the compiled fullscreen.vert/frag .spv, renders a
// fullscreen triangle to an offscreen 64x64 RGBA8 VkImage, copies it to a
// host-visible buffer, reads it back, and verifies the rendered UV gradient
// (the oversized triangle covers the whole viewport, so every pixel carries the
// frag's interpolated vUV output; if nothing drew, all pixels would be the
// clear color and the checks fail). Proves shaders +
// pipeline + renderpass + draw + readback end to end. Fail-soft: logs + returns
// false on any error. spvDir = directory holding the compiled .spv files.
bool mc2_vulkan_probe_triangle(const char* spvDir);

// VULKAN-DESCRIPTOR-SMOKE-1: headless descriptor-set plumbing probe.
// Creates a headless VkDevice (no surface/swapchain/render), builds a
// VkDescriptorSetLayout mirroring the inventory's Set-0 shape (binding 0 = UBO,
// binding 1 = SSBO), a VkDescriptorPool, allocates a VkDescriptorSet, creates a
// small uniform + storage VkBuffer, binds them to the set via
// vkUpdateDescriptorSets, then destroys everything. With MC2_VULKAN_VALIDATION
// set the KHRONOS validation layer is enabled and ZERO validation errors is the
// real proof the descriptors are wired correctly. Fail-soft: any VkResult error
// -> log + return false. No pipeline/renderpass/draw needed.
bool mc2_vulkan_probe_descriptors();

// VULKAN-SAMPLED-IMAGE-SMOKE-1: headless sampled-image + sampler descriptor probe.
// Creates a headless VkDevice (no surface/swapchain/window/render), builds a
// small device-local VkImage (8x8 RGBA8), clears it and transitions it to
// SHADER_READ_ONLY via a one-shot command-buffer barrier, creates a VkImageView,
// a linear+repeat VkSampler and (cheap) a compare VkSampler (compareEnable, one
// of the shadow-sampler shapes the sampler recon identified), a descriptor set
// with a COMBINED_IMAGE_SAMPLER binding, binds via vkUpdateDescriptorSets, then
// destroys everything. Proves the sampled-image path Vulkan needs -- per-pass
// rebind (GL) does not survive Vk; Vk needs explicit VkImage/View/Sampler +
// COMBINED_IMAGE_SAMPLER descriptors. With MC2_VULKAN_VALIDATION set the KHRONOS
// validation layer is enabled and ZERO validation errors is the real proof.
// Fail-soft: any VkResult error -> log + return false.
bool mc2_vulkan_probe_sampled_image();

// VULKAN-EDGEFOG-SYNTHETIC-FIXTURE-1: headless shader-MATH oracle for the edge-fog
// Vulkan port. Runs the shipped edge_fog.{vert,frag}.spv on a KNOWN synthetic depth
// texture + KNOWN invViewProj + KNOWN fog uniforms (documented in the .cpp), reads
// back the RGBA16F fog output, computes the SAME edge_fog.frag math on the CPU, and
// compares GPU-vs-CPU per pixel within a tight tolerance. Unlike the golden-bookmark
// oracle (frame-integration correctness), this proves the shader MATH in isolation
// (no mission/camera/GL depth-copy). The pass uses LOAD_OP_CLEAR + blend DISABLED so
// the readback IS the raw frag output vec4(fogColor, fogFactor) -- not blended over a
// scene. Fail-soft: any VkResult error -> log + return false. spvDir = dir with the
// compiled edge_fog .spv (build output). With MC2_VULKAN_VALIDATION set, ZERO
// validation errors is an additional proof.
bool mc2_vulkan_probe_edgefog_fixture(const char* spvDir);

// VULKAN-OOB-FOG-ISLAND-1: headless shader-MATH oracle for the OOB-fog Vulkan port.
// Runs the shipped fog_oob.{vert,frag}.spv on a KNOWN synthetic depth + invViewProj
// + fog uniforms, reads back the RGBA16F output, computes the SAME fog_oob.frag math
// on the CPU, and compares GPU-vs-CPU per pixel within a tight 1/1024 tolerance.
// LOAD_OP_CLEAR + blend DISABLED so the readback IS the raw frag output vec4(col,
// alpha). Fail-soft: any VkResult error -> log + return false. spvDir = dir with the
// compiled fog_oob .spv (build output).
bool mc2_vulkan_probe_oobfog_fixture(const char* spvDir);

// VK-REGION-SSAO-FIXTURE-1: headless shader-MATH oracle for the SSAO occlusion
// Vulkan port (closes the VK-REGION-SSAO-1 acceptance debt). Runs the shipped
// ssao.{vert,frag}.spv on a KNOWN synthetic depth+normal pair + exact-inverse
// sheared viewProj/invViewProj (transpose-bug detector) and checks a CLASS-BASED
// oracle: sky/flat-floor/wall pixel classes are provably rotation-independent
// (the sin-hash TBN rotation is not CPU<->GPU bit-portable) and must be exactly
// 1.0; the wall-adjacent band is checked against a closed AO envelope + a
// GPU-and-CPU occlusion-liveness signal. R==G==B and A==1 structurally, 1/1024
// FP16 tolerance. Fail-soft: any VkResult error -> log + return false. spvDir =
// dir with the compiled ssao .spv (build output).
bool mc2_vulkan_probe_ssao_fixture(const char* spvDir);

// VK-REGION-HZB-1: headless BIT-TIGHT oracle for the HZB reduce-chain Vulkan
// port. Runs the shipped hzb_reduce.{vert,frag}.spv over a KNOWN 64x64
// synthetic reverse-Z depth pattern, builds the full 7-level ceil ladder
// (separate R32F image per level, per-level B13 external-dependency barrier,
// dynamic viewport -- exactly the island's shape), reads back EVERY level and
// compares against a CPU min-reduce reference with tolerance 0: min() of
// floats is bit-portable across compilers and pow2 dims make every sampling
// computation exact in binary fractions (no ssao/fog transcendental caveat).
// Fail-soft: any VkResult error -> log + return false. spvDir = dir with the
// compiled hzb_reduce .spv (build output).
bool mc2_vulkan_probe_hzb_fixture(const char* spvDir);

// VK-COMPARE-SAMPLER-FIXTURE-1: headless depth-COMPARE sampler oracle. Creates a
// small VK_FORMAT_D32_SFLOAT depth image with a KNOWN pattern and a VkSampler
// with compareEnable=TRUE / compareOp=VK_COMPARE_OP_LESS, runs the shipped
// compare_shadow.{vert,frag}.spv (texture(sampler2DShadow, vec3(uv,ref))) at
// texel-center taps, reads the result back and compares against a CPU reference
// computing the SAME hardware compare (ref < stored ? 1 : 0). NEAREST + texel
// centers -> deterministic 0/1, tolerance 0 (bit-tight). This is the Vulkan
// analog of GL sampler2DShadow / GL_COMPARE_REF_TO_TEXTURE that L7.2 Shadow
// needs. Fail-soft: any VkResult error -> log + return false. spvDir = dir with
// the compiled compare_shadow .spv (build output).
bool mc2_vulkan_probe_comparesampler_fixture(const char* spvDir);

// VK-COMPARE-SAMPLER-FIXTURE-1: headless CSM 2D_ARRAY depth-compare oracle.
// Creates an N-layer VK_FORMAT_D32_SFLOAT 2D_ARRAY depth image (distinct pattern
// per layer) + a sampler2DArrayShadow compare sampler, runs the shipped
// csm_array_shadow.{vert,frag}.spv (per-texel cascade select +
// texture(sampler2DArrayShadow, vec4(uv,layer,ref))), reads the result back and
// compares against a per-layer CPU reference (ref < stored ? 1 : 0). Bit-tight
// at texel centers, tolerance 0. This is the Vulkan analog of GL
// sampler2DArrayShadow (calcDynamicShadow CSM path) that L7.3 Terrain needs.
// Fail-soft. spvDir = dir with the compiled csm_array_shadow .spv.
bool mc2_vulkan_probe_csmarray_fixture(const char* spvDir);

// VK-REGION-SHADOW-1 (sub-slice A): reusable DEPTH-ONLY INSTANCED shadow-caster
// pipeline (the WRITE side of the shadow path; comparesampler/csmarray proved the
// READ side). Defined in vulkan_shadow_island.cpp (compiled ONLY under MC2_VULKAN,
// additive island code -- does NOT touch the GL shadow producer). The pipeline has
// NO color attachment, depthTestEnable+depthWriteEnable=TRUE, D32_SFLOAT depth
// target, compareOp=LESS. Descriptor layout: set0 = per-frame view/cascade UBO,
// set1 = instance ring bound STORAGE_BUFFER_DYNAMIC (per-instance transforms).
// Vertex input = a single vec3 position stream (a unit quad, instanced). The
// caller owns the render pass (single D32 vs N-layer CSM D32 array), images, and
// buffers; this builds only the pipeline + its two set layouts. Fail-soft: returns
// false + leaves *out zeroed on any error.
struct Mc2VkShadowDepthPipeline {
    VkPipeline            pipeline   = VK_NULL_HANDLE;
    VkPipelineLayout      layout     = VK_NULL_HANDLE;
    VkDescriptorSetLayout frameSetLayout    = VK_NULL_HANDLE; // set0: UBO
    VkDescriptorSetLayout instanceSetLayout = VK_NULL_HANDLE; // set1: STORAGE_BUFFER_DYNAMIC
};

// Builds the depth-only instanced pipeline into *out against `renderPass`
// (subpass 0, depth-only). `vert`/`frag` are the compiled shadow_depth modules.
// Fail-soft: returns false + leaves *out zeroed on any error.
bool mc2_vulkan_build_shadow_depth_pipeline(VkDevice device, VkRenderPass renderPass,
                                            VkShaderModule vert, VkShaderModule frag,
                                            Mc2VkShadowDepthPipeline* out);

// Destroys everything in *out and zeroes it. No-op on VK_NULL_HANDLE fields.
void mc2_vulkan_destroy_shadow_depth_pipeline(VkDevice device, Mc2VkShadowDepthPipeline* p);

// VK-REGION-SHADOW-1 (sub-slice A): headless shadow-depth-render FIXTURE. Drives
// the depth-only instanced pipeline with SYNTHETIC instanced geometry (a handful of
// full-viewport quads at KNOWN world transforms fed via a synthetic instance-ring
// SSBO bound STORAGE_BUFFER_DYNAMIC) into (a) a single D32_SFLOAT shadow map and
// (b) a 3-layer CSM D32_SFLOAT array (a distinct light-space matrix per layer),
// reads back the depth and validates BIT-TIGHT against a CPU reference that
// computes the same projected depth per instance (depth-test LESS + clear 1.0 ->
// per-texel min over instances). Geometry/transforms are chosen so every projected
// depth is exactly representable in binary -> tolerance 0. Fail-soft: any VkResult
// error -> log + return false. spvDir = dir with the compiled shadow_depth .spv.
bool mc2_vulkan_probe_shadowdepth_fixture(const char* spvDir);

// VK-BOOTSTRAP-INTEGRATE-1: Vulkan swapchain PROBE (create + query + destroy).
// Uses vk-bootstrap's vkb::SwapchainBuilder against a bespoke devs[0] device and
// a hidden 256x256 SDL Vulkan window/surface. Creates a swapchain, queries its
// image count / format / extent, logs "swapchain-probe: OK image_count=.. ..",
// then destroys everything in reverse. NO present, NO frame loop. Fail-soft: any
// step logs a reason + returns false, never crashes (this is the OpenGL-user
// path too). Defined in vulkan_swapchain_probe.cpp (compiled only under MC2_VULKAN).
bool mc2_vulkan_probe_swapchain();

// VULKAN-SWAPCHAIN-PRESENT-1: Layer-5 -- the engine OWNS a Vulkan swapchain and
// PRESENTS a controlled frame safely, fail-soft to GL. Creates a SEPARATE visible
// SDL Vulkan window (default SHOWN; MC2_VULKAN_SWAPCHAIN_PRESENT_HIDDEN -> hidden
// for headless CI) distinct from the GL window (shares NO GL context), owns its
// own VkInstance/surface/device/swapchain, presents 16 frames (clear to teal +
// fullscreen triangle), handles a mid-loop resize (frame 8) via OUT_OF_DATE ->
// recreate, tears everything down in reverse, and emits a
// [VK_SWAPCHAIN_PRESENT_HEALTH] line. NO game render pass routed through it, NO
// renderer migration. Fail-soft at every step (log + return false, never crash --
// this is also the no-Vulkan-runtime path). Returns true iff all 16 frames
// presented VK_SUCCESS with zero validation errors and no fallback. Defined in
// vulkan_swapchain_present.cpp (compiled only under MC2_VULKAN). spvDir = dir
// holding the compiled fullscreen.*.spv.
bool mc2_vulkan_probe_swapchain_present(const char* spvDir);

// L6.6-COMPOSITE-PRESENT-SCAFFOLD: the CompositePresent TEST region's driver.
// Stands up the PERSISTENT mc2vk::VulkanPresentObject, presents `frames` frames
// (0 -> default 16) to its OWN separate Vulkan window, exercising a resize at the
// midpoint -- proving the region->acquire->present->resize path + the per-image
// render-semaphore discipline WITHOUT touching the GL game window. Fail-soft (no
// crash). Optional out-params receive present-frame / resize / validation-error
// tallies. Full game-window Vulkan present is BLOCKED on L7 interop (ledgered).
// Defined in vulkan_swapchain_present.cpp (compiled only under MC2_VULKAN).
bool mc2_vulkan_composite_present_scaffold(const char* spvDir, unsigned int frames,
                                           unsigned int* presentFramesOut,
                                           unsigned int* resizeCountOut,
                                           unsigned int* validationErrorsOut);

// Returns true if a VkInstance was created, at least one physical device was
// enumerated, and the capability dump completed. Fail-soft: logs + returns
// false on any error.
bool mc2_vulkan_probe();

// Runs mc2_vulkan_probe() only if the MC2_VULKAN_PROBE env var is set.
// No-op (returns false) otherwise. Safe to call once at startup.
bool mc2_vulkan_probe_if_env();

#endif // MC2_VULKAN

#endif // MC2_VULKAN_BACKEND_SKELETON_H
