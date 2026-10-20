//---------------------------------------------------------------------------
//
// ffile.cpp - This file contains the class functions for FastFile
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include files
#ifndef FFILE_H
#include"ffile.h"
#include "gos_crashbundle.h"
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <utility>
// Format, print, and feed the crash-bundle ring in one call.
namespace {

template <class... Args>
void CB_PRINTF(std::format_string<Args...> fmt, Args&&... args)
{
    crashbundle_printf<512>("", fmt, std::forward<Args>(args)...);
}
}
#endif

#ifndef FASTFILE_H
#include "fastfile.h"
#endif

#ifndef UTILITIES_H
#include"utilities.h"
#endif

#include"lz.h"

#include<zlib.h>

#include"platform_windows.h"
#include"platform_io.h"

#include<string.h>
#include"platform_str.h"
#include<gameos.hpp>
#include "mc2_dialog.h"
#include "Gates/gate.h"

MemoryPtr 		LZPacketBuffer = nullptr;
unsigned int	LZPacketBufferSize = 512000;

extern char CDInstallPath[];
void EnterWindowMode();
void EnterFullScreenMode();
void __stdcall ExitGameOS();

char FileMissingString[512];
char CDMissingString[1024];
char MissingTitleString[256];
//---------------------------------------------------------------------------
//	class FastFile member functions
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
FastFile::FastFile (void)
{
	files = nullptr;
	numFiles = 0;

	fileName = nullptr;
	handle = nullptr;
	length = 0;
	logicalPosition = 0;

	useLZCompress = false;

	numWrittenFiles = 0;
}
			
//---------------------------------------------------------------------------
FastFile::~FastFile (void)
{
	close();

	if (LZPacketBuffer)
	{
		free(LZPacketBuffer);
		LZPacketBuffer = nullptr;
	}
}

long FastFile::writeVersion(FILE* handle)
{
	fseek(handle, 0, SEEK_SET);
	int version = useLZCompress ? FASTFILE_VERSION_LZ : FASTFILE_VERSION;
	int result = static_cast<int>(fwrite(&version, 1, 4, handle));  
	logicalPosition += result;

	if (result != 4)
	{
		long lastError = errno;
		return lastError;
	}

	return NO_ERR;
}

long FastFile::writeNumFiles(FILE* handle, int num_files)
{
	fseek(handle, 4, SEEK_SET);
	int result = static_cast<int>(fwrite(&numFiles, 1, 4, handle));
	if (result != 4)
	{
		long lastError = errno;
		return lastError;
	}

	return NO_ERR;
}

long FastFile::writeFileEntries(FILE* handle, std::span<FILE_HANDLE> files, int offset)
{
	fseek(handle, FASTFILE_ENTRY_TABLE_START + offset * sizeof(FILEENTRY), SEEK_SET);
	int result  = 0;
	for (auto& file : files)
	{
		result += static_cast<int>(fwrite(file.pfe, 1, sizeof(FILEENTRY), handle));
	}

	return result == sizeof(FILEENTRY) * files.size() ? NO_ERR : BAD_WRITE_ERR;
}

