//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include <memory>
#include"stuffheaders.hpp"

#if !defined(TOOLOS_HPP)
#include<toolos.hpp>
#endif

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
