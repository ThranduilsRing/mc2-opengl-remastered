cmake_minimum_required (VERSION 3.10)
project(gameos)

# Profiling is local debugging: never advertise or accept remote connections.
set_source_files_properties(${CMAKE_SOURCE_DIR}/3rdparty/tracy/TracyClient.cpp
    PROPERTIES COMPILE_DEFINITIONS "TRACY_ONLY_LOCALHOST;TRACY_NO_BROADCAST")

# G2: the root configure rejects MC2_VULKAN=OFF (Vulkan is the required renderer backend). Fail here
# too so a standalone/partial configure cannot silently drop the Vulkan sources and the
# MC2_VULKAN_ISLAND layout define. Unset (standalone tool trees) keeps the historical behaviour.
if(DEFINED MC2_VULKAN AND NOT MC2_VULKAN)
    message(FATAL_ERROR "gameos requires MC2_VULKAN=ON (Vulkan is the required renderer backend).")
endif()

set(SOURCES ${SOURCES}
    gameos.cpp
    gameos_graphics.cpp
    gameos_graphics_draw.cpp
    gameos_graphics_water.cpp
    gameos_graphics_bridge.cpp
    gameos_graphics_shadow.cpp
    gameos_graphics_tparams.cpp
    gameos_graphics_hud.cpp
    gameos_graphics_capi.cpp
    gameos_graphics_overlay.cpp
    gameos_graphics_font.cpp
    gameos_graphics_texture.cpp
    texture_decode.cpp
    terrain_field_store.cpp
    static_caster_set.cpp
    terrain_surface_asset.cpp
    cook_cache.cpp
    shadow_mask_asset.cpp
    terrain_surface_cook.cpp
    gameos_graphics_params.cpp
    gameos_res.cpp
    MC2Strings.cpp
    gameos_fileio.cpp
    gameos_input.cpp
    gameos_debugging.cpp
    gameos_sound.cpp
    gos_render.cpp
    render_frame_driver.cpp
    gos_hdri.cpp
    gos_postprocess.cpp
    gos_postprocess_gates.cpp
    gos_postprocess_targets.cpp
    gos_postprocess_skybox.cpp
    gos_postprocess_executor.cpp
    gos_static_prop_batcher.cpp
    # The gos_static_prop_batcher pieces below are contiguous slices of the old file: keep them in this order (namespace-scope initialisers run in link order).
    gos_static_prop_batcher_state.cpp
    gos_static_prop_batcher_program.cpp
    gos_static_prop_batcher_mapload.cpp
    gos_static_prop_batcher_finalize.cpp
    gos_static_prop_batcher_finalize_textures.cpp
    gos_static_prop_batcher_submit.cpp
    gos_static_prop_batcher_flush.cpp
    gos_static_prop_batcher_shadow.cpp
    gos_static_prop_batcher_seam.cpp
    gos_static_prop_registry.cpp
    gos_static_prop_registry_flush_range.cpp
    gos_static_prop_registry_flush_tail.cpp
    gos_particle_bridge.cpp
    gos_particle_bridge_tube.cpp
    gos_vfx_mesh_bridge.cpp
    gos_vegetation.cpp
    gos_materials.cpp
    gos_mech_batcher.cpp
    gos_mech_batcher_geometry.cpp
    gos_mech_batcher_flush.cpp
    gos_mech_batcher_api.cpp
    gos_object_recon_tracy.cpp
    gos_terrain_water_stream.cpp
    gos_terrain_indirect.cpp
    gos_terrain_indirect_diag.cpp
    gos_terrain_indirect_atlas.cpp
    gos_terrain_indirect_cement.cpp
    gos_terrain_indirect_dispatch.cpp
    gos_terrain_indirect_mine_decal.cpp
    gos_terrain_lighting.cpp
    gos_terrain_lod_chunk.cpp
    gos_terrain_lod_chunk_gates.cpp
    gos_terrain_lod_chunk_feed.cpp
    gos_terrain_lod_chunk_draw.cpp
    gos_terrain_lod_chunk_upload.cpp
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
    sim_overlap.cpp
    gpu_memory_report.cpp
    debug_renderer.cpp
    gos_font.cpp
    gos_input.cpp
    asset_scale.cpp
    gpu_driven_common.cpp
    gpu_cull_record.cpp
    gpu_cull_substrate.cpp
    gpu_cull_parity.cpp
    gpu_cull_compute.cpp
    vk_actor_cull_native.cpp
    vk_vegetation_state.cpp
    vk_scene_readback.cpp
    native_render_state.cpp
    gpu_cull_readback.cpp
    render_snapshot.cpp
    gos_render_pass_timer.cpp
    vk_render_pass_timer.cpp
    vk_gpu_profile.cpp
    gos_frame_pass_stats.cpp
    draw_packet_emitter.cpp
    view_registry.cpp
    debug_state_dump.cpp
    runtime_bridge.cpp
    runtime_bridge_protocol.cpp
    dbg_asset_globals.cpp
    diagnostic_trace.cpp
    visual_tuning_profile.cpp
    terrain_material_lib.cpp
    # VK-P13-SELECTOR-1 / T1: top-level MC2_RENDER_BACKEND selector (GL-safe; no
    # Vulkan headers, always compiled -- inert on =gl/unset, byte-identical).
    render_backend_select.cpp

    utils/stream.cpp
    utils/color_utils.cpp
    utils/Image.cpp
    utils/mc2log.cpp
    utils/vec.cpp
    utils/timing.cpp
    ${CMAKE_SOURCE_DIR}/3rdparty/tracy/TracyClient.cpp
    ${CMAKE_SOURCE_DIR}/3rdparty/tinyexr/miniz.c
    utils/string_utils.cpp
    utils/file_utils.cpp
    mc2_hitch_trace.cpp
    )

