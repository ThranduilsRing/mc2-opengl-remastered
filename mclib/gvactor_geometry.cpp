// GVAppearance: paint schemes, gestures, object/mover parameters, bounds, destruction and node positions.
// Declared in the class headers; shared names in gvactor_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "gvactor_internal.h"
#include "appearance_rng.h"
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"


void GVAppearance::setPaintScheme (void)
{
	DWORD gosHandle = mcTextureManager->get_gosTextureHandle(gvShape->GetTextureHandle(0));

	if (gosHandle && gosHandle != 0xffffffff)
	{
		TEXTUREPTR textureData;
		gos_LockTexture(gosHandle, 0, 0, &textureData);

		//-------------------------------------------------------
		// Dominant-channel paint classifier. A pixel belongs to a paint slot
		// iff its R/G/B dominates the other two by at least kRatio. Boundary
		// (antialiased) pixels blend tint vs original proportional to how
		// cleanly they dominate -- the original strict-zero check missed every
		// antialiased mask edge and every upscaled gradient pixel.
		// Bug fixed: inner loop was j < textureData.Height (should be Width).
		DWORD *textureMemory = textureData.pTexture;
		for (long i=0;i<textureData.Height;i++)
		{
			for (long j=0;j<textureData.Width;j++)
			{
				DWORD srcColor = *textureMemory;
				DWORD srcA = (srcColor & 0xff000000);
				int   srcR = (int)((srcColor >> 16) & 0xff);
				int   srcG = (int)((srcColor >>  8) & 0xff);
				int   srcB = (int)( srcColor        & 0xff);

				int   slot       = -1;
				DWORD slotColor  = 0;
				float shade      = 0.0f;
				float mixFactor  = 0.0f;
				const int   kMinDom      = 16;
				const float kRatio       = 3.0f;
				const float kRatioMargin = 1.5f;

				int domChan = 0, maxOther = 0;
				if (srcR >= kMinDom && (float)srcR >= kRatio * (float)srcG && (float)srcR >= kRatio * (float)srcB)
				{
					slot = 0; slotColor = psRed;
					domChan = srcR;
					maxOther = srcG > srcB ? srcG : srcB;
				}
				else if (srcG >= kMinDom && (float)srcG >= kRatio * (float)srcR && (float)srcG >= kRatio * (float)srcB)
				{
					slot = 1; slotColor = psGreen;
					domChan = srcG;
					maxOther = srcR > srcB ? srcR : srcB;
				}
				else if (srcB >= kMinDom && (float)srcB >= kRatio * (float)srcR && (float)srcB >= kRatio * (float)srcG)
				{
					slot = 2; slotColor = psBlue;
					domChan = srcB;
					maxOther = srcR > srcG ? srcR : srcG;
				}

				if (slot != -1)
				{
					float d = (float)domChan / 255.0f;
					shade = 1.0f - (1.0f - d) * (1.0f - d);

					float ratio = (float)domChan / (float)(maxOther + 1);
					float margin = (ratio - kRatio) / (kRatio * (kRatioMargin - 1.0f));
					if (margin > 1.0f) margin = 1.0f;
					if (margin < 0.0f) margin = 0.0f;
					mixFactor = margin;
				}

				DWORD newColor = srcColor;
				if (slot != -1 && mixFactor > 0.0f)
				{
					float tintR = (float)((slotColor >> 16) & 0xff) * shade;
					float tintG = (float)((slotColor >>  8) & 0xff) * shade;
					float tintB = (float)( slotColor        & 0xff) * shade;

					float outR = tintR * mixFactor + (float)srcR * (1.0f - mixFactor);
					float outG = tintG * mixFactor + (float)srcG * (1.0f - mixFactor);
					float outB = tintB * mixFactor + (float)srcB * (1.0f - mixFactor);

					unsigned char r = (unsigned char)(outR < 0.0f ? 0.0f : outR > 255.0f ? 255.0f : outR);
					unsigned char g = (unsigned char)(outG < 0.0f ? 0.0f : outG > 255.0f ? 255.0f : outG);
					unsigned char b = (unsigned char)(outB < 0.0f ? 0.0f : outB > 255.0f ? 255.0f : outB);

					newColor = srcA | ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
				}

				*textureMemory = newColor;
				textureMemory++;
			}
		}

		gos_UnLockTexture(gosHandle);
	}
}	

DWORD bgrTorgb (DWORD frontRGB);

