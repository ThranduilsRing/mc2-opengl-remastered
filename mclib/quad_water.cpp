// TerrainQuad: TerrainQuad::drawWater, the CPU water draw and its depth-transition probe.
// Declared in the class headers; shared names in quad_internal.h.
#include "quad_internal.h"


//---------------------------------------------------------------------------
void TerrainQuad::drawWater (void)
{
	float cloudOffsetX = cos(360.0f * DEGREES_TO_RADS * 32.0f * cloudScrollX) * 0.1f;
	float cloudOffsetY = sin(360.0f * DEGREES_TO_RADS * 32.0f * cloudScrollY) * 0.1f;
	
	float sprayOffsetX = cloudScrollX * 10.0f;
	float sprayOffsetY = cloudScrollY * 10.0f;

	//Gotta be able to run the untextured maps!!!
	float oneOverWaterTF = 1.0f / 64.0f;
	float oneOverTF = 1.0f / 64.0f;

	if (Terrain::terrainTextures2)
	{
		oneOverWaterTF = (Terrain::terrainTextures2->getWaterDetailTilingFactor() / Terrain::worldUnitsMapSide);
		oneOverTF = (Terrain::terrainTextures2->getWaterTextureTilingFactor() / Terrain::worldUnitsMapSide);
	}

	if (waterHandle != 0xffffffff)
	{
		numTerrainFaces++;

		// [DEPTH_TRANSITION v1] CPU-water REAL screen-z sample (env-gated,
		// silent default). The zoom-step depth-pop diagnosis has been wrong
		// 3x; sampling the ACTUAL produced value is strictly better evidence
		// than a CPU re-derivation of the Stuff eye->projectForTerrainAdmission
		// pipeline (which is NOT reachable from the gos_terrain_indirect.cpp
		// transition dump site). The drawn CPU-water screen-z is exactly
		// `vertices[k]->wz + WATER_DEPTH_FUDGE` (== WATER_DEPTH_FUDGE_RASTER
		// 0.0025; see the gVertex[*].z assignments at ~:3345 and the
		// terrain_depth_bias.h RASTER regime). We latch the value for the
		// water vertex nearest the FIXED mid-map probe point P so the dump's
		// dz is frame-comparable. world XY is vertices[k]->vx / ->vy (the
		// same pre-projection world coords drawWater emits). g_cpuWaterProbeZ
		// carries a frame stamp because CPU water and the GPU fast path are
		// MUTUALLY EXCLUSIVE per frame (terrain.cpp:1225-1229 early-returns
		// renderWater when WaterFastPathOwnsArmedDraw()): on an armed
		// transition frame this static is the LAST un-armed steady value, not
		// this-frame data. The dump emits g_cpuWaterProbeFrameValid so a
		// stale CPU sample is visibly flagged, never silently trusted.
		extern float    g_cpuWaterProbeZ;
		extern double   g_cpuWaterProbeBestD2;
		extern bool     g_cpuWaterProbeAny;
		static const bool s_depthTransProbe = false;
		if (s_depthTransProbe)
		{
			const float px = Terrain::mapTopLeft3d.x
			                 + Terrain::worldUnitsMapSide * 0.5f;
			const float py = Terrain::mapTopLeft3d.y
			                 - Terrain::worldUnitsMapSide * 0.5f;
			for (auto & vertice : vertices)
			{
				const float dx = vertice->vx - px;
				const float dy = vertice->vy - py;
				const double d2 = static_cast<double>(dx) * dx + static_cast<double>(dy) * dy;
				if (!g_cpuWaterProbeAny || d2 < g_cpuWaterProbeBestD2)
				{
					g_cpuWaterProbeBestD2 = d2;
					g_cpuWaterProbeZ = vertice->wz + WATER_DEPTH_FUDGE;
					g_cpuWaterProbeAny = true;
				}
			}
		}

		//---------------------------------------
		// GOS 3D draw Calls now!

		float minU = 0.0f;
		float maxU = 0.9999999f;

		float minV = 0.0f;
		float maxV = 0.9999999f;

		//-----------------------------------------------------
		// FOG time.  Set Render state to FOG on!
		if (useFog)
		{
			DWORD fogColor = eye->fogColor;
			//gos_SetRenderState( gos_State_Fog, (int)&fogColor);
			gos_SetRenderState( gos_State_Fog, fogColor);
		}
		else
		{
			gos_SetRenderState( gos_State_Fog, 0);
		}

		gos_VERTEX gVertex[3];
		if (uvMode == BOTTOMRIGHT)
		{
			//--------------------------
			// Top Triangle
			gVertex[0].x		= vertices[0]->wx;
			gVertex[0].y		= vertices[0]->wy;
			gVertex[0].z		= vertices[0]->wz + WATER_DEPTH_FUDGE;
			gVertex[0].rhw		= vertices[0]->ww;
			gVertex[0].u		= minU + cloudOffsetX;
			gVertex[0].v		= minV + cloudOffsetY;
			gVertex[0].argb		= vertices[0]->pVertex->selected ? SELECTION_COLOR : vertices[0]->lightRGB;
			gVertex[0].frgb		= vertices[0]->fogRGB;
			gVertex[0].frgb		= (gVertex[0].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[0]->pVertex->terrainType);

			gVertex[1].x		= vertices[1]->wx;
			gVertex[1].y		= vertices[1]->wy;
			gVertex[1].z		= vertices[1]->wz + WATER_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[1]->ww;
			gVertex[1].u		= maxU + cloudOffsetX;
			gVertex[1].v		= minV + cloudOffsetY;
			gVertex[1].argb		= vertices[1]->pVertex->selected ? SELECTION_COLOR : vertices[1]->lightRGB;
			gVertex[1].frgb		= vertices[1]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[1]->pVertex->terrainType);

			gVertex[2].x		= vertices[2]->wx;
			gVertex[2].y		= vertices[2]->wy;
			gVertex[2].z		= vertices[2]->wz + WATER_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[2]->ww;
			gVertex[2].u		= maxU + cloudOffsetX;
			gVertex[2].v		= maxV + cloudOffsetY;
			gVertex[2].argb		= vertices[2]->pVertex->selected ? SELECTION_COLOR : vertices[2]->lightRGB;
			gVertex[2].frgb		= vertices[2]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[2]->pVertex->terrainType);

			gVertex[0].u = (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[0].v = (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[1].u = (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[1].v = (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[2].u = (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[2].v = (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTF + cloudOffsetY; 

			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					if (waterHandle != 0xffffffff)
					{
						DWORD alphaMode0 = Terrain::alphaMiddle;
						DWORD alphaMode1 = Terrain::alphaMiddle;
						DWORD alphaMode2 = Terrain::alphaMiddle;
		
						if (vertices[0]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode0 = Terrain::alphaEdge;
						}
		
						if (vertices[1]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode1 = Terrain::alphaEdge;
						}
		
						if (vertices[2]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode2 = Terrain::alphaEdge;
						}
		
						if (vertices[0]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode0 = Terrain::alphaDeep;
						}
		
						if (vertices[1]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode1 = Terrain::alphaDeep;
						}
		
						if (vertices[2]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode2 = Terrain::alphaDeep;
						}
		
						gVertex[0].argb = (vertices[0]->lightRGB & 0x00ffffff) + alphaMode0;
						gVertex[1].argb = (vertices[1]->lightRGB & 0x00ffffff) + alphaMode1;
						gVertex[2].argb = (vertices[2]->lightRGB & 0x00ffffff) + alphaMode2;
		
						if ((gVertex[0].u > MaxMinUV) || 
							(gVertex[0].v > MaxMinUV) ||
							(gVertex[1].u > MaxMinUV) || 
							(gVertex[1].v > MaxMinUV) ||
							(gVertex[2].u > MaxMinUV) || 
							(gVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(gVertex[0].u,fmax(gVertex[1].u,gVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(gVertex[0].v,fmax(gVertex[1].v,gVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							gVertex[0].u -= maxU;
							gVertex[1].u -= maxU;
							gVertex[2].u -= maxU;
							
							gVertex[0].v -= maxV;
							gVertex[1].v -= maxV;
							gVertex[2].v -= maxV;
						}
						
						if (alphaMode0 + alphaMode1 + alphaMode2)
						{
							mc2::renderSubmissions().addVertices(waterHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER);
						}
					}
						
					//----------------------------------------------------
					// Draw the sky reflection on the water.
					if (useWaterInterestTexture && (waterDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						sVertex[0].u		= ((vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[0].v		= ((Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[1].u		= ((vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[1].v		= ((Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[2].u		= ((vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[2].v		= ((Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[0].argb = (sVertex[0].argb & 0xff000000) + 0xffffff;
						sVertex[1].argb = (sVertex[1].argb & 0xff000000) + 0xffffff; 
						sVertex[2].argb = (sVertex[2].argb & 0xff000000) + 0xffffff; 

						mc2::renderSubmissions().addVertices(waterDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATERDETAIL);
					}
				}
 			}
   
			//--------------------------
			//Bottom Triangle
			//
			// gVertex[0] same as above gVertex[0].
			// gVertex[1] is same as above gVertex[2].
			// gVertex[2] is calced from vertex[3].
			gVertex[1].x		= gVertex[2].x;	
			gVertex[1].y		= gVertex[2].y;	
			gVertex[1].z		= gVertex[2].z;	
			gVertex[1].rhw		= gVertex[2].rhw;
			gVertex[1].u		= gVertex[2].u;
			gVertex[1].v		= gVertex[2].v;	
			gVertex[1].argb		= gVertex[2].argb;
			gVertex[1].frgb		= gVertex[2].frgb;

			gVertex[2].x		= vertices[3]->wx;
			gVertex[2].y		= vertices[3]->wy;
			gVertex[2].z		= vertices[3]->wz + WATER_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[3]->ww;
			gVertex[2].u		= minU + cloudOffsetX;
			gVertex[2].v		= maxV + cloudOffsetY;
			gVertex[2].argb		= vertices[3]->pVertex->selected ? SELECTION_COLOR : vertices[3]->lightRGB;
			gVertex[2].frgb		= vertices[3]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[3]->pVertex->terrainType);

			gVertex[0].u = (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[0].v = (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF + cloudOffsetY;

			gVertex[1].u = (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[1].v = (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTF + cloudOffsetY;

			gVertex[2].u = (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[2].v = (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF + cloudOffsetY;

			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					if (waterHandle != 0xffffffff)
					{
	
						DWORD alphaMode0 = Terrain::alphaMiddle;
						DWORD alphaMode1 = Terrain::alphaMiddle;
						DWORD alphaMode2 = Terrain::alphaMiddle;
		
						if (vertices[0]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode0 = Terrain::alphaEdge;
						}
		
						if (vertices[2]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode1 = Terrain::alphaEdge;
						}
		
						if (vertices[3]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode2 = Terrain::alphaEdge;
						}
		
						if (vertices[0]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode0 = Terrain::alphaDeep;
						}
		
						if (vertices[2]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode1 = Terrain::alphaDeep;
						}
		
						if (vertices[3]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode2 = Terrain::alphaDeep;
						}
		
						gVertex[0].argb = (vertices[0]->lightRGB & 0x00ffffff) + alphaMode0;
						gVertex[1].argb = (vertices[2]->lightRGB & 0x00ffffff) + alphaMode1;
						gVertex[2].argb = (vertices[3]->lightRGB & 0x00ffffff) + alphaMode2;
		
						if ((gVertex[0].u > MaxMinUV) || 
							(gVertex[0].v > MaxMinUV) ||
							(gVertex[1].u > MaxMinUV) || 
							(gVertex[1].v > MaxMinUV) ||
							(gVertex[2].u > MaxMinUV) || 
							(gVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(gVertex[0].u,fmax(gVertex[1].u,gVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(gVertex[0].v,fmax(gVertex[1].v,gVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							gVertex[0].u -= maxU;
							gVertex[1].u -= maxU;
							gVertex[2].u -= maxU;
							
							gVertex[0].v -= maxV;
							gVertex[1].v -= maxV;
							gVertex[2].v -= maxV;
						}
						
						if (alphaMode0 + alphaMode1 + alphaMode2)
						{
							mc2::renderSubmissions().addVertices(waterHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER);
						}
					}
						
					//----------------------------------------------------
					// Draw the sky reflection on the water.
					if (useWaterInterestTexture && (waterDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						sVertex[0].u		= ((vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[0].v		= ((Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[1].u		= ((vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[1].v		= ((Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[2].u		= ((vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[2].v		= ((Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[0].argb = (sVertex[0].argb & 0xff000000) + 0xffffff;
						sVertex[1].argb = (sVertex[1].argb & 0xff000000) + 0xffffff; 
						sVertex[2].argb = (sVertex[2].argb & 0xff000000) + 0xffffff; 

						mc2::renderSubmissions().addVertices(waterDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATERDETAIL);
					}
				}
			}
		}
		else if (uvMode == BOTTOMLEFT)
		{
			//------------------------------
			// Top Triangle.
			gVertex[0].x		= vertices[0]->wx;
			gVertex[0].y		= vertices[0]->wy;
			gVertex[0].z		= vertices[0]->wz + WATER_DEPTH_FUDGE;
			gVertex[0].rhw		= vertices[0]->ww;
			gVertex[0].u		= minU + cloudOffsetX;;
			gVertex[0].v		= minV + cloudOffsetY;;
			gVertex[0].argb		= vertices[0]->pVertex->selected ? SELECTION_COLOR : vertices[0]->lightRGB;
			gVertex[0].frgb		= vertices[0]->fogRGB;
			gVertex[0].frgb		= (gVertex[0].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[0]->pVertex->terrainType);

			gVertex[1].x		= vertices[1]->wx;
			gVertex[1].y		= vertices[1]->wy;
			gVertex[1].z		= vertices[1]->wz + WATER_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[1]->ww;
			gVertex[1].u		= maxU + cloudOffsetX;;
			gVertex[1].v		= minV + cloudOffsetY;;
			gVertex[1].argb		= vertices[1]->pVertex->selected ? SELECTION_COLOR : vertices[1]->lightRGB;
			gVertex[1].frgb		= vertices[1]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[1]->pVertex->terrainType);

			gVertex[2].x		= vertices[3]->wx;
			gVertex[2].y		= vertices[3]->wy;
			gVertex[2].z		= vertices[3]->wz + WATER_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[3]->ww;
			gVertex[2].u		= minU + cloudOffsetX;;
			gVertex[2].v		= maxV + cloudOffsetY;;
			gVertex[2].argb		= vertices[3]->pVertex->selected ? SELECTION_COLOR : vertices[3]->lightRGB;
			gVertex[2].frgb		= vertices[3]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[3]->pVertex->terrainType);

			gVertex[0].u = (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[0].v = (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF + cloudOffsetY;
																   
			gVertex[1].u = (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[1].v = (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[2].u = (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[2].v = (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF + cloudOffsetY; 

			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					if (waterHandle != 0xffffffff)
					{

						DWORD alphaMode0 = Terrain::alphaMiddle;
						DWORD alphaMode1 = Terrain::alphaMiddle;
						DWORD alphaMode2 = Terrain::alphaMiddle;
		
						if (vertices[0]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode0 = Terrain::alphaEdge;
						}
		
						if (vertices[1]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode1 = Terrain::alphaEdge;
						}
		
						if (vertices[3]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode2 = Terrain::alphaEdge;
						}
		
						if (vertices[0]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode0 = Terrain::alphaDeep;
						}
		
						if (vertices[1]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode1 = Terrain::alphaDeep;
						}
		
						if (vertices[3]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode2 = Terrain::alphaDeep;
						}
		
						gVertex[0].argb = (vertices[0]->lightRGB & 0x00ffffff) + alphaMode0;
						gVertex[1].argb = (vertices[1]->lightRGB & 0x00ffffff) + alphaMode1;
						gVertex[2].argb = (vertices[3]->lightRGB & 0x00ffffff) + alphaMode2;
		
						if ((gVertex[0].u > MaxMinUV) || 
							(gVertex[0].v > MaxMinUV) ||
							(gVertex[1].u > MaxMinUV) || 
							(gVertex[1].v > MaxMinUV) ||
							(gVertex[2].u > MaxMinUV) || 
							(gVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(gVertex[0].u,fmax(gVertex[1].u,gVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(gVertex[0].v,fmax(gVertex[1].v,gVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							gVertex[0].u -= maxU;
							gVertex[1].u -= maxU;
							gVertex[2].u -= maxU;
							
							gVertex[0].v -= maxV;
							gVertex[1].v -= maxV;
							gVertex[2].v -= maxV;
						}
						
						if (alphaMode0 + alphaMode1 + alphaMode2)
						{
							mc2::renderSubmissions().addVertices(waterHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER);
						}
					}
						
					//----------------------------------------------------
					// Draw the sky reflection on the water.
					if (useWaterInterestTexture && (waterDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						sVertex[0].u		= ((vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[0].v		= ((Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[1].u		= ((vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[1].v		= ((Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[2].u		= ((vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[2].v		= ((Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[0].argb = (sVertex[0].argb & 0xff000000) + 0xffffff;
						sVertex[1].argb = (sVertex[1].argb & 0xff000000) + 0xffffff; 
						sVertex[2].argb = (sVertex[2].argb & 0xff000000) + 0xffffff; 

						mc2::renderSubmissions().addVertices(waterDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATERDETAIL);
					}
				}
			}

			//---------------------------------------
			// Bottom Triangle.
			// gVertex[0] is same as above gVertex[1]
			// gVertex[1] is new and calced from vertex[2].
			// gVertex[2] is same as above.
			gVertex[0].x		= gVertex[1].x;	
			gVertex[0].y		= gVertex[1].y;	
			gVertex[0].z		= gVertex[1].z;	
			gVertex[0].rhw		= gVertex[1].rhw;	
			gVertex[0].u		= gVertex[1].u;	
			gVertex[0].v		= gVertex[1].v;	
			gVertex[0].argb		= gVertex[1].argb;
			gVertex[0].frgb		= gVertex[1].frgb;

			gVertex[1].x		= vertices[2]->wx;
			gVertex[1].y		= vertices[2]->wy;
			gVertex[1].z		= vertices[2]->wz + WATER_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[2]->ww;
			gVertex[1].u		= maxU + cloudOffsetX;;
			gVertex[1].v		= maxV + cloudOffsetY;;
			gVertex[1].argb		= vertices[2]->pVertex->selected ? SELECTION_COLOR : vertices[2]->lightRGB;
			gVertex[1].frgb		= vertices[2]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[2]->pVertex->terrainType);

			gVertex[0].u = (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[0].v = (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTF + cloudOffsetY;
																   
			gVertex[1].u = (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[1].v = (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[2].u = (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[2].v = (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF + cloudOffsetY; 
																   
			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					if (waterHandle != 0xffffffff)
					{
	
						DWORD alphaMode0 = Terrain::alphaMiddle;
						DWORD alphaMode1 = Terrain::alphaMiddle;
						DWORD alphaMode2 = Terrain::alphaMiddle;
						
						if (vertices[1]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode0 = Terrain::alphaEdge;
						}
		
						if (vertices[2]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{									 
							alphaMode1 = Terrain::alphaEdge;
						}
		
						if (vertices[3]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode2 = Terrain::alphaEdge;
						}
						
						if (vertices[1]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode0 = Terrain::alphaDeep;
						}
		
						if (vertices[2]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode1 = Terrain::alphaDeep;
						}
		
						if (vertices[3]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode2 = Terrain::alphaDeep;
						}
		
						gVertex[0].argb = (vertices[1]->lightRGB & 0x00ffffff) + alphaMode0;
						gVertex[1].argb = (vertices[2]->lightRGB & 0x00ffffff) + alphaMode1;
						gVertex[2].argb = (vertices[3]->lightRGB & 0x00ffffff) + alphaMode2;
		
						if ((gVertex[0].u > MaxMinUV) || 
							(gVertex[0].v > MaxMinUV) ||
							(gVertex[1].u > MaxMinUV) || 
							(gVertex[1].v > MaxMinUV) ||
							(gVertex[2].u > MaxMinUV) || 
							(gVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(gVertex[0].u,fmax(gVertex[1].u,gVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(gVertex[0].v,fmax(gVertex[1].v,gVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							gVertex[0].u -= maxU;
							gVertex[1].u -= maxU;
							gVertex[2].u -= maxU;
							
							gVertex[0].v -= maxV;
							gVertex[1].v -= maxV;
							gVertex[2].v -= maxV;
						}
		 
						if (alphaMode0 + alphaMode1 + alphaMode2)
						{
							mc2::renderSubmissions().addVertices(waterHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER);
						}
					}
						
					//----------------------------------------------------
					// Draw the sky reflection on the water.
					if (useWaterInterestTexture && (waterDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						sVertex[0].u		= ((vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[0].v		= ((Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[1].u		= ((vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[1].v		= ((Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[2].u		= ((vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[2].v		= ((Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[0].argb = (sVertex[0].argb & 0xff000000) + 0xffffff;
						sVertex[1].argb = (sVertex[1].argb & 0xff000000) + 0xffffff; 
						sVertex[2].argb = (sVertex[2].argb & 0xff000000) + 0xffffff; 

						mc2::renderSubmissions().addVertices(waterDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATERDETAIL);
					}
				}
			}
		}
	}
}