# Vulkan sources compile only when MC2_VULKAN=ON; when OFF no Vulkan headers/loader are pulled in.
if(MC2_VULKAN)
    # VK-P13-SELECTOR-1 / T2: the standalone SHARED engine Vulkan context
    # (instance/device/queue/VMA owned HERE). Entirely #ifdef MC2_VULKAN.
    list(APPEND SOURCES vulkan_context.cpp vk_texture_residency.cpp vk_render_infra.cpp)
    list(APPEND SOURCES vk_device_profile.cpp)
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
        list(APPEND SOURCES vulkan_water_island_images.cpp vulkan_water_island_init.cpp vulkan_water_island_run.cpp)
        # VK-POST-FOG-NATIVE-1 (rung 8.1, VK-RUNG8-LADDER-1): the fog pair (edge+oob)
        # rendered NATIVELY on the shared engine VulkanContext — samples the SHARED
        # scene depth, blends into the SHARED scene color0 in place (zero copies,
        # zero CPU readbacks, no private device; supersedes the subgraph path when
        # gated on). Gate MC2_VK_POST_FOG_NATIVE default-OFF -> GL fog chain
        # byte-identical. Layout-affecting header members under MC2_VULKAN_ISLAND
        # (same ODR discipline as the subgraph's).
        list(APPEND SOURCES vulkan_post_fog_native.cpp)
        # VK-POST-SCREENFX-NATIVE-1 (rung 8.3): cloud-shadow + shoreline-foam
        # natively on the shared engine context -- samples the SHARED scene
        # depth (cloud) + normal (shoreline), multiplies into the SHARED scene
        # color0 in place (zero copies, zero readbacks, no private device).
        # Screen-shadow NOT ported (no zero-copy CSM-array producer until rung 2).
        # Gate MC2_VK_POST_SCREENFX default-OFF -> GL passes byte-identical.
        list(APPEND SOURCES vulkan_post_screenfx_native.cpp)
        # Weapon impact box decals (docs/impact-damage-design.md): instanced boxes over the shared depth/normal,
        # multiply + emissive into the shared scene colour mid-frame.
        list(APPEND SOURCES vulkan_impact_decals.cpp)
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
        list(APPEND SOURCES vulkan_shadow_island_live.cpp vulkan_mover_shadow.cpp vulkan_shadow_mech_casters.cpp vulkan_mech_skin.cpp vulkan_shadow_bake.cpp vulkan_shadow_bake_cache.cpp vk_depth_caster_pipeline.cpp)
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
        list(APPEND SOURCES vulkan_terrain_island_island.cpp)
        list(APPEND SOURCES vulkan_terrain_island_images.cpp)
        list(APPEND SOURCES vulkan_terrain_island_run.cpp)
        list(APPEND SOURCES vulkan_terrain_island_overlay.cpp)
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
        # The first layout of the shared scene images (UNDEFINED at creation; every island declares an attachment layout from frame 1).
        list(APPEND SOURCES vk_first_layout.cpp)
        # Record, submit and wait for one setup command buffer (first layouts, query pool resets).
        list(APPEND SOURCES vk_one_shot.cpp)
        # Per-frame transient memory (FrameRing): two slot regions of one host-visible buffer, reclaimed by completion tickets.
        list(APPEND SOURCES vk_frame_ring.cpp)
        # Streaming uploads: staging in the frame ring, copies queued and submitted ahead of the next graphics submit, no CPU wait.
        list(APPEND SOURCES vk_upload.cpp)
        # Non-blocking GPU -> CPU readbacks (ReadbackRing): request, submit, poll by ticket, tag-checked.
        list(APPEND SOURCES vk_readback.cpp)
        # Move-only owners of the Vulkan objects an island creates (RAII; the destructor is the only release path).
        list(APPEND SOURCES vk_owned.cpp)
        # STEP 3.4P: native Vulkan compute producers (Slang compute pipelines; terrain lighting + water cull/pack).
        list(APPEND SOURCES vk_compute_pipeline.cpp vk_native_producers.cpp)
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
        list(APPEND SOURCES engine_present_vk.cpp vulkan_spatial_aa.cpp)
    endif()
