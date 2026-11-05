#include "mc2_czstring.h"
#include "../code/tick_commit.h"
//---------------------------------------------------------------------------
//
//	genactor.cpp - This file contains the code for the generic appearance classes
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "texture_assets.h"
#include "genactor_internal.h"
#include "appearance_rng.h"
#include "mc2fmt.h"


//-----------------------------------------------------------------------------
// class GenericAppearanceType
void GenericAppearanceType::init (mc2::czstring fileName)
{
	AppearanceType::init(fileName);

	//----------------------------------------------
	// Base shape.  In stand Pose by default.
	genShape = new TG_TypeMultiShape;
	gosASSERT(genShape != nullptr);

	FullPathFileName iniName;
	iniName.init(tglPath,fileName,".ini");

	FitIniFile iniFile;
	long result = iniFile.open(iniName);
	if (result != NO_ERR)
		Fatal(result,"Could not find building appearance INI file");

	// ASSIMP-GEN-IMPORT-1 — optional GLB probe for generic props. Mirror of
	// mech3d.cpp [Import] pattern. LOD0 only; damage shape stays ASE.
	char importSourceBase[256] = "";
	if (iniFile.seekBlock("Import") == NO_ERR &&
	    iniFile.readIdString("Source", importSourceBase, 255) == NO_ERR &&
	    importSourceBase[0])
	{
		char* dot = strrchr(importSourceBase, '.');
		if (dot) *dot = '\0';
	}

	result = iniFile.seekBlock("TGLData");
	if (result != NO_ERR)
		Fatal(result,"Could not find block in building appearance INI file");

	char aseFileName[512];
	result = iniFile.readIdString("FileName",aseFileName,511);
	if (result != NO_ERR)
		Fatal(result,"Could not find ASE FileName in building appearance INI file");

	if (importSourceBase[0]) {
		// META-FIX design B PART 1: REQUIRED slot — checked import.
		long rc = genShape->LoadFromFile(importSourceBase); // ASSIMP-GEN-IMPORT-1: opt-in GLB probe
		tglVerifyShapeLoaded(genShape, rc, "genactor.base_import",
			importSourceBase, &baseShapeLoadFailed);
	} else {
		FullPathFileName genName;
		genName.init(tglPath,aseFileName,".ase");
		// META-FIX design B PART 1: REQUIRED slot — checked load.
		tglLoadShapeChecked(genShape, genName,
			"genactor.base", &baseShapeLoadFailed);
	}

	result = iniFile.readIdString("TextureName",textureName,49);
	if (result != NO_ERR)
		textureName[0] = 0;

	result = iniFile.readIdULong("RGB",dotRGB);
	if (result != NO_ERR)
		dotRGB = 0x00ffffff;

	result = iniFile.seekBlock("TGLDamage");
	if (result == NO_ERR)
	{
		result = iniFile.readIdString("FileName",aseFileName,511);
		if (result != NO_ERR)
			Fatal(result,"Could not find ASE FileName in building appearance INI file");
	
		FullPathFileName dmgName;
		dmgName.init(tglPath,aseFileName,".ase");
	
		genDmgShape = new TG_TypeMultiShape;
		gosASSERT(genDmgShape != nullptr);
		genDmgShape->LoadTGMultiShapeFromASE(dmgName); // TGL-LOAD-EXEMPT(optional damage shape; GetNumShapes-checked delete-on-fail)

		if (!genDmgShape->GetNumShapes())
		{
			delete genDmgShape;
			genDmgShape = nullptr;
		}
	}
	else
	{
		genDmgShape = nullptr;
	}

	//--------------------------------------------------------------------
	// Load Animation Information.
    // sebi: how this could possible work!!! MAX_BD_ANIMATIONS=10 
    // but MAX_GEN_ANIMATIONS=5! so 5 times out of bounds array access!!!
	// We can load up to 10 Animation States.
	for (int i=0;i<MAX_GEN_ANIMATIONS;i++)
	{
		char blockId[512];
		mc2::fmt::to_buf(blockId, "Animation:{:d}", mc2::fmt::as_i32(i));
		
		result = iniFile.seekBlock(blockId);
		if (result == NO_ERR)
		{
			char animName[512];
			result = iniFile.readIdString("AnimationName",animName,511);
			gosASSERT(result == NO_ERR);
			
			result = iniFile.readIdBoolean("LoopAnimation",genAnimLoop[i]);
			gosASSERT(result == NO_ERR);
			
			result = iniFile.readIdBoolean("Reverse",genReverse[i]);
			gosASSERT(result == NO_ERR);
			
			result = iniFile.readIdBoolean("Random",genRandom[i]);
			gosASSERT(result == NO_ERR);
			
			result = iniFile.readIdLong("StartFrame",genStartF[i]);
			if (result != NO_ERR)
				genStartF[i] = 0;
				
 			//-------------------------------
			// We have an animation to load.
			FullPathFileName animPath;
			animPath.init(tglPath,animName,".ase");

			FullPathFileName otherPath;
			otherPath.init(tglPath,animName,".agl");

			if (fileExists(animPath) || fileExists(otherPath))
			{
				genAnimData[i] = new TG_AnimateShape;
				gosASSERT(genAnimData[i] != nullptr);
	
				//--------------------------------------------------------
				// If this animation does not exist, it is not a problem!
				// Building will simply freeze until animation is "over"
				genAnimData[i]->LoadTGMultiShapeAnimationFromASE(animPath,genShape);
			}
			else
				genAnimData[i] = nullptr;
		}
		else
		{
			genAnimData[i] = nullptr;
		}
	}
	
	//--------------------------------------------------------------------
	// We can also load the node to pitch and yaw for spotlights/turrets.
	result = iniFile.seekBlock("AnimationNode");
	if (result == NO_ERR)
	{
		result = iniFile.readIdString("AnimationNodeId",rotationalNodeId,24);
		gosASSERT(result == NO_ERR);
	}
	else
	{
		strcpy(rotationalNodeId,"NONE");
	}

}

