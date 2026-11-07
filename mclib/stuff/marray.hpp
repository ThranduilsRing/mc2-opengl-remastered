//===========================================================================//
// File:	MArray.hpp                                                       //
// Contents: 																 //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once

#include"stuff.hpp"
#include <memory>
#include <vector>

namespace Stuff {

	//##########################################################################
	//#########################    DynamicArrayOf    ###########################
	//##########################################################################
	// Kept only for GOSVertexPool (B7): a move-only, resize-only array over unique_ptr<T[]>.

	template <class T> class DynamicArrayOf
	{
	public:
		DynamicArrayOf() = default;

		T*
			GetData()
				{return data.get();}
		size_t
			GetLength() const
				{return length;}

		void
			SetLength(size_t length_to_set)
		{
			if (length_to_set == length)
				return;
			std::unique_ptr<T[]> new_data;
			if (length_to_set > 0)
			{
				new_data = std::make_unique_for_overwrite<T[]>(length_to_set);
				for (size_t i = std::min(length_to_set, length); i--; )
					new_data[i] = data[i];
			}
			data = std::move(new_data);
			length = length_to_set;
		}

	private:
		std::unique_ptr<T[]>
			data;
		size_t
			length = 0;
	};

	// std::vector stream payload: a uint32 element count, then count*sizeof(T) raw element bytes (native endian).
	// Elements are read and written as raw bytes; Vector2DOf is not trivially copyable, so there is no
	// static_assert here.
	template <class T> MemoryStream&
		MemoryStreamIO_Write(
			MemoryStream *stream,
			const std::vector<T> *array
		)
	{
		Check_Object(stream);
		Check_Pointer(array);
		uint32_t length = static_cast<uint32_t>(array->size());
		MemoryStreamIO::Write(stream, &length);
		if (length > 0)
			stream->WriteBytes(array->data(), length * sizeof(T));
		return *stream;
	}

	template <class T> MemoryStream&
		MemoryStreamIO_Read(
			MemoryStream *stream,
			std::vector<T> *array
		)
	{
		Check_Object(stream);
		Check_Pointer(array);
		uint32_t length;
		MemoryStreamIO::Read(stream, &length);
		array->resize(length);
		if (length > 0)
			stream->ReadBytes(array->data(), length * sizeof(T));
		return *stream;
	}

}
