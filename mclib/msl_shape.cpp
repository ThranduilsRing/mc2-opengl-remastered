// Multi shape layer: TG_MultiShape texture handles, node rotation and position queries, light list, render, shadows, detach and child tests.
// Declared in the class headers; shared names in msl_internal.h.
#include "msl_internal.h"
#include "mc2fmt.h"


//-------------------------------------------------------------------------------
// TG_TypeMultiShape
void *TG_MultiShape::operator new (size_t mySize)
{
	void *result = TG_Shape::tglHeap->Malloc(mySize);
	return result;
}
 
//-------------------------------------------------------------------------------
 void TG_MultiShape::operator delete (void *us)
{
	TG_Shape::tglHeap->Free(us);
}

//-------------------------------------------------------------------------------
//Function returns 0 if OK.  realLength if textureName is longer then nameLength-1.
//Returns -1 if texture requested is out of range.
//This function digs the texture name(s) out of the ASE file so that the
//User can load and manage them anyway they want to.
long TG_TypeMultiShape::GetTextureName (DWORD textureNum, char *tName, long nameLength)
{
	if (textureNum >= numTextures)
		return(-1);
		
	char baseName[512];
	char extension[256];
	_splitpath(listOfTextures[textureNum].textureName,nullptr,nullptr,baseName,extension);

	char basePath[1024];
	mc2::fmt::to_buf(basePath, "{}{}", mc2::fmt::safe(baseName), mc2::fmt::safe(extension));

	if (nameLength < (strlen(basePath)+1))
		return static_cast<long>(strlen(basePath)+1);
		
	strcpy(tName,basePath);
	return(0);
}	

//-------------------------------------------------------------------------------
//Function returns 0 if OK.  -1 if textureNum is out of range of numTextures.
// Assigns a MCTextureNodeIndex to this NOT a GOS handle anymore.  Must go through
// cache in case of too many handles.
// MC2_TEX_LIFECYCLE_TRACE=1 — see mclib/txmmgr.cpp for full event taxonomy.
// Logs every TG_TypeMultiShape::SetTextureHandle call (the bdactor.cpp/genactor.cpp
// "actor.update() → re-cache" pathway). Absence of these logs for a given multiType
// during gameplay = appearance->touch() ran instead of update() (UPDATE_SKIP=1 path).
static const bool s_msl_texLifecycleTrace = mc2::gate::get<"MC2_TEX_LIFECYCLE_TRACE">();

long TG_TypeMultiShape::SetTextureHandle (DWORD textureNum, DWORD gosTextureHandle)
{
	if (textureNum >= numTextures)
		return(-1);

	listOfTextures[textureNum].mcTextureNodeIndex = gosTextureHandle;
	listOfTextures[textureNum].gosTextureHandle = 0xffffffff;

	// 2026-05-11 leaf propagation. Mirror the multi-level write into every
	// leaf TG_TypeShape's TG_TinyTexture so consumers reading at leaf level
	// (GpuStaticPropBatcher::finalizeGeometry's texture-array build reads
	// `src->listOfTextures[slot].mcTextureNodeIndex`, where src is a leaf
	// TG_TypeShape) see the live nodeIdx. Without this, damage-shape leaves
	// permanently report 0xFFFFFFFF (the CreateListOfTextures default) and
	// the per-packet texture-array build skips them with layer=-1.
	for (long i = 0; i < numTG_TypeShapes; ++i) {
		if (listOfTypeShapes[i]) {
			listOfTypeShapes[i]->SetTextureHandle(textureNum, gosTextureHandle);
		}
	}

	// PERF DIAGNOSTIC 2026-05-07: per-frame call rate makes the unfiltered
	// log unusable for diagnosis (see capture 3 — console flooding tanks
	// frame rate). Rate-limit to: (a) first 32 events per session, then
	// (b) 1-in-256 sampling, plus (c) periodic summary every 1024 events.
	// Set MC2_TEX_LIFECYCLE_TRACE_VERBOSE=1 to restore unfiltered output
	// for narrow-window debugging where flood is acceptable.
	if (s_msl_texLifecycleTrace) {
		static uint32_t s_eventCount = 0;
		static const bool s_verbose = mc2::gate::get<"MC2_TEX_LIFECYCLE_TRACE_VERBOSE">();
		const bool firstWindow = (s_eventCount < 32);
		const bool sample = ((s_eventCount & 0xFF) == 0);  // 1/256
		if (s_verbose || firstWindow || sample) {
			mc2::fmt::print_out("[TEX_LIFECYCLE v1] event=recache_multi multiType={:016X} texNum={:d} nodeIdx=0x{:08x} numTex={:d} total={:d}\n", mc2::fmt::as_ptr((void*)this), mc2::fmt::as_u32((unsigned long)textureNum), mc2::fmt::as_u32((unsigned long)gosTextureHandle), mc2::fmt::as_i32((long)numTextures), mc2::fmt::as_u32(s_eventCount));
			fflush(stdout);
		}
		++s_eventCount;
	}

	return(0);
}

