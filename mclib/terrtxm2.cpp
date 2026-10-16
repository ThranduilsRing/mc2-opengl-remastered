//---------------------------------------------------------------------------
//
// TerrTxm2.h -- File contains class definitions for the Terrain Textures
//
//	MechCommander 2 -- Microsoft
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files
#include"platform_windows.h" /*only for declaration of DeleteFile() */

#include "terrtxm2_internal.h"
#include "mc2fmt.h"


bool forceShadowBurnIn = false;

DWORD			TerrainColorMap::terrainTypeIDs[ TOTAL_COLORMAP_TYPES ] =
{
	20000,
	20001,
	20002,
	20003,
	20004
};
//---------------------------------------------------------------------------
// Class TerrainColorMap
void TerrainColorMap::init (void)
{
	ColorMap = nullptr;

	numTextures = 0;
	textures = nullptr;
	txmRAM = nullptr;
	
	numTexturesAcross = fractionPerTexture = 0.0f;
	
	colorMapStarted = false;
	
	detailTextureRAM = nullptr;
	detailTextureNodeIndex = 0xffffffff;
	detailTextureTilingFactor = 30.0f;

	waterTextureRAM = nullptr;
	waterTextureNodeIndex = 0xffffffff;
	waterTextureTilingFactor = 48.0f;

	numWaterDetailFrames = 0;
	int i;
	for (i = 0; i < MAX_WATER_DETAIL_TEXTURES; i++)
	{
		waterDetailNodeIndex[i] = 0xffffffff;
	}
	waterDetailFrameRate = 8;
	waterDetailTilingFactor = 45.0f;
	
	hGauss = 5.0f;
	roughDistance = -1.0f;

	normalMapTextures = nullptr;
	numNormalMapTextures = 0;
	detailNormalNodeIndex = 0xffffffff;
	hasNormalMap = false;
	lastResultTexture = -1;
	cpuDispAlpha = nullptr;
	cpuDispAlphaSize = 0;
	cpuColorMap = nullptr;
	cpuColorMapSize = 0;
	ktx2ColormapPath[0] = '\0';
}

MemoryPtr TerrainColorMap::ownColorMap (size_t bytes)
{
	colorMapStore.assign(bytes ? bytes : 1, 0);
	ColorMap = colorMapStore.data();
	return ColorMap;
}

void TerrainColorMap::ownTextureHandles (DWORD count)
{
	texturesStore.assign(count ? count : 1, ColorMapTextures{0xffffffff});
	textures = texturesStore.data();
}

void TerrainColorMap::ownTileRam (DWORD count)
{
	constexpr size_t tileBytes = sizeof(DWORD) * COLOR_MAP_TEXTURE_SIZE * COLOR_MAP_TEXTURE_SIZE;
	txmRamStore.assign(count ? count : 1, ColorMapRAM{nullptr});
	tileRamStore.assign(tileBytes * (count ? count : 1), 0);
	for (DWORD i = 0; i < count; i++)
		txmRamStore[i].ourRAM = tileRamStore.data() + tileBytes * i;
	txmRAM = txmRamStore.data();
}

// The full colour map and the per-tile copies handed to the texture manager are only needed while a load or bake runs.
void TerrainColorMap::releaseColorMapScratch (void)
{
	txmRAM = nullptr;
	ColorMap = nullptr;
	std::vector<ColorMapRAM>().swap(txmRamStore);
	std::vector<unsigned char>().swap(tileRamStore);
	std::vector<unsigned char>().swap(colorMapStore);
}

