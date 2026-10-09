cmake_minimum_required (VERSION 3.10)
project(gameos)

set(SOURCES ${SOURCES}
    gameos.cpp
    gameos_graphics.cpp
    gameos_graphics_font.cpp
    gameos_graphics_texture.cpp
    gameos_graphics_light_ssbo.cpp
    gameos_graphics_params.cpp
    gameos_res.cpp
    MC2Strings.cpp
    gameos_fileio.cpp
    gameos_input.cpp
    gameos_debugging.cpp
    gameos_sound.cpp
    gos_render.cpp
    gos_render_context.cpp
    render_frame_driver.cpp
    gos_hdri.cpp
    gos_postprocess.cpp
    gos_static_prop_batcher.cpp
    gos_static_prop_registry.cpp
    gos_particle_bridge.cpp
    gos_vfx_mesh_bridge.cpp
    gos_cardcloud_sim.cpp
    gos_materials.cpp
    gos_mech_batcher.cpp
    gos_vegetation.cpp
    gos_object_recon_tracy.cpp
    gos_terrain_patch_stream.cpp
    gos_terrain_water_stream.cpp
    gos_terrain_indirect.cpp
    gos_terrain_surface.cpp
    gos_terrain_mask_dispatch.cpp
    gos_terrain_lighting.cpp
    gos_terrain_height_tex.cpp
    gos_terrain_lod_chunk.cpp
    gos_object_parity.cpp
    gos_validate.cpp
    gos_screenshot.cpp
    gos_dev_shell.cpp
    # RENDERER-INPUT-READBACK-1: engine half of the `shadow_feed` / `pixel`
    # command-channel readback verbs (raw-GL pixel read + static-prop shadow
    # feed scan; both forbidden to code/ by the M6 firewall).
    gos_renderer_input_probe.cpp
    gos_visual_capture.cpp
    gos_visual_diff.cpp
    gos_rdoc_capture.cpp
    gos_crashbundle.cpp
    mc2_verify.cpp
    gos_smoke.cpp
    debug_renderer.cpp
    gos_font.cpp
    gos_input.cpp
    asset_scale.cpp
    gos_gpu_sync.cpp
    gos_cluster_depth_pyramid.cpp
    gos_lightgrid_build.cpp
    gos_postprocess_blur.cpp
    gpu_driven_common.cpp
    gpu_cull_record.cpp
    gpu_cull_substrate.cpp
    gpu_cull_parity.cpp
    gpu_cull_compute.cpp
    gpu_cull_readback.cpp
    render_snapshot.cpp
    gos_render_pass_timer.cpp
    gos_frame_pass_stats.cpp
    draw_packet_emitter.cpp
    pipeline_binder.cpp
    GLBackend.cpp
    view_uniforms_gl.cpp
    gpu_buffer_gl.cpp
    debug_state_dump.cpp
    dbg_asset_globals.cpp
    diagnostic_trace.cpp
    visual_tuning_profile.cpp
    terrain_material_lib.cpp
    # VK-P13-SELECTOR-1 / T1: top-level MC2_RENDER_BACKEND selector (GL-safe; no
    # Vulkan headers, always compiled -- inert on =gl/unset, byte-identical).
    render_backend_select.cpp

    utils/stream.cpp
    utils/camera.cpp
    utils/gl_utils.cpp
    utils/gl_render_constants.cpp
    utils/Image.cpp
    utils/logging.cpp
    utils/mc2log.cpp
    utils/shader_builder.cpp
    utils/matrix.cpp
    utils/vec.cpp
    utils/timing.cpp
    ${CMAKE_SOURCE_DIR}/3rdparty/tracy/TracyClient.cpp
    ${CMAKE_SOURCE_DIR}/3rdparty/tinyexr/miniz.c
    utils/string_utils.cpp
    utils/file_utils.cpp
    mc2_hitch_trace.cpp
    )

