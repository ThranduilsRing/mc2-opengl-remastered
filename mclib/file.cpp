//---------------------------------------------------------------------------
//
// file.cpp - This file contains the class functions for File
//
//				The File class simply calls the Windows file functions.
//				It is purely a wrapper.
//
//				The mmFile Class is a wrapper for the Win32 Memory Mapped
//				file functionality.  It is used exactly the same as above class.
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include files
#ifndef FILE_H
#include"file.h"
#endif
#include <algorithm>

#ifndef HEAP_H
#include"heap.h"
#endif

#ifndef FFILE_H
#include"ffile.h"
#endif

#ifndef PACKET_H
#include"packet.h"
#endif

#ifndef FASTFILE_H
#include"fastfile.h"
#endif

#ifndef UTILITIES_H
#include"utilities.h"
#endif

#include<string.h>
#include"platform_io.h"
#include<ctype.h>
#include<errno.h>
#include<cstdlib> // std::abs sebi

#include "platform_windows.h"

//#ifndef _MBCS
#include<gameos.hpp>
#include "mc2_dialog.h"
//#else
//#include<assert.h>
//#define gosASSERT assert
//#define gos_Malloc malloc
//#define gos_Free free
//#endif

#include "platform_str.h"
#include "fst_hash.h"   // fst_normalize_loose_key (mod-index key canonicalizer)
#include "mod_conflict.h"  // MOD-LOAD-ORDER-CONFLICT-REPORT-1: pure conflict classifier
#include <algorithm>

#include "file_mod.inc"

//-----------------
// Static Variables
unsigned long File::lastError = NO_ERR;

char CDInstallPath[1024];
void EnterWindowMode();
void EnterFullScreenMode();
void __stdcall ExitGameOS();

extern char FileMissingString[];
extern char CDMissingString[];
extern char MissingTitleString[];

#undef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE	-1
constexpr int CLOSED_HANDLE_VALUE = -2;
//---------------------------------------------------------------------------
// Global Functions
long fileExists (const char* fName, long destination_mask)
{
	// META-FIX design E (confirmed issue-#8-class bug): this probe passed the
	// RAW caller string to FastFileFind while File::open normalized first, so
	// "data\\missions\\x.fit" opened fine via FST but fileExists said 0 (the
	// loose _stat masked it only when a loose override existed). Normalize a
	// stack copy at entry so the probe and the opener agree on the canonical
	// key (lowercase, forward slash — mclib/fst_hash.cpp chokepoint).
	char normName[2048];
	if (!fName) return 0;
	strncpy(normName, fName, sizeof(normName) - 1);
	normName[sizeof(normName) - 1] = '\0';
	fst_normalize_loose_key_inplace(normName);

	// Active mod wins; falls back to base data, then FastFile.
	if (auto modHandle = TryModOpen(normName)) {
		_close(*modHandle);
		return 1 & destination_mask;
	}

	struct _stat st;
	if (_stat(normName,&st) != -1)
	{
		return 1 & destination_mask;
	}

	long fastFileHandle = -1;
	FastFilePtr	fastFile = FastFileFind(normName,fastFileHandle);
	if (fastFile)
		return 2 & destination_mask;

	// Full-ladder miss. fileExists is a probe API (the CD-presence check
	// calls it internally), so misses here are often benign — the nightly
	// check-file-resolve-miss.py baseline absorbs the known probes.
	++g_resolveMisses;
	if (logFileResolve >= 2) {
		EmitResolveRecord(normName, fName, "MISS", "", nullptr);
	}
	return 0;
}

//---------------------------------------------------------------------------
long fileExistsOnCD (const char* fName)
{
	//Just add the CD path here and see if its there.
	char bigPath[2048];
	strcpy(bigPath,CDInstallPath);
	strcat(bigPath,fName);

	struct _stat st;
	if (_stat(bigPath,&st) != -1)
	{
		return 1;
	}

	return 0;
}

//---------------------------------------------------------------------------
bool file1OlderThan2 (const char* file1, const char* file2)
{
	if ((fileExists(file1) == 1) && (fileExists(file2) == 1))
	{
		struct _stat st1, st2;
		_stat(file1,&st1);
		_stat(file2,&st2);
		
		if (st1.st_mtime < st2.st_mtime)
			return true;
	}

	return false;
}

//---------------------------------------------------------------------------
//	class File member functions


//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
File::File (void)
{
	fileMode = NOMODE;
	handle = INVALID_HANDLE_VALUE;
	fastFileHandle = -1;
	length = 0;
	logicalPosition = 0;
	bufferResult = 0;

	parent = nullptr;
	parentOffset = 0;
	physicalLength = 0;

	inRAM = FALSE;
	fileImage = nullptr;

	fastFile = nullptr;
}
			
//---------------------------------------------------------------------------
inline void File::setup (void)
{
	logicalPosition = 0;
	
	//----------------------------------------------------------------------
	//This is only called from an open with a filename, not a file pointer.
	// ie. It assumes we are the parent.
	if (isOpen())
		length = fileSize();
	else
		length = 0;

	parent = nullptr;
	parentOffset = 0;
	physicalLength = length;
	
	m_children.clear();
}

//---------------------------------------------------------------------------
File::~File (void)
{
	close();
}

//---------------------------------------------------------------------------
bool File::eof (void)
{
	return (logicalPosition >= getLength());
}