void GVAppearance::setPaintScheme (DWORD mcRed, DWORD mcGreen, DWORD mcBlue)
{
#ifdef BGR
	// These come into here bgr instead of RGB.  CONVERT!
	psRed = bgrTorgb(mcRed);
	psBlue = bgrTorgb(mcBlue);
	psGreen = bgrTorgb(mcGreen);
#else /*BGR*/
	psRed = mcRed;
	psBlue = mcBlue;
	psGreen = mcGreen;
#endif /*BGR*/

	setPaintScheme();	
}	

void GVAppearance::getPaintScheme( DWORD& red, DWORD& green, DWORD& blue )
{
#ifdef BGR
	red = bgrTorgb(psRed);
	blue = bgrTorgb(psBlue);
	green = bgrTorgb(psGreen);
#else /*BGR*/
	red = psRed;
	blue = psBlue;
	green = psGreen;
#endif /*BGR*/
}

void GVAppearance::resetPaintScheme (DWORD red, DWORD green, DWORD blue)
{
	
	if (localTextureHandle)
		mcTextureManager->releaseNode(localTextureHandle);
	
	char txmName[1024];
	gvShape->GetTextureName(0,txmName,256);

   	char texturePath[1024];
   	mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));

   	FullPathFileName textureName;
   	textureName.init(texturePath,txmName,"");

	/* The texture manager asks for a unique 32bit identifier for every texture instance.
	However, it requires 72 bits to fully describe a mech texture (the base color (stored in
	the variable "red"), highlight color1 (blue), and highlight color2 (green), each of which
	is a 24bit number made up of 8bit r, g, and b components). Instead of creating a
	mapping between the mech textures used (probably less than 2^32 of them) and
	32bit identifiers, for the sake of expediency I'm just taking the 3 most significant bits of
	the 9 rgb components to make a 27 bit identifier. This means that two mech textures
	that are close in color (i.e. all of the 3 most significant bits of the 9 rgb components are
	the same) will be treated as the same texture, which is not necessarily a bad thing in
	our case.  */
	DWORD ccbase = ((red >> 5) & 7) + (((red >> 13) & 7) << 3) + (((red >> 21) & 7) << 6);
	DWORD cchighlight1 = ((green >> 5) & 7) + (((green >> 13) & 7) << 3) + (((green >> 21) & 7) << 6);
	DWORD cchighlight2 = ((blue >> 5) & 7) + (((blue >> 13) & 7) << 3) + (((blue >> 21) & 7) << 6);
	DWORD paintInstance = (ccbase << 18) + (cchighlight1 << 9) + (cchighlight2);
	
	if (fileExists(textureName))
	{
		if (S_strnicmp(txmName,"a_",2) == 0)
		{
			DWORD textureInstanceAlreadyExists = mcTextureManager->textureInstanceExists(textureName,gos_Texture_Solid,gosHint_MipmapFilter0 | gosHint_DontShrink,paintInstance);
			if (!textureInstanceAlreadyExists)
				localTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_MipmapFilter0 | gosHint_DontShrink,paintInstance);
			else
				localTextureHandle = textureInstanceAlreadyExists;
				
			gvShape->SetTextureHandle(0,localTextureHandle);
			gvShape->SetTextureAlpha(0,true);
			
			if (textureInstanceAlreadyExists)
			{
				/* In this case, the texture returned should already have the paint scheme
				applied. */
	
				psRed = red;
				psGreen = green;
				psBlue = blue;
				return;
			}
		}
		else
		{
			DWORD textureInstanceAlreadyExists = mcTextureManager->textureInstanceExists(textureName,gos_Texture_Solid,gosHint_MipmapFilter0 | gosHint_DontShrink,paintInstance);
			if (!textureInstanceAlreadyExists)
				localTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_MipmapFilter0 | gosHint_DontShrink,paintInstance);
			else
				localTextureHandle = textureInstanceAlreadyExists;
				
 			gvShape->SetTextureHandle(0,localTextureHandle);
			gvShape->SetTextureAlpha(0,false);
			if (textureInstanceAlreadyExists)
			{
				/* In this case, the texture returned should already have the paint scheme
				applied. */
	
				psRed = red;
				psGreen = green;
				psBlue = blue;
				return;
			}
		}
	}
	else
	{
		gvShape->SetTextureHandle(0,0xffffffff);
	}
	
	setPaintScheme(red,green,blue);
}	

