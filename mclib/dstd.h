//---------------------------------------------------------------------------
//
// dstd.h - This file contains the standard definitions
//
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef DSTD_H
#define DSTD_H
//---------------------------------------------------------------------------
// Include files

//---------------------------------------------------------------------------
// Type Definitions
using MemoryPtr = unsigned char*;
using byte = unsigned char;
//sebi
//typedef unsigned size_t;

#ifndef TRUE
#define TRUE	1
#endif

#ifndef FALSE
#define FALSE	0
#endif

inline constexpr int NO_ERR = 0;  // shared success code; every legacy header that used to define it includes this one
inline constexpr float RADS_TO_DEGREES = 57.2957795132f;		// Used to convert results in Rad to Deg
inline constexpr float DEGREES_TO_RADS = 0.0174532925199f; 	// Used to convert results in Deg to Rad
inline constexpr float COS_SIN_45 = 0.70710678f;		 	// Used to convert results in Deg to Rad
#ifndef PI
#define PI				3.1415926535897932384626433832795
#endif

// !NB sebi
#ifndef PLATFORM_WINDOWS
using DWORD = unsigned int;
using UINT = unsigned int;
using BYTE = unsigned char;
using PVOID = void*;
#else
using DWORD = unsigned long; // sizeof(long) is 32bit on both 32bit and 64 bit Visual C++ compilers
using UINT = unsigned int;
using BYTE = unsigned char;
using PVOID = void*;
#endif

//--------------------------------------------------------------------------
// Macro Definitions
inline constexpr int NONE = -1;

//--------------------------------------------------------------------------
// Pragmas
#pragma warning(disable:4244)
#pragma warning(disable:4514)		// Unused Inline Functions
#pragma warning(disable:4800)
#pragma warning(disable:4244)

//---------------------------------------------------------------------------
#endif

//---------------------------------------------------------------------------
//
// Edit Log
//
//
//---------------------------------------------------------------------------