//---------------------------------------------------------------------------
long File::open (const char* fName, FileMode _mode, long numChild, bool doNotLower)
{
	gosASSERT( !isOpen() );
	//-------------------------------------------------------------
	m_fileName = fName;
	fileMode = _mode;
	//_fmode = _O_BINARY;

	// META-FIX design E: canonical-key chokepoint (mclib/fst_hash.cpp).
	// Game path (doNotLower=false): case+slash fold — behaviorally identical
	// to the historical S_strlwr + backslash loop, now funneled through the
	// unit-tested normalizer. doNotLower callers (sole caller
	// data_tools/pak.cpp, which opens the .fst archive itself) keep on-disk
	// case and get the slash-only fold (fst_normalize_key, in-place safe).
	if (!doNotLower)
		fst_normalize_loose_key_inplace(m_fileName.data());
	else
		fst_normalize_key(m_fileName.data(), m_fileName.data());

	if (fileMode == CREATE)
	{
		// sebi: changed _creat to this, because otherwise non binary file is created which is wrong
		handle = _open(m_fileName.c_str(), _O_CREAT | _O_TRUNC | _O_BINARY | _O_RDWR, _S_IREAD | _S_IWRITE);
		if (handle == INVALID_HANDLE_VALUE)
		{
			lastError = errno;
			return lastError;
		}
	}
	else
	{
		// sebi: add this check because we do not use fileMode and for some reason assume that it is read only
		gosASSERT(fileMode == READ);
		//----------------------------------------------------------------
		//-- Active mod wins; falls back to base data/.
		{
			// Pass original fName as req so level-2 trace shows pre-lowercase casing.
			if (auto modHandle = TryModOpen(m_fileName.c_str(), fName))
			{
				handle = *modHandle;
				++g_resolveOpens;
				if (mc2::gate::get<"MC2_LOG_MECH_ICON">() && strstr(m_fileName.c_str(), "mechicon"))
					printf("[MECHICON] File::open MOD-HIT: %s\n", m_fileName.c_str());
			}
			else
			{
				handle = _open(m_fileName.c_str(), _O_RDONLY | _O_BINARY);
				if (handle != INVALID_HANDLE_VALUE)
					++g_resolveOpens;
				if (mc2::gate::get<"MC2_LOG_MECH_ICON">() && strstr(m_fileName.c_str(), "mechicon") && handle != INVALID_HANDLE_VALUE)
					printf("[MECHICON] File::open LOOSE: %s\n", m_fileName.c_str());
				if (handle != INVALID_HANDLE_VALUE && logFileResolve >= 2) {
					std::string key = NormalizeKey(m_fileName.c_str());
					EmitResolveRecord(key.c_str(), fName, "base-loose", m_fileName.c_str(), nullptr);
				}
			}
		}

		// Try stripping a numeric size subdir (e.g. data/tgl/128/foo.tga -> data/tgl/foo.tga).
		// Upscaled loose overrides live in the parent dir without the size component.
		if (handle == INVALID_HANDLE_VALUE) {
			for (const char* p = m_fileName.c_str(); *p; ++p) {
				if (*p != '/') continue;
				const char* digits = p + 1;
				const char* q = digits;
				while (*q >= '0' && *q <= '9') ++q;
				if (q > digits && *q == '/') {
					char stripped[2048];
					const size_t prefixLen = static_cast<size_t>((digits - m_fileName.c_str()));
					const char* suffix = q + 1;
					if (prefixLen + strlen(suffix) < sizeof(stripped)) {
						memcpy(stripped, m_fileName.c_str(), prefixLen);
						strcpy(stripped + prefixLen, suffix);
						handle = _open(stripped, _O_RDONLY | _O_BINARY);
						if (handle != INVALID_HANDLE_VALUE)
							++g_resolveOpens;
						if (handle != INVALID_HANDLE_VALUE && logFileResolve >= 2) {
							std::string key = NormalizeKey(m_fileName.c_str());
							EmitResolveRecord(key.c_str(), fName, "base-strip", stripped, nullptr);
						}
					}
					break;
				}
			}
		}

		//------------------------------------------
		//-- Next, see if file is in fastFile.
		if (handle == INVALID_HANDLE_VALUE)
		{
			lastError = errno;

			fastFile = FastFileFind(m_fileName.c_str(),fastFileHandle);
			if (fastFile)
				++g_resolveOpens;
			if (mc2::gate::get<"MC2_LOG_MECH_ICON">() && fastFile && strstr(m_fileName.c_str(), "mechicon"))
				printf("[MECHICON] File::open FST-HIT: %s\n", m_fileName.c_str());
			if (fastFile && logFileResolve >= 2) {
				std::string key = NormalizeKey(m_fileName.c_str());
				EmitResolveRecord(key.c_str(), fName, "fastfile", "", nullptr);
			}
			if (!fastFile)
			{
                if(!Environment.checkCDForFiles) {
                    ++g_resolveMisses;
                    if (logFileResolve >= 2) {
                        std::string key = NormalizeKey(m_fileName.c_str());
                        EmitResolveRecord(key.c_str(), fName, "MISS", "", nullptr);
                    }
                    return 2;
                }

				//Not in main installed directory and not in fastfile.  Look on CD.

				char actualPath[2048];
				strcpy(actualPath,CDInstallPath);
				strcat(actualPath,m_fileName.c_str());
				handle = _open(actualPath,_O_RDONLY);
				if (handle == INVALID_HANDLE_VALUE)
				{
					bool openFailed = false;
					bool alreadyFullScreen = (Environment.fullScreen != 0);
					while (handle == INVALID_HANDLE_VALUE)
					{
						openFailed = true;

						//OK, check to see if the CD is actually present.
						// Do this by checking for tgl.fst on the CD Path.
						// If its there, the CD is present BUT the file is missing.
						// MANY files in MechCommander 2 are LEGALLY missing!
						// Tell it to the art staff.
						char testCDPath[2048];
						strcpy(testCDPath,CDInstallPath);
						strcat(testCDPath,"tgl.fst");

						DWORD findCD = fileExists(testCDPath);
						if (findCD == 1)	//File exists. CD is in drive.  Return 2 to indicate file not found.
						{
							++g_resolveMisses;
							if (logFileResolve >= 2) {
								std::string key = NormalizeKey(m_fileName.c_str());
								EmitResolveRecord(key.c_str(), fName, "MISS", "", nullptr);
							}
							return 2;
						}

						if (!gos_NoUserPresentRequested()) EnterWindowMode();
		
						char data[2048];
						sprintf(data,FileMissingString,m_fileName.c_str(),CDMissingString);
						DWORD result1 = mc2MessageBox(data,MissingTitleString,MB_OKCANCEL | MB_ICONWARNING,IDCANCEL);
						if (result1 == IDCANCEL)
						{
							ExitGameOS();
							return (2);		//File not found.  Never returns though!
						}
		
						handle = _open(actualPath,_O_RDONLY);
					}
		
					if (openFailed && (Environment.fullScreen == 0) && alreadyFullScreen)
						EnterFullScreenMode();
				}
				else
				{
					++g_resolveOpens;
					if (logFileResolve >= 2 && handle != INVALID_HANDLE_VALUE) {
						std::string key = NormalizeKey(m_fileName.c_str());
						EmitResolveRecord(key.c_str(), fName, "cd", actualPath, nullptr);
					}
					setup();

					//------------------------------------------------------------
					// NEW FUNCTIONALITY!!!
					// 
					// Each file may have a number of files open as children which
					// use the parent's handle for reads and writes.  This would
					// allow us to open a packet file and read a packet as a fitIni
					// or allow us to write a packet as a fit ini and so forth.
					//
					// It also allows us to use the packet file extensions as tree
					// files to avoid the ten thousand file syndrome.
					//
					// There is now an open which takes a FilePtr and a size.
					m_children.assign(numChild > 0 ? static_cast<size_t>(numChild) : 0, nullptr);

					return (NO_ERR);
				}
			}

			//---------------------------------------------------------------------
			//-- FastFiles are all compressed.  Must read in entire chunk into RAM
			//-- Then close fastfile!!!!!
			inRAM = TRUE;

			// A corrupt archive entry can claim any size; an image that big would end the process in std::bad_alloc where the old malloc
			// returned null. Refuse it with one log line and leave the file closed: the archive entry is released here because
			// close() sees no handle and no image and would skip it.
			if (!imageSizePlausible(fileSize()))
			{
				MC2_LOG_ONCE(Io, Error, "[FILE_IMAGE_SIZE_BAD] file={} size={} (an archive entry claims more than {} bytes; not opened)",
					m_fileName.c_str(), fileSize(), static_cast<long>(kMaxInRamImage));
				fastFile->closeFast(fastFileHandle);
				fastFile = nullptr;
				fastFileHandle = -1;
				m_fileName.clear();
				length = 0;
				inRAM = FALSE;
				return FILE_NOT_OPEN;
			}

			// Never empty, so a zero-length member still has an image to read from (what malloc(0) gave).
			m_ownedImage.assign(std::max<size_t>(fileSize(), 1), 0);
			fileImage = m_ownedImage.data();
			if (fileImage)
			{
				fastFile->readFast(fastFileHandle,fileImage,fileSize());

				physicalLength = getLength();
				//------------------------------------
				//-- Image is in RAM.  Shut the file.
				//fastFile->closeFast(fastFileHandle);
				//fastFile = NULL;
				//fastFileHandle = -1;

				logicalPosition = 0;
			}

			return NO_ERR;
		}
		else
		{
			setup();
	
			//------------------------------------------------------------
			// NEW FUNCTIONALITY!!!
			// 
			// Each file may have a number of files open as children which
			// use the parent's handle for reads and writes.  This would
			// allow us to open a packet file and read a packet as a fitIni
			// or allow us to write a packet as a fit ini and so forth.
			//
			// It also allows us to use the packet file extensions as tree
			// files to avoid the ten thousand file syndrome.
			//
			// There is now an open which takes a FilePtr and a size.
			m_children.assign(numChild > 0 ? static_cast<size_t>(numChild) : 0, nullptr);
	
			return (NO_ERR);
		}
	}
	
	return(NO_ERR);
}
		
