//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//----------------------------------------------------------------------------
// Global Fast File Code.
//

#include"heap.h"
#include"fastfile.h"
#include"fst_hash.h"   // fst_normalize_loose_key_inplace (canonical-key chokepoint)
#include<ctype.h>
#include<string.h>
#include"platform_str.h"

// NS3 boundary: the FastFile registry storage belongs to the FastFile
// subsystem (this TU), not to whatever game/tool main happens to link it.
// extern'd for all consumers in ffile.h. Previously redefined in every main.
FastFile	**fastFiles = nullptr;
long		numFastFiles = 0;
long		maxFastFiles = 0;

long ffLastError = 0;

#define NO_ERR		0
//-----------------------------------------------------------------------------------
bool FastFileInit (const char *fname)
{
	if (numFastFiles == maxFastFiles)
	{
		ffLastError = -1;
		return FALSE;
	}

	//-----------------------------------------------------------------------------
	//-- Open this fast file, add it to the list O pointers and return TRUE if OK!
	fastFiles[numFastFiles] = new FastFile;
	long result = fastFiles[numFastFiles]->open(fname);
	if (result == FASTFILE_VERSION)
	{
		ffLastError = result;
		return FALSE;
	}

	numFastFiles++;

	return TRUE;
}

//-----------------------------------------------------------------------------------
void FastFileFini (void)
{
	if (fastFiles)
	{
		long currentFastFile = 0;
		while (currentFastFile < maxFastFiles)
		{
			if (fastFiles[currentFastFile])
			{
				fastFiles[currentFastFile]->close();
	
				delete fastFiles[currentFastFile];
				fastFiles[currentFastFile] = nullptr;
			}
	
			currentFastFile++;
		}
	}

	free(fastFiles);
	fastFiles = nullptr;
	numFastFiles= 0;
}

//-----------------------------------------------------------------------------------
FastFile *FastFileFind (const char *fname, long &fastFileHandle)
{
	if (fastFiles)
	{
		// META-FIX design E: callee-side canonical-key chokepoint (issue #8
		// family). FST keys are lowercase forward-slash (fst_hash.h; the
		// engine historically lowercased in File::open BEFORE calling here,
		// while fileExists forgot to — so the same logical path hashed to two
		// different keys depending on the caller). Normalize ONCE here
		// (case+slash) and use the SAME buffer for the elfHash and the
		// openFast name compare, so every caller resolves identically.
		// Uppercase-named FST entries were already unreachable via
		// File::open's lowered path — this is strictly widening.
		char normName[1024];
		if (!fname) return nullptr;
		strncpy(normName, fname, sizeof(normName) - 1);
		normName[sizeof(normName) - 1] = '\0';
		fst_normalize_loose_key_inplace(normName);

		DWORD thisHash = elfHash(normName);
		long currentFastFile = 0;
		long tempHandle = -1;
		while (currentFastFile < numFastFiles)
		{
			tempHandle = fastFiles[currentFastFile]->openFast(thisHash,normName);
			if (tempHandle != -1)
			{
				fastFileHandle = tempHandle;
				return fastFiles[currentFastFile];
			}

			currentFastFile++;
		}

// sebi: our .fst files now all have forward slashes
#if 0
        // sebi NB!
        // if we are here file was not found, try with backslashes
        char* fname2 = _strdup(fname);
        int len = (int)strlen(fname2);
        for(int i=0;i<len;++i) {
            if(fname2[i]=='/')
                fname2[i] = '\\';
        }

		thisHash = elfHash(fname2);
		currentFastFile = 0;
		tempHandle = -1;
		while (currentFastFile < numFastFiles)
		{
			tempHandle = fastFiles[currentFastFile]->openFast(thisHash,fname2);
			if (tempHandle != -1)
			{
				fastFileHandle = tempHandle;
				return fastFiles[currentFastFile];
			}

			currentFastFile++;
		}
#endif

	}


	return nullptr;
}

//------------------------------------------------------------------
// elfHash() relocated to mclib/fst_hash.cpp so the tests/unit target can
// link it without pulling in the engine include stack. Decl still lives
// in mclib/fastfile.h; engine callers are unchanged.

//-----------------------------------------------------------------------------------