# VULKAN-BACKEND-SKELETON-1: only compile the skeleton TU when MC2_VULKAN=ON.
# When OFF the TU is not added -> no Vulkan headers/loader pulled in.
if(MC2_VULKAN)
    list(APPEND SOURCES vulkan_backend_skeleton.cpp)
    # VK-REGION-SHADOW-1: depth-only instanced shadow-caster pipeline builder.
    # TU is #ifdef MC2_VULKAN (same gate as the skeleton, which calls its
    # mc2_vulkan_build/destroy_shadow_depth_pipeline symbols from the
    # shadowdepth fixture). Registration omitted in commit 95095a94 -> link
    # break; restored here (T-minus-1 build unblock).
    list(APPEND SOURCES vulkan_shadow_island.cpp)
    # VK-BOOTSTRAP-INTEGRATE-1: swapchain probe TU + vendored vk-bootstrap impl.
    list(APPEND SOURCES vulkan_swapchain_probe.cpp)
    # VULKAN-SWAPCHAIN-PRESENT-1: Layer-5 swapchain PRESENT path (own window +
    # swapchain, present 16 frames, resize, fail-soft). Same MC2_VULKAN gate.
    list(APPEND SOURCES vulkan_swapchain_present.cpp)
    # L6.6-COMPOSITE-PRESENT-SCAFFOLD: the PERSISTENT present object extracted
    # from the L5 probe (reusable acquire/present/resize + per-image semaphores).
    # Same MC2_VULKAN gate (NOT _ISLAND -- the swapchain-present probe uses it).
    list(APPEND SOURCES vulkan_present_object.cpp)
    # VK-P13-SELECTOR-1 / T2: the standalone SHARED engine Vulkan context
    # (instance/device/queue/VMA owned HERE, not in the present object). Same
    # MC2_VULKAN gate as the present object; entirely #ifdef MC2_VULKAN.
    list(APPEND SOURCES vulkan_context.cpp)
    list(APPEND SOURCES "${CMAKE_SOURCE_DIR}/3rdparty/vk_bootstrap/VkBootstrap.cpp")
    # VULKAN-VMA-INTEGRATION-1: single VMA_IMPLEMENTATION TU (header-only lib).
    list(APPEND SOURCES vma_impl.cpp)
    # VULKAN-VOLK-LOADER-1: volk meta-loader (dynamic Vulkan load; no hard link
    # to vulkan-1.dll). Only compiled when MC2_VULKAN=ON.
    list(APPEND SOURCES "${CMAKE_SOURCE_DIR}/3rdparty/volk/volk.c")
    if(MC2_VULKAN_ISLAND)
        # VK-WATER-TIER1: the water island renders the armed water base layer in
        # place into the shared scene GBuffer. The old P9a side-image gate is
        # retired; no public raw water interop gate remains.
        # No layout-affecting header members (extern "C" entry points only).
        # No layout-affecting header members (extern "C" entry points only).
        list(APPEND SOURCES vulkan_water_island.cpp)
        # VK-POST-FOG-NATIVE-1 (rung 8.1, VK-RUNG8-LADDER-1): the fog pair (edge+oob)
        # rendered NATIVELY on the shared engine VulkanContext — samples the SHARED
        # scene depth, blends into the SHARED scene color0 in place (zero copies,
        # zero CPU readbacks, no private device; supersedes the subgraph path when
        # gated on). Gate MC2_VK_POST_FOG_NATIVE default-OFF -> GL fog chain
        # byte-identical. Layout-affecting header members under MC2_VULKAN_ISLAND
        # (same ODR discipline as the subgraph's).
        list(APPEND SOURCES vulkan_post_fog_native.cpp)
        # VK-POST-SSAO-NATIVE-1 (rung 8.2): SSAO gen+apply natively on the shared
        # engine context — samples the SHARED scene depth+normal into a VK-owned
        # half-res R16F occlusion image, multiplies into the SHARED scene color0 in
        # place (zero copies, zero CPU readbacks, no private device; retires the
        # SSAO island's readback pattern when gated on). Gate MC2_VK_POST_SSAO
        # default-OFF -> GL SSAO chain byte-identical.
        list(APPEND SOURCES vulkan_post_ssao_native.cpp)
        # VK-POST-SCREENFX-NATIVE-1 (rung 8.3): cloud-shadow + shoreline-foam
        # natively on the shared engine context -- samples the SHARED scene
        # depth (cloud) + normal (shoreline), multiplies into the SHARED scene
        # color0 in place (zero copies, zero readbacks, no private device).
        # Screen-shadow NOT ported (no zero-copy CSM-array producer until rung 2).
        # Gate MC2_VK_POST_SCREENFX default-OFF -> GL passes byte-identical.
        list(APPEND SOURCES vulkan_post_screenfx_native.cpp)
        # VK-POST-HZB-NATIVE-1 (rung 8.4): native HZB pyramid on the shared
        # engine context.
        list(APPEND SOURCES vulkan_post_hzb_native.cpp)
        # VK-P8a SHADOW-INTEROP: the shadow island — SECOND VK GEOMETRY region and
        # FIRST VK-PRODUCES-DEPTH region (water was GL-produces/VK-consumes; shadow
        # reverses it: VK renders the static-prop shadow-caster depth into an
        # exportable D32, GL samples it via the unchanged hardware calcShadow path,
        # lighting up the vk2gl back-channel). Borrows the shared engine
        # VulkanContext (no second device). Gate MC2_VK_SHADOW_INTEROP default-OFF
        # -> GL shadow path byte-identical. extern "C" entry points only.
        list(APPEND SOURCES vulkan_shadow_island_live.cpp)
        # VK-P11 STATIC-PROP COLOR: the color island — FIRST VK-native COLOR geometry
        # region rendering REAL static-prop triangle geometry in-place into the shared
        # scene GBuffer instead of the GL GpuStaticPropBatcher::flush() color draws. Borrows the shared
        # engine VulkanContext (no second device) + reuses the shadow island's shared
        # static-prop VBO/IBO feed. Gate MC2_VK_PROP_COLOR_TIER1 default-OFF -> GL prop
        # color path byte-identical. extern "C" entry points only (no layout-affecting
        # header members).
        list(APPEND SOURCES vulkan_color_prop_island.cpp)
        # VK-P10 TERRAIN TIER-1: the terrain island — co-writes the LIVE LOD0
        # (lodStep==1) terrain opaque MAIN-triangle draw IN-PLACE into the SHARED
        # scene GBuffer (color MRT0 + normal MRT1 a=1.0 + shared depth) as the opaque
        # PREFIX, reusing the prop Tier-1 shared-GBuffer plumbing + LOAD-preserve
        # renderpass + reverse-Z/double-sided pipeline. Consumes the GL height SSBO
        # via a per-change interop snapshot. Gate MC2_VK_TERRAIN_TIER1 default-OFF
        # (requires MC2_VK_SHARED_SCENE_DEPTH) -> GL terrain path byte-identical.
        # extern "C" entry points only (no layout-affecting header members).
        list(APPEND SOURCES vulkan_terrain_island.cpp)
        # VK-P14 MECH TIER-1: the mech island — the FIRST VK-native SKINNED region.
        # Co-writes the GL mech batcher's visible instances IN-PLACE into the SHARED
        # scene GBuffer (color MRT0 + normal MRT1 a=0.0 + shared depth), VK-skinned
        # (linear-blend over 4 bone weights + MC2 axis swap), reusing the prop Tier-1
        # shared-GBuffer plumbing (LOAD-preserve renderpass + reverse-Z pipeline).
        # Feed (instances + bone palette + shared VBO/IBO + per-run slot-0 paint tex)
        # exported by gos_mech_batcher.cpp. Gate MC2_VK_MECH_TIER1 default-OFF (requires
        # MC2_VK_SHARED_SCENE_DEPTH) -> GL mech color flush byte-identical. extern "C"
        # entry points only (no layout-affecting header members).
        list(APPEND SOURCES vulkan_mech_island.cpp)
        # VK-PARITY-F1: the legacy objects-loop island (ground vehicles etc.). Armed with
        # the mech island (no env of its own); gate off -> GL loop byte-identical.
        list(APPEND SOURCES vulkan_legacy_shape_island.cpp)
        # VK-PARITY-F3: HDRI equirect sky pass (VK had a flat clear colour instead).
        list(APPEND SOURCES vulkan_sky_island.cpp)
        # VK-FIELD-VFX-1: the VFX particle island — co-writes the GPU-particle
        # billboard stream (gos_particle_bridge) IN-PLACE into the SHARED scene
        # color0 as translucent, depth-tested (GEQUAL, no-write) colored soft
        # billboards, so weapon fire survives the VK-only present umbrella. Gate
        # MC2_VK_VFX default-ON under MC2_VK_SHARED_GBUFFER (vanishing content),
        # explicit =0 forces off -> GL particle draw. UNTEXTURED slice (textured
        # sprites + tube ribbons + gosFX mesh shapes are the deferred remainder).
        # extern "C" entry points only (no layout-affecting header members).
        list(APPEND SOURCES vulkan_vfx_island.cpp)
        # VK-INTEROP-BRIDGE-1 / T1: the GL-free VK-owned image core. vk_image_resource =
        # raw-allocated (dedicated) NON-exportable VkImage + double-buffered sync objects.
        # VK-RUNG8-ITEM1 deleted gl_interop_view.cpp (the GL-boundary import adapter) and
        # VK-RUNG8-ITEM2 deleted vk_image_resource's exportable half -- both caller-free
        # after item 6 retired the static-shadow substrate. #ifdef MC2_VULKAN_ISLAND.
        list(APPEND SOURCES vk_image_resource.cpp)
        # VK-LIGHTFEED-DEDUP-1: ONE shared per-frame LightsData SSBO (3-slot
        # host-visible ring on the shared engine device) consumed by the mech +
        # color-prop islands instead of twin per-island ~3.5 MB copies of the
        # same CPU array every frame. Killswitch MC2_VK_LIGHTFEED_DEDUP=0
        # restores the twin-copy paths. #ifdef MC2_VULKAN_ISLAND.
        list(APPEND SOURCES vk_shared_light_feed.cpp)
        # VK-TIER1 SHARED SCENE DEPTH (step 2): back sceneDepthTex_ with a VK-built
        # exportable D32S8 image imported into GL as GL_DEPTH24_STENCIL8, at the
        # single gos_postprocess scene-FBO depth alloc+attach site. GL-only slice
        # (no VK depth consumer yet) using the interop primitives above. Gate
        # MC2_VK_SHARED_SCENE_DEPTH default-OFF -> plain GL depth alloc (byte-
        # identical). Layout-affecting header member (sharedSceneDepth_) added
        # under #ifdef MC2_VULKAN_ISLAND, propagated by the root ODR-harden block.
        list(APPEND SOURCES vulkan_shared_scene_depth.cpp)
        # VK-P11 TIER-1: shared scene COLOR/NORMAL substrate — sibling of the shared
        # depth above. Backs sceneColorTex_ (MRT0) + sceneNormalTex_ (MRT1) with VK-
        # built exportable RGBA16F images imported into GL, so the VK prop island co-
        # writes props into the SAME GBuffer attachments in-place (Tier-1). Gate
        # MC2_VK_PROP_COLOR_TIER1 default-OFF (requires MC2_VK_SHARED_SCENE_DEPTH) ->
        # plain GL alloc (byte-identical). Layout-affecting header members
        # (sharedSceneColor_/sharedSceneNormal_) under #ifdef MC2_VULKAN_ISLAND.
        list(APPEND SOURCES vulkan_shared_scene_color.cpp)
        # VK-P13-SELECTOR-1 / T3: the whole-frame GL->VK present intercept. Borrows
        # the T2 shared VulkanContext device (no second device); owns the hidden
        # present window + surface + swapchain + the exportable interop image. Uses
        # the interop primitives above, so it is gated on MC2_VULKAN_ISLAND (the TU
        # #if requires BOTH MC2_VULKAN and MC2_VULKAN_ISLAND). No new link libs.
        list(APPEND SOURCES engine_present_vk.cpp)
    endif()
