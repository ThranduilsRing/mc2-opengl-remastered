# LINK-CONFIG-FIX: a few game-layer globals (eye, MaxMinUV, ObjectManager,
# g_tobjAngularCyc, RunGameplayPickSelfTest) are referenced by the renderer/mclib
# object files but defined only in code/ TUs the tools don't compile. Provide
# trivial definitions via a shared stub TU rather than linking the whole game.
set(DATA_TOOLS_STUBS
    "tools_game_stubs.cpp"
    # Real (light, dependency-free) game TUs linked instead of stubbed:
    "${CMAKE_SOURCE_DIR}/code/action_executor_registry.cpp"   # theActionExecutorRegistry
    "${CMAKE_SOURCE_DIR}/code/unitprofile.cpp"                # mc2UnitProfileDataEnabled
)

set(MAKEFST_SOURCES "makefst.cpp" ${DATA_TOOLS_STUBS})
set(PAK_SOURCES "pak.cpp" "common.hpp" ${DATA_TOOLS_STUBS})
set(ASECONV_SOURCES "aseconv.cpp" "common.hpp" ${DATA_TOOLS_STUBS})
set(MAKERSP_SOURCES "makersp.cpp" ${DATA_TOOLS_STUBS})

add_compile_definitions(DISABLE_GAMEOS_MAIN)
include_directories("${CMAKE_SOURCE_DIR}/code")

# Archive tools need file/compression services, not renderer registration objects.
set(DATA_TOOLS_FILE_LIBS mclib gameos windows ZLIB::ZLIB
    ${SDL2_LIBRARIES} SDL2_mixer::SDL2_mixer ${ADDITIONAL_LIBS})

add_executable(makefst ${MAKEFST_SOURCES})
target_link_libraries(makefst ${DATA_TOOLS_FILE_LIBS})

add_executable(pak ${PAK_SOURCES})
target_link_libraries(pak ${DATA_TOOLS_FILE_LIBS})

add_executable(aseconv ${ASECONV_SOURCES})
# Geometry conversion also needs the shape/MLR services.
target_compile_definitions(aseconv PRIVATE DATA_TOOLS_STUBS_HAVE_EYE_AND_MAXMINUV)
target_link_libraries(aseconv ${DATA_TOOLS_FILE_LIBS}
    gameadapters editorbridge gosfx mlr particles fx_trace imgui gui_runtime stuff)

add_executable(makersp ${MAKERSP_SOURCES})
target_link_libraries(makersp gameos windows ${SDL2_LIBRARIES} ${ADDITIONAL_LIBS})