//----------------------------------------------------------------------------
void GenericAppearanceType::destroy (void)
{
	AppearanceType::destroy();

	if (genShape)
	{
		delete genShape;
		genShape = nullptr;
	}

	if (genDmgShape)
	{
		delete genDmgShape;
		genDmgShape = nullptr;
	}
}

//-----------------------------------------------------------------------------
void GenericAppearanceType::setAnimation (TG_MultiShapePtr shape, DWORD animationNum)
{
	gosASSERT(shape != nullptr);
	gosASSERT(animationNum != 0xffffffff);
	gosASSERT(animationNum < MAX_GEN_ANIMATIONS);

	if (genAnimData[animationNum])
		genAnimData[animationNum]->SetAnimationState(shape);
	else
		shape->ClearAnimation();
}

//-----------------------------------------------------------------------------
// class GenericAppearance
void GenericAppearance::init (AppearanceTypePtr tree, GameObjectPtr obj)
{
	Appearance::init(tree,obj);
	appearType = static_cast<GenericAppearanceType *>(tree);

	shapeMin.x = shapeMin.y = -25;
	shapeMax.x = shapeMax.y = 50;

	genAnimationState =-1;
	currentFrame = 0.0f;
	genFrameRate = 0.0f;
	isReversed = false;
	isLooping = false;
	setFirstFrame = false;
	canTransition = true;

	// Slice 2 (object-offload) substrate: never set true in Stage 2.A.
	needsFullBakeNextFrame = false;

	paintScheme = -1;
	objectNameId = 30469;
	hazeFactor = 0.0f;
	skyNumber = 0;

	screenPos.x = screenPos.y = screenPos.z = screenPos.w = -999.0f;
	position = glm::vec3(0.0f);
	rotation = 0.0f;
	pitch = 0.0f;
	selected = 0;
	teamId = -1;
	homeTeamRelationship = 0;
	actualRotation = rotation;

    status = OBJECT_STATUS_NORMAL; // sebi: init to not contain garbage

	OBBRadius = -1.0f;
	
	if (appearType)
	{
		genShape = appearType->genShape->CreateFrom();

		//-------------------------------------------------
		// Load the texture and store its handle.
		for (long i=0;i<genShape->GetNumTextures();i++)
		{
			char txmName[1024];
			if ((i == 0) && (appearType->textureName[0]))
				strcpy(txmName,appearType->textureName);
			else
				genShape->GetTextureName(i,txmName,256);

			char texturePath[1024];
			mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));
	
			FullPathFileName textureName;
			textureName.init(texturePath,txmName,"");
	
			if (fileExists(textureName))
			{
				if (S_strnicmp(txmName,"a_",2) == 0)
				{
					DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
					genShape->SetTextureHandle(i,gosTextureHandle);
					genShape->SetTextureAlpha(i,true);
				}
				else
				{
					DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
					genShape->SetTextureHandle(i,gosTextureHandle);
					genShape->SetTextureAlpha(i,false);
				}
			}
			else
			{
				//PAUSE(("Warning: %s texture name not found",textureName));
				genShape->SetTextureHandle(i,0xffffffff);
			}
		}

		glm::vec3 boxCoords[8];
		glm::vec3 nodeCenter = genShape->GetRootNodeCenter();

		boxCoords[0].x = position.x + genShape->GetMinBox().x + nodeCenter.x;
		boxCoords[0].y = position.y + genShape->GetMinBox().z + nodeCenter.z;
		boxCoords[0].z = position.z + genShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[1].x = position.x + genShape->GetMinBox().x + nodeCenter.x;
		boxCoords[1].y = position.y + genShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[1].z = position.z + genShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[2].x = position.x + genShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[2].y = position.y + genShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[2].z = position.z + genShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[3].x = position.x + genShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[3].y = position.y + genShape->GetMinBox().z + nodeCenter.z;
		boxCoords[3].z = position.z + genShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[4].x = position.x + genShape->GetMinBox().x + nodeCenter.x;
		boxCoords[4].y = position.y + genShape->GetMinBox().z + nodeCenter.z;
		boxCoords[4].z = position.z + genShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[5].x = position.x + genShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[5].y = position.y + genShape->GetMinBox().z + nodeCenter.z;
		boxCoords[5].z = position.z + genShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[6].x = position.x + genShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[6].y = position.y + genShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[6].z = position.z + genShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[7].x = position.x + genShape->GetMinBox().x + nodeCenter.x;
		boxCoords[7].y = position.y + genShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[7].z = position.z + genShape->GetMinBox().y + nodeCenter.y;
		
		float testRadius = 0.0;
		
		for (const auto & boxCoord : boxCoords)
		{
			testRadius = glm::length(boxCoord);
			if (OBBRadius < testRadius)
				OBBRadius = testRadius;
		}

		appearType->boundsUpperLeftX = (-OBBRadius * 2.0);
		appearType->boundsUpperLeftY = (-OBBRadius * 2.0);
		   					 
		appearType->boundsLowerRightX = (OBBRadius * 2.0);
		appearType->boundsLowerRightY = (OBBRadius);
		
 		if (!appearType->getDesignerTypeBounds())
		{
			appearType->typeUpperLeft = genShape->GetMinBox();
			appearType->typeLowerRight = genShape->GetMaxBox();
		}

		// GPU static-prop batcher: register this generic prop's type shape.
		GpuStaticPropBatcher::instance().registerMultiShape(appearType->genShape);
		GpuStaticPropBatcher::instance().registerMultiShape(appearType->genDmgShape);
	}
}

