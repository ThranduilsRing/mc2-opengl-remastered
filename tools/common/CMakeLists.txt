# tools/common -- code shared by the viewport tools. mc2_tool_vk is the Vulkan window shell (see tool_vk_shell.h).
if(NOT TARGET mc2_tool_vk)
    add_library(mc2_tool_vk STATIC
        tool_vk_shell.cpp
        tool_vk_gpu.cpp
        "${CMAKE_SOURCE_DIR}/3rdparty/volk/volk.c"
    )
    target_compile_features(mc2_tool_vk PRIVATE cxx_std_17)
    target_compile_definitions(mc2_tool_vk PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK VK_NO_PROTOTYPES)
    if(WIN32)
        target_compile_definitions(mc2_tool_vk PRIVATE VK_USE_PLATFORM_WIN32_KHR)
    endif()
    target_include_directories(mc2_tool_vk PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/3rdparty/volk"
    )
    target_link_libraries(mc2_tool_vk PUBLIC imgui Vulkan::Headers)
    if(TARGET SDL2::SDL2)
        target_link_libraries(mc2_tool_vk PUBLIC SDL2::SDL2)
    endif()
endif()