void GVAppearance::setGesture (unsigned long gestureId)
{
	if (gestureId >= MAX_GV_ANIMATIONS)
		return;

	if ((status == OBJECT_STATUS_DESTROYED) || (status == OBJECT_STATUS_DISABLED))
		return;
		
	appearType->setAnimation(gvShape,gestureId);
	gvAnimationState = gestureId;
	currentFrame = 0.0f;
	if (appearType->gvStartF[gestureId])
		currentFrame = appearType->gvStartF[gestureId];
		
	isReversed = false;
	
	if (appearType->isReversed(gvAnimationState))
	{
		currentFrame = appearType->getNumFrames(gvAnimationState)-1;
		isReversed = true;
	}
	
	if (appearType->isRandom(gvAnimationState))
	{
		currentFrame = mc2_appearance_rng::randomNumber(appearType->getNumFrames(gvAnimationState)-1);
	}
	
	isLooping = appearType->isLooped(gvAnimationState);
	
	gvFrameRate = appearType->getFrameRate(gvAnimationState);
	
	setFirstFrame = true;
	canTransition = false;
}

glm::vec3 GVAppearance::getHitNode (void)
{
	if (hitNodeId == -1)
		hitNodeId = gvShape->GetNodeNameId("hitnode");

	glm::vec3 result = getNodeIdPosition(hitNodeId);
 	return result;
}

void GVAppearance::setObjectParameters (const glm::vec3 &pos, float Rot, long sel, long team, long homeRelations)
{
	movedThisFrame = false;
	if ((rotation != Rot) || (!mc2::math::closeEnough(pos, position)))
		movedThisFrame = true;
	
	rotation = Rot;
	turretRotation = Rot;

	position = pos;

	selected = sel;

	actualRotation = Rot;

	teamId = team;
	homeTeamRelationship = homeRelations;
}

void GVAppearance::setMoverParameters (float turretRot, float lArmRot, float rArmRot, bool isAirborne)
{
	turretRotation = turretRot;
	pitch = lArmRot;
	roll = rArmRot;
	isInfantry = isAirborne;
}	

void GVAppearance::debugUpdate (void)
{
	if (!inDebugMoveMode)
		return;

	if (userInput->getKeyDown(KEY_LEFT) && userInput->ctrl())
	{
		vehicleDebugAngle += 11.25;
		if (vehicleDebugAngle > 180.0)
			vehicleDebugAngle -= 360;
	}

	if (userInput->getKeyDown(KEY_RIGHT) && userInput->ctrl())
	{
		vehicleDebugAngle -= 11.25;
		if (vehicleDebugAngle < -180.0)
			vehicleDebugAngle += 360.0;
	}

	if (userInput->getKeyDown(KEY_UP) && userInput->ctrl())
	{
		turretDebugAngle += 11.25;
		if (turretDebugAngle > 180.0)
			turretDebugAngle -= 360;
	}

	if (userInput->getKeyDown(KEY_DOWN) && userInput->ctrl())
	{
		turretDebugAngle -= 11.25;
		if (turretDebugAngle < -180.0)
			turretDebugAngle += 360.0;
	}

	if (userInput->getKeyDown(KEY_EQUALS) && userInput->ctrl())
	{
		debugVelMag += 10.0;
	}
						
	if (userInput->getKeyDown(KEY_MINUS) && userInput->ctrl())
	{
		debugVelMag -= 10.0;
	}

	if (userInput->getKeyDown(KEY_0) && userInput->ctrl())
	{
		debugVelMag = 0.0;
	}

	glm::vec3 velocity;
	velocity.x = 0.7071f;
	velocity.z = 0.0;
	velocity.y = -0.7071f;

	Rotate(velocity,-vehicleDebugAngle);

	float velMag = debugVelMag;

	glm::vec3 currentNormal = land->getTerrainNormal(debugGVActorPosition);
	float angle = angle_from(velocity,currentNormal);
	if (angle != 90.0)
	{
		float hillFactor = cos(angle * DEGREES_TO_RADS) * velMag;
		velMag += hillFactor;
	}

	velocity *= velMag * worldUnitsPerMeter;

	velocity *= frameLength;

	debugGVActorPosition += velocity;
	debugGVActorPosition.z = land->getTerrainElevation(debugGVActorPosition);

	setObjectParameters(debugGVActorPosition,vehicleDebugAngle,true,0, 0);
	update();
	recalcBounds();
}

bool GVAppearance::isMouseOver (float px, float py)
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

