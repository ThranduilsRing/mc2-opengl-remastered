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

// Returns true if a VkInstance was created, at least one physical device was
// enumerated, and the capability dump completed. Fail-soft: logs + returns
// false on any error.
bool mc2_vulkan_probe();

// Runs mc2_vulkan_probe() only if the MC2_VULKAN_PROBE env var is set.
// No-op (returns false) otherwise. Safe to call once at startup.
bool mc2_vulkan_probe_if_env();

#endif // MC2_VULKAN

#endif // MC2_VULKAN_BACKEND_SKELETON_H