void TerrainColorMap::destroy (void)
{
	// [TXMMGR-TEXTURE-LEAK-FIX-1] Release the master texture-node slots this
	// per-mission colormap allocated. The colormap tile + normal-map nodes are
	// pinned neverFLUSH (setTextureNeverFlush, see init()), so the per-mission
	// flush() skips them; without an explicit removeTextureNode() here their
	// 4096-slot masterTextureNodes[] entries accumulate every mission and
	// eventually STOP("TOO Many textures"). These textures are rebuilt next
	// mission in init(fileName), so freeing them on unload is correct.
	if (mcTextureManager)
	{
		if (textures)
		{
			for (unsigned long i=0;i<numTextures;i++)
			{
				if (textures[i].mcTextureNodeIndex != 0xffffffff)
				{
					mcTextureManager->removeTextureNode(textures[i].mcTextureNodeIndex);
					textures[i].mcTextureNodeIndex = 0xffffffff;
				}
			}
		}
		if (normalMapTextures)
		{
			for (unsigned long i=0;i<numNormalMapTextures;i++)
			{
				if (normalMapTextures[i].mcTextureNodeIndex != 0xffffffff)
				{
					mcTextureManager->removeTextureNode(normalMapTextures[i].mcTextureNodeIndex);
					normalMapTextures[i].mcTextureNodeIndex = 0xffffffff;
				}
			}
		}
		if (detailTextureNodeIndex != 0xffffffff)
		{
			mcTextureManager->removeTextureNode(detailTextureNodeIndex);
			detailTextureNodeIndex = 0xffffffff;
		}
		if (detailNormalNodeIndex != 0xffffffff)
		{
			mcTextureManager->removeTextureNode(detailNormalNodeIndex);
			detailNormalNodeIndex = 0xffffffff;
		}
		if (waterTextureNodeIndex != 0xffffffff)
		{
			mcTextureManager->removeTextureNode(waterTextureNodeIndex);
			waterTextureNodeIndex = 0xffffffff;
		}
		for (DWORD i=0;i<numWaterDetailFrames;i++)
		{
			if (waterDetailNodeIndex[i] != 0xffffffff)
			{
				mcTextureManager->removeTextureNode(waterDetailNodeIndex[i]);
				waterDetailNodeIndex[i] = 0xffffffff;
			}
		}
	}

	textures = nullptr;
	std::vector<ColorMapTextures>().swap(texturesStore);
	normalMapTextures = nullptr;
	std::vector<ColorMapTextures>().swap(normalMapStore);
	releaseColorMapScratch();

 	numTextures = 0;

	retireCpuCopies();
}

//---------------------------------------------------------------------------
void TerrainColorMap::retireCpuCopies (void)
{
	if (cpuDispAlpha) { free(cpuDispAlpha); cpuDispAlpha = nullptr; }
	cpuDispAlphaSize = 0;
	if (cpuColorMap) { free(cpuColorMap); cpuColorMap = nullptr; }
	cpuColorMapSize = 0;
}

//---------------------------------------------------------------------------
void TerrainColorMap::getColorMapData (MemoryPtr ourRAM, long index, long width)
{
	long numWide = width / COLOR_MAP_TEXTURE_SIZE;

	long startCol = ((index % numWide) * COLOR_MAP_TEXTURE_SIZE);
	long startRow = ((index / numWide) * width * COLOR_MAP_TEXTURE_SIZE);

	// 1px overlap into neighbor tile to hide seams (constant, not cumulative)
	if (startCol > 0)
		startCol -= 1;
	if (startRow > 0)
		startRow -= width;
	
	MemoryPtr ourColor = ColorMap + (startCol + startRow) * sizeof(DWORD);
	
	for (long i=0;i<COLOR_MAP_TEXTURE_SIZE;i++)
	{
		memcpy(ourRAM,ourColor,COLOR_MAP_TEXTURE_SIZE * sizeof(DWORD));
		ourRAM += COLOR_MAP_TEXTURE_SIZE * sizeof(DWORD);
		ourColor += width * sizeof(DWORD);
	}
}

