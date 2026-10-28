# tools/tg_import_dump/CMakeLists.txt
# MECH-IMPORT-TGDUMP-1 — game-free inspector over the REAL engine import path.
#
# Calls ImportGeometryFromFile (the actual mclib importer) on a GLB/FBX and dumps
# the resulting TG_TypeMultiShape as JSON. Catches the engine import->TG bugs the
# Assimp-only mech_import_harness can't see (empty shapes, slot-0 = blip texture,
# merge result, bbox/scale, orientation). NO GL, NO game, NO deploy.
#
# Links the real importer (assimp_importer.cpp) + shared FK (mech_skel_import.cpp)
# + tgl/msl/stuff TUs + the SAME stubs the tgl_loader_standalone_spike proved
# (CRT-backed heaps/files; gos_*/eye/land/mc2::textures::ready() are render-only and
# never hit on the import path). The linker is the proof it stays game-free.
#
# STANDALONE BUILD (own tree; never touches build64/):
#   cmake -S tools/tg_import_dump -B build64-tgdump -G "Visual Studio 17 2022" -A x64
#   cmake --build build64-tgdump --config RelWithDebInfo --target tg_import_dump
#   build64-tgdump/RelWithDebInfo/tg_import_dump.exe <model.glb> [--ground]

cmake_minimum_required(VERSION 3.16)
project(tg_import_dump CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Mc2Standalone.cmake")

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Vendored Assimp (FBX+GLTF, bundled zlib). zlibstatic (from assimp) doubles as
# the zlib the FastFile/lzdecomp TUs need — one zlib, no external zlib.lib, no
# duplicate-symbol conflict.
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
set(ZLIB_FOUND FALSE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
add_subdirectory(${MC2_ROOT}/3rdparty/assimp ${CMAKE_BINARY_DIR}/out/3rdparty/assimp EXCLUDE_FROM_ALL)

add_executable(tg_import_dump
    main.cpp
    stubs.cpp
    tgdump_seams.cpp
    ${MC2_ROOT}/mclib/mc2_audit.cpp                             # leaf audit emitter msl.cpp calls
    # --- real importer + shared FK ---
    ${MC2_ROOT}/mclib/assimp_importer.cpp
    ${MC2_ROOT}/mclib/mech_skel_import.cpp
    # --- TG types + geometry loader closure (same as tgl spike) ---
    ${MC2_ROOT}/mclib/msl.cpp
    ${MC2_ROOT}/mclib/msl_load.cpp
    ${MC2_ROOT}/mclib/content_ir.cpp
    ${MC2_ROOT}/GameOS/gameos/cook_cache.cpp
    ${MC2_ROOT}/mclib/msl_shape.cpp
    ${MC2_ROOT}/mclib/msl_transform.cpp
    ${MC2_ROOT}/mclib/msl_anim.cpp
    ${MC2_ROOT}/mclib/tgl.cpp
    ${MC2_ROOT}/mclib/tgl_parse.cpp
    ${MC2_ROOT}/mclib/tgl_render.cpp
    ${MC2_ROOT}/mclib/tgl_shadows.cpp
    ${MC2_ROOT}/mclib/tgl_transform.cpp
    ${MC2_ROOT}/mclib/tgl_transform_light.cpp
    ${MC2_ROOT}/mclib/tgl_types.cpp
    ${MC2_ROOT}/mclib/cident.cpp
    ${MC2_ROOT}/mclib/mathfunc.cpp
    ${MC2_ROOT}/GameOS/src/platform_str.cpp
    ${MC2_ROOT}/mclib/file.cpp
    ${MC2_ROOT}/mclib/fastfile.cpp
    ${MC2_ROOT}/mclib/ffile.cpp
    ${MC2_ROOT}/mclib/packet.cpp
    ${MC2_ROOT}/GameOS/gameos/utils/mc2log.cpp   # file.cpp / packet.cpp log malformed content through the facade
    ${MC2_ROOT}/mclib/lzdecomp.cpp
    ${MC2_ROOT}/mclib/fst_hash.cpp
    ${MC2_ROOT}/mclib/heap.cpp
    ${MC2_ROOT}/mclib/paths.cpp
    ${MC2_ROOT}/GameOS/gameos/sim_overlap.cpp
    ${MC2_ROOT}/mclib/timing.cpp
    ${MC2_ROOT}/mclib/cpu_proj_cost_split.cpp
    ${MC2_ROOT}/mclib/projectz_trace.cpp
    ${MC2_ROOT}/mclib/object_admission_predicate.cpp
    # --- Stuff library (game-free math + containers) ---
    ${MC2_ROOT}/mclib/stuff/color.cpp
    ${MC2_ROOT}/mclib/stuff/random.cpp
    ${MC2_ROOT}/mclib/stuff/scalar.cpp
    ${MC2_ROOT}/mclib/stuff/vector4d.cpp
    ${MC2_ROOT}/mclib/stuff/mstring.cpp
    ${MC2_ROOT}/mclib/stuff/chain.cpp
    ${MC2_ROOT}/mclib/stuff/hash.cpp
    ${MC2_ROOT}/mclib/stuff/iterator.cpp
    ${MC2_ROOT}/mclib/stuff/link.cpp
    ${MC2_ROOT}/mclib/stuff/node.cpp
    ${MC2_ROOT}/mclib/stuff/plug.cpp
    ${MC2_ROOT}/mclib/stuff/registeredclass.cpp
    ${MC2_ROOT}/mclib/stuff/safesocket.cpp
    ${MC2_ROOT}/mclib/stuff/socket.cpp
    ${MC2_ROOT}/mclib/stuff/sortedchain.cpp
    ${MC2_ROOT}/mclib/stuff/sortedsocket.cpp
    ${MC2_ROOT}/mclib/stuff/tree.cpp
    ${MC2_ROOT}/mclib/stuff/filestream.cpp
    ${MC2_ROOT}/mclib/stuff/filestreammanager.cpp
    ${MC2_ROOT}/mclib/stuff/memorystream.cpp
    ${MC2_ROOT}/mclib/stuff/notationfile.cpp
    ${MC2_ROOT}/mclib/stuff/note.cpp
    ${MC2_ROOT}/mclib/stuff/page.cpp
    ${MC2_ROOT}/mclib/stuff/style.cpp
    ${MC2_ROOT}/mclib/stuff/trace.cpp
    ${MC2_ROOT}/mclib/stuff/stuff.cpp
)
mc2_link_gate(tg_import_dump)

target_compile_definitions(tg_import_dump PRIVATE
    PLATFORM_WINDOWS
    LINUX_BUILD
    _CRT_SECURE_NO_WARNINGS
    ENABLE_ASSIMP_IMPORTER   # compile the real importer body
)

target_include_directories(tg_import_dump PRIVATE
    ${MC2_ROOT}/mclib
    ${MC2_ROOT}/mclib/stuff
    ${MC2_ROOT}/GameOS/include
    ${MC2_ROOT}/GameOS/gameos
    ${MC2_ROOT}/3rdparty/include
    ${MC2_ROOT}/3rdparty/tracy
    ${MC2_ROOT}/3rdparty/assimp/include
    ${MC2_ROOT}
)

# assimp::assimp = importer; zlibstatic = the one zlib (FastFile + assimp share it).
target_link_libraries(tg_import_dump PRIVATE assimp::assimp zlibstatic)
target_compile_features(tg_import_dump PRIVATE cxx_std_20)