//---------------------------------------------------------------------------
long FastFile::open (const char* fName)
{
	//-------------------------------------------------------------
	long fNameLength = static_cast<long>(strlen(fName));
	fileName = std::make_unique<char[]>(fNameLength+1);

	if (!fileName)
		return(NO_RAM_FOR_FILENAME);
		
	strncpy(fileName.get(),fName,fNameLength+1);

	handle = fopen(fileName.get(),"rb");
	if (handle != nullptr)
	{
		logicalPosition = 0;
		fileSize();				//Sets Length
	}
	else
	{
		//Check on CD.  They may not have been installed.
		char actualPath[2048];
		strcpy(actualPath,CDInstallPath);
		strcat(actualPath,fileName.get());
		handle = fopen(actualPath,"r");
		if (handle == nullptr)
		{
			if (!Environment.checkCDForFiles)
				return (2);  // disk install; FastFile not present anywhere, fail quietly
			//OPEN Error.  Maybe the CD is missing?
			bool openFailed = false;
			bool alreadyFullScreen = (Environment.fullScreen != 0);
			while (handle == nullptr)
			{
				openFailed = true;
				if (!gos_NoUserPresentRequested()) EnterWindowMode();

				char data[2048];
				sprintf(data,FileMissingString,fileName.get(),CDMissingString);
				DWORD result1 = mc2MessageBox(data,MissingTitleString,MB_OKCANCEL | MB_ICONWARNING,IDCANCEL);
				if (result1 == IDCANCEL)
				{
					ExitGameOS();
					return (2);		//File not found.  Never returns though!
				}

				handle = fopen(actualPath,"r");
			}

			if (openFailed && (Environment.fullScreen == 0) && alreadyFullScreen)
				EnterFullScreenMode();
		}
		else
		{
			logicalPosition = 0;
			// sebi
			// NB!: this is incorrect, because if we opened file fromCD we need to change its name(path), otherwise, size wil be incorrect!
			fileSize();				//Sets Length
		}
	}

	//---------------------------------------------
	//-- Read in ALL of the file names and offsets
	logicalPosition = fseek(handle,0,SEEK_SET);

	//---------------------------------------------
	//-- First Long is Version Number of FastFile
	unsigned long result = 0;
	unsigned long version = 0;

	result = static_cast<unsigned long>(fread((&version),1, FASTFILE_VERSION_BYTESIZE,handle));
    logicalPosition += FASTFILE_VERSION_BYTESIZE;

	if (result != FASTFILE_VERSION_BYTESIZE)
	{
		long lastError = errno;
		return lastError;
	}

	if (version != FASTFILE_VERSION && version != FASTFILE_VERSION_LZ)
		return FASTFILE_VERSION;

	if (version == FASTFILE_VERSION_LZ)
		useLZCompress = true;

	//---------------------------------------------
	//-- Second Long is number of filenames present.
	result = static_cast<unsigned long>(fread((&numFiles),1, 4,handle));

	logicalPosition += 4;

	if (result != 4)
	{
		long lastError = errno;
		return lastError;
	}

	files = static_cast<FILE_HANDLE*>(malloc(sizeof(FILE_HANDLE) * numFiles));

	for (DWORD i=0;i<numFiles;i++)
	{
		files[i].pfe = static_cast<FILEENTRY *>(malloc(sizeof(FILEENTRY)));
		memset(files[i].pfe,0,sizeof(FILEENTRY));

		unsigned long result = 0;
		result = static_cast<unsigned long>(fread(files[i].pfe,1,sizeof(FILEENTRY),handle));

		files[i].inuse = FALSE;
		files[i].pos = 0;
	}

	return (0);
}
		
//---------------------------------------------------------------------------
void FastFile::close (void)
{
	//------------------------------------------------------------------------
	// The actual stored filename is also in the parent.  Everyone else just has
	// pointer and, as such, only the parent frees the memory.
	fileName.reset();
	length = 0;

	if (isOpen())
	{
		fclose(handle);
		handle = nullptr;
	}

	//---------------------------------------------
	//-- First Long is number of filenames present.
	for (DWORD i=0;i<numFiles;i++)
	{
		free(files[i].pfe);
	}

	free(files);

	files = nullptr;
	numFiles = 0;
}
long FastFile::create(const char* fName, bool compressed)
{
	if(handle)
		return FILE_ALREADY_OPEN;

	//-------------------------------------------------------------
	long fNameLength = static_cast<long>(strlen(fName));
	fileName = std::make_unique<char[]>(fNameLength+1);

	if (!fileName) {
		return(NO_RAM_FOR_FILENAME);
    }
		
	strncpy(fileName.get(),fName,fNameLength+1);

	handle = fopen(fileName.get(), "wb");
	if (handle != nullptr)
	{
		logicalPosition = 0;
		fileSize();				//Sets Length
	}

	useLZCompress = compressed;

	long res = writeVersion(handle);
	if(NO_ERR != res) {
        fileName.reset();
		return res;
    }

	res = writeNumFiles(handle, 0);

	logicalPosition = ftell(handle);
		
	return NO_ERR;
}

long FastFile::reserve(int num_files)
{
	numWrittenFiles = 0;

	if(files) {

		for (DWORD i=0; i<numFiles; i++) {
			free(files[i].pfe);
		}
		free(files);
	}
	numFiles = num_files;

	long res = writeNumFiles(handle, numFiles);
	if(NO_ERR != res)
		return res;

	files = static_cast<FILE_HANDLE*>(malloc(sizeof(FILE_HANDLE) * numFiles));

	for (DWORD i=0;i<numFiles;i++)
	{
		files[i].pfe = static_cast<FILEENTRY *>(calloc(1, sizeof(FILEENTRY)));
		files[i].inuse = FALSE;
		files[i].pos = 0;
	}

	res = writeFileEntries(handle, std::span<FILE_HANDLE>(files, numFiles), 0);
	if(NO_ERR != res)
		return res;

	logicalPosition = ftell(handle);

	return NO_ERR;
}

