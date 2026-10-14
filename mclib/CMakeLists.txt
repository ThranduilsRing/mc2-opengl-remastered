cmake_minimum_required (VERSION 3.10)
project(mclib)

set(SOURCES ${SOURCES}
    alphapalette.cpp
    cident.cpp
    cmponent.cpp
    color.cpp
    debugging.cpp
    err.cpp
    floathelp.cpp
    heap.cpp
    lzcomp.cpp
    lzdecomp.cpp
    mathfunc.cpp
    mouse.cpp
    msl.cpp
    msl_load.cpp
    msl_shape.cpp
    msl_transform.cpp
    msl_anim.cpp
    paths.cpp
    pqueue.cpp
    routines.cpp
    appearance_rng.cpp
    sim_trace.cpp
    alloc_probe.cpp
    scale.cpp
    sortlist.cpp
    tgainfo.cpp
    tgl.cpp
    tgl_parse.cpp
    tgl_render.cpp
    tgl_shadows.cpp
    tgl_transform.cpp
    tgl_transform_light.cpp
    tgl_types.cpp
    timing.cpp
    txmmgr.cpp
    txmmgr_diag.cpp
    txmmgr_lightrow.cpp
    txmmgr_lights.cpp
    txmmgr_render.cpp
    txmmgr_render_phases.cpp
    txmmgr_render_passes.cpp
    txmmgr_render_overlays.cpp
    txmmgr_texture.cpp
    tex_resolve_table.cpp
    userinput.cpp
    vk_umbrella_gate.cpp
    vport.cpp
    weaponfx.cpp
    csvfile.cpp
    fastfile.cpp
    fst_hash.cpp
    mc2_audit.cpp
    tgl_load_guard.cpp

    ffile.cpp
    file.cpp
    gamelog.cpp
    inifile.cpp
    inifile_read.cpp
    inifile_arrays.cpp
    packet.cpp
    celement.cpp
    celine.cpp
    cellip.cpp
    cevfx.cpp
    cevfx_polys.cpp
    vfx_transform.cpp
    vfx_translatedraw.cpp
    vfxtile.cpp
    abl_script_events.cpp
    abldbug.cpp
    abldbug_print.cpp
    abldecl.cpp
    ablenv.cpp
    ablerr.cpp
    ablexec.cpp
    ablexpr.cpp
    ablrtn.cpp
    ablscan.cpp
    ablstd.cpp
    ablstmt.cpp
    ablsymt.cpp
    ablxexpr.cpp
    ablxstd.cpp
    ablxstd_stack.cpp
    ablxstd_std.cpp
    ablxstmt.cpp
    camera.cpp
    camera_project.cpp
    camera_update.cpp
    camera_daylight.cpp
    camera_matrix.cpp
    camera_input.cpp
    xform_conventions.cpp
    spotlight_diag.cpp
    cone_glow.cpp
    vis_semantics.cpp
    object_admission_predicate.cpp
    projectz_trace.cpp
    projectz_overlay.cpp
    render_contract.cpp
    cpu_proj_cost_split.cpp
    crater.cpp
    dynamic_decal_ring.cpp
    mapdata.cpp
    quad.cpp
    quad_setup.cpp
    quad_draw.cpp
    quad_water.cpp
    quad_lines.cpp
    terrain.cpp
    terrain_init.cpp
    terrain_raycast.cpp
    terrain_update.cpp
    terrain_render.cpp
    terrain_geometry.cpp
    terrain_runtime.cpp
    terrtxm.cpp
    terrtxm2.cpp
    terrtxm2_bake.cpp
    terrtxm2_load.cpp
    burnin_jpeg_decode.cpp
    control_map_png_decode.cpp
    appear.cpp
    apprtype.cpp
    bdactor.cpp
    bdactor_diag.cpp
    bdactor_lightbake.cpp
    bdactor_render.cpp
    bdactor_sidecar.cpp
    bdactor_static.cpp
    bdactor_terrain.cpp
    bdactor_tree.cpp
    bdactor_tree_static.cpp
    bdactor_type.cpp
    bdactor_update.cpp
    cliff_decal_tuning.cpp
    genactor.cpp
    genactor_frame.cpp
    gvactor.cpp
    gvactor_geometry.cpp
    gvactor_render.cpp
    gvactor_update.cpp
    mech3d.cpp
    mech3d_diag.cpp
    mech3d_geometry.cpp
    mech3d_init.cpp
    mech3d_render.cpp
    mech3d_type.cpp
    mech3d_update.cpp
    soundsys.cpp
    move.cpp
    move_globalmap_build.cpp
    move_globalmap_doors.cpp
    move_globalmap_path.cpp
    move_movemap_escape.cpp
    move_movemap_goals.cpp
    move_movemap_path.cpp
    move_recon.cpp
    estring.cpp
    tacmap.cpp
    utilities.cpp
    model_override_registry.cpp
    anim_override_registry.cpp
    fx_def_registry.cpp
    # LINK-CONFIG-FIX: msl.cpp (this lib) references isFrameJobsWorkerThread()
    # -> g_isFrameJobsWorker, whose only definition lived in code/frame_jobs.cpp
    # (compiled only into mc2.exe). Carrying it in mclib resolves every target
    # that links mclib but builds its own main (EditRel, data_tools). The TU is
    # self-contained (stdlib only). Removed from mc2's own SOURCES to avoid a
    # duplicate definition.
    #
    # MC2-JOBS-CORE-1: code/frame_jobs.cpp was evolved into code/job_system.cpp
    # (keeps the legacy free-function API AND adds the dormant mc2::jobs API) +
    # the always-compiled fatal-check TU code/jobs_fatal.cpp. Both are
    # self-contained (stdlib only). frame_jobs.h remains as a back-compat shim
    # header; the .cpp is gone.
    ../code/job_system.cpp
    ../code/jobs_fatal.cpp
    )

