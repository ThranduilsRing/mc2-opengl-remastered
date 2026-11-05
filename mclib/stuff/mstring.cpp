//===========================================================================//
// File:        string.cpp                                                   //
// Contents:                                                                 //                                                                                                     //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"stuffheaders.hpp"

#include"platform_windows.h"
#include <ctype.h> // toupper
//#############################################################################
//#######################    MStringRepresentation    #########################
//#############################################################################

inline constexpr char DELETE_FILL_CHAR = ('0');

size_t
	MStringRepresentation::allocationIncrement = 8;

//
//#############################################################################
//#############################################################################
//
inline size_t
	MStringRepresentation::CalculateSize(size_t needed)
{
	//
	// calculate the allocation size for a string
	//
	Verify(allocationIncrement);
	size_t x =
		((needed + allocationIncrement) / allocationIncrement) *
			allocationIncrement;
	return x;
}

//
//#############################################################################
//#############################################################################
//
MStringRepresentation::MStringRepresentation()
{
	stringLength = 0;
	stringSize = 0;
	stringText = nullptr;
	referenceCount = 0;
}

//
//#############################################################################
//#############################################################################
//
MStringRepresentation::MStringRepresentation(const MStringRepresentation &str)
{
	Check_Object(&str);
	stringLength = str.stringLength;
	stringSize = str.stringSize;

	if (str.stringText.get() == nullptr)
	{
		stringText = nullptr;
	}
	else
	{
		stringText = std::make_unique_for_overwrite<char[]>(stringSize);
		Register_Pointer(stringText.get());
		Mem_Copy(stringText.get(), str.stringText.get(), stringLength + 1, stringSize);
	}
	referenceCount = 0;
}

//
//#############################################################################
//#############################################################################
//
MStringRepresentation::MStringRepresentation(mc2::czstring cstr)
{
	if ((cstr == nullptr) || (cstr[0] == '\x00'))
	{
		stringLength = 0;
		stringSize = 0;
		stringText = nullptr;
	}
	else
	{
		stringLength = static_cast<uint32_t>(strlen(cstr));
		stringSize = CalculateSize(stringLength);

		stringText = std::make_unique_for_overwrite<char[]>(stringSize);
		Register_Pointer(stringText.get());
		Mem_Copy(stringText.get(), cstr, stringLength + 1, stringSize);
	}
	referenceCount = 0;
}

//
//#############################################################################
//#############################################################################
//
MStringRepresentation::~MStringRepresentation()
{
	if (stringText.get() != nullptr)
	{
		#if defined(_ARMOR)
			memset(stringText.get(), DELETE_FILL_CHAR, stringSize);
		#endif
		Unregister_Pointer(stringText.get());
		stringText.reset();
	}
}

//
//#############################################################################
//#############################################################################
//
void
	MStringRepresentation::TestInstance() const
{
#if 0
	if (stringLength > 0 || stringText.get() != nullptr)
	{
		Check_Pointer(stringText.get());
		size_t str_len = strlen(stringText.get());
		Verify(stringLength == str_len);
	}
#endif
}

//
//#############################################################################
//#############################################################################
//
void
	MStringRepresentation::AllocateLength(size_t length)
{
	Check_Object(this);

	if (stringText.get() != nullptr)
	{
      #if defined(_ARMOR)
			memset(stringText.get(), DELETE_FILL_CHAR, stringSize);
		#endif
		Unregister_Pointer(stringText.get());
		stringText.reset();
	}

	if (!length)
	{
		stringLength = 0;
		stringSize = 0;
		stringText = nullptr;
	}
	else
	{
		stringLength = static_cast<uint32_t>(length);
		stringSize = CalculateSize(stringLength);
		stringText = std::make_unique_for_overwrite<char[]>(stringSize);
		Register_Pointer(stringText.get());
	}
}

//
//#############################################################################
//#############################################################################
//
MStringRepresentation
	MStringRepresentation::operator = (const MStringRepresentation &str)
{
	Check_Object(this);

	if (this == &str)
		return *this;

	if (stringText.get() != nullptr)
	{
      #if defined(_ARMOR)
			memset(stringText.get(), DELETE_FILL_CHAR, stringSize);
		#endif
		Unregister_Pointer(stringText.get());
		stringText.reset();
	}

   Check_Object(&str);
	stringLength = str.stringLength;
	stringSize = str.stringSize;

	if (stringSize == 0)
   {
		stringText = nullptr;
   }
	else
	{
		stringText = std::make_unique_for_overwrite<char[]>(stringSize);
		Register_Pointer(stringText.get());
		Mem_Copy(stringText.get(), str.stringText.get(), stringLength + 1, stringSize);
	}

	return *this;
}