//---------------------------------------------------------------------------
long FastFile::openFast (DWORD hash, const char *fName)
{
	//------------------------------------------------------------------
	//-- In order to use this, the file name must be part of the index.
	for (DWORD i=0;i<numFiles;i++)
	{
		if ((hash == files[i].pfe->hash) && (S_stricmp(files[i].pfe->name,fName) == 0))
		{
			files[i].inuse = TRUE;
			files[i].pos = 0;
			return i;
		}
	}

	return -1;
}

//---------------------------------------------------------------------------
void FastFile::closeFast (DWORD fastFileHandle)
{
	if ((fastFileHandle >= 0) && (fastFileHandle < numFiles) && files[fastFileHandle].inuse)
	{
		files[fastFileHandle].inuse = FALSE;
		files[fastFileHandle].pos = 0;
	}
}

//---------------------------------------------------------------------------
long FastFile::seekFast (DWORD fastFileHandle, DWORD off, DWORD how)
{
	if ((fastFileHandle >= 0) && (fastFileHandle < numFiles) && files[fastFileHandle].inuse)
	{
		switch (how)
		{
			case SEEK_SET:
				if (off > files[fastFileHandle].pfe->size)
				{
					return READ_PAST_EOF_ERR;
				}
				break;
	
			case SEEK_END:
				if ((static_cast<DWORD>(abs(static_cast<long>(off))) > files[fastFileHandle].pfe->size) || (off > 0))
				{
					return READ_PAST_EOF_ERR;
				}
				break;
	
			case SEEK_CUR:
				if (off+files[fastFileHandle].pos > files[fastFileHandle].pfe->size)
				{
					return READ_PAST_EOF_ERR;
				}
				break;
		}

		long newPosition = 0;
		switch (how)
		{
			case SEEK_SET:
				newPosition = off;
			break;

			case SEEK_END:
				newPosition = files[fastFileHandle].pfe->size;
				newPosition += off;
			break;

			case SEEK_CUR:
				newPosition = off+files[fastFileHandle].pos;
			break;
		}
			
		if (newPosition == -1)
		{
			return (INVALID_SEEK_ERR);
		}

		files[fastFileHandle].pos = newPosition;

		//-----------------------------------
		//-- Now macro seek the entire file.
		if (fseek(handle,files[fastFileHandle].pos + files[fastFileHandle].pfe->offset,SEEK_SET) == 0)
			logicalPosition = ftell(handle);

		return (files[fastFileHandle].pos);
	}

	return (FILE_NOT_OPEN);
}