endif()

set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/3rdparty/tinyexr/miniz.c
    PROPERTIES LANGUAGE C
)

set(MAIN_SRC gameosmain.cpp)

#find_package(SDL2 REQUIRED PATHS "/opt" NO_DEFAULT_PATH)
#include_directories("${SDL2_PREFIX}/include")

include_directories("../include"  ".")
include_directories(${THIRDPARTY_INCLUDE_DIRS})
# gos_static_prop_batcher.cpp pulls tgl.h / msl.h / Stuff from mclib.
include_directories("${CMAKE_SOURCE_DIR}/mclib" "${CMAKE_SOURCE_DIR}/mclib/stuff")
# RenderCore/ headers (e.g. RenderPassContract.h) use repo-root-relative includes.
include_directories("${CMAKE_SOURCE_DIR}")
# gos_hdri.cpp needs tinyexr.h (defines TINYEXR_IMPLEMENTATION).
include_directories("${CMAKE_SOURCE_DIR}/3rdparty/tinyexr")

add_library(gameos ${SOURCES})
# KtxLoader.cpp lives in rendercore (STATIC) and is called from
# gos_static_prop_batcher.cpp. Make the dependency explicit so CMake
# orders the link correctly and gameos_editor gets the same resolution.
target_link_libraries(gameos PRIVATE rendercore)