//-------------------------------------------------------------------------------
//Function returns 0 if OK.  -1 if textureNum is out of range of numTextures.
//This function takes the gosTextureHandle passed in and assigns it to the
//textureNum entry of the listOfTextures;
long TG_TypeMultiShape::SetTextureAlpha (DWORD textureNum, bool alphaFlag)
{
	if (textureNum >= numTextures)
		return(-1);

	listOfTextures[textureNum].textureAlpha = alphaFlag;

	// 2026-05-11 leaf propagation: TG_TypeShape leaves carry their own
	// listOfTextures (TG_TinyTexture array) which is COPIED from this
	// multi-type at ASE-load time via CreateListOfTextures (msl.cpp:834)
	// and NEVER refreshed by this setter. Consumers reading at leaf level
	// (GpuStaticPropBatcher reads `type.source->listOfTextures[slot].textureAlpha`,
	// where type.source is a TG_TypeShape leaf) saw stale `false` values
	// for damage-shape textures even after bdactor's setObjStatus loop
	// called SetTextureAlpha(true) on the multi-shape. That produced the
	// destroyed-prop opaque-rectangle alpha bug. Mirror the multi-level
	// write down to every leaf so leaf-reads pick up the live value.
	for (long i = 0; i < numTG_TypeShapes; ++i) {
		if (listOfTypeShapes[i]) {
			listOfTypeShapes[i]->SetTextureAlpha(textureNum, alphaFlag);
		}
	}

	return(0);
}

//-------------------------------------------------------------------------------
//This function rotates the heirarchy from this node down.  Used for torso twists, arms, etc.
long TG_MultiShape::SetNodeRotation (const char *nodeName, Stuff::UnitQuaternion *rot)
{
	for (long i=0;i<numTG_Shapes;i++)
	{
		if (S_stricmp(listOfShapes[i].node->myType->getNodeId(),nodeName) == 0)
		{
			//-------------------------------
			// Found it!
			listOfShapes[i].baseRotation = *rot;
			return i;
		}
	}

	return -1;
}	

//----------------------------------------------------------------------------------------------------
//This function returns the world translated position of the vertex asked for with rotation applied.
Stuff::Vector3D TG_MultiShape::GetShapeVertexInEditor(long shapeNum, long vertexNum, float rotation)
{
	Stuff::Vector3D result;
	result.x = result.y = result.z = 0.0f;
	
	if ((shapeNum >= 0) && (shapeNum < numTG_Shapes) && 
		(vertexNum >= 0) && (vertexNum < myMultiType->listOfTypeShapes[shapeNum]->GetNumTypeVertices()))
	{
		//-------------------------------
		// Get the vertex in question.
		Stuff::Vector3D vertexPos;
		TG_TypeShapePtr typeShape = (TG_TypeShapePtr)myMultiType->listOfTypeShapes[shapeNum];
		vertexPos.x = -(typeShape->listOfTypeVertices[vertexNum].position.x + typeShape->GetRelativeNodeCenter().x + GetRootNodeCenter().x);
		vertexPos.z = typeShape->listOfTypeVertices[vertexNum].position.y + typeShape->GetRelativeNodeCenter().y + GetRootNodeCenter().y; 
		vertexPos.y = typeShape->listOfTypeVertices[vertexNum].position.z + typeShape->GetRelativeNodeCenter().z + GetRootNodeCenter().z;
		
		//---------------------------------------
		// Rotate vertex if rotation is NON-zero
		if (rotation != 0.0f)
		{
			Rotate(vertexPos,rotation);
		}
		
		result = vertexPos;


	}
	
	return result;
}

