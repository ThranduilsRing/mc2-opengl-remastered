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

	template <class T> class DynamicArrayOf;

	template <class T> MemoryStream&
		MemoryStreamIO_Write(
			MemoryStream *stream,
			const DynamicArrayOf<T> *array
		);

	template <class T> MemoryStream&
		MemoryStreamIO_Read(
			MemoryStream *stream,
			DynamicArrayOf<T> *array
		);
	
	//##########################################################################
	//#########################    DynamicArrayOf    ###########################
	//##########################################################################

	template <class T> class DynamicArrayOf
		#if defined(_ARMOR)
			: public Stuff::Signature
		#endif
	{
	public:
		DynamicArrayOf();
		DynamicArrayOf(const DynamicArrayOf<T>&);
		DynamicArrayOf(size_t length);
		DynamicArrayOf(const T &value, size_t length);
		DynamicArrayOf(const T *data, size_t length);
		~DynamicArrayOf();

		void
			TestInstance() const
				{}

		void
			TestInstance()
				{}

		DynamicArrayOf<T>&
			operator=(const DynamicArrayOf<T>&);
		T&
			operator[](size_t i);
		const T&
			operator[](size_t i) const;

		void
			AssignValue(const T &value, size_t length);
		void
			AssignData(const T *data, size_t length);

		size_t 
			GetLength() const
				{return length;}
		void
			SetLength(size_t length);
		size_t 
			GetSize() const
				{return length * sizeof(T);}
		T*
			GetData()
				{return data.get();}
		const T*
			GetData() const
				{return data.get();}

#if 0
		bool
			Compare(const DynamicArrayOf<T>&);
#endif

		friend MemoryStream&
			MemoryStreamIO_Write<T>(
				MemoryStream *stream,
				const DynamicArrayOf<T> *array
			);

		friend MemoryStream&
			MemoryStreamIO_Read<T>(
				MemoryStream *stream,
				DynamicArrayOf<T> *array
			);

	private:
		void
			SetStorageLength(size_t length);
		void
			CopyArray(const DynamicArrayOf<T>&);

		std::unique_ptr<T[]>
			data;
		size_t
			length;
	};

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	template<class T>
		DynamicArrayOf<T>::DynamicArrayOf() 
	{ 
		data = nullptr;
		length = 0;
	}

	template<class T>
		DynamicArrayOf<T>::DynamicArrayOf(const DynamicArrayOf<T> &array) 
	{
		data = nullptr;
		length = 0;
		CopyArray(array);
	}

	template<class T>
		DynamicArrayOf<T>::DynamicArrayOf(size_t length_to_set) 
	{
		data = nullptr;
		length = 0;
		SetStorageLength(length_to_set);
		Verify(length == length_to_set);
		#if defined(_ARMOR)
			if (length_to_set > 0)
				Check_Pointer(data.get());
			else
				Verify(!data);
		#endif
	}

	template<class T>
		DynamicArrayOf<T>::DynamicArrayOf(const T &value, size_t length_to_set) 
	{
		data = nullptr;
		length = 0;
		AssignValue(value, length_to_set);
	}

	template<class T>
		DynamicArrayOf<T>::DynamicArrayOf(const T *data_to_copy, size_t length_to_set) 
	{
		data = nullptr;
		length = 0;
		AssignData(data_to_copy, length_to_set);
	}

	template<class T>
		DynamicArrayOf<T>::~DynamicArrayOf() 
	{ 
		if (data != nullptr)
		{
			Unregister_Pointer(data.get());
		}
	}

	template <class T> DynamicArrayOf<T>& 
		DynamicArrayOf<T>::operator=(const DynamicArrayOf<T> &array)
	{ 
		if (this != &array)
			CopyArray(array);
		return *this; 
	}

	template <class T> inline T&
		DynamicArrayOf<T>::operator[](size_t i) 
	{
		Check_Pointer(data.get());
		Verify(i < length);  
		return data[i]; 
	}

	template <class T> inline const T&
		DynamicArrayOf<T>::operator[](size_t i) const
	{
		Verify(i < length);
		return data[i]; 
	}

	template <class T> void
		DynamicArrayOf<T>::AssignValue(const T &value, size_t length_to_set) 
	{
		SetStorageLength(length_to_set);
		// Do not memcopy, object semantics may be required
		for (int i = 0; i < length; i++)
			data[i] = value;
	}

	template <class T> void
		DynamicArrayOf<T>::AssignData(const T *data_to_copy, size_t length_to_set) 
	{
		Check_Pointer(data_to_copy);
		SetStorageLength(length_to_set);
		// Do not memcopy, object semantics may be required
		for (int i = 0; i < length; i++)
			data[i] = data_to_copy[i];
	}

	template <class T> void
		DynamicArrayOf<T>::SetLength(size_t length_to_set)
	{
		if (length_to_set != length)
		{
			if (length_to_set > 0)
			{
				std::unique_ptr<T[]> new_data = std::make_unique_for_overwrite<T[]>(length_to_set);
				Register_Pointer(new_data.get());
				if (data != nullptr)
				{
					// Do not memcopy, object semantics may be required
					size_t i = Min(length_to_set, length);
					while (i--)
						new_data[i] = data[i];
					Unregister_Pointer(data.get());
				}
				data = std::move(new_data);
			}
			else
			{
				Unregister_Pointer(data.get());
				data = nullptr;
			}
			length = length_to_set;
		}
	}