# Track D — Assimp-backed FBX/GLB mech importer.
# Source file always exists; its body is gated on ENABLE_ASSIMP_IMPORTER so
# it compiles to nothing when off.
if(ENABLE_ASSIMP_IMPORTER)
    list(APPEND SOURCES assimp_importer.cpp)
    # Shared skeleton/FK math (BT2018-SKEL-ENGINE-1). Same TU the standalone
    # mech_import_harness links — one math path for tool and game.
    list(APPEND SOURCES mech_skel_import.cpp)
endif()

include_directories(${COMMON_INCLUDE_DIRS} "./mclib" "../Editor" "../DirectX8/include" "./" "../code")
include_directories(${THIRDPARTY_INCLUDE_DIRS})
include_directories(${CMAKE_SOURCE_DIR}/3rdparty/include)
# RenderCore/ headers (e.g. RenderPassContract.h) use repo-root-relative includes.
include_directories(${CMAKE_SOURCE_DIR})

# BGR was a DX-era holdover: setPaintScheme ran bgrTorgb on every color
# DWORD, assuming callers passed BGR-encoded values. In this port all
# callers pass RGB-encoded colors (mission file, prefs, MPlayer->colors,
# moverSpec), so the swap actively miscolored mechs (yellow prefs turned
# into cyan paint) and flipped psRed byte-order on every cache-hit
# resetPaintScheme call, causing the Razorback/Bushwacker LOD color bug.
# Removing the define is the correct minimal fix; it affects only
# mech3d.cpp and mclib/gvactor.cpp, both of which have #if defined(BGR)
# blocks around the same bgrTorgb swaps.
#add_definitions(-DBGR)
add_library(mclib ${SOURCES})
mc2_enable_pch(mclib)

# Track D — link Assimp + define ENABLE_ASSIMP_IMPORTER for mclib's TUs.
# Without this block, the #ifdef gates in msl.cpp / assimp_importer.cpp /
# mech3d.cpp collapse to empty and the whole import path is silently dead.
if(ENABLE_ASSIMP_IMPORTER)
    target_link_libraries(mclib PRIVATE assimp::assimp)
    target_compile_definitions(mclib PRIVATE ENABLE_ASSIMP_IMPORTER)
    # Assimp's defs.h uses C++17 inline variables. mclib's legacy code is C++14,
    # but compilers accept newer standards as a superset, so this just unlocks
    # the language features Assimp needs without breaking the rest of mclib.
    target_compile_features(mclib PRIVATE cxx_std_17)
endif()

# cat MCLib.vcproj | grep ".cpp" | perl -pe 's/.+\"(\w+\.cpp)\".+/\1/g' >> CMakeLists.txt


# Typed gate registry (Gates/): any source in this library may call gate::get<>(); a library that does
# not link it breaks every standalone tool that links this one (makefst, pak, aseconv, ...).
if(TARGET mclib)
    target_link_libraries(mclib PUBLIC mc2_gate)
endif()