//----------------------------------------------------------------------------------------------------
//This function returns the world translated position of the vertex asked for with rotation applied.
Stuff::Vector3D TG_MultiShape::GetShapeVertexInWorld(long shapeNum, long vertexNum, float rotation)
{
	Stuff::Vector3D result;
	result.x = result.y = result.z = 0.0f;

	if ((shapeNum >= 0) && (shapeNum < numTG_Shapes) && 
		(vertexNum >= 0) && (vertexNum < myMultiType->listOfTypeShapes[shapeNum]->GetNumTypeVertices()))
	{
		//-------------------------------
		// Get the vertex in question.
		Stuff::Point3D vertexPos;
		TG_TypeShapePtr typeShape = (TG_TypeShapePtr)myMultiType->listOfTypeShapes[shapeNum];
		
		vertexPos.Multiply(typeShape->listOfTypeVertices[vertexNum].position,listOfShapes[shapeNum].shapeToWorld);
		
		mc2ToRender(result, vertexPos);
		
		//---------------------------------------
		// Rotate vertex if rotation is NON-zero
		if (rotation != 0.0f)
		{
			Rotate(result,rotation);
		}
	}
	
	return result;
}

//-------------------------------------------------------------------------------
//This function sets the fog values for the shape.  Straight fog right now.
void TG_MultiShape::SetFogRGB (DWORD fRGB)
{
	for (long i=0;i<numTG_Shapes;i++)
	{
		listOfShapes[i].node->SetFogRGB(fRGB);
	}
}	

//-------------------------------------------------------------------------------
//This function sets the list of lights used by the TransformShape function
//to light the shape.
//Function returns 0 if lightList entries are all OK.  -1 otherwise.
//
long TG_MultiShape::SetLightList (TG_LightPtr *lightList, DWORD nLights)
{
	//-----------------------------------------------------
	// Static member of TG_Shape.  Only need to call once.
	// NOT a static function in case lights need to change
	// on a shape by shape basis!
	return (listOfShapes[0].node->SetLightList(lightList,nLights));
}	

//-------------------------------------------------------------------------------
Stuff::Vector3D TG_MultiShape::GetTransformedNodePosition (Stuff::Point3D *pos, Stuff::UnitQuaternion *rot, const char *nodeId)
{
	Stuff::LinearMatrix4D 	shapeOrigin;
	Stuff::LinearMatrix4D 	localShapeOrigin;
//	Stuff::LinearMatrix4D 	invShapeOrigin;
	
	shapeOrigin.BuildRotation(*rot);
	shapeOrigin.BuildTranslation(*pos);
//	invShapeOrigin.Invert(shapeOrigin);
	
	Stuff::EulerAngles angles(*rot);

	long i=0;

	Stuff::Vector3D result;
	result.x = result.y = result.z = 0.0f;
	
	for (i=0;i<numTG_Shapes;i++)
	{
		//-----------------------------------------------------------------
		// Scan through the list of shapes and dig out the number needed.
		// DO NOT UPDATE THE HEIRARCHY!!!!
		// This thing may not have updated itself this turn yet!!!
		if (S_stricmp(listOfShapes[i].node->myType->getNodeId(),nodeId) == 0)
		{
			result.x = -listOfShapes[i].shapeToWorld.entries[3];
			result.z = listOfShapes[i].shapeToWorld.entries[7];
			result.y = listOfShapes[i].shapeToWorld.entries[11];
			
			return result;
		}
	}
	
	return result;
}

//-------------------------------------------------------------------------------
Stuff::Vector3D TG_MultiShape::GetTransformedNodePosition (Stuff::Point3D *pos, Stuff::UnitQuaternion *rot, long nodeId)
{
	Stuff::LinearMatrix4D 	shapeOrigin;
	Stuff::LinearMatrix4D 	localShapeOrigin;
//	Stuff::LinearMatrix4D 	invShapeOrigin;
	
	shapeOrigin.BuildRotation(*rot);
	shapeOrigin.BuildTranslation(*pos);
//	invShapeOrigin.Invert(shapeOrigin);
	
	Stuff::EulerAngles angles(*rot);

	Stuff::Vector3D result;
	result.x = result.y = result.z = 0.0f;
	
	if ((nodeId >= 0) && (nodeId < numTG_Shapes))
	{
		result.x = -listOfShapes[nodeId].shapeToWorld.entries[3];
		result.z = listOfShapes[nodeId].shapeToWorld.entries[7];
		result.y = listOfShapes[nodeId].shapeToWorld.entries[11];
			
		return result;
	}
	
	return result;
}

Stuff::UnitQuaternion moveem;