//---------------------------------------------------------------------------
long FastFile::readFast (DWORD fastFileHandle, void *bfr, DWORD size)
{
	size;

	long result = 0;

	if ((fastFileHandle >= 0) && (fastFileHandle < numFiles) && files[fastFileHandle].inuse)
	{
		logicalPosition = fseek(handle,files[fastFileHandle].pos + files[fastFileHandle].pfe->offset,SEEK_SET);

		//ALL files in the fast file are now zLib compressed. NO EXCEPTIONS!!
		// This fixes a bug where the zLib Compressed version is the same length
		// as the raw version.  Yikes but this is rare.  Finally happened though!
		// -fs
		/*
		if (files[fastFileHandle].pfe->size == files[fastFileHandle].pfe->realSize)
		{
			result = fread(bfr,1,size,handle);
			logicalPosition += size;

			if (result != size)
			{
				long lastError = errno;
				return lastError;
			}
		}
		else			//File is NOW zLib Compressed.  Read In Appropriately
		*/
		{
			if (!LZPacketBuffer)
			{
				LZPacketBuffer = static_cast<MemoryPtr>(malloc(LZPacketBufferSize));
				if (!LZPacketBuffer)
					return 0;
			}
				
			if (static_cast<DWORD>(LZPacketBufferSize) < files[fastFileHandle].pfe->size)
			{
				LZPacketBufferSize = files[fastFileHandle].pfe->size;
				
				free(LZPacketBuffer);
				LZPacketBuffer = static_cast<MemoryPtr>(malloc(LZPacketBufferSize));
				if (!LZPacketBuffer)
					return 0;
			}
			
			if (LZPacketBuffer)
			{
				result = static_cast<long>(fread(LZPacketBuffer,1,files[fastFileHandle].pfe->size,handle));
				logicalPosition += files[fastFileHandle].pfe->size;

				// second condition to handle zero-length files
				if (result != files[fastFileHandle].pfe->size && files[fastFileHandle].pfe->size>0)
				{
					if (!Environment.checkCDForFiles)
						return 0;  // disk install; short read, fail quietly
					//READ Error.  Maybe the CD is missing?
					bool openFailed = false;
					bool alreadyFullScreen = (Environment.fullScreen != 0);
					while (result != files[fastFileHandle].pfe->size)
					{
						openFailed = true;
						if (!gos_NoUserPresentRequested()) EnterWindowMode();

						char data[2048];
						sprintf(data,FileMissingString,fileName.get(),CDMissingString);
						DWORD result1 = mc2MessageBox(data,MissingTitleString,MB_OKCANCEL | MB_ICONWARNING,IDCANCEL);
						if (result1 == IDCANCEL)
						{
							ExitGameOS();
							return (2);		//File not found.  Never returns though!
						}

						logicalPosition = fseek(handle,files[fastFileHandle].pos + files[fastFileHandle].pfe->offset,SEEK_SET);
						result = static_cast<long>(fread(LZPacketBuffer,1,files[fastFileHandle].pfe->size,handle));
						logicalPosition += files[fastFileHandle].pfe->size;
					}
		
					if (openFailed && (Environment.fullScreen == 0) && alreadyFullScreen)
						EnterFullScreenMode();
				}

				//--------------------------------------------------------
				// sebi 2026-04-21: per-entry raw/compressed split.
				// Stock MC2 tools always wrote zlib (FASTFILE_VERSION) or LZSS (FASTFILE_VERSION_LZ).
				// Wolfman's FST tool writes entries raw when compression saves nothing — detectable
				// by pfe->size == pfe->realSize. Previous code force-decompressed raw entries,
				// producing silent zero-filled output when zlib rejected the stream.
				unsigned long decompLength = 0;
				static const bool s_ffTrace = mc2::gate::get<"MC2_FF_TRACE">();
				if (files[fastFileHandle].pfe->size == files[fastFileHandle].pfe->realSize)
				{
					// size == realSize is AMBIGUOUS: either a raw-stored entry
					// (wolfman's FST tool) OR the rare stock case where the zlib
					// stream is exactly as long as the raw data (the original -fs
					// comment above; live example: misc.fst
					// data/objects/badpilots.csv, 44 == 44, bytes 78 9c ...).
					// EDITOR-PILOT-CATALOG-FST-1: sniff the zlib header and try to
					// inflate; fall back to verbatim copy when it is not a valid
					// zlib stream of exactly realSize.
					bool copiedVerbatim = true;
					const unsigned char zb0 = LZPacketBuffer[0];
					const unsigned char zb1 = (files[fastFileHandle].pfe->size > 1) ? LZPacketBuffer[1] : 0;
					if (files[fastFileHandle].pfe->size > 2 && zb0 == 0x78 && ((zb0 * 256 + zb1) % 31) == 0)
					{
						unsigned long tryLength = files[fastFileHandle].pfe->realSize;
						if (Z_OK == uncompress(reinterpret_cast<MemoryPtr>(bfr), &tryLength, LZPacketBuffer, files[fastFileHandle].pfe->size)
							&& tryLength == static_cast<unsigned long>(files[fastFileHandle].pfe->realSize))
						{
							if (s_ffTrace) { CB_PRINTF("[FF] RAW-ZLIB {} size={:d} real={:d} (size==realSize but valid zlib; inflated)", files[fastFileHandle].pfe->name, files[fastFileHandle].pfe->size, files[fastFileHandle].pfe->realSize); }
							copiedVerbatim = false;
						}
					}
					if (copiedVerbatim)
					{
						// Raw-stored entry: copy verbatim.
						if (s_ffTrace) { CB_PRINTF("[FF] RAW {} size={:d} real={:d}", files[fastFileHandle].pfe->name, files[fastFileHandle].pfe->size, files[fastFileHandle].pfe->realSize); }
						memcpy(bfr, LZPacketBuffer, files[fastFileHandle].pfe->size);
					}
					decompLength = files[fastFileHandle].pfe->realSize;
				}
				else if (useLZCompress)
				{
					if (s_ffTrace) { CB_PRINTF("[FF] LZSS {} size={:d} real={:d}", files[fastFileHandle].pfe->name, files[fastFileHandle].pfe->size, files[fastFileHandle].pfe->realSize); }
					decompLength = static_cast<unsigned long>(LZDecomp(reinterpret_cast<MemoryPtr>(bfr),LZPacketBuffer,files[fastFileHandle].pfe->size,files[fastFileHandle].pfe->realSize));
				}
				else
				{
					if (s_ffTrace) { CB_PRINTF("[FF] ZLIB {} size={:d} real={:d} firstbytes={:02x}{:02x}{:02x}{:02x}", files[fastFileHandle].pfe->name, files[fastFileHandle].pfe->size, files[fastFileHandle].pfe->realSize, LZPacketBuffer[0], LZPacketBuffer[1], LZPacketBuffer[2], LZPacketBuffer[3]); }
					decompLength = files[fastFileHandle].pfe->realSize;
					long error = uncompress(reinterpret_cast<MemoryPtr>(bfr),&decompLength,LZPacketBuffer,files[fastFileHandle].pfe->size);
					if (error != Z_OK) {
						if (s_ffTrace) { CB_PRINTF("[FF] ZLIB FAILED error={:d} {}", error, files[fastFileHandle].pfe->name); }
						STOP(("Error %d UnCompressing File %s from FastFile %s",error,files[fastFileHandle].pfe->name,fileName.get()));
					}
				}

				if (static_cast<long>(decompLength) != files[fastFileHandle].pfe->realSize)
					result = 0;
				else
					result = decompLength;
			}
		}

		return result;
	}

	return FILE_NOT_OPEN;
}

