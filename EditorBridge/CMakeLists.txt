# EditorBridge/CMakeLists.txt
#
# CARVE-OUT library: bridges game-side (mclib, GameOS) and engine-side
# (RenderCore, RenderWorld) headers. Only EditorRenderBridge.cpp is the
# carve-out TU per scripts/check-include-firewall.allowlist.

add_library(editorbridge STATIC
    EditorRenderBridge.cpp
)

# PUBLIC: rendercore + renderworld symbols appear in EditorRenderBridge.h types.
# Each executable selects gameos or gameos_editor; a static-library PRIVATE link
# still propagates gameos and pulls its competing window implementation into EditRel.
target_link_libraries(editorbridge PUBLIC rendercore renderworld PRIVATE mclib)

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