//-------------------------------------------------------------------------------
//This function takes the current listOfVisibleFaces and draws them using
//gos_DrawTriangle.
void TG_MultiShape::Render (bool refreshTextures, float forceZ)
{
	for (long i=0;i<numTG_Shapes;i++)
	{
		if (listOfShapes[i].processMe && listOfShapes[i].node)
		{
			//-------------------------------------------------------
			// Only need to due for unique instance items like mechs
			if (refreshTextures)
			{
				for (long j=0;j<myMultiType->numTextures;j++)
				{
					listOfShapes[i].node->myType->SetTextureHandle(j,myMultiType->listOfTextures[j].mcTextureNodeIndex);
					listOfShapes[i].node->myType->SetTextureAlpha(j,myMultiType->listOfTextures[j].textureAlpha); 
				}
			}

			Stuff::Matrix4D  shapeToClip;
			shapeToClip.Multiply(listOfShapes[i].shapeToWorld, TG_Shape::s_worldToClip);
		
            Stuff::Matrix4D shape2world(listOfShapes[i].shapeToWorld);
			listOfShapes[i].node->Render(forceZ,isHudElement,alphaValue,isClamped, &shapeToClip, &shape2world);
		}
	}
}	

//-------------------------------------------------------------------------------
//This function takes the current listOfVisibleFaces and draws them using
//gos_DrawTriangle.
void TG_MultiShape::RenderShadows (bool refreshTextures)
{
	long start = 0;
	for (long i=0;i<numTG_Shapes;i++)
	{
		if (listOfShapes[i].processMe && listOfShapes[i].node)
		{
			//-------------------------------------------------------
			// Only need to due for unique instance items like mechs
			if (refreshTextures)
			{
				for (long j=0;j<myMultiType->numTextures;j++)
				{
					listOfShapes[i].node->myType->SetTextureHandle(j,myMultiType->listOfTextures[j].mcTextureNodeIndex);
					listOfShapes[i].node->myType->SetTextureAlpha(j,myMultiType->listOfTextures[j].textureAlpha); 
				}
			}

			start = listOfShapes[i].node->RenderShadows(start);
		}
	}
}	

//-------------------------------------------------------------------------------
// This function takes the shape named nodeName and all of its children and stops processing
// them forever.  Since we can never re-attach a mech arm in the field, this is OK!
// However, should we want this functionality, it is easy to add!
void TG_MultiShape::StopUsing (const char *nodeName)
{
	//First, find all shapes which are children of nodeName, including nodeName!
	TG_ShapeRecPtr childChain[MAX_NODES];
	TG_ShapeRecPtr detachables[MAX_NODES];
	
	long curShape = 0;
	long i=0;
	for (i=0;i<numTG_Shapes;i++)
	{
		//----------------------------------------------
		long curChild = 0;
		childChain[curChild] = &listOfShapes[i];
		while (childChain[curChild]->parentNode)
		{
			curChild++;

			gosASSERT(curChild < MAX_NODES);

			childChain[curChild] = childChain[curChild-1]->parentNode;
		}

		Stuff::Point3D zero;
		zero.x = zero.y = zero.z = 0.0f;
		for (long j=curChild;j>=0;j--)
		{
			if (S_stricmp(childChain[j]->node->getNodeName(),nodeName) == 0)
			{
				detachables[curShape] = childChain[0];
				curShape++;
				break;
			}
		}	
	}
	
	//Then, mark all of the shapes found with a "don't process me" flag
	for (i=0;i<curShape;i++)
	{
		//Keep the joint_xUARM so that the shoulder sparks work.
		if (S_stricmp(detachables[i]->node->getNodeName(),nodeName) != 0)
			detachables[i]->processMe = false;
	}
}

