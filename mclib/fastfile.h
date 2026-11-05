#include "mc2_czstring.h"
//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//----------------------------------------------------------------------------
// Global Fast File Header. 
//

#ifndef DSTD_H
#include"dstd.h"
#endif

#ifndef FFILE_H
#include"ffile.h"
#endif

//-----------------------------------------------------------------------------------
// MechCommander's File classes understand how to deal with the FastFile, once open.
// In other words, the global class needs only to register newly opened fastfiles,
// close them all on exit and provide a find function which returns a pointer to the
// fastfile containing the file we care about.  If no file is found, returns NULL.
extern bool FastFileInit (mc2::czstring fname);
extern void FastFileFini (void);
extern FastFile *FastFileFind (mc2::czstring fname, long &fastFileHandle);
extern "C" uint32_t elfHash (mc2::czstring name); // fixed-width FST hash on Windows and LP64
//-----------------------------------------------------------------------------------


