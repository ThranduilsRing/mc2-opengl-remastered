# RenderWorld/CMakeLists.txt
#
# Slice M1: static library. Engine API surface. Links against
# rendercore INTERFACE for types. Internal legacy backend TU
# (static_prop_backend.cpp) is the ONLY engine TU that includes
# gos_static_prop_batcher.h.

add_library(renderworld STATIC
    RenderWorld.cpp
    legacy/static_prop_backend.cpp
)

target_link_libraries(renderworld PUBLIC rendercore)
target_include_directories(renderworld PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Reach the registry + batcher headers from the legacy backend TU.
# The firewall script allowlists RenderWorld/legacy/ as the only place
# this reach-through is permitted. GLEW + mclib/stuff are transitive
# requirements of gos_static_prop_batcher.h (GL/glew.h + msl.h/tgl.h/Stuff).
target_include_directories(renderworld PRIVATE
    ${CMAKE_SOURCE_DIR}/GameOS/gameos
    ${CMAKE_SOURCE_DIR}/GameOS/include
    ${CMAKE_SOURCE_DIR}/mclib
    ${CMAKE_SOURCE_DIR}/mclib/stuff
    ${THIRDPARTY_INCLUDE_DIRS}
)

target_compile_features(renderworld PUBLIC cxx_std_17)

# Typed gate registry (Gates/): any source in this library may call gate::get<>(); a library that does
# not link it breaks every standalone tool that links this one (makefst, pak, aseconv, ...).
if(TARGET renderworld)
    target_link_libraries(renderworld PUBLIC mc2_gate)
endif()