//-------------------------------------------------------------------------------
// This function takes the shape named nodeName and all of its children, detaches them
// from the current heirarchy and stuffs them into a new MultiShape which it passes back
// Uses are endless but for now limited to blowing the arms off of the mechs!
TG_MultiShapePtr TG_MultiShape::Detach (const char *nodeName)
{
	//First, find all shapes which are children of nodeName, including nodeName!
	TG_ShapeRecPtr childChain[MAX_NODES];
	TG_ShapeRecPtr detachables[MAX_NODES];
	
	long curShape = 0;
	long i=0;
	for (i=0;i<numTG_Shapes;i++)
	{
		//----------------------------------------------
		long curChild = 0;
		childChain[curChild] = &listOfShapes[i];
		while (childChain[curChild]->parentNode)
		{
			curChild++;

			gosASSERT(curChild < MAX_NODES);

			childChain[curChild] = childChain[curChild-1]->parentNode;
		}

		Stuff::Point3D zero;
		zero.x = zero.y = zero.z = 0.0f;
		for (long j=curChild;j>=0;j--)
		{
			if (S_stricmp(childChain[j]->node->getNodeName(),nodeName) == 0)
			{
				detachables[curShape] = childChain[0];
				curShape++;
				break;
			}
		}	
	}
	
	if (!curShape)
		return nullptr;

		//OK to return no shape now.
		//STOP(("Unable to detach %s from shape",nodeName));
		
	//Then, create a new TG_MultiShape and stuff it with the shapes found.
	TG_MultiShapePtr resultShape = new TG_MultiShape;
	resultShape->myMultiType = myMultiType;
	resultShape->numTG_Shapes = curShape;
	
	resultShape->shapeRecStore.assign(curShape, TG_ShapeRec{});
	resultShape->listOfShapes = resultShape->shapeRecStore.data();

	resultShape->frameNum = 0;
	
	for (i=0;i<curShape;i++)
	{
		resultShape->listOfShapes[i] = *detachables[i];
		
		if (S_stricmp(resultShape->listOfShapes[i].node->getNodeName(),nodeName) == 0)
			resultShape->listOfShapes[i].parentNode = nullptr;		//Set new top O heirarchy.
		
		for (long j=0;j<numTG_Shapes;j++)
		{
			if (&listOfShapes[j] == detachables[i])
			{
				//Mark this shape as removable from the previous heirarchy
				listOfShapes[j].node = nullptr;
				listOfShapes[j].parentNode = nullptr;
			}
		}
	}

	//Rebuild resultShape parent node pointers!
	for (i=0;i<resultShape->numTG_Shapes;i++)
	{
		//----------------------------------------------------------------------------------
		// For each shape, look for another node whose NodeId matches this shape's parentId
		for (long j=0;j<resultShape->numTG_Shapes;j++)
		{
			if (strcmp(resultShape->listOfShapes[i].node->myType->getParentId(),resultShape->listOfShapes[j].node->myType->getNodeId()) == 0)
			{
				//-------------------------------
				// Found it!
				resultShape->listOfShapes[i].parentNode = &(resultShape->listOfShapes[j]);
				break;
			}
		}
	}
	
 	//-------------------------------------------------------------
	// Rebuild the currentShape heirarchy to reflect removed things
	std::vector<TG_ShapeRec> newShapeRecs(numTG_Shapes - curShape, TG_ShapeRec{});
	TG_ShapeRecPtr newListOfShapes = newShapeRecs.data();

	long newShapeIndex = 0;
	for (i=0;i<numTG_Shapes;i++)
	{
		if (listOfShapes[i].node != nullptr)
		{
			newListOfShapes[newShapeIndex] = listOfShapes[i];
			newShapeIndex++;
		}
	}

	//-------------------------------------------------------------
	// Move the new list over the old list.
	shapeRecStore = std::move(newShapeRecs);
	listOfShapes = shapeRecStore.data();
	
	numTG_Shapes -= curShape;

	//Rebuild parent node pointers!
	for (i=0;i<numTG_Shapes;i++)
	{
		//----------------------------------------------------------------------------------
		// For each shape, look for another node whose NodeId matches this shape's parentId
		for (long j=0;j<numTG_Shapes;j++)
		{
			if (strcmp(listOfShapes[i].node->myType->getParentId(),listOfShapes[j].node->myType->getNodeId()) == 0)
			{
				//-------------------------------
				// Found it!
				listOfShapes[i].parentNode = &(listOfShapes[j]);
				break;
			}
		}
	}

	return resultShape;
}

//-------------------------------------------------------------------------------
// Tells me if the passed in nodeName is a child of the parentName.
bool TG_MultiShape::isChildOf (const char *nodeName, const char* parentName)
{
	//First, find all shapes which are children of nodeName, including nodeName!
	TG_ShapeRecPtr childChain[MAX_NODES];
	TG_ShapeRecPtr detachables[MAX_NODES];
	
	long curShape = 0;
	long i=0;
	for (i=0;i<numTG_Shapes;i++)
	{
		//----------------------------------------------
		long curChild = 0;
		childChain[curChild] = &listOfShapes[i];
		while (childChain[curChild]->parentNode)
		{
			curChild++;

			gosASSERT(curChild < MAX_NODES);

			childChain[curChild] = childChain[curChild-1]->parentNode;
		}

		Stuff::Point3D zero;
		zero.x = zero.y = zero.z = 0.0f;
		for (long j=curChild;j>=0;j--)
		{
			if (S_stricmp(childChain[j]->node->getNodeName(),parentName) == 0)
			{
				detachables[curShape] = childChain[0];
				curShape++;
				break;
			}
		}	
	}
	
	if (!curShape)
		return false;
		
	for (i=0;i<curShape;i++)
	{
		if (S_stricmp(detachables[i]->node->getNodeName(), nodeName) == 0)
			return true;
	}
	
	return false;
}
