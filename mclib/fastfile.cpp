//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//----------------------------------------------------------------------------
// Global Fast File Code.
//

#include "mc2_czstring.h"
#include"heap.h"
#include"fastfile.h"
#include"fst_hash.h"   // fst_normalize_loose_key_inplace (canonical-key chokepoint)
#include<ctype.h>
#include<string.h>
#include"platform_str.h"
#include <memory>
#include"vfs.h"        // mc2::vfs::invalidate (the merged index folds in the FST mounts)

// NS3 boundary: the FastFile registry storage belongs to the FastFile
// subsystem (this TU), not to whatever game/tool main happens to link it.
// extern'd for all consumers in ffile.h. Previously redefined in every main.
FastFile	**fastFiles = nullptr;
// Archives outlive File borrowers and are destroyed only by FastFileFini.
long		numFastFiles = 0;
long		maxFastFiles = 0;

long ffLastError = 0;

//-----------------------------------------------------------------------------------
bool FastFileInit (mc2::czstring fname)
{
	if (numFastFiles == maxFastFiles)
	{
		ffLastError = -1;
		return FALSE;
	}

	//-----------------------------------------------------------------------------
	//-- Open this fast file, add it to the list O pointers and return TRUE if OK!
	auto file = std::make_unique<FastFile>();
	long result = file->open(fname);
	if (result == FASTFILE_VERSION)
	{
		ffLastError = result;
		return FALSE;
	}

	fastFiles[numFastFiles] = file.release();
	numFastFiles++;
	mc2::vfs::invalidate();

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
	mc2::vfs::invalidate();
}

//-----------------------------------------------------------------------------------
FastFile *FastFileFind (mc2::czstring fname, long &fastFileHandle)
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

	}


	return nullptr;
}

//------------------------------------------------------------------
// elfHash() relocated to mclib/fst_hash.cpp so the tests/unit target can
// link it without pulling in the engine include stack. Decl still lives
// in mclib/fastfile.h; engine callers are unchanged.

//-----------------------------------------------------------------------------------