bool GVAppearance::recalcBounds (void)
{
	// F3 CPU projection cost-baseline: aggregate per-actor scope into the
	// recalcBounds_perframe bucket. No-op when env OFF.
	::mc2_cpu_proj_cost::Scope _f3_recalcBounds_scope(
	    ::mc2_cpu_proj_cost::BUCKET_RECALCBOUNDS_PERFRAME);
	::mc2_cpu_proj_cost::add_workload_recalcbounds(1);
	Stuff::Vector4D tempPos;
	setVisibilityGatesFromLegacy(false);

	float eyeDistance = 0.0f;

	if (eye)
	{
		//ALWAYS need to do this or select is YAYA
		// [PROJECTZ:ScreenXYOracle id=gvactor_screen_pos]
		eye->projectForScreenXY(position,screenPos);
		
		if (eye->usePerspective)
		{
			glm::vec3 Distance;
			glm::vec3 objPosition;
			glm::vec3 eyePosition(eye->getCameraOrigin());
			mc2ToRender(objPosition, position);
	
			Distance = objPosition - eyePosition;
			eyeDistance = glm::length(Distance);
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
				glm::vec3 boxCoords[8];
				Stuff::Vector4D bcsp[8];

				glm::vec3 minBox;
				mc2ToRender(minBox, appearType->typeUpperLeft);
				
				glm::vec3 maxBox;
				mc2ToRender(maxBox, appearType->typeLowerRight);
				
				if (rotation != 0.0f)
					Rotate(minBox,-rotation);
 	
				if (rotation != 0.0f)
					Rotate(maxBox,-rotation);
					
 				boxCoords[0].x = position.x + minBox.x;
				boxCoords[0].y = position.y + minBox.y;
				boxCoords[0].z = position.z + minBox.z;
	
				boxCoords[1].x = position.x + minBox.x;
				boxCoords[1].y = position.y + maxBox.y;
				boxCoords[1].z = position.z + minBox.z;
 	
				boxCoords[2].x = position.x + maxBox.x;
				boxCoords[2].y = position.y + minBox.y;
				boxCoords[2].z = position.z + minBox.z;
    
				boxCoords[3].x = position.x + maxBox.x;
				boxCoords[3].y = position.y + maxBox.y;
				boxCoords[3].z = position.z + minBox.z;
    
				boxCoords[4].x = position.x + maxBox.x;
				boxCoords[4].y = position.y + maxBox.y;
				boxCoords[4].z = position.z + maxBox.z;
    
				boxCoords[5].x = position.x + maxBox.x;
				boxCoords[5].y = position.y + minBox.y;
				boxCoords[5].z = position.z + maxBox.z;
    
				boxCoords[6].x = position.x + minBox.x;
				boxCoords[6].y = position.y + maxBox.y;
				boxCoords[6].z = position.z + maxBox.z;
 	
				boxCoords[7].x = position.x + minBox.x;
				boxCoords[7].y = position.y + minBox.y;
				boxCoords[7].z = position.z + maxBox.z;
 	
				float maxX = 0.0f, maxY = 0.0f;
				float minX = 0.0f, minY = 0.0f;

				for (long i=0;i<8;i++)
				{
					// [PROJECTZ:ScreenXYOracle id=gvactor_box_rect]
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
					if ((status != OBJECT_STATUS_DESTROYED) && (status != OBJECT_STATUS_DISABLED))
					{
						//-------------------------------------------------------------------------------
						//Set LOD of Model here because we have the distance and we KNOW we can see it!
						bool baseLOD = true;
						DWORD selectLOD = 0;
						if (useHighObjectDetail)
						{
							for (long i=1;i<MAX_LODS;i++)
							{
								if (appearType->gvShape[i] && (eyeDistance > appearType->lodDistance[i]))
								{
									baseLOD = false;
									selectLOD = i;
								}
							}
						}
						else	//We always want to use the lowest LOD!!
						{
							if (appearType->gvShape[1])
							{
								baseLOD = false;
								selectLOD = 1;
							}
						}
						
						if (selectLOD != currentLOD)
						{
							currentLOD = selectLOD;

							BYTE alphaValue = gvShape->GetAlphaValue();
							gvShape->ClearAnimation();
							delete gvShape;
							gvShape = nullptr;

							gvShape = appearType->gvShape[currentLOD]->CreateFrom();
							if (gvAnimationState != -1)
								appearType->setAnimation(gvShape,gvAnimationState);

							gvShape->SetAlphaValue(alphaValue);

							for (long j=0;j<gvShape->GetNumTextures();j++)
							{
								char txmName[1024];
								gvShape->GetTextureName(j,txmName,256);

								char texturePath[1024];
								mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));

								FullPathFileName textureName;
								textureName.init(texturePath,txmName,"");

								if (fileExists(textureName))
								{
									if (S_strnicmp(txmName,"a_",2) == 0)
									{
										DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
										gosASSERT(gosTextureHandle != 0xffffffff);
										gvShape->SetTextureHandle(j,gosTextureHandle);
										gvShape->SetTextureAlpha(j,true);
									}
									else
									{
										DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
										gosASSERT(gosTextureHandle != 0xffffffff);
										gvShape->SetTextureHandle(j,gosTextureHandle);
										gvShape->SetTextureAlpha(j,false);
									}
								}
								else
								{
									gvShape->SetTextureHandle(j,0xffffffff);
								}
							}
							DWORD r, g, b;
							getPaintScheme(r, g, b);
							resetPaintScheme(r,g,b);
						}

						if (currentLOD && baseLOD)
						{
							currentLOD = 0;
							
							BYTE alphaValue = gvShape->GetAlphaValue();
							gvShape->ClearAnimation();
							delete gvShape;
							gvShape = nullptr;
							
							gvShape = appearType->gvShape[currentLOD]->CreateFrom();
							gvShape->SetAlphaValue(alphaValue);
							
							for (long i=0;i<gvShape->GetNumTextures();i++)
							{
								char txmName[1024];
								gvShape->GetTextureName(i,txmName,256);
										
								char texturePath[1024];
								mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));
						
								FullPathFileName textureName;
								textureName.init(texturePath,txmName,"");
										
								if (fileExists(textureName))
								{
									if (S_strnicmp(txmName,"a_",2) == 0)
									{
										localTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink,true);
										gosASSERT(localTextureHandle != 0xffffffff);
										gvShape->SetTextureHandle(i,localTextureHandle);
										gvShape->SetTextureAlpha(i,true);
									}
									else
									{
										localTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink,true);
										gosASSERT(localTextureHandle != 0xffffffff);
										gvShape->SetTextureHandle(i,localTextureHandle);
										gvShape->SetTextureAlpha(i,false);
									}
								}
								else
								{
									gvShape->SetTextureHandle(i,0xffffffff);
								}
							}

							DWORD r, g, b;
							getPaintScheme(r, g, b);
							resetPaintScheme(r,g,b);
						}					
					}
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

