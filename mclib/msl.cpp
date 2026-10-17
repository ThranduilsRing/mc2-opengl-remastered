//-------------------------------------------------------------------------------
//
// Multiple TG Shape Layer
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
// Replace MLR for a myriad number of reasons.
//
// Started 4/16/99
//
// FFS
//
//-------------------------------------------------------------------------------

//-------------------------------------------------------------------------------
// Include Files

#include "msl_internal.h"


// FRAME-JOBS-2D: counts ResubmitCachedGpuLightData calls made from worker threads.
// Must remain 0 every frame — nonzero means the split boundary is broken.
std::atomic<int> g_workerResubmitCalls{0};

//-------------------------------------------------------------------------------
void GetNextLine (char *rawData, char *result)
{
	long startIndex = 0;
	long endIndex = 0;
	while (	(rawData[startIndex] != '\n') )
	{
		startIndex++;
	}

	startIndex++;
	endIndex = startIndex;
	while (	(rawData[endIndex] != '\n') )
	{
		endIndex++;
	}

	strncpy(result,&rawData[startIndex],endIndex - startIndex);
	result[endIndex-startIndex] = 0;
}
	
//-------------------------------------------------------------------------------
bool useSlerping = false;
bool useShadows = true;

//-------------------------------------------------------------------------------
// TG_TypeMultiShape
//-------------------------------------------------------------------------------
// TG_MultiShape & TG_TypeMultiShape
//This function copies the entire multi-shape to the new pointer.
//It does malloc and the newShape is a completely independant copy
//That must be destroyed or memory will leak!
TG_MultiShapePtr TG_TypeMultiShape::CreateFrom (void)
{
	TG_MultiShapePtr newShape = nullptr;
	newShape = new TG_MultiShape;
	gosASSERT(newShape != nullptr);
	
#ifdef _DEBUG
	if (numTG_TypeShapes == 0)
		STOP(("Tried to create a shape with no data Named %s",shapeName));
#endif

	//listOfShapes
	newShape->numTG_Shapes = numTG_TypeShapes;
	newShape->shapeRecStore.assign(numTG_TypeShapes, TG_ShapeRec{});
	newShape->listOfShapes = newShape->shapeRecStore.data();

	for (long i=0;i<numTG_TypeShapes;i++)
	{
		newShape->listOfShapes[i].node = listOfTypeShapes[i]->CreateFrom();
	}

	//Setup Heirarchy again because pointers are not valid!
	for (int i=0;i<numTG_TypeShapes;i++)
	{
		newShape->listOfShapes[i].parentNode = nullptr;
		newShape->listOfShapes[i].currentAnimation = nullptr;
		newShape->listOfShapes[i].shapeToWorld = Stuff::LinearMatrix4D::Identity;
		newShape->listOfShapes[i].worldToShape = Stuff::LinearMatrix4D::Identity;
		newShape->listOfShapes[i].calcedThisFrame = -1;
		newShape->listOfShapes[i].processMe = true;
		newShape->listOfShapes[i].baseRotation.w = 1.0f;
		newShape->listOfShapes[i].baseRotation.x = 
		newShape->listOfShapes[i].baseRotation.y = 
		newShape->listOfShapes[i].baseRotation.z = 0.0f;

		//----------------------------------------------------------------------------------
		// For each shape, look for another node whose NodeId matches this shape's parentId
		for (long j=0;j<numTG_TypeShapes;j++)
		{
			if (strcmp(newShape->listOfShapes[i].node->myType->getParentId(),newShape->listOfShapes[j].node->myType->getNodeId()) == 0)
			{
				//-------------------------------
				// Found it!
				newShape->listOfShapes[i].parentNode = &(newShape->listOfShapes[j]);
				break;
			}
		}

		//----------------------------------------------------------------------------
		// Do NOT need to reset nodeCenters as they are correct for each node copied!
	}

	newShape->frameNum = 0.0f;

	newShape->myMultiType = this;
	
	newShape->isHudElement = false;
	
	return newShape;
}	
