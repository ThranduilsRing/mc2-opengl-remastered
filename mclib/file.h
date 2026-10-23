//---------------------------------------------------------------------------
//
// file.h - This file contains the class declaration for File
//
//				The File class simply calls the Windows 32bit file functions.
//				It is purely a wrapper.
//
//				The mmFile Class is a wrapper for the Win32 Memory Mapped
//				file functionality.  It is used exactly the same as above class.
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef FILE_H
#define FILE_H
//---------------------------------------------------------------------------
// Include files

#ifndef DSTD_H
#include"dstd.h"
#endif

#ifndef DFILE_H
#include"dfile.h"
#endif

#ifndef DFFILE_H
#include"dffile.h"
#endif

#ifndef HEAP_H
#include"heap.h"
#endif

#include<stdlib.h>
#include<stdio.h>
#include<fcntl.h>
#include <memory>
#include <string>
#include <optional>
#include <string_view>
#include <vector>

//---------------------------------------------------------------------------
// Enums
enum class FileMode
{
	NOMODE = 0,
	READ,
	CREATE,
	MC2_APPEND,
	WRITE,
	RDWRITE
};
using enum FileMode;

enum class FileClass
{
	BASEFILE = 0,
	INIFILE,
	PACKETFILE,
	CSVFILE
};
using enum FileClass;

//---------------------------------------------------------------------------
//Returns 1 if file is on HardDrive and 2 if file is in FastFile
inline constexpr int FILE_NOT_FOUND = 0;
inline constexpr int FILE_ON_DISK = 1;
inline constexpr int FILE_ON_FST = 2;

// Function Declarations
long fileExists(const char *fName, long destination_mask = FILE_ON_DISK|FILE_ON_FST);
long fileExistsOnCD(const char *fName);
bool file1OlderThan2(const char *file1, const char* file2);
void InitModSearchPaths(const char* modsRoot);
// No-ops — mod is selected at launcher time. Kept for call-site compat.
void ActivateCampaignMod(const char* campaignFitName);
void ActivateModForMission(const char* missionFitKey);
void DeactivateMod();
// Diagnostic: returns owning mod id for a data path, or nullptr if not in mod index.
const char* LookupModOwner(const char* dataPath);
// Winning loose provider for metadata/editing. Base fallback is not editable
// while a mod is active; dependency files are never the active editable override.
struct ModFileSource {
    std::optional<std::string> path;
    bool editable = false;
};
ModFileSource ResolveModFileSource(std::string_view dataPath);
// Lower values win; follows the same active/dependency order as File::open.
int LookupModPriority(const char* dataPath);
// Absolute disk path of the mounted mod file that serves dataPath, or nullptr when no mod owns it.
// For readers that bypass File::open (Assimp, sidecar fopen); see the P2 boundary note in mclib/msl_load.cpp.
const char* LookupModAbsolutePath(std::string_view dataPath);
// Package capabilities: mod.json "capabilities" of the mounted mods (active + dependencies).
bool ModHasCapability(std::string_view modId, std::string_view capability);
bool ModCapabilityMounted(std::string_view capability);
bool ModOwnerHasCapability(std::string_view dataPath, std::string_view capability);
// Archive metadata for a loose override; the normal File::open precedence stays authoritative.
std::vector<unsigned char> ReadArchiveBytes(const char* dataPath, size_t maxBytes);
// Enumerate data/campaign/*.fit files from the active mod (for campaign list UI).
using ModCampaignCallback = void(*)(const char* fitFileName, const char* fullPath, void* userData);
void EnumerateModCampaignFiles(ModCampaignCallback cb, void* userData);
// Enumerate mod-indexed .fit files whose key starts with keyPrefix (e.g. "data/defs/text/en_us/").
// Appends absolute paths to out, sorted by filename. Used by MC2Strings to pick up mod text FITs.
using ModFitCallback = void(*)(const char* absPath, void* userData);
void EnumerateModFitFiles(const char* keyPrefix, ModFitCallback cb, void* userData);
// Winning VFS keys, including non-FIT icon images; File::open remains authoritative.
void EnumerateModFiles(std::string_view keyPrefix, ModFitCallback cb, void* userData);

//---------------------------------------------------------------------------
// Macro Definitions
#ifndef	NO_ERR
#define	NO_ERR					0x00000000
#endif

inline constexpr unsigned int DISK_FULL_ERR = 0xBADF0001;
inline constexpr unsigned int SHARE_ERR = 0xBADF0002;
inline constexpr unsigned int FILE_NOT_FOUND_ERR = 0xBADF0003;
inline constexpr unsigned int PACKET_OUT_OF_RANGE = 0xBADF0004;
inline constexpr unsigned int PACKET_WRONG_SIZE = 0xBADF0005;
inline constexpr unsigned int READ_ONLY_ERR = 0xBADF0006;
inline constexpr unsigned int TOO_MANY_FILES_ERR = 0xBADF0007;
inline constexpr unsigned int READ_PAST_EOF_ERR = 0xBADF0008;
inline constexpr unsigned int INVALID_SEEK_ERR = 0xBADF0009;
inline constexpr unsigned int BAD_WRITE_ERR = 0xBADF000A;
inline constexpr unsigned int BAD_PACKET_VERSION = 0xBADF000B;
inline constexpr unsigned int NO_RAM_FOR_SEEK_TABLE = 0xBADF000C;
inline constexpr unsigned int NO_RAM_FOR_FILENAME = 0xBADF000D;
inline constexpr unsigned int PARENT_NULL = 0xBADF000E;
inline constexpr unsigned int TOO_MANY_CHILDREN = 0xBADF000F;
inline constexpr unsigned int FILE_NOT_OPEN = 0xBADF0010;
inline constexpr unsigned int CANT_WRITE_TO_CHILD = 0xBADF0011;
inline constexpr unsigned int NO_RAM_FOR_CHILD_LIST = 0xBADF0012;
inline constexpr unsigned int MAPPED_WRITE_NOT_SUPPORTED = 0xBADF0013;
inline constexpr unsigned int COULD_NOT_MAP_FILE = 0xBADF0014;
inline constexpr unsigned int FILE_ALREADY_OPEN = 0xBADF0015; //sebi

