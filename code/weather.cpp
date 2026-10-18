//******************************************************************************************
//	weather.cpp - This file contains the weather class code
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//----------------------------------------------------------------------------------
// Include Files
#ifndef WEATHER_H
#include"weather.h"
#endif

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef QUAD_H
#include"quad.h"
#endif

#ifndef TGL_H
#include"tgl.h"
#endif

#include <cstdlib>
#include <vector>   // MC2_RAIN_BATCH staging buffer
#include "sim_trace.h"
#include "Gates/gate.h"
#include "det_rng.h"

//----------------------------------------------------------------------------------
// Macro Definitions
Weather *weather = nullptr;

//----------------------------------------------------------------------------------
// Class Weather
void Weather::destroy (void)
{
	systemHeap->Free(rainDrops);
	rainDrops = nullptr;
	
	init();
}
		
//----------------------------------------------------------------------------------
void Weather::save (FitIniFile *missionFile)
{
	missionFile->writeBlock("Weather");
	missionFile->writeIdULong("MaxRainDrops",totalRainDrops);
	missionFile->writeIdFloat("StartingRainLevel",rainLevel);
	missionFile->writeIdLong("ChanceOfRain",baseRainChance);
	missionFile->writeIdFloat("BaseLighteningChance",baseLighteningChance);
}

//----------------------------------------------------------------------------------
void Weather::load (FitIniFile *missionFile)
{
	long result = missionFile->seekBlock("Weather");
	if (result == NO_ERR)
	{
		weatherActive = true;
		result = missionFile->readIdULong("MaxRainDrops",totalRainDrops);
		if (result != NO_ERR)
			totalRainDrops = 500;
			
		if (totalRainDrops > 500)
			totalRainDrops = 500;
			
		result = missionFile->readIdFloat("StartingRainLevel",rainLevel);
		if (result != NO_ERR)
			rainLevel = 0.0f;
			
		if (rainLevel < 0.0f)
			rainLevel = 0.0f;
			
		if (rainLevel > 4.0f)
			rainLevel = 4.0f;
			
		result = missionFile->readIdLong("ChanceOfRain",baseRainChance);
		if (result != NO_ERR)
			baseRainChance = 0;
			
		if (baseRainChance < 0)
			baseRainChance = 0;
			
		if (baseRainChance > 100)
			baseRainChance = 100;
			
		result = missionFile->readIdFloat("BaseLighteningChance",baseLighteningChance);
		if (result != NO_ERR)
			baseLighteningChance = BASE_LIGHTENING_CHANCE;
			
		if (baseLighteningChance < 0.0f)
			baseLighteningChance = 0.0f;
			
		if (baseLighteningChance > 0.1f)
			baseLighteningChance = 0.1f;
			
		init(totalRainDrops,rainLevel,baseRainChance,baseLighteningChance);
	}
}

//----------------------------------------------------------------------------------
void Weather::init (FitIniFilePtr missionFile)
{
	long result = missionFile->seekBlock("Weather");
	if (result == NO_ERR)
	{
		weatherActive = true;
		result = missionFile->readIdULong("MaxRainDrops",totalRainDrops);
		if (result != NO_ERR)
			totalRainDrops = 500;
			
		if (totalRainDrops > 500)
			totalRainDrops = 500;
			
		result = missionFile->readIdFloat("StartingRainLevel",rainLevel);
		if (result != NO_ERR)
			rainLevel = 0.0f;
			
		if (rainLevel < 0.0f)
			rainLevel = 0.0f;
			
		if (rainLevel > 4.0f)
			rainLevel = 4.0f;
			
		result = missionFile->readIdLong("ChanceOfRain",baseRainChance);
		if (result != NO_ERR)
			baseRainChance = 0;
			
		if (baseRainChance < 0)
			baseRainChance = 0;
			
		if (baseRainChance > 100)
			baseRainChance = 100;
			
		result = missionFile->readIdFloat("BaseLighteningChance",baseLighteningChance);
		if (result != NO_ERR)
			baseLighteningChance = BASE_LIGHTENING_CHANCE;
			
		if (baseLighteningChance < 0.0f)
			baseLighteningChance = 0.0f;
			
		if (baseLighteningChance > 0.1f)
			baseLighteningChance = 0.1f;
			
		init(totalRainDrops,rainLevel,baseRainChance,baseLighteningChance);
	}
	else
	{
		//No Weather.  
		weatherActive = false;
		init(0,0.0f,0.0f);
	}
}

