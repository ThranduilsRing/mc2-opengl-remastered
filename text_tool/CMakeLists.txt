add_definitions(-DUSE_ASSEMBLER_CODE=0)
add_definitions(-DLINUX_BUILD)


set(TEXT_TOOL_SOURCES "main.cpp")

include_directories(${CMAKE_PREFIX_PATH}/include)
# EXCLUDE_FROM_ALL: links gameos but 165 game-layer symbols are unresolved
# (see .claude/cleanup/broken-targets.md). Fix later; build explicitly with
# --target text_tool.
add_executable(text_tool EXCLUDE_FROM_ALL ${TEXT_TOOL_SOURCES})
target_link_libraries(text_tool gameos windows ${SDL2_LIBRARIES} SDL2_ttf::SDL2_ttf ${ADDITIONAL_LIBS} OpenGL::GL)