//---------------------------------------------------------------------------
//									File
//---------------------------------------------------------------------------
class File
{
	// Data Members
	//--------------
	protected:
	
		// The name this file was opened with (lower-cased and slash-folded by open). Empty while closed. A child holds its OWN copy of its
		// parent's name: the parent closing first must not leave a child with a name that no longer exists.
		std::string				m_fileName;
		FileMode				fileMode;
	
		int                     handle;

		FastFilePtr				fastFile;
		long					fastFileHandle;

		unsigned long 			length;
		unsigned long 			logicalPosition;

		unsigned long			bufferResult;

		// The files opened on this one's handle (open(parent, size)). NON-owning: each child closes itself and clears its slot, and the parent's
		// close() closes the children still open, in slot order. The parent must outlive any read made through a child (a child is a view of its
		// parent's handle and RAM image; it keeps its own copy of the name only). One slot per allowed child (open's numChildren), nullptr = free; empty = none allowed.
		std::vector<FilePtr>	m_children;
				
		FilePtr					parent;
		unsigned long			parentOffset;
		unsigned long			physicalLength;

		bool					inRAM;
		// The bytes a RAM file reads and writes. It points into m_ownedImage when this file loaded the bytes itself (a FastFile member, or a child
		// read into RAM) and at the caller's buffer after open(buffer, length); closing never frees a caller's buffer.
		MemoryPtr				fileImage;
		std::vector<unsigned char>	m_ownedImage;

	public:

		static unsigned long	lastError;

	// Member Functions
	//------------------
		protected:

			void setup (void);

		public:

			
			File (void);
			~File (void);

			// A copy would close the same descriptor twice and alias the in-RAM image.
			File (const File&) = delete;
			File& operator= (const File&) = delete;

			bool eof (void);

			virtual long open ( const char* fName, FileMode _mode = READ, long numChildren = 50, bool doNotLower = false);
			virtual long open( const char* buffer, int bufferLength ); // for streaming from memory

			virtual long create (const char* fName);
			virtual long createWithCase(const char* fName ); // don't strlwr for me please!

			virtual void close (void);

			virtual long open (File *_parent, unsigned long fileSize, long numChildren = 50);
			
			bool isLoadedFromDisk(void) const { return (handle != -1) && (fastFile == nullptr); }

			void deleteFile (void);

			long seek (long pos, long from = SEEK_SET);
			void seekEnd (void);
			void skip (long bytesToSkip);

			long read (unsigned long pos, MemoryPtr buffer, long length);
			long read (MemoryPtr buffer, long length);

			//Used to dig the LZ data directly out of the fastfiles.
			// For textures.
			//long readRAW (unsigned long * &buffer);
			long readRAW (DWORD* &buffer);

			unsigned char readByte (void);
			short readWord (void);
			short readShort (void);
			long readLong (void);
			float readFloat( void );
			int readInt( void );

			long readString (MemoryPtr buffer);
			long readLine (MemoryPtr buffer, long maxLength);
			long readLineEx (MemoryPtr buffer, long maxLength);

			long write (unsigned long pos, MemoryPtr buffer, long bytes);
			long write (MemoryPtr buffer, size_t bytes);

			long writeByte (unsigned char value);
			long writeWord (short value);
			long writeShort (short value);
			long writeInt(int value);
			long writeLong (long value);
			long writeFloat (float value);

			long writeString (const char *buffer);
			long writeLine (char *buffer);

			bool isOpen (void);

			virtual FileClass getFileClass (void)
			{
				return BASEFILE;
			}

			// nullptr while the file is closed. Otherwise a pointer into this object's name: valid until its next open or close, so copy it at once.
			const char* getFilename (void) const;

			unsigned long getLength (void);
			unsigned long fileSize (void);
			unsigned long getNumLines (void);

			unsigned long getLastError (void) {
				return(lastError);
			}

			unsigned long getLogicalPosition (void)
			{
				return logicalPosition;
			}
			
			FilePtr getParent (void)
			{
				return parent;
			}

			FileMode getFileMode (void)
			{
				return(fileMode);
			}
			
			int getFileHandle (void)
			{
				return(handle);
			}

			time_t getFileMTime (void);
			
			long addChild (FilePtr child);
			void removeChild (FilePtr child);

};

//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
#endif

//---------------------------------------------------------------------------
//
// Edit Log
//
//
//---------------------------------------------------------------------------
