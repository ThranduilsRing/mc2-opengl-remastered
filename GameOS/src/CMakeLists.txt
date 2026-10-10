cmake_minimum_required (VERSION 3.10)
project(windows)

set(SOURCES ${SOURCES}
    platform_winbase.cpp
    platform_winnls.cpp
    platform_winuser.cpp
    platform_stdlib.cpp
    platform_tchar.cpp
    platform_mbstring.cpp
    platform_eh.cpp
    platform_io.cpp
    platform_mmsystem.cpp
    platform_str.cpp
)

include_directories("../include" )

add_library(windows ${SOURCES})


# Typed gate registry (Gates/): any source in this library may call gate::get<>(); a library that does
# not link it breaks every standalone tool that links this one (makefst, pak, aseconv, ...).
if(TARGET windows)
    target_link_libraries(windows PUBLIC mc2_gate)
endif()
