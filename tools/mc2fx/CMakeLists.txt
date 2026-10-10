# tools/mc2fx — standalone gosFX effect-blob (.fx) inspector / round-trip tool.
#
# Game-free: curates the gosFX + MLR + particles + Stuff TU closure and links a
# slim stubs TU (mc2fx_stubs.cpp) for the gos_*/statistics symbols. NO tgl/mesh,
# NO GL/SDL, NO gameos. Mirrors tools/asset_viewer's isolation rationale.
#
# Slice 1: `dump` subcommand only. Build with -DENABLE_MC2FX=ON (guarded in the
# top CMakeLists next to asset_viewer).

# gosFX + MLR + particles + Stuff source closures (each subdir globs the same way).
# We glob then prune the TUs the authoritative engine CMakeLists deliberately
# OMIT — they are dead/legacy and #include headers that no longer exist
# (mlrstuff.hpp, munga.hpp), so globbing them in breaks the compile.
file(GLOB MC2FX_GOSFX_SRC     "${CMAKE_SOURCE_DIR}/mclib/gosfx/*.cpp")
file(GLOB MC2FX_MLR_SRC       "${CMAKE_SOURCE_DIR}/mclib/mlr/*.cpp")
file(GLOB MC2FX_PARTICLES_SRC "${CMAKE_SOURCE_DIR}/mclib/particles/*.cpp")
file(GLOB MC2FX_STUFF_SRC     "${CMAKE_SOURCE_DIR}/mclib/stuff/*.cpp")

# Stuff *_test.cpp are test mains / include missing munga.hpp — drop them all.
file(GLOB MC2FX_STUFF_TEST_SRC "${CMAKE_SOURCE_DIR}/mclib/stuff/*_test.cpp")
if(MC2FX_STUFF_TEST_SRC)
    list(REMOVE_ITEM MC2FX_STUFF_SRC ${MC2FX_STUFF_TEST_SRC})
endif()
# extentpoly.cpp is a dead Stuff TU (not in mclib/stuff/CMakeLists.txt).
list(REMOVE_ITEM MC2FX_STUFF_SRC
    "${CMAKE_SOURCE_DIR}/mclib/stuff/extentpoly.cpp"
)

add_executable(mc2fx
    main.cpp
    mc2fx_core.cpp
    mc2fx_stubs.cpp
    ${MC2FX_GOSFX_SRC}
    ${MC2FX_MLR_SRC}
    ${MC2FX_PARTICLES_SRC}
    ${MC2FX_STUFF_SRC}
    # gosFX effect/clouds/tube time themselves with fx_cost_split (MC2_FX_COST_SPLIT, default off).
    "${CMAKE_SOURCE_DIR}/mclib/fx_trace/fx_cost_split.cpp"
    # Tracy client (TRACY_ENABLE is global; any TU including gos_profiler.h
    # inline-instantiates the Tracy queue). Same gotcha asset_viewer hit.
    "${CMAKE_SOURCE_DIR}/3rdparty/tracy/TracyClient.cpp"
)

target_compile_features(mc2fx PRIVATE cxx_std_17)

# ---------------------------------------------------------------------------
# Optional AddressSanitizer variant (diagnostic). Gate behind -DMC2FX_ASAN=ON
# so the normal build is unaffected. MSVC cl supports /fsanitize=address
# (compile + link). Disable edit-and-continue / RTC which conflict with ASan.
# ---------------------------------------------------------------------------
option(MC2FX_ASAN "Build mc2fx with AddressSanitizer (/fsanitize=address)" OFF)
if(MC2FX_ASAN)
    if(MSVC)
        target_compile_options(mc2fx PRIVATE /fsanitize=address /Zi)
        # ASan is incompatible with RTC and incremental/EnC linking.
        target_compile_options(mc2fx PRIVATE /wd5072)
        target_link_options(mc2fx PRIVATE /INCREMENTAL:NO)
    else()
        target_compile_options(mc2fx PRIVATE -fsanitize=address -g)
        target_link_options(mc2fx PRIVATE -fsanitize=address)
    endif()
endif()

target_compile_definitions(mc2fx PRIVATE
    PLATFORM_WINDOWS
    LINUX_BUILD
    _CRT_SECURE_NO_WARNINGS
)

target_include_directories(mc2fx PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/mclib"
    "${CMAKE_SOURCE_DIR}/mclib/stuff"
    "${CMAKE_SOURCE_DIR}/mclib/gosfx"      # gosfxheaders.hpp (main.cpp)
    "${CMAKE_SOURCE_DIR}/mclib/mlr"        # mlr headers referenced unqualified
    "${CMAKE_SOURCE_DIR}/mclib/particles"
    "${CMAKE_SOURCE_DIR}/GameOS/include"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos"
    "${CMAKE_SOURCE_DIR}/3rdparty/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/tracy"
    "${CMAKE_SOURCE_DIR}"
)

# zlib — Stuff filestream / PacketFile decompression closure may pull it in.
find_library(MC2FX_ZLIB
    NAMES zlib zlibstatic
    PATHS
        "${CMAKE_SOURCE_DIR}/3rdparty/3rdparty/lib/x64"
        "${CMAKE_SOURCE_DIR}/3rdparty/lib/x64"
        "A:/Games/mc2-opengl-src/3rdparty/3rdparty/lib/x64"
    NO_DEFAULT_PATH
)
if(MC2FX_ZLIB)
    target_link_libraries(mc2fx PRIVATE ${MC2FX_ZLIB})
endif()

if(WIN32)
    target_link_libraries(mc2fx PRIVATE Psapi)  # Tracy
endif()

# The gosFX / mlr / particles TUs read their gates through the typed registry.
target_link_libraries(mc2fx PRIVATE mc2_gate)