//---------------------------------------------------------------------------
long File::open (FilePtr _parent, unsigned long fileSize, long numChild)
{
	if (_parent && (_parent->fastFile == nullptr))
	{
		if (numChild == -1 && fileSize > _parent->getLength())
		{
			// A RAM child is a copy of a slice of its parent: a size past the parent's end is a corrupt packet header, and sizing the image
			// from it would end the process in std::bad_alloc.
			MC2_LOG_ONCE(Io, Error, "[FILE_IMAGE_SIZE_BAD] file={} size={} parent={} (a child claims more bytes than its parent holds; not opened)",
				_parent->m_fileName.c_str(), static_cast<long>(fileSize), static_cast<long>(_parent->getLength()));
			return PACKET_WRONG_SIZE;
		}

		parent = _parent;
		if (parent->getFileMode() != READ)
		{
			return(CANT_WRITE_TO_CHILD);
		}
		
		physicalLength = fileSize;
		parentOffset = parent->getLogicalPosition();
		logicalPosition = 0;

		//-------------------------------------------------------------
		{
			const char* parentName = parent->getFilename();
			m_fileName = parentName ? parentName : "";
		}
		fileMode = parent->getFileMode();
		
		handle = parent->getFileHandle();
		
		long result = parent->addChild(this);
		if (result != NO_ERR)
			return(result);

		//------------------------------------------------------------
		// NEW FUNCTIONALITY!!!
		// 
		// Each file may have a number of files open as children which
		// use the parent's handle for reads and writes.  This would
		// allow us to open a packet file and read a packet as a fitIni
		// or allow us to write a packet as a fit ini and so forth.
		//
		// It also allows us to use the packet file extensions as tree
		// files to avoid the ten thousand file syndrome.
		//
		// There is now an open which takes a FilePtr and a size.
		// 
		// IF a numChild parameter is passed in as -1, we want this file in RAM!!
		// This means NO CHILDREN!!!!!!!!!!!!!
		if (numChild != -1)
		{
			m_children.assign(numChild > 0 ? static_cast<size_t>(numChild) : 0, nullptr);
		}
		else
		{
			m_children.clear();
			inRAM = TRUE;
			unsigned long result = 0;

			// Never empty (see the FastFile case above).
			m_ownedImage.assign(std::max<size_t>(fileSize, 1), 0);
			fileImage = m_ownedImage.data();

			if (_parent->getFileClass() == PACKETFILE)
			{
				result = (static_cast<PacketFilePtr>(_parent))->readPacket((static_cast<PacketFilePtr>(_parent))->getCurrentPacket(),fileImage);
			}
			else
			{
				result = _read(handle,fileImage,fileSize);
				if (result != fileSize)
					lastError = errno;
			}
		}
	}
	else
	{
		return(PARENT_NULL);
	}
	
	return(NO_ERR);
}