# VULKAN-BACKEND-SKELETON-1: link the Vulkan loader + define MC2_VULKAN only
# when the option is ON. gameos_editor gets the same treatment below.
if(MC2_VULKAN)
    # VULKAN-VOLK-LOADER-1: headers only -- volk loads vulkan-1.dll at runtime,
    # so we must NOT link the loader import lib (Vulkan::Vulkan).
    target_link_libraries(gameos PRIVATE Vulkan::Headers)
    target_compile_definitions(gameos PRIVATE MC2_VULKAN)
    # VULKAN-VMA-INTEGRATION-1: vendored header-only VMA (3rdparty/vma).
    target_include_directories(gameos PRIVATE "${CMAKE_SOURCE_DIR}/3rdparty/vma")
    # VULKAN-VOLK-LOADER-1: vendored volk (volk.h/volk.c).
    target_include_directories(gameos PRIVATE "${CMAKE_SOURCE_DIR}/3rdparty/volk")
    # VK-BOOTSTRAP-INTEGRATE-1: vendored vk-bootstrap (SwapchainBuilder).
    target_include_directories(gameos PRIVATE "${CMAKE_SOURCE_DIR}/3rdparty/vk_bootstrap")
    # VK-IMGUI-NATIVE-1: engine_present_vk.cpp owns the Vulkan renderer backend.
    target_include_directories(gameos PRIVATE
        "${CMAKE_SOURCE_DIR}/3rdparty/imgui"
        "${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends")
    # MC2_VULKAN_ISLAND: define the island/native-region gate on gameos so
    # gos_postprocess.cpp's Vulkan-owned render paths are compiled.
    # MUST be PUBLIC: gos_postprocess.h adds a member (vkFogIsland_) + methods under
    # #ifdef MC2_VULKAN_ISLAND, so the gosPostProcess class LAYOUT depends on this
    # macro. Consumers of the header (e.g. gameosmain.cpp in the mc2 target) must
    # see the same layout, or getSceneFBO()/getWidth()/getHeight() read the wrong
    # offset and return garbage (ODR violation -> broken FBO readback / capture).
    # The gated header block only declares an opaque pointer + method decls, so no
    # Vulkan headers leak to consumers.
    if(MC2_VULKAN_ISLAND)
        target_compile_definitions(gameos PUBLIC MC2_VULKAN_ISLAND)
    endif()
