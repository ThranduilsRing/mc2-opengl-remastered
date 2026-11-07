//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"

//#############################################################################
//#####################    MLRIndexedPrimitiveBase    #########################
//#############################################################################

std::vector<unsigned short>
	MLRIndexedPrimitiveBase::clipExtraIndex;

MLRIndexedPrimitiveBase::ClassData*
	MLRIndexedPrimitiveBase::DefaultData = nullptr;

unsigned short
	*indexOffset;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRIndexedPrimitiveBase::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == StaticHeap);
	DefaultData =
		new ClassData(
			MLRIndexedPrimitiveBaseClassID,
			"MidLevelRenderer::MLRIndexedPrimitiveBase",
			MLRPrimitiveBase::DefaultData,
			nullptr
		);
	Register_Object(DefaultData);
	
	clipExtraIndex.resize(Limits::Max_Number_Vertices_Per_Mesh);

	indexOffset = new unsigned short [Limits::Max_Number_Vertices_Per_Mesh+1];
	Register_Pointer(indexOffset);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRIndexedPrimitiveBase::TerminateClass()
{
	std::vector<unsigned short>().swap(clipExtraIndex);

	Unregister_Pointer(indexOffset);
	delete [] indexOffset;

	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRIndexedPrimitiveBase::MLRIndexedPrimitiveBase(
	ClassData *class_data,
	MemoryStream *stream,
	int version
):
	MLRPrimitiveBase(class_data, stream, version)
{
	Check_Pointer(this);
	Check_Object(stream);
	Verify(gos_GetCurrentHeap() == Heap);

	switch(version)
	{
		case 1:
		case 2:
		{
			STOP(("This class got created only after version 2 !"));
		}
		break;
		default:
		{
			MemoryStreamIO_Read(stream, &index);
		}
		break;
	}

	visibleIndexedVerticesKey = false;
	visibleIndexedVertices.assign(coords.size(), 0);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRIndexedPrimitiveBase::Save(MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);

	MLRPrimitiveBase::Save(stream);

	MemoryStreamIO_Write(stream, &index);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRIndexedPrimitiveBase::MLRIndexedPrimitiveBase(ClassData *class_data):
	MLRPrimitiveBase(class_data), index(0)
{
	Verify(gos_GetCurrentHeap() == Heap);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRIndexedPrimitiveBase::~MLRIndexedPrimitiveBase()
{
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRIndexedPrimitiveBase::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRIndexedPrimitiveBase::InitializeDrawPrimitive(unsigned char vis, int parameter)
{
	MLRPrimitiveBase::InitializeDrawPrimitive(vis, parameter);

	gos_indices = nullptr;
	numGOSIndices = -1;

	visibleIndexedVerticesKey = false;
	int i, len = static_cast<int>(visibleIndexedVertices.size());

	for(i=0;i<len;i++)
	{
		visibleIndexedVertices[i] = 0;
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRIndexedPrimitiveBase::SetCoordData(
		const glm::vec3 *data,
		int dataSize
	)
{
	Check_Object(this); 
	Check_Pointer(data);
	Verify(gos_GetCurrentHeap() == Heap);

	Verify(texCoords.size() == 0 || dataSize == texCoords.size());

#if defined (MAX_NUMBER_VERTICES)
	Verify(dataSize <= MAX_NUMBER_VERTICES);
#endif
	coords.assign(data, data + dataSize);

	if(index.size() > 0 && visibleIndexedVertices.size() != static_cast<size_t>(dataSize))
	{
		visibleIndexedVertices.assign(dataSize, 0);
	}


}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRIndexedPrimitiveBase::SetIndexData(
		unsigned short *index_array,
		int index_count
	)
{
	Check_Object(this); 
	Check_Pointer(index_array);
	Verify(gos_GetCurrentHeap() == Heap);

	if(coords.size() > 0)
	{
		visibleIndexedVertices.assign(coords.size(), 0);
	}

#ifdef _ARMOR
	int len = coords.size();
	for(int i=0;i<index_count;i++)
	{
		Verify(index_array[i] < len);
	}
#endif

	index.assign(index_array, index_array + index_count);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRIndexedPrimitiveBase::GetIndexData(
		unsigned short **index_array,
		int *index_count
	)
{
	Check_Object(this); 

	*index_array = index.data();
	*index_count = static_cast<int>(index.size());
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRIndexedPrimitiveBase::Transform(glm::mat4 *mat)
{
	Check_Object(this);

	int i, len = static_cast<int>(coords.size());

	for(i=0;i<len;i++)
	{
		transformedCoords[i].MultiplyPoint(coords[i], *mat);
	}
	
	#ifdef LAB_ONLY
		Set_Statistic(TransformedVertices, TransformedVertices+len);
	#endif
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
bool
	MLRIndexedPrimitiveBase::CheckIndicies()
{
	for(int i=0;i<numGOSIndices;i++)
	{
		if(gos_indices[i] >= numGOSVertices)
		{
			STOP(("Invalid indicies detected !"));
		}
	}
	return true;
}