long File::open(const char* buffer, int bufferLength )
{
	if ( buffer && bufferLength > 0 )
	{	
		m_ownedImage.clear();
		fileImage = reinterpret_cast<unsigned char*>(const_cast<char*>(buffer));
		physicalLength = bufferLength;
		logicalPosition = 0;
		fileMode = RDWRITE;
		inRAM = true;
	}
	else// fail on NULL
	{
		return FILE_NOT_OPEN;
	}

	return NO_ERR;


}

//---------------------------------------------------------------------------
long File::create (const char* fName)
{
	return (open(fName,CREATE));
}

long File::createWithCase(const char* fName )
{
	gosASSERT( !isOpen() );
	//-------------------------------------------------------------
	m_fileName = fName;
	fileMode = CREATE;
	//_fmode = _O_BINARY;

	handle = _creat(m_fileName.c_str(),_S_IWRITE);
	if (handle == INVALID_HANDLE_VALUE)
	{
		lastError = errno;
		return lastError;
	}

	return 0;
}
//---------------------------------------------------------------------------
long File::addChild (FilePtr child)
{
	for (FilePtr& slot : m_children)
	{
		if (slot == nullptr)
		{
			slot = child;
			return NO_ERR;
		}
	}

	return(TOO_MANY_CHILDREN);
}

//---------------------------------------------------------------------------
void File::removeChild (FilePtr child)
{
	for (FilePtr& slot : m_children)
	{
		if (slot == child)
		{
			slot = nullptr;
			break;
		}
	}
}

//---------------------------------------------------------------------------
void File::close (void)
{
	//------------------------------------------------------------------------
	// First, close us if we are the parent.  Otherwise, just NULL the handle
	// DO NOT CALL CLOSE IF WE ARE A CHILD!!
	//
	// Every file holds its own copy of its name, so closing the parent leaves a child's name valid until that child closes.

	m_fileName.clear();
	length = 0;

	if (isOpen())
	{
		if ((parent == nullptr) && (handle != CLOSED_HANDLE_VALUE) && (-1 != handle))
			_close(handle);
			
		handle = CLOSED_HANDLE_VALUE;

		if (fastFile)
		{
   			fastFile->closeFast(fastFileHandle);
		}

		fastFile = nullptr;			//DO NOT DELETE THE FASTFILE!!!!!!!!!!!!!
		fastFileHandle = -1;
	}
	
	//---------------------------------------------------------------------
	// Check if we have any children and close them.  This will set their
	// handle to NULL and their filename to NULL.  It will also close any
	// of THEIR children.
	// Index loop: a child's close() clears its own slot through removeChild(), never resizing the list.
	for (size_t i = 0; i < m_children.size(); i++)
	{
		if (m_children[i])
			m_children[i]->close();
	}
	m_children.clear();
	
	// Forget the parent once the slot is cleared: a second close() of this child (its destructor, after an explicit close) must not reach into a
	// parent that has been destroyed in the meantime.
	if (parent != nullptr)
	{
		parent->removeChild(this);
		parent = nullptr;
	}

	// Only an image this file loaded itself goes away; a caller's buffer (open(buffer, length)) stays attached, as it always did.
	if (!m_ownedImage.empty())
	{
		m_ownedImage.clear();
		m_ownedImage.shrink_to_fit();
		fileImage = nullptr;
		inRAM = FALSE;
	}
}