endif()

# VULKAN-SHADER-TOOLCHAIN-1: compile the trivial fullscreen GLSL vert/frag to
# SPIR-V at build time -- ONLY when MC2_VULKAN=ON. OFF -> this target/step does
# not exist (no SDK tool needed, default build byte-identical). Compiler
# (glslangValidator or glslc) located via find_program from the Vulkan SDK;
# if neither is found we warn + skip (don't hard-fail the ON build).
if(MC2_VULKAN)
    find_program(MC2_GLSLANG_VALIDATOR
        NAMES glslangValidator
        HINTS "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" ENV VULKAN_SDK
        PATH_SUFFIXES Bin bin)
    find_program(MC2_GLSLC
        NAMES glslc
        HINTS ENV VULKAN_SDK
        PATH_SUFFIXES Bin bin)

    set(MC2_VK_SHADER_SRC_DIR "${CMAKE_SOURCE_DIR}/shaders/vulkan")
    set(MC2_VK_SPV_OUT_DIR "${CMAKE_BINARY_DIR}/shaders/vulkan")
    file(MAKE_DIRECTORY "${MC2_VK_SPV_OUT_DIR}")

    # VULKAN-EDGE-FOG-ISLAND-2a: bake edge_fog.{vert,frag} alongside fullscreen.
    # These are only USED when MC2_VULKAN_ISLAND=ON, but baking them under plain
    # MC2_VULKAN is harmless (extra .spv outputs) and keeps the toolchain simple.
    # VULKAN-OOB-FOG-ISLAND-1: bake fog_oob.{vert,frag} alongside edge_fog.
    # VK-REGION-SSAO-1: bake ssao.{vert,frag} + ssao_apply.{vert,frag} (two
    # programs: occlusion pass1, multiplicative-apply pass2). Both verts are
    # the same fullscreen-triangle construction (separate files only because
    # the bake loop expects one vert per basename).
    # VK-REGION-HZB-1: bake hzb_reduce.{vert,frag} (one program, N level draws).
    # VK-COMPARE-SAMPLER-FIXTURE-1: bake compare_shadow.{vert,frag} (depth-COMPARE
    # sampler oracle) + csm_array_shadow.{vert,frag} (2D_ARRAY compare oracle).
    # VK-REGION-SHADOW-1: bake shadow_depth.{vert,frag} (depth-only INSTANCED
    # shadow-caster WRITE pipeline -- vert reads the instance ring SSBO).
    # WATER-MVP-ORACLE-1: bake water_mvp.{vert,frag} (clip-space oracle proving the
    # water MVP UBO pack + reverse-Z depth fudge reproduces the GL water VS
    # bit-tight; frag does M*worldPos + fudge, column_major std140 UBO).
    # VK-P9a WATER-INTEROP: bake water_geom.{vert,frag} (procedural water mesh VS +
    # base-layer FS for the first VK GEOMETRY region). SPIR-V loaded at runtime by
    # vulkan_water_island.cpp via load_spv(spv_dir()+"water_geom.*.spv").
    # VK-P11 STATIC-PROP COLOR: bake color_prop_tier1.{vert,frag} for the in-place
    # shared-GBuffer prop path. The retired Tier-0 color_prop shaders are no longer
    # baked or deployed.
    # VK-P10 TERRAIN TIER-1: bake terrain_tier1.{vert,frag} (LOD0 terrain co-write VS
    # reading the height SSBO snapshot + 2-output GBuffer FS with a=1.0). SPIR-V loaded
    # at runtime by vulkan_terrain_island.cpp via load_spv(spv_dir()+"terrain_tier1.*.spv").
    # VK-P14 MECH TIER-1: bake mech_tier1.{vert,frag} (skinned mech co-write VS reading
    # the instance + bone-palette SSBOs + 2-output GBuffer FS with a=0.0). SPIR-V loaded
    # at runtime by vulkan_mech_island.cpp via load_spv(spv_dir()+"mech_tier1.*.spv").
    # VK-TERRAIN-SKIRT (rung 4): bake terrain_tier1_skirt.{vert,frag} (skirt-strip VS
    # applying the u_skirtDepth pull-down + a byte-identical copy of terrain_tier1.frag,
    # since GL uses the same frag program for skirts as the main patch). SPIR-V loaded
    # at runtime by vulkan_terrain_island.cpp via load_spv(spv_dir()+"terrain_tier1_skirt.*.spv").
    # VK-FIELD-VFX-1: bake vfx_particle.{vert,frag} (procedural billboard VS reading the
    # GpuParticle SSBO + untextured colored soft-blob FS). SPIR-V loaded at runtime by
    # vulkan_vfx_island.cpp via load_spv(spv_dir()+"vfx_particle.*.spv").
    set(MC2_VK_SHADER_BASENAMES fullscreen edge_fog fog_oob ssao ssao_apply hzb_reduce compare_shadow csm_array_shadow shadow_depth shadow_debug_overlay water_mvp water_geom shadow_prop color_prop_tier1 terrain_tier1 terrain_tier1_skirt mech_tier1 legacy_shape_tier1 terrain_overlay_tier1 terrain_decal_tier1 hdri_sky cloud_shadow shoreline screen_shadow_static screen_shadow_csm postprocess postprocess_visual hud_color hud_tex vfx_particle)
    set(MC2_VK_SPV_OUTPUTS "")
    # VK-PARITY-TERRAIN-2: shaders #include files from shaders/vulkan/include and the
    # shared GL/VK shaders/include/*.hglsl (e.g. terrain_light_pool.hglsl); list them as
    # DEPENDS so editing an include re-bakes the SPIR-V (the bake step has no depfile).
    file(GLOB MC2_VK_SHADER_INCLUDES
        "${MC2_VK_SHADER_SRC_DIR}/include/*.glsl"
        "${CMAKE_SOURCE_DIR}/shaders/include/*.hglsl")
    if(MC2_GLSLANG_VALIDATOR OR MC2_GLSLC)
        foreach(_base ${MC2_VK_SHADER_BASENAMES})
        foreach(_stage vert frag)
            set(_src "${MC2_VK_SHADER_SRC_DIR}/${_base}.${_stage}")
            set(_spv "${MC2_VK_SPV_OUT_DIR}/${_base}.${_stage}.spv")
            if(MC2_GLSLANG_VALIDATOR)
                add_custom_command(
                    OUTPUT "${_spv}"
                    COMMAND "${MC2_GLSLANG_VALIDATOR}" -V "${_src}" -o "${_spv}"
                    DEPENDS "${_src}" ${MC2_VK_SHADER_INCLUDES}
                    COMMENT "SPIR-V (glslang): ${_base}.${_stage} -> ${_spv}"
                    VERBATIM)
            else()
                add_custom_command(
                    OUTPUT "${_spv}"
                    COMMAND "${MC2_GLSLC}" "${_src}" -o "${_spv}"
                    DEPENDS "${_src}" ${MC2_VK_SHADER_INCLUDES}
                    COMMENT "SPIR-V (glslc): ${_base}.${_stage} -> ${_spv}"
                    VERBATIM)
            endif()
            list(APPEND MC2_VK_SPV_OUTPUTS "${_spv}")
        endforeach()
        endforeach()
        add_custom_target(mc2_vulkan_shaders ALL DEPENDS ${MC2_VK_SPV_OUTPUTS})
        message(STATUS "VULKAN-SHADER-TOOLCHAIN-1: SPIR-V compile ON -> ${MC2_VK_SPV_OUT_DIR}")
    else()
        message(WARNING "VULKAN-SHADER-TOOLCHAIN-1: neither glslangValidator nor glslc found (Vulkan SDK tools); skipping SPIR-V compile. Shader probe will fail-soft.")
    endif()
