# tools/mc2weapon_gui — Dear ImGui (SDL2 + Vulkan) weapon editor.
#
# Foolproof front-end over the compbas.csv / effects.csv weapon model (same model
# as tools/mc2weapon/mc2weapon.py). Edits write a loose mods/<id> overlay. NO
# engine closure — CSV is plain text, so this is just an ImGui shell + weapon_csv.
#
# Build with -DENABLE_MC2WEAPON_GUI=ON (guarded in the top CMakeLists). SDL2 /
# SDL2 / ImGui discovery mirrors tools/mc2fx_preview exactly.

add_executable(mc2weapon_gui
    main.cpp
    weapon_csv.cpp
)
target_compile_features(mc2weapon_gui PRIVATE cxx_std_17)
target_compile_definitions(mc2weapon_gui PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

if(NOT TARGET SDL2::SDL2)
    find_package(SDL2 CONFIG REQUIRED)
endif()

# SDL include discovery (copied from tools/mc2fx_preview).
set(MC2W_SDL_INCLUDE_DIRS)
if(DEFINED SDL2_INCLUDE_DIRS)
    list(APPEND MC2W_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
endif()
if(DEFINED SDL2_INCLUDE_DIR)
    list(APPEND MC2W_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
endif()
foreach(_p IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${_p}/include/SDL2/SDL.h")
        list(APPEND MC2W_SDL_INCLUDE_DIRS "${_p}/include/SDL2")
    endif()
    if(EXISTS "${_p}/include/SDL.h")
        list(APPEND MC2W_SDL_INCLUDE_DIRS "${_p}/include")
    endif()
endforeach()
foreach(_vr
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows"
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x86-windows"
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-linux"
)
    if(EXISTS "${_vr}/include/SDL2/SDL.h")
        list(APPEND MC2W_SDL_INCLUDE_DIRS "${_vr}/include/SDL2")
    endif()
    if(EXISTS "${_vr}/include/SDL.h")
        list(APPEND MC2W_SDL_INCLUDE_DIRS "${_vr}/include")
    endif()
endforeach()
if(MC2W_SDL_INCLUDE_DIRS)
    list(REMOVE_DUPLICATES MC2W_SDL_INCLUDE_DIRS)
endif()

target_include_directories(mc2weapon_gui PRIVATE
    "${CMAKE_SOURCE_DIR}/3rdparty/imgui"
    "${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends"
    "${CMAKE_SOURCE_DIR}/3rdparty/include"
    ${MC2W_SDL_INCLUDE_DIRS}
)

target_link_libraries(mc2weapon_gui PRIVATE imgui)

if(TARGET SDL2::SDL2)
    target_link_libraries(mc2weapon_gui PRIVATE SDL2::SDL2)
elseif(DEFINED SDL2_LIBRARIES)
    target_link_libraries(mc2weapon_gui PRIVATE ${SDL2_LIBRARIES})
else()
    message(FATAL_ERROR "SDL2 not found for mc2weapon_gui")
endif()
if(TARGET SDL2::SDL2main)
    target_link_libraries(mc2weapon_gui PRIVATE SDL2::SDL2main)
endif()

# Vulkan shell shared by the viewport tools (tools/common). It carries imgui + SDL2 + volk.
if(NOT TARGET mc2_tool_vk)
    add_subdirectory("${CMAKE_SOURCE_DIR}/tools/common" "${CMAKE_BINARY_DIR}/tools_common")
endif()
target_link_libraries(mc2weapon_gui PRIVATE mc2_tool_vk)
if(WIN32)
    target_link_libraries(mc2weapon_gui PRIVATE Psapi)
endif()