endif()

set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/3rdparty/tinyexr/miniz.c
    PROPERTIES LANGUAGE C
)

# Vulkan bring-up + present shared by the game and the editor.
list(APPEND SOURCES gos_host_vk.cpp present_thread.cpp)

set(MAIN_SRC gameosmain.cpp gameosmain_init.cpp gameosmain_frame.cpp)

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

# meshoptimizer subset (vertex cache / overdraw / fetch ordering) for the static-prop batcher.
add_library(mc2_meshopt STATIC
    "${CMAKE_SOURCE_DIR}/3rdparty/meshoptimizer/src/allocator.cpp"
    "${CMAKE_SOURCE_DIR}/3rdparty/meshoptimizer/src/vcacheoptimizer.cpp"
    "${CMAKE_SOURCE_DIR}/3rdparty/meshoptimizer/src/vcacheanalyzer.cpp"
    "${CMAKE_SOURCE_DIR}/3rdparty/meshoptimizer/src/overdrawoptimizer.cpp"
    "${CMAKE_SOURCE_DIR}/3rdparty/meshoptimizer/src/vfetchoptimizer.cpp")
target_include_directories(mc2_meshopt PUBLIC "${CMAKE_SOURCE_DIR}/3rdparty/meshoptimizer/src")

add_library(gameos ${SOURCES})
target_link_libraries(gameos PRIVATE mc2_meshopt)
mc2_enable_pch(gameos)
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