#if 0
	template <class T> void
		DynamicArrayOf<T>::Compare(const DynamicArrayOf<T> &array) 
	{
		if (length != array.length)
			return false;
		for (int i = 0; i < length; i++)
		{
			if (data[i] != array.data[i])
				return false;
		}
		return true;
	}
#endif

	template <class T> void
		DynamicArrayOf<T>::SetStorageLength(size_t length_to_set)
	{
		if (length_to_set != length)
		{
			if (length_to_set > 0)
			{
				if (data != nullptr)
				{
					Unregister_Pointer(data.get());
				}
				length = length_to_set;
				data = std::make_unique_for_overwrite<T[]>(length);
				Register_Pointer(data.get());
			}
			else
			{
				Verify(length_to_set == 0);
				Unregister_Pointer(data.get());
				data = nullptr;
			}
		}
	}

	template <class T> void
		DynamicArrayOf<T>::CopyArray(const DynamicArrayOf<T> &array)
	{ 
		if (array.length > 0)
		{
			Check_Pointer(array.data.get());
			AssignData(&array.data[0], array.length);
		}
		else
		{
			Verify(array.length == 0);
			Verify(array.data == nullptr);
			if (data != nullptr)
			{
				Unregister_Pointer(data.get());
				data = nullptr;
				length = 0;
			}
		}
	}

	template <class T> MemoryStream&
		MemoryStreamIO_Write(
			MemoryStream *stream,
			const DynamicArrayOf<T> *array
		)
	{
		Check_Object(stream);
		Check_Pointer(array);
		uint32_t length = static_cast<uint32_t>(array->length);
		MemoryStreamIO::Write(stream, &length);
		if (length > 0)
			stream->WriteBytes(&array->data[0], length * sizeof(T));
		return *stream;
	}

	template <class T> MemoryStream&
		MemoryStreamIO_Read(
			MemoryStream *stream,
			DynamicArrayOf<T> *array
		)
	{
		Check_Object(stream);
		Check_Pointer(array);
		uint32_t length;
		MemoryStreamIO::Read(stream, &length);
		array->SetLength(length);
		Verify(length == static_cast<uint32_t>(array->length));
		if (length > 0)
			stream->ReadBytes(&array->data[0], length * sizeof(T));
		return *stream;
	}

	// std::vector stream payload, byte-identical to the DynamicArrayOf one above: a uint32 element count, then
	// count*sizeof(T) raw element bytes (native endian). Elements are read and written as raw bytes exactly as
	// before; Vector2DOf is not trivially copyable, so there is no static_assert here.
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