//-----------------------------------------------------------------------------
void GenericAppearance::setObjStatus (long oStatus)
{
	if (status != oStatus)
	{
		if ((oStatus == OBJECT_STATUS_DESTROYED) || (oStatus == OBJECT_STATUS_DISABLED))
		{
			if (appearType->genDmgShape)
			{
				genShape->ClearAnimation();
				delete genShape;
				genShape = nullptr;
				
				genShape = appearType->genDmgShape->CreateFrom();
				
				//-------------------------------------------------
				// Load the texture and store its handle.
				for (long i=0;i<genShape->GetNumTextures();i++)
				{
					char txmName[1024];
					genShape->GetTextureName(i,txmName,256);
					
					char texturePath[1024];
					mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));
			
					FullPathFileName textureName;
					textureName.init(texturePath,txmName,"");
					
					if (fileExists(textureName))
					{
						DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						genShape->SetTextureHandle(i,gosTextureHandle);
					}
					else
					{
						//PAUSE(("Warning: %s texture name not found",textureName));
						genShape->SetTextureHandle(i,0xffffffff);
					}
				}
			}
		}
	}
	
	status = oStatus;
}

//-----------------------------------------------------------------------------
void GenericAppearance::setGesture (unsigned long gestureId)
{
	//------------------------------------------------------------
	// Check if state is possible.
	if (gestureId >= MAX_GEN_ANIMATIONS)
		return;

	//------------------------------------------------------------
	// Check if object destroyed.  If so, no animation!
	if ((status == OBJECT_STATUS_DESTROYED) || (status == OBJECT_STATUS_DISABLED))
		return;
		
	//----------------------------------------------------------------------
	// If state is OK, set animation data, set first frame, set loop and 
	// reverse flag, and start it going until you hear otherwise.
	appearType->setAnimation(genShape,gestureId);
	genAnimationState = gestureId;
	currentFrame = 0.0f;
	if (appearType->genStartF[gestureId])
		currentFrame = appearType->genStartF[gestureId];
		
	isReversed = false;
	
	if (appearType->isReversed(genAnimationState))
	{
		currentFrame = appearType->getNumFrames(genAnimationState)-1;
		isReversed = true;
	}
	
	if (appearType->isRandom(genAnimationState))
	{
        const long count = appearType->getNumFrames(genAnimationState)-1;
        auto randomFrame = [this, count] { currentFrame = mc2_appearance_rng::randomNumber(count); };
        if (!mc2::tick::defer(mc2::tick::Queue::Release, randomFrame)) randomFrame();
	}
	
	isLooping = appearType->isLooped(genAnimationState);
	
	genFrameRate = appearType->getFrameRate(genAnimationState);
	
	setFirstFrame = true;
	canTransition = false;
}

