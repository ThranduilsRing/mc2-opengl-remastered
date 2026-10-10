# EditorBridge/CMakeLists.txt
#
# CARVE-OUT library: bridges game-side (mclib, GameOS) and engine-side
# (RenderCore, RenderWorld) headers. Only EditorRenderBridge.cpp is the
# carve-out TU per scripts/check-include-firewall.allowlist.

add_library(editorbridge STATIC
    EditorRenderBridge.cpp
)

# PUBLIC: rendercore + renderworld symbols appear in EditorRenderBridge.h types.
# PRIVATE: mclib + gameos are carve-out implementation details only; EditorRenderBridge.h
#   exposes none of their types, so consumers must not inherit these dependencies.
target_link_libraries(editorbridge PUBLIC rendercore renderworld PRIVATE mclib gameos)

# Carve-out .cpp reaches game-side and GameOS headers.
target_include_directories(editorbridge PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/mclib
    ${CMAKE_SOURCE_DIR}/mclib/stuff
    ${CMAKE_SOURCE_DIR}/GameOS
    ${CMAKE_SOURCE_DIR}/GameOS/include
    ${CMAKE_SOURCE_DIR}/GameOS/gameos
    ${THIRDPARTY_INCLUDE_DIRS}
)

# Public include: EditorBridge/ itself (for EditorRenderBridge.h).
target_include_directories(editorbridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(editorbridge PUBLIC cxx_std_17)

# Typed gate registry (Gates/): any source in this library may call gate::get<>(); a library that does
# not link it breaks every standalone tool that links this one (makefst, pak, aseconv, ...).
if(TARGET editorbridge)
    target_link_libraries(editorbridge PUBLIC mc2_gate)
endif()
