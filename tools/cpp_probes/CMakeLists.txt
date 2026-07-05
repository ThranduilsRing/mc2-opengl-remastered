# Offline asset-pipeline probe host tools (ASSIMP-IMPORTER-PHASE-0 +).
#
# These are OFFLINE tooling: they link ONLY assimp::assimp (and, in a later
# slice, meshoptimizer). They MUST NOT link mclib, gameos, SDL2, GLEW, or any
# runtime/renderer target. Gated by ENABLE_ASSET_CPP_PROBES in the root
# CMakeLists.txt; this subdir is only added when that option is ON.

# assimp_probe — import a model via Assimp and emit a geometry-summary JSON
# matching the manifest `geometry` section (tools/asset_cook/validate_asset_manifest.py).
add_executable(assimp_probe assimp_probe.cpp)
target_link_libraries(assimp_probe PRIVATE assimp::assimp)
target_compile_features(assimp_probe PRIVATE cxx_std_17)

# meshopt_lod_probe (MESHOPT-LOD-PROBE-1) — procedural mesh, run meshopt_simplify
# + meshopt_buildMeshlets, emit manifest-compatible LOD-stat JSON. Links ONLY the
# vendored meshoptimizer static lib (added gated in the root CMakeLists).
add_executable(meshopt_lod_probe meshopt_lod_probe.cpp)
target_link_libraries(meshopt_lod_probe PRIVATE meshoptimizer)
target_include_directories(meshopt_lod_probe PRIVATE "${CMAKE_SOURCE_DIR}/3rdparty/meshoptimizer/src")
target_compile_features(meshopt_lod_probe PRIVATE cxx_std_17)

# meshopt_cook (MESHOPT-COOK-1) — OFFLINE LOD-simplification cook. Parses a .glb
# with the vendored (parse-only) cgltf.h, runs meshopt_simplify per primitive at
# one or more ratios (LOD1/LOD2), preserves ALL attributes incl. JOINTS/WEIGHTS,
# and hand-writes new LOD .glb outputs. Links ONLY meshoptimizer + cgltf header.
# LOD simplify only — no vertex-cache/fetch reorder (runtime re-welds via Assimp).
add_executable(meshopt_cook meshopt_cook.cpp)
target_link_libraries(meshopt_cook PRIVATE meshoptimizer)
target_include_directories(meshopt_cook PRIVATE
    "${CMAKE_SOURCE_DIR}/3rdparty/meshoptimizer/src"
    "${CMAKE_SOURCE_DIR}/3rdparty/meshoptimizer/extern")
target_compile_features(meshopt_cook PRIVATE cxx_std_17)