//---------------------------------------------------------------------------
void File::deleteFile (void)
{
	//--------------------------------------------------------------
	// Must be the ultimate parent to delete this file.  Close will
	// make sure all of the children close themselves.
	if (isOpen() && (parent == nullptr))
		close();
}

long newPosition = 0;
//---------------------------------------------------------------------------
long File::seek (long pos, long from)
{
	switch (from)
	{
		case SEEK_SET:
			if (pos > (long)getLength())
			{
				return READ_PAST_EOF_ERR;
			}
			break;

		case SEEK_END:
			if ((std::abs(pos) > (long)getLength()) || (pos > 0))
			{
				return READ_PAST_EOF_ERR;
			}
			break;

		case SEEK_CUR:
			if (pos+logicalPosition > getLength())
			{
				return READ_PAST_EOF_ERR;
			}
			break;
	}

	if (inRAM && fileImage)
	{
		if (parent)
		{
			switch (from)
			{
				case SEEK_SET:
					newPosition = pos;
					break;

				case SEEK_END:
					newPosition = getLength()+parentOffset;
					newPosition += pos;
					break;

				case SEEK_CUR:
					newPosition += pos;
					break;
			}
		}
		else
		{
			switch (from)
			{
				case SEEK_SET:
					newPosition = pos;
					break;

				case SEEK_END:
					newPosition = getLength() + pos;
					break;

				case SEEK_CUR:
					newPosition += pos;
					break;
			}
		}

		if (newPosition == -1)
		{
			return (INVALID_SEEK_ERR);
		}

		logicalPosition = newPosition;

	}
	else if (fastFile)
	{
		newPosition = fastFile->seekFast(fastFileHandle,pos,from);
		logicalPosition = newPosition;
	}
	else
	{
		if (parent)
		{
			switch (from)
			{
				case SEEK_SET:
					_lseek(handle,pos+parentOffset,SEEK_SET);
					newPosition = pos;
					break;

				case SEEK_END:
					_lseek(handle,getLength()+parentOffset,SEEK_SET);
					_lseek(handle,pos,SEEK_CUR);
					newPosition = getLength() + pos;
					break;

				case SEEK_CUR:
					_lseek(handle,pos,SEEK_CUR);
					newPosition = logicalPosition + pos;
					break;
			}
		}
		else
		{
			newPosition = _lseek(handle,pos,from);
		}

		if (newPosition == -1)
		{
			return (INVALID_SEEK_ERR);
		}

		logicalPosition = newPosition;
	}

	return (NO_ERR);
}

