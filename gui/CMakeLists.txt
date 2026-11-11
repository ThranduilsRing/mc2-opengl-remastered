cmake_minimum_required (VERSION 3.10)
project(gui)

set(SOURCES ${SOURCES}
    aanim.cpp
    abutton.cpp
    aedit.cpp
    afont.cpp
    alistbox.cpp
    alistbox_dropdown.cpp
    ascroll.cpp
    asystem.cpp
    logisticsscreen.cpp
    UiDefs.cpp
    UiDefs_draw.cpp
    UiDefs_element.cpp
    UiDefs_parse.cpp
    )

include_directories(${COMMON_INCLUDE_DIRS} "../mclib" "../Code")
include_directories(${THIRDPARTY_INCLUDE_DIRS})

add_library(gui ${SOURCES})

# Typed gate registry (Gates/): any source in this library may call gate::get<>(); a library that does
# not link it breaks every standalone tool that links this one (makefst, pak, aseconv, ...).
if(TARGET gui)
    target_link_libraries(gui PUBLIC mc2_gate)
endif()
