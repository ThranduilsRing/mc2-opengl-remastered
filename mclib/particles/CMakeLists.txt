cmake_minimum_required(VERSION 3.10)
project(particles)

# GPU particle batcher (producer side) per integrated plan v5 §5.4 B1.
# One-way dependency: this lib has NO GameOS / GL dependency. The bridge
# entry point gos_particle_bridge_flush is implemented in GameOS/gameos/
# gos_particle_bridge.cpp; this lib only forward-declares it.

set(SOURCES
    batcher.cpp
    spec_library.cpp
    light_manager.cpp
    spawn_card.cpp
    spawn_cardcloud.cpp
    spawn_point.cpp
    spawn_shard.cpp
    spawn_tube.cpp
    spawn.cpp
    gpu_trail.cpp
)

include_directories(${COMMON_INCLUDE_DIRS} "../../" "../../mclib" "./")
include_directories(${THIRDPARTY_INCLUDE_DIRS})

add_library(particles ${SOURCES})

# Typed gate registry (Gates/): any source in this library may call gate::get<>(); a library that does
# not link it breaks every standalone tool that links this one (makefst, pak, aseconv, ...).
if(TARGET particles)
    target_link_libraries(particles PUBLIC mc2_gate)
endif()