//-----------------------------------------------------------------------------
void GenericAppearance::setMoverParameters (float turretRot, float lArmRot, float rArmRot, bool isAirborne)
{
	pitch = turretRot;
}

//-----------------------------------------------------------------------------
void GenericAppearance::setObjectParameters (const glm::vec3 &pos, float Rot, long sel, long team, long homeRelations)
{
	rotation = Rot;

	position = pos;

	selected = sel;

	actualRotation = Rot;

	teamId = team;
	homeTeamRelationship = homeRelations;
}

//-----------------------------------------------------------------------------
void GenericAppearance::changeSkyToSkyNum (char *txmName, char *newName, size_t newNameBytes)
{
	if (S_strnicmp(txmName,"sky",3) != 0)
	{
		strcpy(newName,txmName);
	}
	else
	{
		strcpy(newName,"Sky");
		mc2::fmt::to_buf_n(newName, newNameBytes, "Sky{:02d}{}", mc2::fmt::as_i32(skyNumber), &txmName[3]);
	}
	
	return;
}

//-----------------------------------------------------------------------------
void GenericAppearance::setSkyNumber (long skyNum)
{
	//-------------------------------------------------
	// Load the texture and store its handle.
	for (long i=0;i<genShape->GetNumTextures();i++)
	{
		char txmName[1024];
		char newName[1024];
		if ((i == 0) && (appearType->textureName[0]))
			strcpy(txmName,appearType->textureName);
		else
			genShape->GetTextureName(i,txmName,256);

		char texturePath[1024];
		mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));

		//Make txmName into a SKY%02d texture and load it!!
		skyNumber = skyNum;
		
		changeSkyToSkyNum(txmName,newName,sizeof(newName));
		
		FullPathFileName textureName;
		textureName.init(texturePath,newName,"");

		if (fileExists(textureName))
		{
			if (S_strnicmp(newName,"a_",2) == 0)
			{
				DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
				genShape->SetTextureHandle(i,gosTextureHandle);
				genShape->SetTextureAlpha(i,true);
			}
			else
			{
				DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
				genShape->SetTextureHandle(i,gosTextureHandle);
				genShape->SetTextureAlpha(i,false);
			}
		}
		else
		{
			//PAUSE(("Warning: %s texture name not found",textureName));
			genShape->SetTextureHandle(i,0xffffffff);
		}
	}
}