//----------------------------------------------------------------------------------
// Weather is drawn around the camera, so how many numbers it takes depends on where the camera is. It draws from its own
// stream so that the camera, or the effects option that turns weather off, cannot move the shared stream the simulation uses.
long Weather::randomNumber (long range)
{
	++rngDraws;
	return mc2_det_rng::next15(rngState) % range;
}

// The same shaping as RollDice, on the weather's own stream.
bool Weather::rollDice (long percent)
{
	++rngDraws;
	return ((static_cast<long>(mc2_det_rng::next15(rngState)) * 100) >> 15) < percent;
}

//----------------------------------------------------------------------------------
void Weather::init (DWORD maxDrops, float startingRain, float brChance, float ltChance)
{
	rngState = kWeatherSeed;
	totalRainDrops = maxDrops;
	currentRainDrops = 0;
	
	if (maxDrops)
	{
		rainDrops = (RainDrops *)systemHeap->Malloc(sizeof(RainDrops) * maxDrops);
		gosASSERT(rainDrops != nullptr);
		
		for (long i=0;i<totalRainDrops;i++)
			rainDrops[i].init();
	}
	else
	{
		weatherActive = false;
	}
		
	rainLevel = rainLightLevel = startingRain;
	if (rainLevel != 0.0f)
	{
		rainFactor = 0.05f + float(randomNumber(100)) / 1000.0f;
		rainTrend = 1.0f;
	}

	lighteningLevel = 0.0f;
	
	baseRainChance = brChance;
	baseLighteningChance = ltChance;
	
	//Force update on frame one, in case its already raining!!
	rainUpdateTime = 0.0f;
	
	oldFog = -1.0f;
}
		