float textureOffset = 0.4f;
//---------------------------------------------------------------------------
DWORD TerrainColorMap::resolveTextureHandle (VertexPtr vMin, VertexPtr vMax, TerrainUVData *uvData, long* resultTextureOut, bool realizeTexture)
{
	float posX = 0.0f, posY = 0.0f, maxX = 0.0f, maxY = 0.0f;
	
	Stuff::Vector3D pos1(vMin->vx,vMin->vy,0.0f),pos2(vMax->vx,vMax->vy,0.0f);
	if (Terrain::IsValidTerrainPosition(pos1) && Terrain::IsValidTerrainPosition(pos2))
	{
		//-----------------------------------------------------------------------------------------------------
		//Since we have quads with tris in both directions, we need to determine what max and min really are!!
		if (vMin->vx > vMax->vx)
		{
			//Tris are in a weird arrangement.  Swap max and min X.
			posX = (vMax->vx - Terrain::mapTopLeft3d.x) * Terrain::oneOverWorldUnitsMapSide;
			maxX = (vMin->vx - Terrain::mapTopLeft3d.x) * Terrain::oneOverWorldUnitsMapSide;
		}
		else
		{
			//Tris in normal arrangement.  Just calc 'em out.
			posX = (vMin->vx - Terrain::mapTopLeft3d.x) * Terrain::oneOverWorldUnitsMapSide;
			maxX = (vMax->vx - Terrain::mapTopLeft3d.x) * Terrain::oneOverWorldUnitsMapSide;
		}
		
		posY = (Terrain::mapTopLeft3d.y - vMin->vy) * Terrain::oneOverWorldUnitsMapSide;
		maxY = (Terrain::mapTopLeft3d.y - vMax->vy) * Terrain::oneOverWorldUnitsMapSide;
		
		//------------------------------------------------------
		// Figure out which texture by the vertex v's position.
	   
		//We now have the position in x and y as fraction from 0.0 to 1.0
		// The texture is easy.  Just mutiply by number of textures across.
		#define EDGE_ADJUST_FACTOR		0.0005f

        long txmNumX = (posX + EDGE_ADJUST_FACTOR) * numTexturesAcross;
		long txmNumY = (posY + EDGE_ADJUST_FACTOR) * numTexturesAcross;
        //long txmNumX = (posX) * numTexturesAcross;
		//long txmNumY = (posY) * numTexturesAcross;

		long resultTexture = txmNumX + (txmNumY * numTexturesAcross);
		
		// Now we need the UV Coords in this texture.  This is the fraction from 0.0 to 1.0
		// across the texture we are as vMin and vMax.
		//
		// IMPORTANT OPTIMIZATION NOTE:  We only need to do this ONCE per terrain face at load time.
		// It never changes!!!!!!!!
		// We could calc as we get to the terrain faces and just know they're done, too!
		// Could also store in the new PostcompVertex Structure!!
		
		//Also need to adjust UVs by texturePixelAdjust to handle overlaping correctly.
		float minX = (float)txmNumX * fractionPerTexture;
		uvData->maxU = (maxX - minX) * numTexturesAcross + 0.5f / COLOR_MAP_RES;
		uvData->minU = (posX - minX) * numTexturesAcross + 0.5f / COLOR_MAP_RES;
		
		float minY = (float)txmNumY * fractionPerTexture;
		uvData->maxV = (maxY - minY) * numTexturesAcross + 0.5f / COLOR_MAP_RES;
		uvData->minV = (posY - minY) * numTexturesAcross + 0.5f / COLOR_MAP_RES;
		
		
		float textureAdjustFactor = (textureOffset / COLOR_MAP_RES);
		if (uvData->minU <= textureAdjustFactor)
			uvData->minU = textureAdjustFactor;
		//else
			//uvData->minU += xAdjust;
			
		if (uvData->minV <= textureAdjustFactor)
			uvData->minV = textureAdjustFactor;
		//else
			//uvData->minV += yAdjust;
			
		if (uvData->maxU >= 1.0f)
			uvData->maxU = 1.0f - textureAdjustFactor;
		//else
			//uvData->maxU += xAdjust;
		
		if (uvData->maxV >= 1.0f)
			uvData->maxV = 1.0f - textureAdjustFactor;
		//else
			//uvData->maxV += yAdjust;
		

#if 0
		if ((uvData->minU < 0.0f) || (uvData->minU >= 1.0f))
			STOP(("UvData our of range in minU %f",uvData->minU));
			
		if ((uvData->minV < 0.0f) || (uvData->minV >= 1.0f))
			STOP(("UvData our of range in minV %f",uvData->minV));
	 
		if ((uvData->maxU < 0.0f) || (uvData->maxU >= 1.0f))
			STOP(("UvData our of range in maxU %f",uvData->maxU));
	 
		if ((uvData->maxV < 0.0f) || (uvData->maxV >= 1.0f))
			STOP(("UvData our of range in maxV %f",uvData->maxV));
#endif
	
		{
			long maxTex = (long)(numTexturesAcross * numTexturesAcross);
			if (resultTexture < 0 || resultTexture >= maxTex)
			{
				mc2::fmt::print_err("[TERRAIN] OOB! resultTexture={:d} max={:d} posX={:f} posY={:f} txmX={:d} txmY={:d}\n", mc2::fmt::as_i32(resultTexture), mc2::fmt::as_i32(maxTex), posX, posY, mc2::fmt::as_i32(txmNumX), mc2::fmt::as_i32(txmNumY));
				fflush(stderr);
				uvData->minU = uvData->minV = uvData->maxU = uvData->maxV = 0.0f;
				return 0;
			}
		}
		lastResultTexture = resultTexture;
		if (resultTextureOut)
			*resultTextureOut = resultTexture;
		if (!textures) {
			// COLORMAP-TILES-RETIRE-1: tile textures retired — uvData was already
			// computed correctly above from numTexturesAcross/fractionPerTexture.
			// Do NOT zero uvData: it carries per-tile UV [0,1] that the indirect
			// vertex shader forwards as Texcoord for detail/normal tiling.
			// Zeroing it would set Texcoord=(0,0) for all quads → all detail
			// normals sample the same corner pixel → flat terrain (regression).
			return 0xFFFFFFFFu;
		}
		if (realizeTexture)
		{
			ZoneScopedN("TerrainColorMap::getTextureHandle realizeTexture");
			tex_resolve(textures[resultTexture].mcTextureNodeIndex);
		}
		return textures[resultTexture].mcTextureNodeIndex;
	}

	lastResultTexture = -1;
	if (resultTextureOut)
		*resultTextureOut = -1;
	uvData->minU = uvData->minV = uvData->maxU = uvData->maxV = 0.0f;
	return 0;
}