//
//#############################################################################
//#############################################################################
//
MStringRepresentation
	MStringRepresentation::operator = (mc2::czstring cstr)
{
	Check_Object(this);

	if (stringText.get() != nullptr)
	{
      #if defined(_ARMOR)
			memset(stringText.get(), DELETE_FILL_CHAR, stringSize);
		#endif
		Unregister_Pointer(stringText.get());
		stringText.reset();
	}

	if ((cstr == nullptr) || (cstr[0] == '\x00'))
	{
		stringLength = 0;
		stringSize = 0;
		stringText = nullptr;
	}
	else
	{
		stringLength = static_cast<uint32_t>(strlen(cstr));
		stringSize = CalculateSize(stringLength);

		stringText = std::make_unique_for_overwrite<char[]>(stringSize);
		Register_Pointer(stringText.get());
		Mem_Copy(stringText.get(), cstr, stringLength + 1, stringSize);
	}

	return *this;
}

//
//#############################################################################
//#############################################################################
//
MStringRepresentation
	Stuff::operator + (
		const MStringRepresentation &str1,
		const MStringRepresentation &str2
	)
{
	Check_Object(&str1);
	Check_Object(&str2);

	MStringRepresentation temp;

	unsigned long totalLen =
		str1.stringLength + str2.stringLength;

	if (totalLen == 0)
   {
		return temp;
   }

	temp.stringLength = 0;
	temp.stringSize = MStringRepresentation::CalculateSize(static_cast<size_t>(totalLen));
	temp.stringText = std::make_unique_for_overwrite<char[]>(temp.stringSize);
	Register_Pointer(temp.stringText.get());

   Verify(temp.stringSize >= 1);
	temp.stringText[0] = '\000';

	if (str1.stringText.get() != nullptr)
	{
		Mem_Copy(temp.stringText.get(), str1.stringText.get(), str1.stringLength + 1, temp.stringSize);
		temp.stringLength = str1.stringLength;
	}

	if (str2.stringText.get() != nullptr)
	{
		Verify(temp.stringLength < temp.stringSize);
		Mem_Copy(
			&temp.stringText[temp.stringLength],
			str2.stringText.get(),
			str2.stringLength + 1,
			temp.stringSize - temp.stringLength
		);
		temp.stringLength += str2.stringLength;
	}

	Check_Object(&temp);
	return temp;
}

//
//#############################################################################
//#############################################################################
//
MStringRepresentation
	Stuff::operator + (const MStringRepresentation &str, char ch)
{
	Check_Object(&str);

	MStringRepresentation temp;

	if (str.stringText.get() == nullptr)
	{
		temp.stringLength = 1;
		temp.stringSize = MStringRepresentation::allocationIncrement;
		temp.stringText = std::make_unique_for_overwrite<char[]>(temp.stringSize);
		Register_Pointer(temp.stringText.get());

		Verify(temp.stringSize >= 2);
		temp.stringText[0] = ch;
		temp.stringText[1] = '\000';
	}
	else
	{
		Verify(str.stringLength != UINT_MAX);

		temp.stringLength = str.stringLength + 1;

		if (temp.stringLength == str.stringSize)
			temp.stringSize = str.stringSize + MStringRepresentation::allocationIncrement;
		else
			temp.stringSize = str.stringSize;

		temp.stringText = std::make_unique_for_overwrite<char[]>(temp.stringSize);
		Register_Pointer(temp.stringText.get());
		Mem_Copy(temp.stringText.get(), str.stringText.get(), str.stringLength, temp.stringSize);

		Verify(str.stringLength < temp.stringSize);
		Verify(temp.stringLength < temp.stringSize);
		temp.stringText[str.stringLength] = ch;
		temp.stringText[temp.stringLength] = '\000';
	}

	Check_Object(&temp);
	return temp;
}

//
//#############################################################################
//#############################################################################
//
int
	MStringRepresentation::Compare(const MStringRepresentation &str) const
{
	Check_Object(this);
	Check_Object(&str);

	// handle special cases where one string is empty
	if (stringText.get() == nullptr)
	{
		if (str.stringText.get() == nullptr)
			return 0;
		else
			return -1;
	}
	if (str.stringText.get() == nullptr)
   {
		return 1;
   }

	return strcmp(stringText.get(), str.stringText.get());
}

//
//#############################################################################
//#############################################################################
//
MStringRepresentation
	MStringRepresentation::GetNthToken(
		size_t nth_token,
		char *delimiters
	) const
{
	Check_Object(this);

	//
	// Which delimters to use
	//
	const char *delimter_string = " \t,";

	if (delimiters != nullptr)
	{
		delimter_string = delimiters;
	}
	Check_Pointer(delimter_string);

   //
	// Make temporary
   //
	MStringRepresentation temp(*this);

   if (temp.stringText.get() == nullptr)
		return temp;

   //
	// Parse string with strtok
   //
	size_t i;
	char *ptr;

	Check_Pointer(temp.stringText.get());
	ptr = strtok(temp.stringText.get(), delimter_string);
	for (i = 0; i < nth_token; i++)
	{
		if ((ptr = strtok(nullptr, delimter_string)) == nullptr)
			break;
	}

	if (ptr == nullptr)
	{
		MStringRepresentation null_return;
		return null_return;
	}
	MStringRepresentation token_return(ptr);
	return token_return;
}