//----------------------------------------------------------------------------------
void Weather::update (void)
{
	if (!weatherActive)
		return;
		
	//------------------------------------------------
	// Check if we should update the rain state.
	rainUpdateTime -= frameLength;
	
	if (rainUpdateTime <= 0.0f)
	{
		rainLevel += (rainFactor * rainTrend);
		
		if (rainLevel > 3.0f)
			soundSystem->setIsRaining(RAIN1);
		else if (rainLevel > 2.5f)
			soundSystem->setIsRaining(RAIN2);
		else if (rainLevel > 2.0f)
			soundSystem->setIsRaining(RAIN3);
		else if (rainLevel > 1.0f)
			soundSystem->setIsRaining(RAIN4);
		else
			soundSystem->setIsRaining(0);
		
		//-------------------------------------------------------------------
		// Check if its raining.  If not, check if it should.  If not, exit.
		if ((rainLevel <= 1.5f) && (rainTrend < 0.0f))
		{
			if ((baseRainChance != 0.0f) && rollDice(baseRainChance))
			{
				rainFactor = 0.05f + float(randomNumber(100)) / 1000.0f;
				rainTrend = 1.0f;
			}
			else
			{
				currentRainDrops = 0;
				return;
			}
		}
		else
		{
			if (rainLevel > 3.0f)
			{
				float invRainChance = 100.0f - baseRainChance;
				if ((invRainChance != 0.0f) && rollDice(invRainChance))
				{
					rainFactor = 0.05f + float(randomNumber(100)) / 1000.0f;
					rainTrend = -1.0f;
				}
				
				if (rainLevel > 4.0f)
					rainLevel = 4.0f;
			}
		}
		
		if (rainLevel <= 0.0f)
		{
			rainLevel = 0.0f;
			rainFactor = 0.0f;
			rainTrend = 1.0f;
		}
			
		rainUpdateTime = BASE_RAIN_UPDATE_TIME;
	}
	
	if (rainLevel > 0.0f)
	{
   		glm::vec3 startPos = eye->getPosition();
		startPos.z += BASE_RAIN_HEIGHT;
   		float currentElevation = land->getTerrainElevation(startPos);
		MC2_SIM_TRACE_LINE("WX pos=%.9g,%.9g,%.9g elev=%.9g fl=%.9g rl=%.9g", static_cast<double>(startPos.x), static_cast<double>(startPos.y), static_cast<double>(startPos.z), static_cast<double>(currentElevation), static_cast<double>(frameLength), static_cast<double>(rainLevel));
		
		//----------------------------
		// Update the light level
		// Light Levels for this time of day already calced.
		// Just change 'em based on the weather now.
		// Must change based on whether or not night has or is falling.
		if (rainLightLevel < rainLevel)
			rainLightLevel += BASE_RAIN_LIGHT_RATE * frameLength;
			
		if (rainLightLevel > rainLevel)
			rainLightLevel -= BASE_RAIN_LIGHT_RATE * frameLength;
		 
		if (rainLightLevel > 1.0f)
		{
			float weatherNightFactor = rainLightLevel / 4.0f; 
			if (weatherNightFactor > eye->nightFactor)
				eye->nightFactor = weatherNightFactor;
		
			lighteningCheck -= frameLength;
			if (lighteningCheck < 0.0f)
			{
				//-----------------------
				// Did lightening flash?
				float baseLighteningFlashChance = BASE_LIGHTENING_CHANCE * rainLevel * 100;
				
				if (rollDice(baseLighteningFlashChance))
				{
					lighteningLevel = 64.0f + float(randomNumber(192));
					if (thunderTime == 0.0f)	//Fire off a crack of thunder unless one is on its way!
					{
						thunderTime = randomNumber(BASE_THUNDER_RANDOM_START);
						
						if (lighteningLevel > 210)
						{
							thunderSFX = THUNDER1;
						}
						else if (lighteningLevel > 176)
						{
							thunderSFX = THUNDER3;
						}
						else if (lighteningLevel > 128)
						{
							thunderSFX = THUNDER2;
						}
						else
						{
							thunderSFX = THUNDER4;
						}
					}
				}
				
				lighteningCheck = BASE_LIGHTENING_FLASH_CHECK;
			}

			if (lighteningLevel > 0.0f)
			{
				float lighteningDropoff;

				//Function here to make lightening flicker.
				if (lighteningLevel > 192.0f)
				{
					if (rollDice(50))
					{
						lighteningDropoff = -randomNumber(BASE_LIGHTENING_FALLOFF);
					}
					else
					{
						lighteningDropoff = randomNumber(BASE_LIGHTENING_FALLOFF*0.33f);
					}
				}
				else
				{
					lighteningDropoff = -randomNumber(BASE_LIGHTENING_FALLOFF);
				}

				lighteningLevel += lighteningDropoff * frameLength;
				
				if (lighteningLevel < 0.0f)
					lighteningLevel = 0.0f;
					
				if (lighteningLevel > 255.0f)
					lighteningLevel = 255.0f;
			}
			
			if (thunderTime > 0.0f)
				thunderTime -= frameLength;

			if ((thunderSFX != 0xffffffff) && (thunderTime <= 0.0f))
			{
				thunderTime = 0.0f;
				soundSystem->playDigitalSample(thunderSFX);
				thunderSFX = 0xffffffff;
			}
			
			unsigned char lightening = lighteningLevel;
			TerrainQuad::rainLightLevel = rainLightLevel * 0.2f;
			{
				eye->ambientRed  = eye->dayAmbientRed / rainLightLevel;
				eye->ambientGreen = eye->dayAmbientGreen / rainLightLevel;
				eye->ambientBlue = eye->dayAmbientBlue / rainLightLevel;
								 
				eye->lightRed = eye->dayLightRed / rainLightLevel;
				eye->lightGreen = eye->dayLightGreen / rainLightLevel;
				eye->lightBlue = eye->dayLightBlue / rainLightLevel;
			}
		
			if (lighteningLevel != 0.0f)
			{
				TerrainQuad::lighteningLevel = lightening>>1;
				TG_Shape::lighteningLevel = lightening>>1; 
			}
		}
		else
		{
			{
				eye->ambientRed  = eye->dayAmbientRed;
				eye->ambientGreen = eye->dayAmbientGreen;
				eye->ambientBlue = eye->dayAmbientBlue;
								 
				eye->lightRed = eye->dayLightRed;
				eye->lightGreen = eye->dayLightGreen;
				eye->lightBlue = eye->dayLightBlue;
			}
		}

  		//-----------------------
		// Update the rainDrops.
		if (rainLevel >= 1.0f)
		{
			//---------------------------------------------
			// Calculate how many raindrops are in service
			DWORD newRainDrops = (rainLevel - 1.0f) * totalRainDrops;
			if (newRainDrops > totalRainDrops)
				newRainDrops = totalRainDrops;
				
			if (newRainDrops > currentRainDrops)
			{
				//----------------------------------------------
				// Must put the new raindrops into service.
				for (long i=currentRainDrops;i<newRainDrops;i++)
				{
					glm::vec3 positionOffset;
					positionOffset.x = randomNumber(BASE_RAIN_RANDOM_POS_FACTOR * 2) - BASE_RAIN_RANDOM_POS_FACTOR;
					positionOffset.y = randomNumber(BASE_RAIN_RANDOM_POS_FACTOR * 2) - BASE_RAIN_RANDOM_POS_FACTOR;
					positionOffset.z = randomNumber(BASE_RAIN_RANDOM_HGT_FACTOR * 2) - BASE_RAIN_RANDOM_HGT_FACTOR;
				
					rainDrops[i].position = startPos + positionOffset;
					
					rainDrops[i].length = BASE_RAIN_LENGTH + randomNumber(BASE_RAIN_RANDOM_LEN_FACTOR * 2) - BASE_RAIN_RANDOM_LEN_FACTOR; 
				}
			}
			
			currentRainDrops = newRainDrops;
		}
		
		for (long i=0;i<currentRainDrops;i++)
		{
			//-----------------------------------------------
			//Update the position.  Move rain toward ground.
			rainDrops[i].position.z -= BASE_RAIN_VEL * frameLength * rainLevel;
			
			if (rainDrops[i].position.z <= currentElevation)
			{
				glm::vec3 positionOffset;
				//This raindrop has hit the ground.  Recycle him.
				positionOffset.x = randomNumber(BASE_RAIN_RANDOM_POS_FACTOR * 2) - BASE_RAIN_RANDOM_POS_FACTOR;
				positionOffset.y = randomNumber(BASE_RAIN_RANDOM_POS_FACTOR * 2) - BASE_RAIN_RANDOM_POS_FACTOR;
				positionOffset.z = randomNumber(BASE_RAIN_RANDOM_HGT_FACTOR * 2) - BASE_RAIN_RANDOM_HGT_FACTOR;
			
				rainDrops[i].position = startPos + positionOffset;
				
				rainDrops[i].length = BASE_RAIN_LENGTH + randomNumber(BASE_RAIN_RANDOM_LEN_FACTOR * 2) - BASE_RAIN_RANDOM_LEN_FACTOR; 
			}
		}
	}
	else
	{
		{
			eye->ambientRed  = eye->dayAmbientRed;
			eye->ambientGreen = eye->dayAmbientGreen;
			eye->ambientBlue = eye->dayAmbientBlue;
							 
			eye->lightRed = eye->dayLightRed;
			eye->lightGreen = eye->dayLightGreen;
			eye->lightBlue = eye->dayLightBlue;
		}
	}
}
		
