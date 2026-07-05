cmake_minimum_required (VERSION 3.10)
project(mclib)

set(SOURCES ${SOURCES}
    alphapalette.cpp
    bitflag.cpp
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
    paths.cpp
    pqueue.cpp
    routines.cpp
    scale.cpp
    sortlist.cpp
    tgainfo.cpp
    tgl.cpp
    timing.cpp
    txmmgr.cpp
    tex_resolve_table.cpp
    userinput.cpp
    vport.cpp
    weaponfx.cpp
    csvfile.cpp
    fastfile.cpp
    fst_hash.cpp
    ffile.cpp
    file.cpp
    gamelog.cpp
    inifile.cpp
    packet.cpp
    celement.cpp
    celine.cpp
    cellip.cpp
    cepane.cpp
    cevfx.cpp
    vfx_ellipse.cpp
    vfx_map_polygon.cpp
    vfx_transform.cpp
    vfx_translatedraw.cpp
    vfxtile.cpp
    abldbug.cpp
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
    ablxstmt.cpp
    camera.cpp
    xform_conventions.cpp
    spotlight_diag.cpp
    object_admission_predicate.cpp
    projectz_trace.cpp
    projectz_overlay.cpp
    render_contract.cpp
    cpu_proj_cost_split.cpp
    crater.cpp
    dynamic_decal_ring.cpp
    mapdata.cpp
    quad.cpp
    terrain.cpp
    terrain_runtime.cpp
    terrtxm.cpp
    terrtxm2.cpp
    burnin_jpeg_decode.cpp
    control_map_png_decode.cpp
    txmconv.cpp
    appear.cpp
    apprtype.cpp
    bdactor.cpp
    cliff_decal_tuning.cpp
    genactor.cpp
    gvactor.cpp
    mech3d.cpp
    soundsys.cpp
    move.cpp
    move_recon.cpp
    echarstring.cpp
    estring.cpp
    ewcharstring.cpp
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
    ../code/frame_jobs.cpp
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