//
//#############################################################################
//#############################################################################
//
void
	MStringRepresentation::ToUpper()
{
	Check_Object(this);
	if (stringText.get() != nullptr)
	{
		for (int i = 0; i < stringLength; i++)
		{
			stringText[i] = static_cast<char>(toupper(stringText[i]));
		}
	}
}

//
//#############################################################################
//#############################################################################
//
void MStringRepresentation::ToLower()
{
	Check_Object(this);
	if (stringText.get() != nullptr)
	{
		CharLower(stringText.get());
	}
}

//
//#############################################################################
//#############################################################################
//
MemoryStream&
	MemoryStreamIO::Read(
		MemoryStream *stream,
		MStringRepresentation *str
   )
{
	Check_Object(stream);
   Check_Object(str);

   // sebi
   //size_t string_length;
   uint32_t string_length;

	Read(stream, &string_length);
	if (string_length > 0)
	{
		auto ptrOwner = std::make_unique_for_overwrite<char[]>(string_length + 1);
		char *ptr = ptrOwner.get();
		Register_Pointer(ptr);
		stream->ReadBytes(ptr, string_length + 1);

		*str = ptr;

		Unregister_Pointer(ptr);
		ptrOwner.reset();
	}

	Check_Object(str);
	return *stream;
}

//
//#############################################################################
//#############################################################################
//
MemoryStream&
	MemoryStreamIO::Write(
		MemoryStream *stream,
		const MStringRepresentation &str
	)
{
	Check_Object(stream);
	Check_Object(&str);

	Write(stream, &str.stringLength);
	if (str.stringLength > 0)
	{
		stream->WriteBytes(str.stringText.get(), str.stringLength + 1);
	}
	return *stream;
}

//
//#############################################################################
//#############################################################################
//
void
	Stuff::Convert_From_Ascii(
		const char *str,
		MStringRepresentation *value
	)
{
	#if 0
		Check_Pointer(str);
		Check_Object(value);
		*value = str;
	#else
		if (str == nullptr)
		{
			STOP(("Convert_From_Ascii - str == NULL"));
		}
		if (value == nullptr)
		{
			STOP(("Convert_From_Ascii - value == NULL"));
		}
		*value = str;
	#endif
}

//#############################################################################
//##############################    MString    ################################
//#############################################################################

//
//#############################################################################
//#############################################################################
//
MString&
	MString::operator = (const MString &str)
{
	Check_Object(this);
	Check_Object(&str);

	if (this == &str)
		return *this;

	Check_Object(representation);
	representation->DecrementReferenceCount();
	representation = str.representation;
	representation->IncrementReferenceCount();
	return *this;
}

//
//#############################################################################
//#############################################################################
//
MString&
	MString::operator = (mc2::czstring cstr)
{
	Check_Object(representation);
	representation->DecrementReferenceCount();
	representation = new MStringRepresentation(cstr);
	Register_Object(representation);
	representation->IncrementReferenceCount();
	return *this;
}

//
//#############################################################################
//#############################################################################
//
void
	MString::ToUpper()
{
	MStringRepresentation *old = representation;
	Check_Object(old);

	representation = new MStringRepresentation(*old);
	Register_Object(representation);
	representation->IncrementReferenceCount();

	old->DecrementReferenceCount();

	representation->ToUpper();
}

//
//#############################################################################
//#############################################################################
//
void
	MString::ToLower()
{
	MStringRepresentation *old = representation;
	Check_Object(old);

	representation = new MStringRepresentation(*old);
	Register_Object(representation);
	representation->IncrementReferenceCount();

	old->DecrementReferenceCount();

	representation->ToLower();
}

//
//#############################################################################
//#############################################################################
//
MemoryStream&
	MemoryStreamIO::Read(
		MemoryStream *stream,
		MString *str
	)
{
	Check_Object(str);
	Check_Object(str->representation);
	str->representation->DecrementReferenceCount();
	str->representation = new MStringRepresentation;
	Register_Object(str->representation);
	str->representation->IncrementReferenceCount();
	Verify(str->representation->referenceCount == 1);
	return Read(stream, str->representation);
}

//
//#############################################################################
//#############################################################################
//
void
	Stuff::Convert_From_Ascii(
		const char *str,
		MString *value
	)
{
	Check_Object(value);
	Check_Object(value->representation);
	value->representation->DecrementReferenceCount();
	value->representation = new MStringRepresentation;
	Register_Object(value->representation);
	value->representation->IncrementReferenceCount();
	Verify(value->representation->referenceCount == 1);
	Convert_From_Ascii(str, value->representation);
}
//
//#############################################################################
//#############################################################################
//
bool
	Stuff::Close_Enough(
		const char *str1,
		const char *str2,
		Scalar e
	)
{
	Check_Pointer(str1);
	Check_Pointer(str2);

	return
		!S_stricmp(str1, str2);
}

		IteratorPosition
			GetHashFunctions::GetHashValue(const MString &value)
				{
					return value.GetHashValue();
				}