bool GVAppearance::playDestruction (void)
{
	if (appearType->destructEffect[0])
	{
		unsigned flags = gosFX::Effect::ExecuteFlag;

		Check_Object(gosFX::EffectLibrary::Instance);
		gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(appearType->destructEffect);
		
		if (gosEffectSpec)
		{
			destructFX = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
			gosASSERT(destructFX != nullptr);
		
			MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
		
			glm::vec3			tPosition;
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			
			mc2ToRender(tPosition, position);
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, tPosition);
			
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
			destructFX->Start(&info);
			
			return true;
		}
		
		return false;
	}
	
	return false;		//We didn't have a destruct effect.  Tell the object to play its default.
}

glm::vec3 GVAppearance::getNodeNamePosition (const char *nodeName)
{
	glm::vec3 result = position;

	// C3: route to GPU-lagged visibility when killswitch is enabled.
	if (s_gpuCullLifecycle) {
		if (!gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_)))
			return result;
	} else {
		if (!inView)
			return result;
	}

   	glm::quat qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
   
   	glm::vec3 xlatPosition;
   	mc2ToRender(xlatPosition, position);
   
   	glm::quat torsoRot;
   	torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(turretRotation * DEGREES_TO_RADS),0.0f));
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = gvShape->SetNodeRotation(appearType->rotationalNodeId,&torsoRot);

	gvShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);
   
	result = gvShape->GetTransformedNodePosition(&xlatPosition,&qRotation,nodeName);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}

glm::vec3 GVAppearance::getNodeIdPosition (long nodeId)
{
	glm::vec3 result = position;

	// C3: route to GPU-lagged visibility when killswitch is enabled.
	if (s_gpuCullLifecycle) {
		if (!gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_)))
			return result;
	} else {
		if (!inView)
			return result;
	}

   	glm::quat qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
   
   	glm::vec3 xlatPosition;
   	mc2ToRender(xlatPosition, position);
   
   	glm::quat torsoRot;
   	torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(turretRotation * DEGREES_TO_RADS),0.0f));
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = gvShape->SetNodeRotation(appearType->rotationalNodeId,&torsoRot);

	gvShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);
   
	result = gvShape->GetTransformedNodePosition(&xlatPosition,&qRotation,nodeId);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}
