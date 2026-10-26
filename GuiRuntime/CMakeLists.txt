add_library(gui_runtime STATIC
    GuiRuntime.cpp
    GuiRuntime_draw.cpp
    PlayerFrontEnd.cpp
    EditorInspector.cpp
    EditorInspector_sections.cpp
    GraphicsOptionsWindow.cpp
    GraphicsOptionsWindow_mech.cpp
    GraphicsOptionsWindow_passes.cpp
    GraphicsOptionsWindow_terrain.cpp
)

target_include_directories(gui_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(gui_runtime PRIVATE
    ${THIRDPARTY_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/GameOS/include
    ${CMAKE_SOURCE_DIR}/GameOS/gameos
    ${CMAKE_SOURCE_DIR}/RenderCore
)

target_link_libraries(gui_runtime PUBLIC imgui renderworld)

# Typed gate registry (Gates/): any source in this library may call gate::get<>(); a library that does
# not link it breaks every standalone tool that links this one (makefst, pak, aseconv, ...).
if(TARGET gui_runtime)
    target_link_libraries(gui_runtime PUBLIC mc2_gate)
endif()