endif()

# gameos_editor: same sources as gameos minus gos_render.cpp (the GL
# renderer main). The MC2 Mission Editor (EditRel) links this instead of
# gameos to avoid pulling in a second renderer init. From engine-standalone PR #36.
set(GAMEOS_EDITOR_SOURCES ${SOURCES})
list(REMOVE_ITEM GAMEOS_EDITOR_SOURCES gos_render.cpp)
add_library(gameos_editor ${GAMEOS_EDITOR_SOURCES})
target_link_libraries(gameos_editor PRIVATE rendercore)
if(MC2_VULKAN)
    # VULKAN-VOLK-LOADER-1: headers only -- volk owns runtime dispatch.
    target_link_libraries(gameos_editor PRIVATE Vulkan::Headers)
    target_compile_definitions(gameos_editor PRIVATE MC2_VULKAN)
    # VULKAN-VMA-INTEGRATION-1: vendored header-only VMA (3rdparty/vma).
    target_include_directories(gameos_editor PRIVATE "${CMAKE_SOURCE_DIR}/3rdparty/vma")
    # VULKAN-VOLK-LOADER-1: vendored volk (volk.h/volk.c).
    target_include_directories(gameos_editor PRIVATE "${CMAKE_SOURCE_DIR}/3rdparty/volk")
    # VK-BOOTSTRAP-INTEGRATE-1: vendored vk-bootstrap (SwapchainBuilder).
    target_include_directories(gameos_editor PRIVATE "${CMAKE_SOURCE_DIR}/3rdparty/vk_bootstrap")
    # engine_present_vk.cpp (shared SOURCES) includes imgui headers; gameos_editor was missing the dirs.
    target_include_directories(gameos_editor PRIVATE
        "${CMAKE_SOURCE_DIR}/3rdparty/imgui"
        "${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends")
endif()
# VULKAN-ISLAND-ODR-HARDEN-1: gameos_editor uses the same ${SOURCES} as gameos
# (incl. gos_postprocess.cpp) and its consumers (editor/*.cpp) include
# gos_postprocess.h, so its gosPostProcess layout must match the shared static
# libs (mclib/renderworld/gameadapters) it links. Propagate the island define so
# EditRel's link is uniform if ever built with MC2_VULKAN_ISLAND=ON. No effect on
# any shipped build: the island option is OFF whenever the editor is built.
if(MC2_VULKAN_ISLAND)
    target_compile_definitions(gameos_editor PRIVATE MC2_VULKAN_ISLAND)
endif()

add_library(gameos_main ${MAIN_SRC})
