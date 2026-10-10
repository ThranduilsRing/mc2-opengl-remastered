# GameAdapters/CMakeLists.txt
#
# Slice M1: STATIC library. The ONLY module that bridges game-side
# (mclib/appear.h) and engine-side (RenderWorld) per spec section 12.

add_library(gameadapters STATIC
    StaticPropRenderAdapter.cpp
    MechRenderAdapter.cpp
    SkyRenderAdapter.cpp
    VegetationAdapter.cpp
)

target_link_libraries(gameadapters PUBLIC rendercore renderworld)

# Adapter .cpp reaches game-side headers + GOS internal headers. This
# is the ONLY library where the firewall script allows such reach-through.
target_include_directories(gameadapters PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/mclib
    ${CMAKE_SOURCE_DIR}/mclib/stuff
    ${CMAKE_SOURCE_DIR}/GameOS
    ${CMAKE_SOURCE_DIR}/GameOS/include
    ${CMAKE_SOURCE_DIR}/GameOS/gameos
)

# Inherit thirdparty include dirs (GLEW, etc) — same as renderworld
# needs because gos_static_prop_batcher.h transitively pulls GL/glew.h.
target_include_directories(gameadapters PRIVATE
    ${THIRDPARTY_INCLUDE_DIRS}
)

target_include_directories(gameadapters PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(gameadapters PUBLIC cxx_std_17)

# Typed gate registry (Gates/): any source in this library may call gate::get<>(); a library that does
# not link it breaks every standalone tool that links this one (makefst, pak, aseconv, ...).
if(TARGET gameadapters)
    target_link_libraries(gameadapters PUBLIC mc2_gate)
endif()
