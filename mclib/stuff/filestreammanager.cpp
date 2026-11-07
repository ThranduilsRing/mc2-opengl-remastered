//===========================================================================//
// File:	filestrmmgr.cpp                                                  //
// Contents: Implementation Details of resource management                   //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"stuffheaders.hpp"
#include<toolos.hpp>

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MString*
	Stuff::StripExtension(MString* file_name)
{
	Check_Object(file_name);

	if(file_name->GetLength() == 0)
	{
		return file_name;
	}

	char *p = strrchr(*file_name, '.');
	if (p)
	{
		*p = '\0';
		file_name->SetLength(p - static_cast<char *>(*file_name));
	}

	Check_Object(file_name);
	return file_name;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MString*
	Stuff::IsolateDirectory(MString* file_name)
{
	Check_Object(file_name);

    PAUSE(("")); // sebi

	if(file_name->GetLength() == 0)
	{
		return file_name;
	}

	char *p = strrchr(*file_name, '\\');
	if (p)
	{
		*++p = '\0';
	}
	else
	{
		p = *file_name;
		*p = '\0';
	}
	file_name->SetLength(p - static_cast<char *>(*file_name));
	Check_Object(file_name);
	return file_name;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MString*
	Stuff::StripDirectory(MString* file_name)
{

    PAUSE(("")); // sebi

	Check_Object(file_name);
	char *p = strrchr(*file_name, '\\');
	if (p)
	{
		char *q = *file_name;
		do
		{
			*q++ = *++p;
		}
		while (*p);
		// The following does not handle all cases...
		// file_name->SetLength(p - (char *)*file_name);
		file_name->SetLength(strlen(*file_name));
	}
	Check_Object(file_name);
	return file_name;
}
