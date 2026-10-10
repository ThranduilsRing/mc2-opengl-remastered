cmake_minimum_required (VERSION 3.10)
project(gosfx)

set(SOURCES ${SOURCES}
    effect.cpp
    effectlibrary.cpp
    fcurve.cpp
    cardcloud.cpp
    debriscloud.cpp
    effectcloud.cpp
    particlecloud.cpp
    pertcloud.cpp
    pointcloud.cpp
    shapecloud.cpp
    shardcloud.cpp
    spinningcloud.cpp
    card.cpp
    shape.cpp
    singleton.cpp
    tube.cpp
    pointlight.cpp
    gosfx.cpp
    gosfxheaders.cpp
)

include_directories(${COMMON_INCLUDE_DIRS} "../../mclib")
# Q2-S0: Tracy headers (<tracy/Tracy.hpp>) for the FX cost-split CPU zones in
# effect.cpp / particlecloud.cpp / tube.cpp. TRACY_ENABLE is defined project-
# wide (root CMakeLists), but the gosfx subdir is added before the root
# include_directories(${THIRDPARTY_INCLUDE_DIRS}) line, so the tracy path is
# not otherwise inherited here.
include_directories(${THIRDPARTY_INCLUDE_DIRS})

add_library(gosfx ${SOURCES})

# cat gosFX.vcproj | grep ".cpp" | perl -pe 's/.+\"(\w+\.cpp)\".+/\1/g' >> CMakeLists.txt



# Typed gate registry (Gates/): any source in this library may call gate::get<>(); a library that does
# not link it breaks every standalone tool that links this one (makefst, pak, aseconv, ...).
if(TARGET gosfx)
    target_link_libraries(gosfx PUBLIC mc2_gate)
endif()
