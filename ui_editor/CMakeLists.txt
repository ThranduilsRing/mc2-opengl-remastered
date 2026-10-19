# UI Editor standalone target.
#
# Keep SDL include resolution local to this target. The ImGui SDL backend
# includes "SDL.h", so the SDL2 include folder itself must be on the
# include path, not just the parent include directory.

set(UI_EDITOR_SOURCES
    "UiEditorMain.cpp"
    "UiEditorFitLoader.cpp"
    "UiEditorImageCache.cpp"
    "UiEditorVersion.cpp"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos/utils/Image.cpp"
)

add_executable(ui_editor ${UI_EDITOR_SOURCES})

target_compile_features(ui_editor PRIVATE cxx_std_17)
target_compile_definitions(ui_editor PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLEW)

if(NOT TARGET SDL2::SDL2)
    find_package(SDL2 CONFIG REQUIRED)
endif()

if(NOT TARGET OpenGL::GL)
    find_package(OpenGL REQUIRED)
endif()

set(MC2R_UI_EDITOR_SDL_INCLUDE_DIRS)

if(DEFINED SDL2_INCLUDE_DIRS)
    list(APPEND MC2R_UI_EDITOR_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
endif()

if(DEFINED SDL2_INCLUDE_DIR)
    list(APPEND MC2R_UI_EDITOR_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
endif()

foreach(_mc2r_prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${_mc2r_prefix}/include/SDL2/SDL.h")
        list(APPEND MC2R_UI_EDITOR_SDL_INCLUDE_DIRS "${_mc2r_prefix}/include/SDL2")
    endif()

    if(EXISTS "${_mc2r_prefix}/include/SDL.h")
        list(APPEND MC2R_UI_EDITOR_SDL_INCLUDE_DIRS "${_mc2r_prefix}/include")
    endif()
endforeach()

if(DEFINED VCPKG_TARGET_TRIPLET)
    foreach(_mc2r_vcpkg_root
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}"
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}"
    )
        if(EXISTS "${_mc2r_vcpkg_root}/include/SDL2/SDL.h")
            list(APPEND MC2R_UI_EDITOR_SDL_INCLUDE_DIRS "${_mc2r_vcpkg_root}/include/SDL2")
        endif()

        if(EXISTS "${_mc2r_vcpkg_root}/include/SDL.h")
            list(APPEND MC2R_UI_EDITOR_SDL_INCLUDE_DIRS "${_mc2r_vcpkg_root}/include")
        endif()
    endforeach()
endif()

# Common vcpkg layout for this repo's build64 tree.
foreach(_mc2r_vcpkg_root
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows"
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x86-windows"
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-linux"
)
    if(EXISTS "${_mc2r_vcpkg_root}/include/SDL2/SDL.h")
        list(APPEND MC2R_UI_EDITOR_SDL_INCLUDE_DIRS "${_mc2r_vcpkg_root}/include/SDL2")
    endif()

    if(EXISTS "${_mc2r_vcpkg_root}/include/SDL.h")
        list(APPEND MC2R_UI_EDITOR_SDL_INCLUDE_DIRS "${_mc2r_vcpkg_root}/include")
    endif()
endforeach()

if(MC2R_UI_EDITOR_SDL_INCLUDE_DIRS)
    list(REMOVE_DUPLICATES MC2R_UI_EDITOR_SDL_INCLUDE_DIRS)
endif()


target_include_directories(ui_editor PRIVATE
    "${CMAKE_SOURCE_DIR}/3rdparty/imgui"
    "${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos"
    ${MC2R_UI_EDITOR_SDL_INCLUDE_DIRS}
)

target_link_libraries(ui_editor PRIVATE imgui)

if(TARGET SDL2::SDL2)
    target_link_libraries(ui_editor PRIVATE SDL2::SDL2)
elseif(DEFINED SDL2_LIBRARIES)
    target_link_libraries(ui_editor PRIVATE ${SDL2_LIBRARIES})
else()
    message(FATAL_ERROR "SDL2 target or SDL2_LIBRARIES was not found for ui_editor")
endif()

if(TARGET SDL2::SDL2main)
    target_link_libraries(ui_editor PRIVATE SDL2::SDL2main)
endif()

if(NOT TARGET mc2_tool_vk)
    add_subdirectory("${CMAKE_SOURCE_DIR}/tools/common" "${CMAKE_BINARY_DIR}/tools_common")
endif()
target_link_libraries(ui_editor PRIVATE mc2_tool_vk)
if(WIN32)
    target_link_libraries(ui_editor PRIVATE ole32 windowscodecs uuid)
endif()

set(MC2R_UI_EDITOR_RUNTIME_DEFS_DIR "${CMAKE_SOURCE_DIR}/data/defs")
if(EXISTS "${MC2R_UI_EDITOR_RUNTIME_DEFS_DIR}")
    add_custom_command(TARGET ui_editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${MC2R_UI_EDITOR_RUNTIME_DEFS_DIR}"
            "$<TARGET_FILE_DIR:ui_editor>/data/defs"
        COMMENT "Copying MC2R data/defs for UI Editor")
endif()



set(MC2R_UI_EDITOR_RUNTIME_GUI_ART_DIR "${CMAKE_SOURCE_DIR}/data/art/gui")
if(EXISTS "${MC2R_UI_EDITOR_RUNTIME_GUI_ART_DIR}")
    add_custom_command(TARGET ui_editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:ui_editor>/data/art/gui"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${MC2R_UI_EDITOR_RUNTIME_GUI_ART_DIR}"
            "$<TARGET_FILE_DIR:ui_editor>/data/art/gui"
        COMMENT "Copying MC2R data/art/gui for UI Editor image previews")
endif()

set(MC2R_UI_EDITOR_RUNTIME_FONT_DIR "${CMAKE_SOURCE_DIR}/assets/graphics/fonts")
if(EXISTS "${MC2R_UI_EDITOR_RUNTIME_FONT_DIR}")
    add_custom_command(TARGET ui_editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:ui_editor>/assets/graphics/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${MC2R_UI_EDITOR_RUNTIME_FONT_DIR}"
            "$<TARGET_FILE_DIR:ui_editor>/assets/graphics/fonts"
        COMMENT "Copying MC2R runtime fonts for UI Editor")
endif()
