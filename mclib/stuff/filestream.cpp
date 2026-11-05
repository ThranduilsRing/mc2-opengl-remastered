//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include <memory>
#include"stuffheaders.hpp"

#if !defined(TOOLOS_HPP)
#include<toolos.hpp>
#endif

//#############################################################################
//#############################    Directory    ###############################
//#############################################################################

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
Directory::Directory(
	char *find_files,
	bool directories
):
	fileEntries(nullptr,false),
	folderEntries(nullptr,false)
{
	Check_Pointer(find_files);

	//
	//------------------------------------------------
	// Isolate the search path that we are looking for
	//------------------------------------------------
	//
	MString new_directory_string = find_files;

	//
	//---------------------------------------------------------
	// Now look for the files and add each entry into the chain
	//---------------------------------------------------------
	//
	const char* file_name = gos_FindFiles(new_directory_string);
	while (file_name)
	{
		MString file_name_string = file_name;
		DirectoryEntry *entry = new DirectoryEntry(file_name_string);
		Check_Object(entry);
		fileEntries.AddValue(entry,file_name_string);
		file_name = gos_FindFilesNext();
	}
	gos_FindFilesClose();
	fileWalker = std::make_unique<SortedChainIteratorOf<DirectoryEntry*,MString>>(&fileEntries);
	Check_Object(fileWalker.get());

	//
	//---------------------------------------------------
	// Look through the directories if we are supposed to
	//---------------------------------------------------
	//
	folderWalker = nullptr;
	if (directories)
	{
		file_name = gos_FindDirectories(new_directory_string);
		while (file_name)
		{
			MString file_name_string = file_name;
			DirectoryFolder *entry = new DirectoryFolder(file_name_string);
			Check_Object(entry);
			folderEntries.AddValue(entry,file_name_string);
			file_name = gos_FindDirectoriesNext();
		}
		gos_FindDirectoriesClose();
		folderWalker = std::make_unique<SortedChainIteratorOf<DirectoryFolder*,MString>>(&folderEntries);
		Check_Object(folderWalker.get());
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
Directory::~Directory(void)
{
	Check_Object(this);
	Check_Object(fileWalker.get());
	fileWalker->DeletePlugs();
	Check_Object(fileWalker.get());
	fileWalker.reset();
	if (folderWalker != nullptr)
	{
		Check_Object(folderWalker.get());
		folderWalker->DeletePlugs();
		Check_Object(folderWalker.get());
		folderWalker.reset();
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
char*
	Directory::GetCurrentFileName(void)
{
	Check_Object(this);
	Check_Object(fileWalker.get());

	DirectoryEntry *entry;
	
	if ((entry = fileWalker->GetCurrent()) != nullptr)
	{
		Check_Object(entry);
		return entry->GetItem();
	}
	return nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	Directory::AdvanceCurrentFile(void)
{
	Check_Object(this);
	Check_Object(fileWalker.get());
	fileWalker->Next();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
char * Directory::GetCurrentFolderName( void )
{
	Check_Object(this);
	if (folderWalker == nullptr)
		STOP(("Directory class was instantiated without directory support:\n           Set the <directories> parameter to true to enable."));
	Check_Object(folderWalker.get());

	DirectoryFolder *entry;
	
	if ((entry = folderWalker->GetCurrent()) != nullptr)
	{
		Check_Object(entry);
		return entry->GetItem();
	}
	return nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void Directory::AdvanceCurrentFolder( void )
{
	Check_Object(this);
	if (folderWalker == nullptr)
		STOP(("Directory class was instantiated without directory support:\n           Set the <directories> parameter to true to enable."));
	Check_Object(folderWalker.get());
	folderWalker->Next();
}

//#############################################################################
//###########################    FileStream    ################################
//#############################################################################

FileStream::ClassData*
	FileStream::DefaultData = nullptr;

const char *
	FileStream::WhiteSpace = " \t\n";

char
	FileStream::RedirectedName[256];
bool
	FileStream::IsRedirected=false,
	FileStream::IgnoreReadOnlyFlag=false;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	FileStream::InitializeClass()
{
	Verify(!DefaultData);
	DefaultData =
		new ClassData(
			FileStreamClassID,
			"Stuff::FileStream",
			MemoryStream::DefaultData
		);
	Check_Object(DefaultData);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	FileStream::TerminateClass()
{
	Check_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	FileStream::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
FileStream::FileStream():
	MemoryStream(DefaultData, nullptr, 0)
{
	fileName = nullptr;
	isOpen = false;
	fileHandle = nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
FileStream::FileStream(
	const char* file_name,
	WriteStatus writable
):
	MemoryStream(DefaultData, nullptr, 0)
{
	fileName = nullptr;
	isOpen = false;
	Open(file_name, writable);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
FileStream::~FileStream()
{
	Close();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	FileStream::Open(
		const char* file_name,
		WriteStatus writable
	)
{
	Check_Object(this);
	Check_Pointer(file_name);

	writeEnabled = writable;
	currentPosition = streamStart = nullptr;
	streamSize = 0;
	fileHandle = nullptr;

	//
	//------------------------------------------------
	// If this is a readonly file, have gos read it in
	//------------------------------------------------
	//
	if (writeEnabled == ReadOnly)
	{
		if (IsRedirected)
		{
			void (__stdcall *old_hook)(const char*, BYTE**, DWORD*) = Environment.HookGetFile;
			Environment.HookGetFile = nullptr;
			gos_GetFile(RedirectedName, &streamStart, &streamSize);
			Environment.HookGetFile = old_hook;
			fileName = _strdup(RedirectedName);
		}
		else
		{
			gos_GetFile(file_name, &streamStart, &streamSize);
			if (IsRedirected)
				fileName = _strdup(RedirectedName);
			else
				fileName = _strdup(file_name);
		}
		isOpen = true;
		currentPosition = streamStart;
	}

	//
	//-----------------------------------------------
	// Write only flags go through standard file open
	//-----------------------------------------------
	//
	else
	{
		writeEnabled = writable;
		if (IgnoreReadOnlyFlag)
		{
			auto old_hook = Environment.HookDoesFileExist;
			Environment.HookDoesFileExist = nullptr;
			if (gos_DoesFileExist(file_name))
				gos_FileSetReadWrite(file_name);
			Environment.HookDoesFileExist = old_hook;
		}
		gos_OpenFile(
			&fileHandle,
			file_name,
			READWRITE
		);
		isOpen = true;
		streamSize = 0xFFFFFFFFU;
		fileName = _strdup(file_name);
	}

	//
	//--------------------------
	// Try and open the filename
	//--------------------------
	//
	IsRedirected = false;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	FileStream::Close()
{
	Check_Object(this);

	//
	//---------------------------------
	// If the file was opened, close it
	//---------------------------------
	//
	if (fileHandle)
		gos_CloseFile(fileHandle);
	else if (streamStart != nullptr)
		gos_Free(streamStart);

	//
	//----------------
	// Delete the name
	//----------------
	//
	isOpen = false;
	if (fileName)
	{
		Check_Pointer(fileName);
		free(fileName);
		fileName = nullptr;
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	FileStream::SetPointer(DWORD index)
{
	Check_Object(this);
	Verify(IsFileOpened());
	Verify(writeEnabled == ReadOnly);
	MemoryStream::SetPointer(index);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MemoryStream&
	FileStream::AdvancePointer(DWORD index)
{
	Check_Object(this);
	Verify(IsFileOpened());
	Verify(writeEnabled == ReadOnly);
	return MemoryStream::AdvancePointer(index);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MemoryStream&
	FileStream::RewindPointer(DWORD index)
{
	Check_Object(this);
	Verify(IsFileOpened());
	Verify(writeEnabled == ReadOnly);
	return MemoryStream::RewindPointer(index);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MemoryStream&
	FileStream::ReadBytes(
		void *ptr,
		DWORD number_of_bytes
	)
{
	Check_Object(this);
	Verify(IsFileOpened());
	Verify(writeEnabled == ReadOnly);
	return MemoryStream::ReadBytes(ptr, number_of_bytes);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MemoryStream&
	FileStream::WriteBytes(
		const void *ptr,
		DWORD number_of_bytes
	)
{
	Check_Object(this);
	Verify(IsFileOpened());
	Verify(writeEnabled == WriteOnly);
	DWORD written =
		gos_WriteFile(
			fileHandle,
			Cast_Pointer(BYTE*, const_cast<void*>(ptr)),
			number_of_bytes
		);
	Verify(written == number_of_bytes);
	currentPosition += written;
	return *this;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
bool
	FileStream::IsFileOpened()
{
	Check_Object(this);
	return isOpen;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
bool
	Stuff::CreateDirectories(mc2::czstring directory_path)
{

    PAUSE(("")); // sebi
	
	if (directory_path == nullptr)
		return false;

	Check_Pointer(directory_path);


	const char *start_position = directory_path;
	const char *current_position = directory_path;

	if (*current_position != '\\')
	{
	
		if ((current_position + 1) != nullptr)
		{
			if((current_position + 2) != nullptr)
			{
				if ( *(current_position + 1) == ':' && *(current_position + 2) == '\\')
				{
					current_position += 2;
				}
			}
		}
	}

	if (*current_position != '\\')
		return false;


	while((current_position != nullptr))
	{
		//make a substring with the path...
		const char *next_slash;
		next_slash = strchr( current_position + 1, '\\' );

		std::unique_ptr<char[]> newStringOwner;
		char *new_string = nullptr;

		if (next_slash == nullptr)
		{
			//copy the whole string
			size_t length = strlen(start_position)+1;
			newStringOwner = std::make_unique_for_overwrite<char[]>(length);
			new_string = newStringOwner.get();
			Check_Pointer(new_string);

			Str_Copy(new_string, start_position, length);
		}
		else
		{

			if (next_slash - current_position == 0)
			{
				return false;
			}
			if (next_slash - current_position == 1)
			{
				return false;
			}

			//copy the sub string
			size_t length = next_slash - start_position;
			newStringOwner = std::make_unique_for_overwrite<char[]>(length+1);
			new_string = newStringOwner.get();
			Check_Pointer(new_string);

			strncpy(new_string, start_position, length);

			new_string[length] = '\0';

		}

		Verify(new_string != nullptr);

		
		if (!gos_DoesFileExist(new_string))
		{
			gos_CreateDirectory(new_string);
		}



		Check_Pointer(new_string);
		newStringOwner.reset();

		current_position = next_slash;
	}




	return true;
	
}
