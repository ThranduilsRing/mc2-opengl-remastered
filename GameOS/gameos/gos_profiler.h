// gos_profiler.h — MC2 profiling wrapper
// Include this in any file that needs profiling zones.
// Tracy is always compiled in; overhead is ~1ns per zone when profiler not connected.
#pragma once

#include <tracy/Tracy.hpp>
#include <tracy/TracyOpenGL.hpp>