//---------------------------------------------------------------------------
long File::read (unsigned long pos, MemoryPtr buffer, long length)
{
	long result = 0;

	if (inRAM && fileImage)
	{
		char *readAddress = (reinterpret_cast<char *>(fileImage))+pos;
		memcpy(reinterpret_cast<char *>(buffer),readAddress,length);
		return(length);
	}
	else if (fastFile)
	{
		if (logicalPosition != pos)
			fastFile->seekFast(fastFileHandle,pos);

		result = fastFile->readFast(fastFileHandle,buffer,length);
	}
	else
	{
		if (isOpen())
		{
			if (logicalPosition != pos)
				seek(pos);

			result = _read(handle,buffer,length);
			if (result != length)
				lastError = errno;
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}
		
	return(result);
}

//---------------------------------------------------------------------------
unsigned char File::readByte (void)
{
	unsigned char value = 0;
	long result = 0;

	if (inRAM && fileImage)
	{
		char *readAddress = reinterpret_cast<char*>(fileImage)+logicalPosition;
		memcpy(reinterpret_cast<char *>(&value),readAddress,sizeof(value));
		logicalPosition += sizeof(value);
	}
	else if (fastFile)
	{
		result = fastFile->readFast(fastFileHandle,reinterpret_cast<char *>(&value),sizeof(value));
		logicalPosition += sizeof(value);
	}
	else
	{
		if (isOpen())
		{
			result = _read(handle,(&value),sizeof(value));
			logicalPosition += sizeof(value);
			
			if (result != sizeof(value))
				lastError = errno;
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}

	return value;
}

//---------------------------------------------------------------------------
short File::readWord (void)
{
	short value = 0;
	long result =0;

	if (inRAM && fileImage)
	{
		char *readAddress = reinterpret_cast<char*>(fileImage)+logicalPosition;
		memcpy(reinterpret_cast<char *>((&value)),readAddress,sizeof(value));
		logicalPosition += sizeof(value);
	}
	else if (fastFile)
	{
		result = fastFile->readFast(fastFileHandle,reinterpret_cast<char *>(&value),sizeof(value));
		logicalPosition += sizeof(value);
	}
	else
	{
		if (isOpen())
		{
			result = _read(handle,(&value),sizeof(value));
			logicalPosition += sizeof(value);
			
			if (result != sizeof(value))
				lastError = errno;
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}

	return value;
}

//---------------------------------------------------------------------------
short File::readShort (void)
{
	return (readWord());
}

//---------------------------------------------------------------------------
int File::readInt(void)
{
	int value = 0;
	int result = 0;

	if (inRAM && fileImage)	
	{
		char *readAddress = reinterpret_cast<char*>(fileImage)+logicalPosition;
		memcpy(reinterpret_cast<char *>((&value)),readAddress,sizeof(value));
		logicalPosition += sizeof(value);
	}
	else if (fastFile)
	{
		result = fastFile->readFast(fastFileHandle,reinterpret_cast<char *>(&value),sizeof(value));
		logicalPosition += sizeof(value);
	}
	else
	{
		if (isOpen())
		{
			result = _read(handle,static_cast<void*>((&value)),sizeof(value));
			logicalPosition += sizeof(value);

			if (result != sizeof(value))
				lastError = errno;
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}
	
	return value;
}

//---------------------------------------------------------------------------
long File::readLong (void)
{
    //gosASSERT(0 && "readLong: Most probably this function should not be called!!!");
    return readInt();
}

bool isNAN(float *pFloat)
{
	/* We're assuming ansi/ieee 754 floating point representation. See http://www.research.microsoft.com/~hollasch/cgindex/coding/ieeefloat.html. */
	BYTE *byteArray = reinterpret_cast<BYTE *>(pFloat);
	if ((0x7f == (0x7f & byteArray[3])) && (0x80 == (0x80 & byteArray[2]))) {
		if (0x80 == (0x80 & byteArray[3])) {
			/* if the mantissa is a 1 followed by all zeros in this case then it is technically
			"Indeterminate" rather than an NaN, but we'll just count it as a NaN here. */
			return true;
		} else {
			return true;
		}
	}
	return false;
}

float File::readFloat( void )
{
	float value = 0;
	unsigned long result = 0;

	if (inRAM && fileImage)	
	{
		char *readAddress = reinterpret_cast<char*>(fileImage)+logicalPosition;
		memcpy(reinterpret_cast<char *>((&value)),readAddress,sizeof(value));
		logicalPosition += sizeof(value);
	}
	else if (fastFile)
	{
		result = fastFile->readFast(fastFileHandle,reinterpret_cast<char *>(&value),sizeof(value));
		logicalPosition += sizeof(value);
	}
	else
	{
		if (isOpen())
		{
			result = _read(handle,(&value),sizeof(value));
			logicalPosition += sizeof(value);

			if (result != sizeof(value))
				lastError = errno;
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}

	if (isNAN(&value)) {
		gosASSERT(false);
		value = 1.0/*arbitrary value that seems safe*/;
	}
	return value;
}

//---------------------------------------------------------------------------
long File::readString (MemoryPtr buffer)
{
	long last = 0;

	if (isOpen())
	{
		for(;;)
		{
			byte ch = readByte();

			buffer[last] = ch;

			if (ch)
				++last;
			else
				break;
		}
	}
	else
	{
		lastError = FILE_NOT_OPEN;
	}

	return last;
}

//---------------------------------------------------------------------------
long File::read (MemoryPtr buffer, long length)
{
	long result = 0;
	
	if (inRAM && fileImage)
	{
		char *readAddress = reinterpret_cast<char *>(fileImage)+logicalPosition;
		memcpy(reinterpret_cast<char *>(buffer),readAddress,length);
		logicalPosition += length;
		return(length);
	}
	else if (fastFile)
	{
		result = fastFile->readFast(fastFileHandle,buffer,length);
		logicalPosition += result;
	}
	else
	{
		if (isOpen())
		{
			result = _read(handle,buffer,length);
			if (result != length)
				lastError = errno;
			else
				logicalPosition += result;
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}
	
	return result;
}

//---------------------------------------------------------------------------
long File::readRAW (DWORD* &buffer)
{
	long result = 0;
	
	if (fastFile && fastFile->isLZCompressed())
	{
		long lzSizeNeeded = fastFile->lzSizeFast(fastFileHandle);
        // sebi
		//buffer = (unsigned long *)std::malloc(lzSizeNeeded);
		buffer = static_cast<DWORD*>(std::malloc(lzSizeNeeded));

		result = fastFile->readFastRAW(fastFileHandle,buffer,lzSizeNeeded);
		logicalPosition += result;
	}
	
	return result;
}

//---------------------------------------------------------------------------
long File::readLine (MemoryPtr buffer, long maxLength)
{
	long i = 0;
	
	if (inRAM && fileImage)
	{
		if (isOpen())
		{
			unsigned char *readAddress = reinterpret_cast<unsigned char *>(fileImage)+logicalPosition;

            //sebi support linux created files
			while ((i<maxLength) && ((i+logicalPosition) < fileSize()) && readAddress[i]!='\r' && readAddress[i]!='\n' )
				i++;

			memcpy( buffer, readAddress, i );

			buffer[i++]=0;

			logicalPosition+=i;

			if ( logicalPosition > fileSize() )
				return READ_PAST_EOF_ERR;

			if( readAddress[i]=='\n' )
				logicalPosition+=1;
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}
	else if (fastFile)
	{
		long bytesread;
		bytesread = fastFile->readFast(fastFileHandle,buffer,maxLength);

		if (maxLength > bytesread)
			maxLength = bytesread;

        //sebi support linux created files
		while ((i<maxLength) && (buffer[i]!='\r') && (buffer[i]!='\n'))
			i++;

        int skipChar = 0;
        // skip next \n;
        if(i<maxLength && buffer[i]=='\r')
            skipChar = 1;

		buffer[i++]=0;
		logicalPosition += i;

		//if( buffer[i]=='\n' )
		//	logicalPosition+=1;
        logicalPosition+=skipChar;

		fastFile->seekFast(fastFileHandle,logicalPosition);
	}
	else
	{
		if (isOpen())
		{
			long bytesread;
			bytesread = _read(handle,buffer,maxLength);
			if( maxLength > bytesread )
				maxLength=bytesread;

            //sebi support linux created files
			while( i<maxLength && buffer[i]!='\r' && buffer[i]!='\n')
				i++;

#if 1
            int skipChar = 0;
            // skip next \n;
            if(i<maxLength && buffer[i]=='\r')
                skipChar = 1;
#endif

			buffer[i++]=0;

			logicalPosition+=i;

			//if( buffer[i]=='\n' )
			//	logicalPosition+=1;
#if 1
			logicalPosition+=skipChar;
#endif


			seek(logicalPosition);
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}
	return i;
}

//---------------------------------------------------------------------------
long File::readLineEx (MemoryPtr buffer, long maxLength)
{
	long i = 0;
	
	if (inRAM && fileImage)
	{
		if (isOpen())
		{
			unsigned char *readAddress = reinterpret_cast<unsigned char *>(fileImage)+logicalPosition;

			while( i<maxLength && readAddress[i]!='\n' )
				i++;

			i++;									//Include Newline
			memcpy( buffer, readAddress, i );

			buffer[i++]=0;

			logicalPosition+=(i-1);
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}
	else if (fastFile)
	{
		long bytesread;
		bytesread = fastFile->readFast(fastFileHandle,buffer,maxLength);

		if (maxLength > bytesread)
			maxLength = bytesread;

		while ((i<maxLength) && (buffer[i]!='\n'))
			i++;

		i++;					//Include Newline
		buffer[i++]=0;
		logicalPosition += (i-1);

		fastFile->seekFast(fastFileHandle,logicalPosition);
	}
	else
	{
		if (isOpen())
		{
			long bytesread = _read(handle,buffer,maxLength);
			if( maxLength > bytesread )
				maxLength=bytesread;

			while( i<maxLength && buffer[i]!='\n' )
				i++;

			i++;
			buffer[i++]=0;

			logicalPosition+= (i-1);

			seek(logicalPosition);
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}
	return i;
}

//---------------------------------------------------------------------------
long File::write (unsigned long pos, MemoryPtr buffer, long bytes)
{
	unsigned long result = 0;

	if (parent == nullptr)	
	{
		if (isOpen())
		{
			if (logicalPosition != pos)
				seek(pos);

			if ( inRAM )
			{
				if ( logicalPosition + bytes > physicalLength )
					return BAD_WRITE_ERR;
				memcpy( fileImage + logicalPosition, buffer, bytes );
				result = bytes;

			}
			else
			{
				result = _write(handle,buffer,bytes);
				if (result != length)
					lastError = errno;
			}
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}
	else
	{
		lastError = CANT_WRITE_TO_CHILD;
	}
	
	return(result);
}

//---------------------------------------------------------------------------
long File::writeByte (byte value)
{
	long result = 0;

	if (parent == nullptr)
	{
		if (isOpen())	
		{
			if ( inRAM )
			{
				if ( logicalPosition + sizeof(byte) > physicalLength )
					return BAD_WRITE_ERR;
				memcpy( fileImage + logicalPosition, &value, sizeof( byte ) );
				result = sizeof( byte );				
			}
			else
				result = _write(handle,(&value),sizeof(value));
			if (result == sizeof(value))
			{
				logicalPosition += sizeof(value);
				result = NO_ERR;
			}
			else
			{
				result = BAD_WRITE_ERR;
			}
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}
	else
	{
		lastError = CANT_WRITE_TO_CHILD;
	}
	
	return(result);
}

//---------------------------------------------------------------------------
long File::writeWord (short value)
{
	unsigned long result = 0;
	
	if (parent == nullptr)
	{
		if (isOpen())
		{
			if ( inRAM )
			{
				if ( logicalPosition + sizeof( short ) > physicalLength )
					return BAD_WRITE_ERR;
				memcpy( fileImage + logicalPosition, &value, sizeof( short ) );
				result = sizeof( value );				
			}
			else
				result = _write(handle,(&value),sizeof(value));

			if (result == sizeof(value))
			{
				logicalPosition += sizeof(value);
				result = NO_ERR;
			}
			else
			{
				result = BAD_WRITE_ERR;
			}
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}
	else
	{
		lastError = CANT_WRITE_TO_CHILD;
	}

	return(result);
}

//---------------------------------------------------------------------------
long File::writeShort (short value)
{
	long result = writeWord(value);
	return(result);
}

//---------------------------------------------------------------------------
long File::writeLong (long value)
{
    gosASSERT(0 && "writeLong: Most probably this function should not be called!!!");

	unsigned long result = 0;
	
	if (parent == nullptr)
	{
		if (isOpen())
		{
			if ( inRAM )
			{
				if ( logicalPosition + sizeof( value ) > physicalLength )
					return BAD_WRITE_ERR;
				memcpy( fileImage + logicalPosition, &value, sizeof( value ) );
				result = sizeof( value );				
			}
			else
				result = _write(handle,(&value),sizeof(value));

			if (result == sizeof(value))
			{
				logicalPosition += sizeof(value);
				result = NO_ERR;	
			}
			else
			{
				result = BAD_WRITE_ERR;
			}
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}
	else
	{
		lastError = CANT_WRITE_TO_CHILD;
	}

	return(result);
}

long File::writeInt (int value)
{
	unsigned long result = 0;
	
	if (parent == nullptr)
	{
		if (isOpen())
		{
			if ( inRAM )
			{
				if ( logicalPosition + sizeof( value ) > physicalLength )
					return BAD_WRITE_ERR;
				memcpy( fileImage + logicalPosition, &value, sizeof( value ) );
				result = sizeof( value );				
			}
			else
				result = _write(handle,(&value),sizeof(value));

			if (result == sizeof(value))
			{
				logicalPosition += sizeof(value);
				result = NO_ERR;	
			}
			else
			{
				result = BAD_WRITE_ERR;
			}
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}
	else
	{
		lastError = CANT_WRITE_TO_CHILD;
	}

	return(result);
}

//---------------------------------------------------------------------------
long File::writeFloat (float value)
{
	unsigned long result = 0;

	gosASSERT(!isNAN(&value));
	if (parent == nullptr)
	{
		if (isOpen())
		{
			if ( inRAM )
			{
				if ( logicalPosition + sizeof( value ) > physicalLength )
					return BAD_WRITE_ERR;
				memcpy( fileImage + logicalPosition, &value, sizeof( value ) );
				result = sizeof( value );				
			}
			else
				result = _write(handle,(&value),sizeof(float));

			if (result == sizeof(float))
			{
				logicalPosition += sizeof(float);
				result = NO_ERR;	
			}
			else
			{
				result = BAD_WRITE_ERR;
			}
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}
	else
	{
		lastError = CANT_WRITE_TO_CHILD;
	}

	return(result);
}

//---------------------------------------------------------------------------

long File::writeString (const char *buffer)
{
	long result = -1;
	
	if (parent == nullptr)
	{
		if (isOpen())
		{
			const char *ch = buffer;

			for(; *ch; ++ch)
				writeByte((byte)* ch);
			
			return ch - buffer;
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}
	else
	{
		lastError = CANT_WRITE_TO_CHILD;
	}
	
	return(result);
}

//---------------------------------------------------------------------------
long File::writeLine (char *buffer)
{
	long result = -1;
	
	if (parent == nullptr)
	{
		if (isOpen())
		{
			char *ch = buffer;

			for(; *ch; ++ch)
				writeByte((byte)* ch);

			writeByte('\r');
			writeByte('\n');
			
			return ch - buffer;
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}
	else
	{
		lastError = CANT_WRITE_TO_CHILD;
	}
	
	return(result);
}

//---------------------------------------------------------------------------
long File::write(MemoryPtr buffer, size_t bytes)
{
	long result = 0;
	
	if (parent == nullptr)
	{
		if (isOpen())
		{
			if ( inRAM )
			{
				if ( logicalPosition + bytes > physicalLength )
					return BAD_WRITE_ERR;
				memcpy( fileImage + logicalPosition, buffer, bytes );
				result = static_cast<long>(bytes);
			}
			else
			{
				result = _write(handle,buffer,static_cast<unsigned int>(bytes));
				if (result != bytes)
				{
					lastError = errno;
					return result;
				}
			}

			logicalPosition += result;
		}
		else
		{
			lastError = FILE_NOT_OPEN;
		}
	}
	else
	{
		lastError = CANT_WRITE_TO_CHILD;
	}
	
	return result;
}

//---------------------------------------------------------------------------
bool File::isOpen (void)
{
	return ((handle != CLOSED_HANDLE_VALUE && handle != -1) || (fileImage != nullptr));
}

//---------------------------------------------------------------------------
const char* File::getFilename (void) const
{
	return m_fileName.empty() ? nullptr : m_fileName.c_str();
}

//---------------------------------------------------------------------------
time_t File::getFileMTime (void)
{
	time_t mTime;

	if (isOpen())
	{
		struct _stat st;
		_fstat(handle,&st);
		mTime = st.st_mtime;

		//Time\Date Stamp is WAY out of line.
		// Return January 1, 1970
		if (mTime == -1)
			mTime = 0;
	}

	return mTime;
}

//---------------------------------------------------------------------------
unsigned long File::getLength (void)
{
	if (fastFile && (length == 0))
	{
		length = fastFile->sizeFast(fastFileHandle);
	}
	else if ((length == 0) && (parent || inRAM))
	{
		length = physicalLength;
	}
	else if (isOpen() && ((length == 0) || ((fileMode > READ) && !inRAM)))
	{
		/* _fstat() was being used to get the length of the file, but it was wrong. It was
		   not giving the *logical* size, which is what we want. */
		length = _filelength(handle);
	}

	return length;
}

//---------------------------------------------------------------------------
unsigned long File::fileSize (void)
{
	return getLength();
}

//---------------------------------------------------------------------------
unsigned long File::getNumLines (void)
{
	unsigned long currentPos = logicalPosition;
	unsigned long numLines = 0;

	seek(0);
	for (unsigned long i=0;i<getLength();i++)
	{
		unsigned char check1 = readByte();
		if (check1 == '\n')
			numLines++;
	}	
	
	seek(currentPos);

	return numLines;
}

//---------------------------------------------------------------------------
void File::seekEnd (void)
{
	seek(0,SEEK_END);
}

//---------------------------------------------------------------------------
void File::skip (long bytesToSkip)
{
	if (bytesToSkip)
	{
		seek(logicalPosition+bytesToSkip);
	}
}
//---------------------------------------------------------------------------

