# Platform R0: the window / input / file seam. INTERFACE target whose sources compile INTO the consumer
# (gameos, gameos_editor), so they keep the consumer's include dirs, flags and the input:: state they read;
# link it PRIVATE (a PUBLIC link would compile the sources a second time into mc2).
add_library(mc2_platform INTERFACE)
target_sources(mc2_platform INTERFACE "${CMAKE_SOURCE_DIR}/Platform/WindowInput.cpp")
target_include_directories(mc2_platform INTERFACE "${CMAKE_SOURCE_DIR}/Platform")
