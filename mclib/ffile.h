//---------------------------------------------------------------------------
//
// ffile.h - This file contains the class declaration for FastFile
//
// This class will catch a fileOpen and try to find the file in the FastFiles
// open before allowing the call to go through to the system
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef FFILE_H
#define FFILE_H
//---------------------------------------------------------------------------
// Include files

#ifndef DSTD_H
#include<dstd.h>
#include<memory>
#endif

#ifndef FILE_H
#include<file.h>
#endif

#include "mc2_czstring.h"
#include<sys/types.h>
#include<sys/stat.h>

#include<span>
//---------------------------------------------------------------------------
inline constexpr int MAX_FILENAME_SIZE = 250;

inline constexpr int FASTFILE_VERSION_BYTESIZE = 4; //sebi
inline constexpr unsigned int FASTFILE_VERSION = 0xCADDECAF;
inline constexpr unsigned int FASTFILE_VERSION_LZ = 0xFADDECAF;

inline constexpr int FASTFILE_ENTRY_TABLE_START = 8; //sebi


#pragma pack(1)
struct FILEENTRY {
    DWORD	offset;
    DWORD	size;						//LZ Compressed Size
	DWORD	realSize;					//Uncompressed Size
	DWORD	hash;						//Hash Compare to weed out stinky files faster then StrCmp
    char	name[MAX_FILENAME_SIZE];
};
#pragma pack()

struct FILE_HANDLE {
    DWORD       inuse;
    DWORD       pos;
    FILEENTRY	*pfe;
};

//---------------------------------------------------------------------------
// Class FastFile
class FastFile
{
	protected:
		DWORD		numFiles;
		FILE_HANDLE	*files;

		std::unique_ptr<char[]> fileName;
		FILE		*handle;

		DWORD 		length;
		DWORD 		logicalPosition;

		bool		useLZCompress;

		// used when creating fast file
		int						numWrittenFiles;

		long writeVersion(FILE* handle);
		long writeNumFiles(FILE* handle, int num_files);
		long writeFileEntries(FILE* handle, std::span<FILE_HANDLE> files, int offset);

	public:
		FastFile (void);
		~FastFile (void);


		long open (mc2::czstring fName);
		void close (void);

		bool isOpen (void)
		{
			return (handle != nullptr);
		}

		long fileSize (void)
		{
			if (isOpen() && (length == 0))
			{
				struct stat st;
				stat(fileName.get(),&st);
				length = st.st_size;
			}

			return length;			
		}

		long getNumFiles (void)
		{
			return numFiles;
		}
		const FILE_HANDLE* getFilesInfo() const { return files; }

		const char* getFileName() { return fileName.get(); }

		long openFast (DWORD hash, mc2::czstring fName);

		void closeFast (DWORD localHandle);

		long seekFast (DWORD fastFileHandle, DWORD off, DWORD from = SEEK_SET);
		long readFast (DWORD fastFileHandle, void *bfr, DWORD size);
		long readFastRAW (DWORD fastFileHandle, void *bfr, DWORD size);
		long tellFast (DWORD fastFileHandle);
		long sizeFast (DWORD fastFileHandle);
		long lzSizeFast (DWORD fastFileHandle);

		bool isLZCompressed (void)
		{
			return useLZCompress;
		}


		long create(mc2::czstring fName, bool compressed);
		long reserve(int num_files);
		long writeFast (mc2::czstring fastFileName, void* buffer, int nbytes);
};

//---------------------------------------------------------------------------
extern FastFile 	**fastFiles;
extern long 		numFastFiles;
extern long			maxFastFiles;
//---------------------------------------------------------------------------
#endif