//---------------------------------------------------------------------------
long FastFile::writeFast (const char* fastFileName, void* buffer, int nbytes)
{
	if(handle == nullptr)
		return FILE_NOT_OPEN;
	
	if (static_cast<DWORD>(numWrittenFiles) >= numFiles)
		return TOO_MANY_CHILDREN;

	const int fastFileHandle = numWrittenFiles;

	fseek(handle, 0, SEEK_END);
	const int file_pos = ftell(handle);

	files[fastFileHandle].inuse = true;
	files[fastFileHandle].pos = 0;
	files[fastFileHandle].pfe->offset = file_pos;

	strncpy(files[fastFileHandle].pfe->name, fastFileName, MAX_FILENAME_SIZE-1);
	files[fastFileHandle].pfe->name[MAX_FILENAME_SIZE-1] = '\0';
	files[fastFileHandle].pfe->hash = elfHash(files[fastFileHandle].pfe->name);
	files[fastFileHandle].pfe->realSize = nbytes;

	fseek(handle, file_pos, SEEK_SET);

	if(useLZCompress)
	{
		unsigned long workBufferSize = (nbytes << 1);
		workBufferSize = workBufferSize < 4096 ? 4096 : workBufferSize;

		if (!LZPacketBuffer)
		{
			LZPacketBuffer = static_cast<MemoryPtr>(malloc(LZPacketBufferSize));
			if (!LZPacketBuffer)
				return 0;
		}
				
		if (LZPacketBufferSize < workBufferSize)
		{
			LZPacketBufferSize = workBufferSize;
				
			free(LZPacketBuffer);
			LZPacketBuffer = static_cast<MemoryPtr>(malloc(LZPacketBufferSize));
			if (!LZPacketBuffer)
				return 0;
		}

		gosASSERT(LZPacketBuffer);

		size_t compressedSize = LZCompress(LZPacketBuffer, static_cast<Bytef*>(buffer), nbytes);
		size_t uncompressedSize = LZDecomp(static_cast<Bytef*>(buffer), LZPacketBuffer, compressedSize, nbytes);
		if (nbytes != uncompressedSize)
			STOP(("fast File size changed after compression.  Was %d is now %d", nbytes, uncompressedSize));

		files[fastFileHandle].pfe->size = static_cast<DWORD>(compressedSize);
	
		int result = static_cast<int>(fwrite(LZPacketBuffer, compressedSize, 1, handle));
		if(result != 1)
			return BAD_WRITE_ERR;

		if(0)
		{
			int file = creat("d:/ffile.bin", S_IWRITE);
			int result = write(file, LZPacketBuffer, static_cast<unsigned int>(compressedSize));
			_close(file);

			auto fbc = std::make_unique<char[]>(compressedSize);
			auto fb = std::make_unique<char[]>(nbytes);

			file = _open("d:/ffile.bin", _O_RDONLY | _O_BINARY);
			result = _read(file, fbc.get(), static_cast<unsigned int>(compressedSize));
			_close(file);

			gosASSERT(memcmp(LZPacketBuffer, fbc.get(), compressedSize) == 0);

			size_t uncompressedSize = LZDecomp(reinterpret_cast<Bytef*>(fb.get()), reinterpret_cast<Bytef*>(fbc.get()), compressedSize, nbytes);
			gosASSERT(nbytes == uncompressedSize);
		}
	}
	else
	{
		files[fastFileHandle].pfe->size = nbytes;
	
		int result = static_cast<int>(fwrite(buffer, nbytes, 1, handle));
		if(result != 1 && nbytes > 0) // to handle case when input file has zero length
			return BAD_WRITE_ERR;
	}

	long res = writeFileEntries(handle, std::span<FILE_HANDLE>(&files[fastFileHandle], 1), fastFileHandle);
	if(NO_ERR != res)
		return res;

	numWrittenFiles++;

	return NO_ERR;
}

