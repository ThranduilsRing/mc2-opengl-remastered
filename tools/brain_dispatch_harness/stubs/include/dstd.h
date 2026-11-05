#pragma once
// minimal dstd.h stub
// Provides common typedefs used by engine headers.
#include <cstdint>
#include <cstddef>

typedef unsigned char  uint8;
typedef unsigned short uint16;
typedef unsigned int   uint32;
typedef signed char    int8;
typedef signed short   int16;
typedef signed int     int32;
typedef float          float32;
typedef double         float64;

inline constexpr int NO_ERR = 0;  // shared success code (mirrors mclib/dstd.h)