# VULKAN-SHADER-TOOLCHAIN-1: compile the GLSL listed in MC2_VK_SHADER_BASENAMES to
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

    # VULKAN-EDGE-FOG-ISLAND-2a: bake edge_fog.{vert,frag} with the rest of the list.
    # These are only USED when MC2_VULKAN_ISLAND=ON, but baking them under plain
    # MC2_VULKAN is harmless (extra .spv outputs) and keeps the toolchain simple.
    # VULKAN-OOB-FOG-ISLAND-1: bake fog_oob.{vert,frag} alongside edge_fog.
    # VK-REGION-SSAO-1: bake ssao.{vert,frag} + ssao_apply.{vert,frag} (two
    # programs: occlusion pass1, multiplicative-apply pass2). Both verts are
    # the same fullscreen-triangle construction (separate files only because
    # the bake loop expects one vert per basename).
    # VK-REGION-HZB-1: bake hzb_reduce.{vert,frag} (one program, N level draws).
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
    # applying the u_skirtDepth pull-down; the frag is baked from terrain_tier1.frag
    # itself -- see MC2_VK_SHADER_SRC_ALIAS -- since GL uses the same frag program for
    # skirts as the main patch). SPIR-V loaded at runtime by vulkan_terrain_island.cpp
    # via load_spv(spv_dir()+"terrain_tier1_skirt.*.spv").
    # VK-FIELD-VFX-1: bake vfx_particle.{vert,frag} (procedural billboard VS reading the
    # GpuParticle SSBO + untextured colored soft-blob FS). SPIR-V loaded at runtime by
    # vulkan_vfx_island.cpp via load_spv(spv_dir()+"vfx_particle.*.spv").
    set(MC2_VK_SHADER_BASENAMES edge_fog fog_oob hzb_reduce hzb_cull shadow_debug_overlay water_geom shadow_prop shadow_prop_alpha shadow_mech shadow_terrain_static color_prop_tier1 terrain_tier1 terrain_tier1_skirt mech_tier1 mech_depth legacy_shape_tier1 terrain_overlay_tier1 terrain_decal_tier1 hdri_sky cloud_shadow shoreline screen_shadow_csm impact_decal postprocess postprocess_visual hud_color hud_tex vfx_particle)
    set(MC2_VK_SPV_OUTPUTS "")
    list(APPEND MC2_VK_SHADER_BASENAMES aa_smaa_edges aa_smaa_weights aa_smaa_blend aa_fxaa)
    list(APPEND MC2_VK_SHADER_BASENAMES water_reflection)
    list(APPEND MC2_VK_SHADER_BASENAMES vegetation_cull vegetation_scan vegetation_compact vfx_vegetation)
    list(APPEND MC2_VK_SHADER_BASENAMES mech_skin)
    list(APPEND MC2_VK_SHADER_BASENAMES shadow_bake_pcss shadow_bake_terrain_march shadow_bake_resolve shadow_bake_bc4)
    # Output stages that bake from another basename's source: the skirt pipeline needs its
    # own .frag.spv name but runs the main-patch fragment program, so one source feeds both.
    # check-vk-pipeline-layout.py carries the same alias.
    set(MC2_VK_SHADER_SRC_ALIAS_terrain_tier1_skirt_frag terrain_tier1)
    # Mech-lab panel pass: mech_tier1.frag baked with -DMECH_PREVIEW=1 (no terrain set 1,
    # in-shader exposure/tonemap for the RGBA8 preview target). Loaded by vulkan_mech_island.cpp.
    list(APPEND MC2_VK_SHADER_BASENAMES mech_preview mech_backdrop)
    set(MC2_VK_SHADER_SRC_ALIAS_mech_preview_frag mech_tier1)
    set(MC2_VK_SHADER_DEFINES_mech_preview_frag -DMECH_PREVIEW=1)
    # VK-PARITY-TERRAIN-2: shaders #include files from shaders/vulkan/include and the
    # shared GL/VK shaders/include/*.hglsl (e.g. terrain_light_pool.hglsl); list them as
    # DEPENDS so editing an include re-bakes the SPIR-V (the bake step has no depfile).
    file(GLOB MC2_VK_SHADER_INCLUDES CONFIGURE_DEPENDS
        "${MC2_VK_SHADER_SRC_DIR}/include/*.glsl"
        "${CMAKE_SOURCE_DIR}/3rdparty/smaa/SMAA.hlsl"
        "${CMAKE_SOURCE_DIR}/3rdparty/fastnoiselite/FastNoiseLite.glsl"
        "${CMAKE_SOURCE_DIR}/shaders/include/*.hglsl")
    if(MC2_GLSLANG_VALIDATOR OR MC2_GLSLC)
        foreach(_base ${MC2_VK_SHADER_BASENAMES})
        foreach(_stage vert frag comp)
            set(_src_base "${_base}")
            if(DEFINED MC2_VK_SHADER_SRC_ALIAS_${_base}_${_stage})
                set(_src_base "${MC2_VK_SHADER_SRC_ALIAS_${_base}_${_stage}}")
            endif()
            set(_src "${MC2_VK_SHADER_SRC_DIR}/${_src_base}.${_stage}")
            set(_spv "${MC2_VK_SPV_OUT_DIR}/${_base}.${_stage}.spv")
            # A basename may name one stage only (shadow_prop_alpha is a fragment shader over the shared shadow_prop vertex shader).
            if(NOT EXISTS "${_src}")
                continue()
            endif()
            set(_defs "")
            if(DEFINED MC2_VK_SHADER_DEFINES_${_base}_${_stage})
                set(_defs ${MC2_VK_SHADER_DEFINES_${_base}_${_stage}})
            endif()
            if(MC2_GLSLANG_VALIDATOR)
                add_custom_command(
                    OUTPUT "${_spv}"
                    COMMAND "${MC2_GLSLANG_VALIDATOR}" -V ${_defs} "${_src}" -o "${_spv}"
                    DEPENDS "${_src}" ${MC2_VK_SHADER_INCLUDES}
                    COMMENT "SPIR-V (glslang): ${_base}.${_stage} -> ${_spv}"
                    VERBATIM)
            else()
                add_custom_command(
                    OUTPUT "${_spv}"
                    COMMAND "${MC2_GLSLC}" ${_defs} "${_src}" -o "${_spv}"
                    DEPENDS "${_src}" ${MC2_VK_SHADER_INCLUDES}
                    COMMENT "SPIR-V (glslc): ${_base}.${_stage} -> ${_spv}"
                    VERBATIM)
            endif()
            list(APPEND MC2_VK_SPV_OUTPUTS "${_spv}")
        endforeach()
        endforeach()
        message(STATUS "VULKAN-SHADER-TOOLCHAIN-1: SPIR-V compile ON -> ${MC2_VK_SPV_OUT_DIR}")
    else()
        message(WARNING "VULKAN-SHADER-TOOLCHAIN-1: neither glslangValidator nor glslc found (Vulkan SDK tools); skipping SPIR-V compile. Shader probe will fail-soft.")
    endif()
    find_program(MC2_DXC NAMES dxc HINTS "$ENV{VULKAN_SDK}/Bin" "${Vulkan_INCLUDE_DIR}/../Bin")
    if(NOT MC2_DXC)
        message(FATAL_ERROR "Spatial CMAA2 requires Vulkan SDK dxc (HLSL to SPIR-V).")
    endif()
    foreach(_entry EdgesColor2x2CS ComputeDispatchArgsCS ProcessCandidatesCS DeferredColorApply2x2CS)
        set(_spv "${MC2_VK_SPV_OUT_DIR}/aa_cmaa2_${_entry}.comp.spv")
        add_custom_command(OUTPUT "${_spv}"
            COMMAND "${MC2_DXC}" -spirv -fspv-target-env=vulkan1.1 -T cs_6_0 -E "${_entry}"
                -fvk-t-shift 8 0 -DCMAA2_UAV_STORE_TYPED=1 -DCMAA2_UAV_STORE_CONVERT_TO_SRGB=1
                -DCMAA2_UAV_STORE_TYPED_UNORM_FLOAT=1 -DCMAA2_STATIC_QUALITY_PRESET=1 -DCMAA_MSAA_SAMPLE_COUNT=1
                "${CMAKE_SOURCE_DIR}/3rdparty/cmaa2/CMAA2.hlsl" -Fo "${_spv}"
            DEPENDS "${CMAKE_SOURCE_DIR}/3rdparty/cmaa2/CMAA2.hlsl" VERBATIM)
        list(APPEND MC2_VK_SPV_OUTPUTS "${_spv}")
    endforeach()
    file(GLOB MC2_VK_SLANG_STAGES CONFIGURE_DEPENDS
        "${MC2_VK_SHADER_SRC_DIR}/*.vert.slang"
        "${MC2_VK_SHADER_SRC_DIR}/*.frag.slang"
        "${MC2_VK_SHADER_SRC_DIR}/*.comp.slang")
    if(MC2_VK_SLANG_STAGES)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        find_program(MC2_SLANGC NAMES slangc HINTS ENV VULKAN_SDK PATH_SUFFIXES Bin bin REQUIRED)
        find_program(MC2_SPIRV_CROSS NAMES spirv-cross HINTS ENV VULKAN_SDK PATH_SUFFIXES Bin bin REQUIRED)
        # Conservative dependencies keep imports and ABI edits from leaving stale binaries.
        file(GLOB_RECURSE MC2_SLANG_DEPENDS CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/shaders/*.slang"
            "${CMAKE_SOURCE_DIR}/shaders/*.h"
            "${CMAKE_SOURCE_DIR}/shaders/*.glsl"
            "${CMAKE_SOURCE_DIR}/shaders/*.hglsl"
            "${CMAKE_SOURCE_DIR}/RenderCore/gpu_abi/*.h")
        foreach(_src ${MC2_VK_SLANG_STAGES})
            get_filename_component(_name "${_src}" NAME_WLE)
            set(_spv "${MC2_VK_SPV_OUT_DIR}/${_name}.spv")
            if(_spv IN_LIST MC2_VK_SPV_OUTPUTS)
                message(FATAL_ERROR "Duplicate GLSL/Slang shader output: ${_spv}")
            endif()
            add_custom_command(OUTPUT "${_spv}"
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/tools/shader_offline_build/slang_compile.py"
                    "${_src}" -o "${_spv}"
                    --slangc "${MC2_SLANGC}" --spirv-cross "${MC2_SPIRV_CROSS}"
                DEPENDS ${MC2_SLANG_DEPENDS}
                    "${CMAKE_SOURCE_DIR}/tools/shader_offline_build/slang_compile.py"
                    "${CMAKE_SOURCE_DIR}/tools/gpu_abi_gen.py"
                    "${CMAKE_SOURCE_DIR}/tools/slang/SLANG_VERSION"
                    "${CMAKE_SOURCE_DIR}/docs/render-backend-seams/binding-slot-occupancy.json"
                COMMENT "SPIR-V (Slang): ${_name}"
                VERBATIM)
            list(APPEND MC2_VK_SPV_OUTPUTS "${_spv}")
        endforeach()
    endif()
    if(MC2_VK_SPV_OUTPUTS)
        add_custom_target(mc2_vulkan_shaders ALL DEPENDS ${MC2_VK_SPV_OUTPUTS})
    endif()
endif()

# gameos_editor: same sources as gameos minus gos_render.cpp (the GL
# renderer main). The MC2 Mission Editor (EditRel) links this instead of
# gameos to avoid pulling in a second renderer init. From engine-standalone PR #36.
set(GAMEOS_EDITOR_SOURCES ${SOURCES})
list(REMOVE_ITEM GAMEOS_EDITOR_SOURCES gos_render.cpp)
add_library(gameos_editor ${GAMEOS_EDITOR_SOURCES})
target_link_libraries(gameos_editor PRIVATE rendercore mc2_meshopt)
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

# Typed gate registry (Gates/): any source in this library may call gate::get<>(); a library that does
# not link it breaks every standalone tool that links this one (makefst, pak, aseconv, ...).
if(TARGET gameos)
    target_link_libraries(gameos PUBLIC mc2_gate)
endif()
if(TARGET gameos_editor)
    target_link_libraries(gameos_editor PUBLIC mc2_gate)
endif()
if(TARGET gameos_main)
    target_link_libraries(gameos_main PUBLIC mc2_gate)
endif()

# A single generated object supplies identity to game, editor, and tool consumers.
target_link_libraries(gameos PUBLIC mc2_build_identity)
target_link_libraries(gameos_editor PUBLIC mc2_build_identity)
target_link_libraries(gameos_main PUBLIC mc2_build_identity)
