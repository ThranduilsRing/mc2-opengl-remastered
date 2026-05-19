/***************************************************************
* FILENAME: StdAfx.h
* DESCRIPTION: Defines shared precompiled-header includes for the Editor target.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster comments and attribution header.
****************************************************************/

// stdafx.h : include file for standard system include files,
//  or project specific include files that are used frequently, but
//      are changed infrequently
//
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_

#include <winsock2.h>
#include <windows.h>
#include <afxwin.h>         // MFC core and standard components

#if !defined(AFX_STDAFX_H__05FE54C7_36AB_4243_BAB1_3FA8FB6F103F__INCLUDED_)
#define AFX_STDAFX_H__05FE54C7_36AB_4243_BAB1_3FA8FB6F103F__INCLUDED_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#define VC_EXTRALEAN		// Exclude rarely-used stuff from Windows headers

//include "mclib.h"
#include <afxext.h>         // MFC extensions
#include <afxdtctl.h>		// MFC support for Internet Explorer 4 Common Controls
#ifndef _AFX_NO_AFXCMN_SUPPORT
#include <afxcmn.h>			// MFC support for Windows Common Controls
#endif // _AFX_NO_AFXCMN_SUPPORT
// Suppress C4091: typedef ignored on left of PlatformType (GameOS/include/platform.hpp).
// This warning originates inside a GameOS engine header that is out of Editor scope.
// Suppress here so it does not flood the Editor build log.
#pragma warning(disable: 4091)

#include "MFCPlatform.hpp"

// ---------------------------------------------------------------------------
// POSIX naming compatibility for MSVC (C4996 suppression).
// Maps bare POSIX names to their underscore-prefixed MSVC equivalents.
// Editor-local only. Do not copy into shared mclib or GameOS headers.
// ---------------------------------------------------------------------------
#ifdef _MSC_VER
#  ifndef stricmp
#    define stricmp  _stricmp
#  endif
#  ifndef strcmpi
#    define strcmpi  _stricmp
#  endif
#  ifndef strnicmp
#    define strnicmp _strnicmp
#  endif
#  ifndef strdup
#    define strdup   _strdup
#  endif
#  ifndef strlwr
#    define strlwr   _strlwr
#  endif
#  ifndef strupr
#    define strupr   _strupr
#  endif
#  ifndef itoa
#    define itoa     _itoa
#  endif
#endif // _MSC_VER

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif // !defined(AFX_STDAFX_H__05FE54C7_36AB_4243_BAB1_3FA8FB6F103F__INCLUDED_)
