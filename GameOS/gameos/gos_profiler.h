// gos_profiler.h — MC2 profiling wrapper
// Include this in any file that needs profiling zones.
// Tracy is always compiled in; overhead is ~1ns per zone when profiler not connected.
#pragma once

// GL headers must come before TracyOpenGL.hpp to avoid "undeclared identifier" errors
#include <GL/glew.h>
#include <GL/gl.h>

#include <tracy/Tracy.hpp>
#include <tracy/TracyOpenGL.hpp>