//---------------------------------------------------------------------------
DWORD TerrainColorMap::getTextureHandle (VertexPtr vMin, VertexPtr vMax, TerrainUVData *uvData)
{
	return resolveTextureHandle(vMin, vMax, uvData, nullptr, true);
}

long TerrainColorMap::saveDetailTexture(const char *fileName)
{
	char dName[1024];
	mc2::fmt::to_buf(dName, "{}.detail", mc2::fmt::safe(fileName));
	FullPathFileName detailFile;
	detailFile.init(texturePath,dName,".tga");
	return mcTextureManager->saveTexture(detailTextureNodeIndex, detailFile);
}

long TerrainColorMap::saveWaterTexture(const char *fileName)
{
	char dName[1024];
	mc2::fmt::to_buf(dName, "{}.water", mc2::fmt::safe(fileName));
	FullPathFileName waterFile;
	waterFile.init(texturePath,dName,".tga");
	return mcTextureManager->saveTexture(waterTextureNodeIndex, waterFile);
}

long TerrainColorMap::saveWaterDetail(const char *fileName)
{
	long result = NO_ERR;
	int i;
	for (i = 0; i < getWaterDetailNumFrames(); i++)
	{
		char dName[1024];
		mc2::fmt::to_buf(dName, "{}.water{:04d}", mc2::fmt::safe(fileName), mc2::fmt::as_i32(i));
		FullPathFileName waterDetailFile;
		waterDetailFile.init(texturePath,dName,".tga");
		long lResult = mcTextureManager->saveTexture(waterDetailNodeIndex[i], waterDetailFile);
		if (NO_ERR != lResult)
		{
			result = lResult;
		}
	}
	for (i = getWaterDetailNumFrames(); i < MAX_WATER_DETAIL_TEXTURES; i++)
	{
		char dName[1024];
		mc2::fmt::to_buf(dName, "{}.water{:04d}", mc2::fmt::safe(fileName), mc2::fmt::as_i32(i));
		FullPathFileName waterDetailFile;
		waterDetailFile.init(texturePath,dName,".tga");
		File textureFile;
		long textureFileOpenResult = textureFile.open(waterDetailFile);
		textureFile.close();
		if (NO_ERR != textureFileOpenResult)
		{
			break;
		}
		textureFileOpenResult = textureFile.create(waterDetailFile);
		textureFile.deleteFile(); /*what is this supposed to do?*/
		textureFile.close();
		DeleteFile(waterDetailFile);
	}
	return result;
}

long TerrainColorMap::saveTilingFactors(FitIniFile *fitFile)
{
	long result = NO_ERR;
	fitFile->writeIdFloat( "DetailTextureTilingFactor", getDetailTilingFactor() );
	fitFile->writeIdFloat( "WaterTextureTilingFactor", getWaterTextureTilingFactor() );
	fitFile->writeIdFloat( "WaterDetailTilingFactor", getWaterDetailTilingFactor() );
	return result;
}
