//---------------------------------------------------------------------------
//
// cident.cpp - This file contains some misc string classes
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include files

#ifndef CIDENT_H
#include"cident.h"
#endif

#include "mc2_czstring.h"
#include"heap.h"
#include<ctype.h>

#include"platform_windows.h"

// sebi
//#ifndef _MBCS
#include<gameos.hpp>
//#else
//#include<assert.h>
//#define gosASSERT assert
//#define gos_Malloc malloc
//#define gos_Free free
//#endif
#include "platform_str.h"

//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
//								FullPathFileName
//---------------------------------------------------------------------------
FullPathFileName::~FullPathFileName (void)
{
	destroy();
}

//---------------------------------------------------------------------------
void FullPathFileName::destroy (void)
{
	if (fullName)
		std::free(fullName);

	fullName = nullptr;
}

//---------------------------------------------------------------------------
void FullPathFileName::init (mc2::czstring dir_path, mc2::czstring name, mc2::czstring ext, bool do_not_make_lower)
{
	destroy();

	long total_length = static_cast<long>(strlen(dir_path));
	total_length += static_cast<long>(strlen(name));
	total_length += static_cast<long>(strlen(ext));
	total_length++;


	fullName = static_cast<char *>(std::malloc(total_length));
	gosASSERT(fullName != nullptr);
	fullName[0] = 0;

	if ( strstr( name, dir_path ) != name )
		strcpy(fullName,dir_path);
	strcat(fullName,name);

	// don't append if its already there
	if (ext && (strlen(fullName) < strlen(ext) ||
		S_stricmp(fullName + strlen(fullName) - strlen(ext), ext) != 0))
		strcat(fullName,ext);

    if(!do_not_make_lower)
	    CharLower(fullName);
}

void FullPathFileName::changeExt (mc2::czstring from, mc2::czstring to)
{
	if (strlen(from) != strlen(to))
		return;

	char *ext = strstr(fullName,from);
	if (ext)
	{
		for (unsigned long i=0;i<strlen(to);i++)
			ext[i] = to[i];
	}
}
//---------------------------------------------------------------------------
//
// Edit Log
//
// $Log$
//
//---------------------------------------------------------------------------
