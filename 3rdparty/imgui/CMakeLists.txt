# Dear ImGui by Omar Cornut et al. -- https://github.com/ocornut/imgui
# Integrated for MC2 OpenGL port in collaboration with @methuselas.
add_library(imgui STATIC
    imgui.cpp
    imgui_demo.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    backends/imgui_impl_sdl2.cpp
)

target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/backends
)

if(MC2_VULKAN)
    # Native renderer shared by the game, editor and viewport tools.
    target_sources(imgui PRIVATE backends/imgui_impl_vulkan.cpp)
    target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
    target_include_directories(imgui PUBLIC "${CMAKE_SOURCE_DIR}/3rdparty/volk")
    target_link_libraries(imgui PUBLIC Vulkan::Headers)
endif()

# SDL2 headers needed by imgui_impl_sdl2.cpp.
# That backend uses #include <SDL.h>, so the include path must point directly
# at the directory containing SDL.h, commonly <prefix>/include/SDL2.
set(IMGUI_SDL2_INCLUDE_DIRS "")

if(TARGET SDL2::SDL2)
    target_link_libraries(imgui PUBLIC SDL2::SDL2)

    get_target_property(_imgui_sdl2_target_includes SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
    if(_imgui_sdl2_target_includes)
        list(APPEND IMGUI_SDL2_INCLUDE_DIRS ${_imgui_sdl2_target_includes})
    endif()
endif()

if(DEFINED SDL2_INCLUDE_DIRS)
    list(APPEND IMGUI_SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
endif()

if(DEFINED SDL2_INCLUDE_DIR)
    list(APPEND IMGUI_SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
endif()

foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    if(NOT "${_prefix}" STREQUAL "")
        file(TO_CMAKE_PATH "${_prefix}" _prefix_cmake)
        if(EXISTS "${_prefix_cmake}/include/SDL2/SDL.h")
            list(APPEND IMGUI_SDL2_INCLUDE_DIRS "${_prefix_cmake}/include/SDL2")
        endif()
        if(EXISTS "${_prefix_cmake}/include/SDL.h")
            list(APPEND IMGUI_SDL2_INCLUDE_DIRS "${_prefix_cmake}/include")
        endif()
    endif()
endforeach()

foreach(_repo_sdl2_candidate
        "${CMAKE_SOURCE_DIR}/3rdparty/include/SDL2"
        "${CMAKE_SOURCE_DIR}/3rdparty/include")
    if(EXISTS "${_repo_sdl2_candidate}/SDL.h")
        list(APPEND IMGUI_SDL2_INCLUDE_DIRS "${_repo_sdl2_candidate}")
    endif()
endforeach()

list(REMOVE_DUPLICATES IMGUI_SDL2_INCLUDE_DIRS)

if(IMGUI_SDL2_INCLUDE_DIRS)
    target_include_directories(imgui PUBLIC ${IMGUI_SDL2_INCLUDE_DIRS})
    message(STATUS "imgui SDL2 include dirs: ${IMGUI_SDL2_INCLUDE_DIRS}")
else()
    message(FATAL_ERROR "imgui SDL2 backend requires SDL.h, but no SDL2 include directory containing SDL.h was found.")
endif()

# Suppress warnings in vendored code
if(MSVC)
    target_compile_options(imgui PRIVATE /W0)
else()
    target_compile_options(imgui PRIVATE -w)
endif()