//----------------------------------------------------------------------------------
void Weather::render (void)
{
	if (!weatherActive)
		return;
		
 	if (rainLevel >= 1.0f)
	{
		float ambientFactor = eye->ambientRed + eye->ambientBlue + eye->ambientGreen;
		ambientFactor /= 3.0f;
		
		if (ambientFactor < 170.0f)
			ambientFactor += ambientFactor * 0.5f;
		else
			ambientFactor = 255.0f;
			
		if (ambientFactor < 96.0f)
			ambientFactor = 96.0f;
		
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 0);
		gos_SetRenderState( gos_State_Perspective, 1);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 1);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
		gos_SetRenderState( gos_State_Texture, 0);
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		
		// MC2_RAIN_BATCH=1 → collect all visible streaks into one vertex array and
		// issue a single gos_DrawLines call instead of one per streak (up to 500
		// calls → 1), eliminating per-streak applyRenderStates / setTransform thrash.
		// Default OFF (unset or "0") until validated on mc2_17.
		static int s_rainBatchMode = -1; // -1 = uninitialised
		if (s_rainBatchMode < 0) {
			// DEFAULT-ON since 2026-06-04 (smoke-clean mc2_17, geometrically identical
			// to legacy — same verts/one draw call): only literal "0" opts out.
			s_rainBatchMode = mc2::gate::get<"MC2_RAIN_BATCH">();
		}

		if (s_rainBatchMode) {
			// --- BATCHED PATH: collect all visible streaks, one draw call ---
			std::vector<gos_VERTEX> batchVerts;
			batchVerts.reserve(currentRainDrops * 2);

			for (long i = 0; i < currentRainDrops; i++)
			{
				Stuff::Vector4D screen1, screen2;
				// [PROJECTZ:Both id=weather_raindrop_top]
				PROJECTZ_SITE("weather_raindrop_top", "Both");
				bool onScreen = eye->projectForEffectAdmission(rainDrops[i].position, screen1);
				if (onScreen)
				{
					glm::vec3  botPos = rainDrops[i].position;
					botPos.z -= rainDrops[i].length;

					// [PROJECTZ:Both id=weather_raindrop_bot]
					PROJECTZ_SITE("weather_raindrop_bot", "Both");
					onScreen = eye->projectForEffectAdmission(botPos, screen2);
					if (onScreen)
					{
						unsigned char amb = ambientFactor * screen1.z;
						DWORD rainColor = (amb << 24) + (0xff << 16) + (0xff << 8) + (0xff);

						gos_VERTEX v0, v1;

						v0.x    = screen1.x;
						v0.y    = screen1.y;
						v0.z    = screen1.z;
						v0.rhw  = screen1.w;
						v0.u    = v0.v = 0.0f;
						v0.argb = rainColor;
						v0.frgb = 0xff000000;

						v1.x    = screen2.x;
						v1.y    = screen2.y;
						v1.z    = screen2.z;
						v1.rhw  = screen2.w;
						v1.u    = v1.v = 0.0f;
						v1.argb = rainColor;
						v1.frgb = 0xff000000;

						batchVerts.push_back(v0);
						batchVerts.push_back(v1);
					}
				}
			}

			// One draw call for all visible rain streaks.
			if (!batchVerts.empty())
				gos_DrawLines(batchVerts.data(), (int)batchVerts.size());
		}
		else {
			// --- LEGACY PATH (default): one draw call per streak ---
			for (long i=0;i<currentRainDrops;i++)
			{
				Stuff::Vector4D screen1, screen2;
				// [PROJECTZ:Both id=weather_raindrop_top]
				PROJECTZ_SITE("weather_raindrop_top", "Both");
				bool onScreen = eye->projectForEffectAdmission(rainDrops[i].position,screen1);
				if (onScreen)
				{
					glm::vec3  botPos = rainDrops[i].position;
					botPos.z -= rainDrops[i].length;

					// [PROJECTZ:Both id=weather_raindrop_bot]
					PROJECTZ_SITE("weather_raindrop_bot", "Both");
				onScreen = eye->projectForEffectAdmission(botPos,screen2);
					if (onScreen)
					{
						// Reverse-Z (Step 6d): screen1.z is post-divide reverse-Z
						// [near->1, far->0]. The original (1.0f - z) made rain
						// brightest near the camera under forward-Z (near=0).
						// Under reverse-Z near=1, so the near-bright factor is
						// screen1.z itself. Scene screen-z arithmetic consumer.
						unsigned char amb = ambientFactor * screen1.z;
						DWORD rainColor = (amb << 24) + (0xff << 16) + (0xff << 8) + (0xff);

						//Gotta draw this one!
						gos_VERTEX sVertices[2];

						sVertices[0].x = screen1.x;
						sVertices[0].y = screen1.y;
						sVertices[0].z = screen1.z;
						sVertices[0].rhw = screen1.w;
						sVertices[0].u = sVertices[0].v = 0.0f;
						sVertices[0].argb = rainColor;
						sVertices[0].frgb = 0xff000000;

						sVertices[1].x = screen2.x;
						sVertices[1].y = screen2.y;
						sVertices[1].z = screen2.z;
						sVertices[1].rhw = screen2.w;
						sVertices[1].u = sVertices[1].v = 0.0f;
						sVertices[1].argb = rainColor;
						sVertices[1].frgb = 0xff000000;

						gos_DrawLines(sVertices,2);
					}
				}
			}
		}
	}
}

//----------------------------------------------------------------------------------