//---------------------------------------------------------------------------
// This function pulls the raw compressed data out of the file and sticks it in the buffer
// passed in.  This way, we can load the textures directly from file to RAM and not
// have to decompress them!!
long FastFile::readFastRAW (DWORD fastFileHandle, void *bfr, DWORD size)
{
	size;

	long result = 0;

	if ((fastFileHandle >= 0) && (fastFileHandle < numFiles) && files[fastFileHandle].inuse)
	{
		logicalPosition = fseek(handle,files[fastFileHandle].pos + files[fastFileHandle].pfe->offset,SEEK_SET);

		if (size >= files[fastFileHandle].pfe->size)
		{
			result = static_cast<long>(fread(bfr,1,files[fastFileHandle].pfe->size,handle));
			logicalPosition += files[fastFileHandle].pfe->size;
		}

		if (result != files[fastFileHandle].pfe->size)
		{
			if (!Environment.checkCDForFiles)
				return 0;  // disk install; short read, fail quietly
			//READ Error.  Maybe the CD is missing?
			bool openFailed = false;
			bool alreadyFullScreen = (Environment.fullScreen != 0);
			while (result != files[fastFileHandle].pfe->size)
			{
				openFailed = true;
				if (!gos_NoUserPresentRequested()) EnterWindowMode();

				char data[2048];
				sprintf(data,FileMissingString,fileName.get(),CDMissingString);
				DWORD result1 = mc2MessageBox(data,MissingTitleString,MB_OKCANCEL | MB_ICONWARNING,IDCANCEL);
				if (result1 == IDCANCEL)
				{
					ExitGameOS();
					return (2);		//File not found.  Never returns though!
				}

				logicalPosition = fseek(handle,files[fastFileHandle].pos + files[fastFileHandle].pfe->offset,SEEK_SET);
				result = static_cast<long>(fread(bfr,1,files[fastFileHandle].pfe->size,handle));
				logicalPosition += files[fastFileHandle].pfe->size;
			}

			if (openFailed && (Environment.fullScreen == 0) && alreadyFullScreen)
				EnterFullScreenMode();
		}

		return result;
	}

	return FILE_NOT_OPEN;
}

//---------------------------------------------------------------------------
long FastFile::tellFast (DWORD fastFileHandle)
{
	if ((fastFileHandle >= 0) && (fastFileHandle < numFiles) && files[fastFileHandle].inuse)
		return files[fastFileHandle].pos;

	return -1;
}

//---------------------------------------------------------------------------
long FastFile::sizeFast (DWORD fastFileHandle)
{
	if ((fastFileHandle >= 0) && (fastFileHandle < numFiles) && files[fastFileHandle].inuse)
		return files[fastFileHandle].pfe->realSize;

	return -1;
}

//---------------------------------------------------------------------------
long FastFile::lzSizeFast (DWORD fastFileHandle)
{
	if ((fastFileHandle >= 0) && (fastFileHandle < numFiles) && files[fastFileHandle].inuse)
		return files[fastFileHandle].pfe->size;

	return -1;
}

//---------------------------------------------------------------------------
//
// Edit Log
//
//
//---------------------------------------------------------------------------
