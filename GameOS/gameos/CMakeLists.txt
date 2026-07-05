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
    debug_state_dump.cpp
    dbg_asset_globals.cpp
    diagnostic_trace.cpp
    visual_tuning_profile.cpp
    terrain_material_lib.cpp

    utils/stream.cpp
    utils/camera.cpp
    utils/gl_utils.cpp
    utils/gl_render_constants.cpp
    utils/Image.cpp
    utils/logging.cpp
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
    # VK-BOOTSTRAP-INTEGRATE-1: swapchain probe TU + vendored vk-bootstrap impl.
    list(APPEND SOURCES vulkan_swapchain_probe.cpp)
    # VULKAN-SWAPCHAIN-PRESENT-1: Layer-5 swapchain PRESENT path (own window +
    # swapchain, present 16 frames, resize, fail-soft). Same MC2_VULKAN gate.
    list(APPEND SOURCES vulkan_swapchain_present.cpp)
    list(APPEND SOURCES "${CMAKE_SOURCE_DIR}/3rdparty/vk_bootstrap/VkBootstrap.cpp")
    # VULKAN-VMA-INTEGRATION-1: single VMA_IMPLEMENTATION TU (header-only lib).
    list(APPEND SOURCES vma_impl.cpp)
    # VULKAN-VOLK-LOADER-1: volk meta-loader (dynamic Vulkan load; no hard link
    # to vulkan-1.dll). Only compiled when MC2_VULKAN=ON.
    list(APPEND SOURCES "${CMAKE_SOURCE_DIR}/3rdparty/volk/volk.c")
    # VULKAN-EDGE-FOG-ISLAND-2a: the real edge-fog Vulkan render island (compiled
    # into gameos, but entirely inert unless MC2_VULKAN_ISLAND is also defined --
    # the whole TU is #ifdef MC2_VULKAN_ISLAND). Only added when MC2_VULKAN_ISLAND=ON.
    if(MC2_VULKAN_ISLAND)
        list(APPEND SOURCES vulkan_edge_fog_island.cpp)
        # VULKAN-OOB-FOG-ISLAND-1: the second offscreen Vulkan island (OOB fog).
        # Same #ifdef MC2_VULKAN_ISLAND discipline; the layout-affecting header
        # members are added under the same macro, already propagated to all
        # consumers by the ODR-harden block in the root CMakeLists.
        list(APPEND SOURCES vulkan_oob_fog_island.cpp)
        # VULKAN-POSTPROCESS-SUBGRAPH-1: the Layer-4 fused subgraph (EdgeFog + OOB
        # fog in ONE native Vulkan render pass, Vulkan-owned intermediate color
        # image, no CPU readback between the two passes). Same #ifdef MC2_VULKAN_ISLAND
        # discipline; layout-affecting header members added under the same macro,
        # already propagated to all consumers by the root ODR-harden block.
        list(APPEND SOURCES vulkan_postprocess_subgraph.cpp)
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
    # VULKAN-EDGE-FOG-ISLAND-2a: define the island gate on gameos so
    # vulkan_edge_fog_island.cpp compiles + the gos_postprocess.cpp seam is active.
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
    set(MC2_VK_SHADER_BASENAMES fullscreen edge_fog fog_oob)
    set(MC2_VK_SPV_OUTPUTS "")
    if(MC2_GLSLANG_VALIDATOR OR MC2_GLSLC)
        foreach(_base ${MC2_VK_SHADER_BASENAMES})
        foreach(_stage vert frag)
            set(_src "${MC2_VK_SHADER_SRC_DIR}/${_base}.${_stage}")
            set(_spv "${MC2_VK_SPV_OUT_DIR}/${_base}.${_stage}.spv")
            if(MC2_GLSLANG_VALIDATOR)
                add_custom_command(
                    OUTPUT "${_spv}"
                    COMMAND "${MC2_GLSLANG_VALIDATOR}" -V "${_src}" -o "${_spv}"
                    DEPENDS "${_src}"
                    COMMENT "SPIR-V (glslang): ${_base}.${_stage} -> ${_spv}"
                    VERBATIM)
            else()
                add_custom_command(
                    OUTPUT "${_spv}"
                    COMMAND "${MC2_GLSLC}" "${_src}" -o "${_spv}"
                    DEPENDS "${_src}"
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