//-----------------------------------------------------------------------------
bool GenericAppearance::isMouseOver (float px, float py)
{
	if (inView)
	{
		if ((px <= lowerRight.x) && (py <= lowerRight.y) &&
			(px >= upperLeft.x) &&
			(py >= upperLeft.y))
		{
			return inView;
		}
		else
		{
			return FALSE;
		}
	}
	
	return(inView);
}	

//-----------------------------------------------------------------------------
bool GenericAppearance::recalcBounds (void)
{
	// F3 CPU projection cost-baseline: aggregate per-actor scope into the
	// recalcBounds_perframe bucket. No-op when env OFF.
	::mc2_cpu_proj_cost::Scope _f3_recalcBounds_scope(
	    ::mc2_cpu_proj_cost::BUCKET_RECALCBOUNDS_PERFRAME);
	::mc2_cpu_proj_cost::add_workload_recalcbounds(1);
	Stuff::Vector4D tempPos;
	setVisibilityGatesFromLegacy(false);

	if (eye)
	{
		//ALWAYS need to do this or select is YAYA
		// [PROJECTZ:ScreenXYOracle id=genactor_screen_pos]
		eye->projectForScreenXY(position,screenPos);
		
 		//--------------------------------------------------
		// First, if we are using perspective, figure out
		// if object too far from camera.  Far Clip Plane.
		if (eye->usePerspective)
		{
			glm::vec3 Distance;
			glm::vec3 eyePosition(eye->getPosition());
			glm::vec3 objPosition(position);
	
			Distance = objPosition - eyePosition;
			float eyeDistance = glm::length(Distance);
			if (eyeDistance > Camera::MaxClipDistance)
			{
				hazeFactor = 1.0f;
				setVisibilityGatesFromLegacy(false);
			}
			else if (eyeDistance > Camera::MinHazeDistance)
			{
				Camera::HazeFactor = (eyeDistance - Camera::MinHazeDistance) * Camera::DistanceFactor;
				setVisibilityGatesFromLegacy(true);
			}
			else
			{
				Camera::HazeFactor = 0.0f;
				setVisibilityGatesFromLegacy(true);
			}
			
			//-----------------------------------------------------------------
			// If inside farClip plane, check if behind camera.
			// Find angle between lookVector of Camera and vector from camPos
			// to Target.  If angle is less then halfFOV, object is visible.
			if (inView)
			{
				glm::vec3 Distance;
				glm::vec3 objPosition;
				glm::vec3 eyePosition(eye->getCameraOrigin());
				mc2ToRender(objPosition, position);
		
				Distance = objPosition - eyePosition;
				Distance = glm::normalize(Distance);
				
				float cosine = glm::dot(Distance, eye->getLookVector());
 				if (cosine > eye->cosHalfFOV)
					setVisibilityGatesFromLegacy(true);
				else
					setVisibilityGatesFromLegacy(false);
			}
		}
		else
		{
			Camera::HazeFactor = 0.0f;
			setVisibilityGatesFromLegacy(true);
		}
		
		if (inView)
		{
			if (reloadBounds)
				appearType->reinit();

			appearType->boundsLowerRightY = (OBBRadius * eye->getTiltFactor() * 2.0f);
			
			if (screenPos.z > 0.999999f)
				screenPos.z = 0.999999f;
			//-------------------------------------------------------------------------
			// do a rough check if on screen.  If no where near, do NOT do the below.
			// Mighty mighty slow!!!!
			// Use the original check done before all this 3D madness.  Dig out sourceSafe tomorrow!
			tempPos = screenPos;
			upperLeft.x = tempPos.x;
			upperLeft.y = tempPos.y;
			
			lowerRight.x = tempPos.x;
			lowerRight.y = tempPos.y;
			
			upperLeft.x += (appearType->boundsUpperLeftX * eye->getScaleFactor());
			upperLeft.y += (appearType->boundsUpperLeftY * eye->getScaleFactor());
	
			lowerRight.x += (appearType->boundsLowerRightX * eye->getScaleFactor());
			lowerRight.y += (appearType->boundsLowerRightY * eye->getScaleFactor());

			if ((lowerRight.x >= 0) && (lowerRight.y >= 0) &&
				(upperLeft.x <= eye->getScreenResX()) &&
				(upperLeft.y <= eye->getScreenResY()))
			{
				//We are on screen.  Figure out selection box.
				glm::vec3 boxCoords[8];
				Stuff::Vector4D bcsp[8];
	
				boxCoords[0].x = position.x + appearType->typeUpperLeft.x;
				boxCoords[0].y = position.y + appearType->typeUpperLeft.y;
				boxCoords[0].z = position.z + appearType->typeUpperLeft.z;
	
				boxCoords[1].x = position.x + appearType->typeUpperLeft.x;
				boxCoords[1].y = position.y + appearType->typeLowerRight.y;
				boxCoords[1].z = position.z + appearType->typeUpperLeft.z;
	
				boxCoords[2].x = position.x + appearType->typeLowerRight.x;
				boxCoords[2].y = position.y + appearType->typeUpperLeft.y;
				boxCoords[2].z = position.z + appearType->typeUpperLeft.z;
	
				boxCoords[3].x = position.x + appearType->typeLowerRight.x;
				boxCoords[3].y = position.y + appearType->typeLowerRight.y;
				boxCoords[3].z = position.z + appearType->typeUpperLeft.z;
	
				boxCoords[4].x = position.x + appearType->typeLowerRight.x;
				boxCoords[4].y = position.y + appearType->typeLowerRight.y;
				boxCoords[4].z = position.z + appearType->typeLowerRight.z;
	
				boxCoords[5].x = position.x + appearType->typeLowerRight.x;
				boxCoords[5].y = position.y + appearType->typeUpperLeft.y;
				boxCoords[5].z = position.z + appearType->typeLowerRight.z;
	
				boxCoords[6].x = position.x + appearType->typeUpperLeft.x;
				boxCoords[6].y = position.y + appearType->typeLowerRight.y;
				boxCoords[6].z = position.z + appearType->typeLowerRight.z;
	
				boxCoords[7].x = position.x + appearType->typeUpperLeft.x;
				boxCoords[7].y = position.y + appearType->typeUpperLeft.y;
				boxCoords[7].z = position.z + appearType->typeLowerRight.z;
	
				float maxX = 0.0f, maxY = 0.0f;
				float minX = 0.0f, minY = 0.0f;

				for (long i=0;i<8;i++)
				{
					// [PROJECTZ:ScreenXYOracle id=genactor_box_rect]
					eye->projectForScreenXY(boxCoords[i],bcsp[i]);
					if (!i)
					{
						maxX = minX = bcsp[i].x;
						maxY = minY = bcsp[i].y;
					}

					if (i)
					{
						if (bcsp[i].x > maxX)
							maxX = bcsp[i].x;
						
						if (bcsp[i].x < minX)
							minX = bcsp[i].x;
							
						if (bcsp[i].y > maxY)
							maxY = bcsp[i].y;
						
						if (bcsp[i].y < minY)
							minY = bcsp[i].y;
					}
				}
		
				upperLeft.x = minX;
				upperLeft.y = minY;
				lowerRight.x = maxX;
				lowerRight.y = maxY;
				
				if ((lowerRight.x >= 0) && (lowerRight.y >= 0) &&
					(upperLeft.x <= eye->getScreenResX()) &&
					(upperLeft.y <= eye->getScreenResY()))
				{
					setVisibilityGatesFromLegacy(true);
				}
				else
				{
					setVisibilityGatesFromLegacy(false);		//Did alot of extra work checking this, but WHY draw and insult to injury?
				}
			}
			else
			{
				setVisibilityGatesFromLegacy(false);
			}
		}
	}

	
	return(inView);
}
