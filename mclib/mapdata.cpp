//---------------------------------------------------------------------------
//
// MapData.cpp -- File contains class code for the terrain mesh
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files
#ifndef MAPDATA_H
#include"mapdata.h"
#endif

#include"../GameOS/gameos/gos_profiler.h"
#include"tex_resolve_table.h"

#ifndef TERRAIN_H
#include"terrain.h"
#endif

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#include<gameos.hpp>
#include"terrtxm2.h"
#include<vector>
#include<algorithm>

//---------------------------------------------------------------------------
// c'tors for postCompVertex
PostcompVertex& PostcompVertex::operator=( const PostcompVertex& src )
{
	if ( &src != this )
	{
		vertexNormal = src.vertexNormal;
		elevation = src.elevation;
		textureData = src.textureData;
		localRGBLight = src.localRGBLight;
		terrainType = src.terrainType;
		selected = src.selected;
		shadow = src.shadow;
		highlighted = src.highlighted;
	}

	return *this;
}

//---------------------------------------------------------------------------
PostcompVertex::PostcompVertex( const PostcompVertex& src )
{
	vertexNormal = src.vertexNormal;
	elevation = src.elevation;
	textureData = src.textureData;
	localRGBLight = src.localRGBLight;
	terrainType = src.terrainType;
	selected = src.selected;
	shadow = src.shadow;
	highlighted = src.highlighted;
}

//---------------------------------------------------------------------------
PostcompVertex::PostcompVertex()
{
	elevation = 0.0f;
	textureData = 0;
	localRGBLight = 0;
	terrainType = 0;
	selected = 0;

}

//---------------------------------------------------------------------------
// class MapData
float					MapData::waterDepth = 0.0f;
float					MapData::shallowDepth = 0.0f;
float					MapData::alphaDepth = 0.0f;
DWORD					MapData::WaterTXMData = 0xffffffff;

float					cloudScrollSpeedX = 0.002f;
float					cloudScrollSpeedY = 0.002f;

float					cloudScrollX = 0.0f;
float					cloudScrollY = 0.0f;
static int				gTerrainTilesRealizedDuringLoad = 0;
static int				gTerrainTilesFirstRealizedDuringGameplay = 0;
static int				gTerrainColormapRealizedLoad = 0;
static int				gTerrainColormapRealizedGameplay = 0;
static int				gTerrainDetailRealizedLoad = 0;
static int				gTerrainDetailRealizedGameplay = 0;
static int				gTerrainOverlayRealizedLoad = 0;
static int				gTerrainOverlayRealizedGameplay = 0;
static int				gTerrainBaseRealizedLoad = 0;
static int				gTerrainBaseRealizedGameplay = 0;

static long worldQuadCacheWidth (void)
{
	return Terrain::realVerticesMapSide - 1;
}

static long worldQuadCacheIndex (long tileR, long tileC)
{
	return tileC + tileR * worldQuadCacheWidth();
}

static long worldQuadUVMode (long tileR, long tileC)
{
	return ((tileR & 1) == (tileC & 1)) ? BOTTOMRIGHT : BOTTOMLEFT;
}

static bool isTextureNodeResidentLocal (DWORD nodeId)
{
	return mcTextureManager->isTextureNodeResident(nodeId);
}

static void fillWorldCacheVertex (Vertex& vertex, PostcompVertexPtr pVertex, long tileR, long tileC)
{
	vertex.init();
	vertex.pVertex = pVertex;
	vertex.vx = float(tileC - Terrain::halfVerticesMapSide) * Terrain::worldUnitsPerVertex;
	vertex.vy = float(Terrain::halfVerticesMapSide - tileR) * Terrain::worldUnitsPerVertex;
	vertex.posTile = (tileR << 16) + (tileC & 0x0000ffff);
}

void *MapData::operator new (size_t mySize)
{
	void *result = Terrain::terrainHeap->Malloc(mySize);
	return(result);
}

//---------------------------------------------------------------------------
void MapData::operator delete (void *us)
{
	Terrain::terrainHeap->Free(us);
}
		
//---------------------------------------------------------------------------
void MapData::destroy (void)
{
	HeapManager::destroy();

	invalidateTerrainFaceCache();

	if (blankVertex)
	{
		delete blankVertex;
		blankVertex = NULL;
	}
}

//---------------------------------------------------------------------------
void MapData::newInit (long numVertices)
{
	if (heap)
		destroy();

	long result = createHeap((numVertices+1) * sizeof(PostcompVertex));
	gosASSERT(result == NO_ERR);

	result = commitHeap();
	gosASSERT(result == NO_ERR);

	//-----------------------------------------------------------
	// There is one (1) block of memory for the ENTIRE terrain now.
	// Since we no longer cache any aspect of the terrain, this is
	// FAR more efficient and easier to understand.
	MemoryPtr start = getHeapPtr();
	blocks = (PostcompVertexPtr)start;

	PostcompVertex* pTmp = blocks;

	// gotta set all of the z's to 1
	for ( int i = 0; i < numVertices; ++i )
	{
		pTmp->vertexNormal.z = 1.0;
		pTmp->terrainType = MC_MUD_TYPE;
		pTmp->textureData = 0xffff0000;
		pTmp++;
	}

	Terrain::recalcLight = true;
	Terrain::recalcShadows = false;
	
	invalidateTerrainFaceCache();
	calcTransitions();
}

//---------------------------------------------------------------------------
void MapData::newInit (PacketFile* newFile, long numVertices)
{
	newInit( numVertices );

	newFile->readPacket(newFile->getCurrentPacket(), (MemoryPtr)blocks );

	calcTransitions();
}

//---------------------------------------------------------------------------
long MapData::save( PacketFile* file, int whichPacket )
{
	return file->writePacket( whichPacket, (unsigned char*)blocks, 
		Terrain::realVerticesMapSide * Terrain::realVerticesMapSide *sizeof(PostcompVertex ) );
}

//---------------------------------------------------------------------------
void MapData::invalidateTerrainFaceCache (void)
{
	if (terrainFaceCache)
	{
		Terrain::terrainHeap->Free(terrainFaceCache);
		terrainFaceCache = NULL;
	}
}

//---------------------------------------------------------------------------
const MapData::WorldQuadTerrainCacheEntry* MapData::getTerrainFaceCacheEntry (long tileR, long tileC) const
{
	if (!terrainFaceCache)
		return NULL;

	const long maxTile = Terrain::realVerticesMapSide - 1;
	if ((tileR < 0) || (tileC < 0) || (tileR >= maxTile) || (tileC >= maxTile))
		return NULL;

	return &terrainFaceCache[worldQuadCacheIndex(tileR, tileC)];
}

//---------------------------------------------------------------------------
void MapData::buildTerrainFaceCache (volatile float* progress, float progressRange)
{
	ZoneScopedN("MapData::buildTerrainFaceCache");
	if (!blocks || !Terrain::terrainTextures2 || !Terrain::terrainTextures)
		return;

	const long cacheWidth = worldQuadCacheWidth();
	const long cacheCount = cacheWidth * cacheWidth;
	if (!terrainFaceCache)
	{
		terrainFaceCache = (WorldQuadTerrainCacheEntry*)Terrain::terrainHeap->Malloc(sizeof(WorldQuadTerrainCacheEntry) * cacheCount);
		gosASSERT(terrainFaceCache != NULL);
	}

	for (long i = 0; i < cacheCount; ++i)
		terrainFaceCache[i].init();

	Vertex worldVertices[4];
	for (long tileR = 0; tileR < cacheWidth; ++tileR)
	{
		for (long tileC = 0; tileC < cacheWidth; ++tileC)
		{
			const long index = tileC + tileR * Terrain::realVerticesMapSide;
			PostcompVertexPtr pVertex1 = &blocks[index];
			PostcompVertexPtr pVertex2 = pVertex1 + 1;
			PostcompVertexPtr pVertex3 = pVertex1 + Terrain::realVerticesMapSide + 1;
			PostcompVertexPtr pVertex4 = pVertex1 + Terrain::realVerticesMapSide;

			fillWorldCacheVertex(worldVertices[0], pVertex1, tileR, tileC);
			fillWorldCacheVertex(worldVertices[1], pVertex2, tileR, tileC + 1);
			fillWorldCacheVertex(worldVertices[2], pVertex3, tileR + 1, tileC + 1);
			fillWorldCacheVertex(worldVertices[3], pVertex4, tileR + 1, tileC);

			WorldQuadTerrainCacheEntry& entry = terrainFaceCache[worldQuadCacheIndex(tileR, tileC)];
			const DWORD baseTexture = pVertex1->textureData & 0x0000ffff;
			const bool isCement = Terrain::terrainTextures->isCement(baseTexture);
			const bool isAlpha = Terrain::terrainTextures->isAlpha(baseTexture);

			if (isCement)
				entry.flags |= TERRAIN_CACHE_CEMENT;
			if (isAlpha)
				entry.flags |= TERRAIN_CACHE_ALPHA;

			if (!isCement)
			{
				const long uvMode = worldQuadUVMode(tileR, tileC);
				VertexPtr vMin = (uvMode == BOTTOMRIGHT) ? &worldVertices[0] : &worldVertices[1];
				VertexPtr vMax = (uvMode == BOTTOMRIGHT) ? &worldVertices[2] : &worldVertices[3];
				entry.terrainHandle = Terrain::terrainTextures2->resolveTextureHandle(vMin, vMax, &entry.uvData, NULL, false);
				entry.terrainDetailHandle = Terrain::terrainTextures2->peekDetailHandle();
				entry.flags |= TERRAIN_CACHE_COLORMAP;
			}
			else if (isAlpha)
			{
				entry.overlayHandle = Terrain::terrainTextures->peekTextureHandle(baseTexture);
				entry.terrainHandle = Terrain::terrainTextures2->resolveTextureHandle(&worldVertices[1], &worldVertices[3], &entry.uvData, NULL, false);
				entry.terrainDetailHandle = Terrain::terrainTextures2->peekDetailHandle();
				entry.flags |= TERRAIN_CACHE_COLORMAP;
			}
			else
			{
				entry.terrainHandle = Terrain::terrainTextures->peekTextureHandle(baseTexture);
			}

			entry.flags |= TERRAIN_CACHE_VALID;
		}

		if (progress && (cacheWidth > 0))
			*progress += progressRange / float(cacheWidth);
	}
}

//---------------------------------------------------------------------------
bool MapData::ensureTerrainFaceCacheEntryResident (const WorldQuadTerrainCacheEntry& entry, bool duringMissionLoad) const
{
	bool realizedColorMapTile = false;
	bool realizedDetail = false;
	bool realizedOverlay = false;
	bool realizedBase = false;

	if (entry.usesColorMap() && !isTextureNodeResidentLocal(entry.terrainHandle))
	{
		tex_resolve(entry.terrainHandle);
		realizedColorMapTile = true;
	}

	if (!isTextureNodeResidentLocal(entry.terrainDetailHandle))
	{
		tex_resolve(entry.terrainDetailHandle);
		realizedDetail = true;
	}

	if (!isTextureNodeResidentLocal(entry.overlayHandle))
	{
		tex_resolve(entry.overlayHandle);
		realizedOverlay = true;
	}

	if (!entry.usesColorMap() && !isTextureNodeResidentLocal(entry.terrainHandle))
	{
		tex_resolve(entry.terrainHandle);
		realizedBase = true;
	}

	if (realizedColorMapTile)
	{
		if (duringMissionLoad)
		{
			++gTerrainTilesRealizedDuringLoad;
			TracyPlot("Terrain tiles realized during mission load", int64_t(gTerrainTilesRealizedDuringLoad));
			++gTerrainColormapRealizedLoad;
			TracyPlot("Terrain colormap realized load", int64_t(gTerrainColormapRealizedLoad));
		}
		else
		{
			++gTerrainTilesFirstRealizedDuringGameplay;
			TracyPlot("Terrain tiles first realized during gameplay", int64_t(gTerrainTilesFirstRealizedDuringGameplay));
			++gTerrainColormapRealizedGameplay;
			TracyPlot("Terrain colormap realized gameplay", int64_t(gTerrainColormapRealizedGameplay));
		}
	}
	if (realizedDetail)
	{
		if (duringMissionLoad) { ++gTerrainDetailRealizedLoad; TracyPlot("Terrain detail realized load", int64_t(gTerrainDetailRealizedLoad)); }
		else                   { ++gTerrainDetailRealizedGameplay; TracyPlot("Terrain detail realized gameplay", int64_t(gTerrainDetailRealizedGameplay)); }
	}
	if (realizedOverlay)
	{
		if (duringMissionLoad) { ++gTerrainOverlayRealizedLoad; TracyPlot("Terrain overlay realized load", int64_t(gTerrainOverlayRealizedLoad)); }
		else                   { ++gTerrainOverlayRealizedGameplay; TracyPlot("Terrain overlay realized gameplay", int64_t(gTerrainOverlayRealizedGameplay)); }
	}
	if (realizedBase)
	{
		if (duringMissionLoad) { ++gTerrainBaseRealizedLoad; TracyPlot("Terrain base realized load", int64_t(gTerrainBaseRealizedLoad)); }
		else                   { ++gTerrainBaseRealizedGameplay; TracyPlot("Terrain base realized gameplay", int64_t(gTerrainBaseRealizedGameplay)); }
	}

	return realizedColorMapTile;
}

//---------------------------------------------------------------------------
void MapData::warmTerrainFaceCacheResidency (volatile float* progress, float progressRange)
{
	ZoneScopedN("MapData::warmTerrainFaceCacheResidency");
	if (!terrainFaceCache)
		return;

	gTerrainTilesRealizedDuringLoad = 0;
	gTerrainTilesFirstRealizedDuringGameplay = 0;
	gTerrainColormapRealizedLoad = 0;
	gTerrainColormapRealizedGameplay = 0;
	gTerrainDetailRealizedLoad = 0;
	gTerrainDetailRealizedGameplay = 0;
	gTerrainOverlayRealizedLoad = 0;
	gTerrainOverlayRealizedGameplay = 0;
	gTerrainBaseRealizedLoad = 0;
	gTerrainBaseRealizedGameplay = 0;
	TracyPlot("Terrain tiles realized during mission load", int64_t(gTerrainTilesRealizedDuringLoad));
	TracyPlot("Terrain tiles first realized during gameplay", int64_t(gTerrainTilesFirstRealizedDuringGameplay));
	TracyPlot("Terrain colormap realized load", int64_t(0));
	TracyPlot("Terrain colormap realized gameplay", int64_t(0));
	TracyPlot("Terrain detail realized load", int64_t(0));
	TracyPlot("Terrain detail realized gameplay", int64_t(0));
	TracyPlot("Terrain overlay realized load", int64_t(0));
	TracyPlot("Terrain overlay realized gameplay", int64_t(0));
	TracyPlot("Terrain base realized load", int64_t(0));
	TracyPlot("Terrain base realized gameplay", int64_t(0));

	const long cacheWidth = worldQuadCacheWidth();
	std::vector<DWORD> handlesToWarm;
	handlesToWarm.reserve(cacheWidth * cacheWidth);

	for (long tileR = 0; tileR < cacheWidth; ++tileR)
	{
		for (long tileC = 0; tileC < cacheWidth; ++tileC)
		{
			const WorldQuadTerrainCacheEntry& entry = terrainFaceCache[worldQuadCacheIndex(tileR, tileC)];
			if (!entry.isValid())
				continue;

			if (entry.usesColorMap() && entry.terrainHandle != 0xffffffff && entry.terrainHandle != 0)
				handlesToWarm.push_back(entry.terrainHandle);
			if (entry.terrainDetailHandle != 0xffffffff && entry.terrainDetailHandle != 0)
				handlesToWarm.push_back(entry.terrainDetailHandle);
			if (entry.overlayHandle != 0xffffffff && entry.overlayHandle != 0)
				handlesToWarm.push_back(entry.overlayHandle);
			if (!entry.usesColorMap() && entry.terrainHandle != 0xffffffff && entry.terrainHandle != 0)
				handlesToWarm.push_back(entry.terrainHandle);
		}
	}

	std::sort(handlesToWarm.begin(), handlesToWarm.end());
	handlesToWarm.erase(std::unique(handlesToWarm.begin(), handlesToWarm.end()), handlesToWarm.end());

	const size_t totalHandles = handlesToWarm.size();
	for (size_t i = 0; i < totalHandles; ++i)
	{
		const DWORD handle = handlesToWarm[i];
		const bool residentBefore = isTextureNodeResidentLocal(handle);
		tex_resolve(handle);
		if (!residentBefore)
		{
			bool counted = false;
			for (long tileR = 0; tileR < cacheWidth && !counted; ++tileR)
			{
				for (long tileC = 0; tileC < cacheWidth; ++tileC)
				{
					const WorldQuadTerrainCacheEntry& entry = terrainFaceCache[worldQuadCacheIndex(tileR, tileC)];
					if (entry.usesColorMap() && entry.terrainHandle == handle)
					{
						++gTerrainTilesRealizedDuringLoad;
						TracyPlot("Terrain tiles realized during mission load", int64_t(gTerrainTilesRealizedDuringLoad));
						counted = true;
						break;
					}
				}
			}
		}

		if (progress && totalHandles)
			*progress += progressRange / float(totalHandles);
	}
}

//---------------------------------------------------------------------------
void MapData::highlightAllTransitionsOver2 (void)
{
	unhighlightAll();
	
	PostcompVertexPtr currentVertex = blocks;

	//--------------------------------------------------------------------------
	// This pass is used to mark the transitions over the value of 2 

	for (long y=0;y<(Terrain::realVerticesMapSide-1);y++)
	{
		for (long x=0;x<(Terrain::realVerticesMapSide-1);x++)
		{
			//-----------------------------------------------
			// Get the data needed to make this terrain quad
			PostcompVertex *pVertex1 = currentVertex;
			PostcompVertex *pVertex2 = currentVertex + 1;
			PostcompVertex *pVertex3 = currentVertex + Terrain::realVerticesMapSide + 1;
			PostcompVertex *pVertex4 = currentVertex + Terrain::realVerticesMapSide;
				
			//-------------------------------------------------------------------------------
			long totalNotEqual1 = abs((pVertex1->terrainType != pVertex2->terrainType) +
									(pVertex3->terrainType != pVertex4->terrainType) + 
									(pVertex2->terrainType != pVertex4->terrainType));

			long totalNotEqual2 = abs((pVertex2->terrainType != pVertex3->terrainType) +
									(pVertex1->terrainType != pVertex4->terrainType) + 
									(pVertex1->terrainType != pVertex3->terrainType));
									
			if ((totalNotEqual1 >= 2) && (totalNotEqual2 >= 2))
			{
				pVertex1->highlighted = true;
				pVertex2->highlighted = true;
				pVertex3->highlighted = true;
				pVertex4->highlighted = true;
			}
			
 			currentVertex++;
		}
			
		currentVertex++;
	}
}

//---------------------------------------------------------------------------
void MapData::calcTransitions()
{
	PostcompVertexPtr currentVertex = blocks;

	//--------------------------------------------------------------------------
	// This pass is used to calc the transitions.  

	for (long y=0;y<(Terrain::realVerticesMapSide-1);y++)
	{
		for (long x=0;x<(Terrain::realVerticesMapSide-1);x++)
		{
			//-----------------------------------------------
			// Get the data needed to make this terrain quad
			PostcompVertex *pVertex1 = currentVertex;
			PostcompVertex *pVertex2 = currentVertex + 1;
			PostcompVertex *pVertex3 = currentVertex + Terrain::realVerticesMapSide + 1;
			PostcompVertex *pVertex4 = currentVertex + Terrain::realVerticesMapSide;
				
			//-------------------------------------------------------------------------------
			// Store texture in bottom part from TxmIndex provided by TerrainTextureManager
			DWORD terrainType = pVertex1->terrainType + 
								(pVertex2->terrainType << 8) +
								(pVertex3->terrainType << 16) +
								(pVertex4->terrainType << 24);

			DWORD overlayType = (pVertex1->textureData >> 16);
   			if (overlayType < Terrain::terrainTextures->getFirstOverlay())
   			{
   				pVertex1->textureData = 0xffff0000;		//Erase the overlay, the numbers changed!
   				overlayType = 0xffff;
   			}
 			
			//Insure Base Texture is zero.
			pVertex1->textureData &= (pVertex1->textureData & 0xffff0000);
			DWORD txmResult = Terrain::terrainTextures->setTexture(terrainType,overlayType);

			pVertex1->textureData += txmResult;

			gosASSERT((pVertex1->textureData & 0x0000ffff) != 0xffff);

			currentVertex++;
		}
			
		currentVertex++;
	}

	DWORD terrainType =  MC_BLUEWATER_TYPE + 
						(MC_BLUEWATER_TYPE << 8) +
						(MC_BLUEWATER_TYPE << 16) +
						(MC_BLUEWATER_TYPE << 24);

	WaterTXMData = Terrain::terrainTextures->setTexture(terrainType,0xffff);
}	

long lowElevation = 255;		//Stores the water level for old Maps
								//ONLY used by conversion code.

long waterTest = 0;				//Stores the water test elevation.  Everything at or below this is underwater!
//---------------------------------------------------------------------------
void MapData::calcWater (float wDepth, float sDepth, float aDepth)
{
	PostcompVertexPtr currentVertex = blocks;
	//---------------------------------------------------------------------------
	// BETTER.  Store bits in top to indicate which way water goes.  Store 2 bits
	// so water doesn't have to animate either!
	// Try random.  May look way cool!
	bool odd1 = false;
	bool odd2 = false;
	BYTE marker = 0;
	for (long y=0;y<(Terrain::realVerticesMapSide-1);y++)
	{
		for (long x=0;x<(Terrain::realVerticesMapSide-1);x++)
		{
			if (currentVertex->elevation < wDepth)
			{
				currentVertex->water = 1 + marker;
			}
			else
			{		
				currentVertex->water = 0 + marker;
			}

			currentVertex++;

			if (RollDice(50))
				odd1 ^= true;
			if (RollDice(50))
				odd2 ^= true;

			marker = (odd1 ? 0x80 : 0x0);
			marker += (odd2 ? 0x40 : 0x0);
		}

		currentVertex++;		//Do not flip odd here so each row starts opposite the previous
	}

	Terrain::waterElevation = wDepth + sDepth;
	waterDepth = wDepth;
	shallowDepth = sDepth;
	alphaDepth = aDepth;
}	

//---------------------------------------------------------------------------
void MapData::recalcWater (void)
{
	PostcompVertexPtr currentVertex = blocks;
	bool odd1 = false;
	bool odd2 = false;

	BYTE marker = 0;
	for (long y=0;y<(Terrain::realVerticesMapSide-1);y++)
	{
		for (long x=0;x<(Terrain::realVerticesMapSide-1);x++)
		{
			if (currentVertex->elevation < waterDepth)
			{
				currentVertex->water = 1 + marker;
			}
			else
			{		
				currentVertex->water = 0 + marker;
			}

			currentVertex++;

			if (RollDice(50))
				odd1 ^= true;
			if (RollDice(50))
				odd2 ^= true;

			marker = (odd1 ? 0x80 : 0x0);
			marker += (odd2 ? 0x40 : 0x0);
		}

		currentVertex++;		//Do not flip odd here so each row starts opposite the previous
	}
}	

//---------------------------------------------------------------------------
float MapData::getTopLeftElevation (void)
{
	float result = 0.0;
	
	if (blocks)
	{
		result = blocks->elevation;
	}
	
	return(result);
}

float MAX_UV = 0.98f;
float MIN_UV = 0.02f;

float ScrollUV1 = MIN_UV;
float ScrollUV2 = MAX_UV;

float SCROLL_RATE =	0.005f;
//---------------------------------------------------------------------------
void MapData::setVertexHeight( int VertexIndex, float Val )
{
	blocks[VertexIndex].elevation = Val;
}


//---------------------------------------------------------------------------
float MapData::getVertexHeight( int VertexIndex )
{
	return blocks[VertexIndex].elevation;
}

#define ContrastEnhance 1.0f
//---------------------------------------------------------------------------
void MapData::calcLight (void)
{
	Terrain::recalcLight = false;

	//----------------------------------------
	// Let's calc the map dimensions...
	long height, width;
	height = width = Terrain::verticesBlockSide * Terrain::blocksMapSide;
	long totalVertices = height * width;

	Stuff::Vector3D lightDir;
	lightDir.x = lightDir.y = 0.0f;
	lightDir.z = 1.0f;
	
	if (eye)
		lightDir = eye->lightDirection;
		
	lightDir *= 64.0f;
	
	//---------------------
	//Lighting Pass Here
	PostcompVertexPtr currentVertex = blocks;
	for (long i=0;i<totalVertices;i++)
	{
		long diskMapIndex = i;

		long x = i % Terrain::realVerticesMapSide;
		long y = i / Terrain::realVerticesMapSide;
		
		//------------------------------------------------
		// Check Bounds to make sure we don't go off map
		if (((diskMapIndex - Terrain::realVerticesMapSide - 1) < 0) ||
			((diskMapIndex - Terrain::realVerticesMapSide) < 0) ||
			((diskMapIndex - 1) < 0))
		{
			//---------------------------------------------
			// Cant generate a normal.  TOO close to edge.
			// Default data please!
		}
		else if (((diskMapIndex + 1) >= totalVertices) ||
				((diskMapIndex + Terrain::realVerticesMapSide) >= totalVertices) ||
				((diskMapIndex + Terrain::realVerticesMapSide + 1) >= totalVertices))
		{
			//---------------------------------------------
			// Cant generate a normal.  TOO close to edge.
			// Default data please!
		}
		else
		{
			//---------------------------------------------
			// No problem
			// Generate at will!
			PostcompVertexPtr v0,v1,v2,v3,v4,v5,v6,v7,v8;

			v0 = currentVertex;
			v1 = currentVertex - Terrain::realVerticesMapSide - 1;
			v2 = currentVertex - Terrain::realVerticesMapSide;
			v3 = currentVertex - Terrain::realVerticesMapSide + 1;
			v4 = currentVertex                                + 1;
			v5 = currentVertex + Terrain::realVerticesMapSide + 1;
			v6 = currentVertex + Terrain::realVerticesMapSide;
			v7 = currentVertex + Terrain::realVerticesMapSide - 1;
			v8 = currentVertex                                - 1;

			//-----------------------------------------------------
			// Try and project shadow lines.
			if (Terrain::recalcShadows)
			{
				Stuff::Vector3D vertexPos;
				vertexPos.x = ((float(x) * Terrain::worldUnitsPerVertex) + Terrain::mapTopLeft3d.x);
				vertexPos.y = (Terrain::mapTopLeft3d.y - (float(y) * Terrain::worldUnitsPerVertex));
				vertexPos.z = 0.0f;
				vertexPos.z = terrainElevation(vertexPos);
				v0->shadow = 0;
				
				if ((lightDir.x != 0.0f) || (lightDir.y != 0.0f))
				{
					vertexPos.Add(vertexPos,lightDir);
					while (eye && Terrain::IsValidTerrainPosition(vertexPos))
					{
						float elev = terrainElevation(vertexPos);
						if ((elev+20.0f) >= vertexPos.z)
						{
							v0->shadow=1;		//Mark as shadow edge
							break;				//Its in shadow.  Stop Projecting!
						}
						
						vertexPos.Add(vertexPos,lightDir);
					}
				}
			}
				
			Stuff::Vector3D		normals[8];
			Stuff::Vector3D		triVect[2];
			
			//-------------------------------------
			// Tri 021
			triVect[0].x = 0.0;
			triVect[0].y = Terrain::worldUnitsPerVertex;
			triVect[0].z = (v2->getElevation() - v0->getElevation()) * ContrastEnhance;
			
			triVect[1].x = -Terrain::worldUnitsPerVertex;
			triVect[1].y = Terrain::worldUnitsPerVertex;
			triVect[1].z = (v1->getElevation() - v0->getElevation()) * ContrastEnhance;
			
			normals[0].Cross(triVect[0],triVect[1]);
			gosASSERT(normals[0].z > 0.0);
			
			normals[0].Normalize(normals[0]);
			
			//-------------------------------------
			// Tri 032
			triVect[0].x = Terrain::worldUnitsPerVertex;
			triVect[0].y = 0.0;
			triVect[0].z = (v3->getElevation() - v0->getElevation()) * ContrastEnhance;
			
			triVect[1].x = 0.0;
			triVect[1].y = Terrain::worldUnitsPerVertex;
			triVect[1].z = (v2->getElevation() - v0->getElevation()) * ContrastEnhance;
			
			normals[1].Cross(triVect[0],triVect[1]);
			gosASSERT(normals[1].z > 0.0);
			
			normals[1].Normalize(normals[1]);
			
			//-------------------------------------
			// Tri 043
			triVect[0].x = Terrain::worldUnitsPerVertex;
			triVect[0].y = -Terrain::worldUnitsPerVertex;
			triVect[0].z = (v4->getElevation() - v0->getElevation()) * ContrastEnhance;

			triVect[1].x = Terrain::worldUnitsPerVertex;
			triVect[1].y = 0.0;
			triVect[1].z = (v3->getElevation() - v0->getElevation()) * ContrastEnhance;
			
		
			normals[2].Cross(triVect[0],triVect[1]);
			gosASSERT(normals[2].z > 0.0);
			
			normals[2].Normalize(normals[2]);
			
			//-------------------------------------
			// Tri 054
			triVect[0].x = 0.0;
			triVect[0].y = -Terrain::worldUnitsPerVertex;
			triVect[0].z = (v5->getElevation() - v0->getElevation()) * ContrastEnhance;
			
			triVect[1].x = Terrain::worldUnitsPerVertex;
			triVect[1].y = -Terrain::worldUnitsPerVertex;
			triVect[1].z = (v4->getElevation() - v0->getElevation()) * ContrastEnhance;
			
			normals[3].Cross(triVect[0],triVect[1]);
			gosASSERT(normals[3].z > 0.0);
			
			normals[3].Normalize(normals[3]);
				
			//-------------------------------------
			// Tri 065
			triVect[0].x = -Terrain::worldUnitsPerVertex;
			triVect[0].y = 0.0;
			triVect[0].z = (v6->getElevation() - v0->getElevation()) * ContrastEnhance;
			
			triVect[1].x = 0.0;
			triVect[1].y = -Terrain::worldUnitsPerVertex;
			triVect[1].z = (v5->getElevation() - v0->getElevation()) * ContrastEnhance;
			
			normals[4].Cross(triVect[0],triVect[1]);
			gosASSERT(normals[4].z > 0.0);
			
			normals[4].Normalize(normals[4]);
			
			//-------------------------------------
			// Tri 076
			triVect[0].x = -Terrain::worldUnitsPerVertex;
			triVect[0].y = 0.0;
			triVect[0].z = (v7->getElevation() - v0->getElevation()) * ContrastEnhance;
			
			triVect[1].x = 0.0;
			triVect[1].y = -Terrain::worldUnitsPerVertex;
			triVect[1].z = (v6->getElevation() - v0->getElevation()) * ContrastEnhance;
			
			normals[5].Cross(triVect[0],triVect[1]);
			gosASSERT(normals[5].z > 0.0);
			
			normals[5].Normalize(normals[5]);
	
			//-------------------------------------
			// Tri 087
			triVect[0].x = -Terrain::worldUnitsPerVertex;
			triVect[0].y = 0.0;
			triVect[0].z = (v8->getElevation() - v0->getElevation()) * ContrastEnhance;
			
			triVect[1].x = 0.0;
			triVect[1].y = -Terrain::worldUnitsPerVertex;
			triVect[1].z = (v7->getElevation() - v0->getElevation()) * ContrastEnhance;
			
			normals[6].Cross(triVect[0],triVect[1]);
			gosASSERT(normals[6].z > 0.0);
			
			normals[6].Normalize(normals[6]);
	
			//-------------------------------------
			// Tri 018
			triVect[0].x = -Terrain::worldUnitsPerVertex;
			triVect[0].y = Terrain::worldUnitsPerVertex;
			triVect[0].z = (v1->getElevation() - v0->getElevation()) * ContrastEnhance;
			
			triVect[1].x = -Terrain::worldUnitsPerVertex;
			triVect[1].y = 0.0;
			triVect[1].z = (v8->getElevation() - v0->getElevation()) * ContrastEnhance;
			
			normals[7].Cross(triVect[0],triVect[1]);
			gosASSERT(normals[7].z > 0.0);
			
			normals[7].Normalize(normals[7]);
			
			currentVertex->vertexNormal.x = normals[0].x + normals[1].x + normals[2].x + normals[3].x + normals[4].x + normals[5].x + normals[6].x + normals[7].x;
			currentVertex->vertexNormal.y = normals[0].y + normals[1].y + normals[2].y + normals[3].y + normals[4].y + normals[5].y + normals[6].y + normals[7].y;
			currentVertex->vertexNormal.z = normals[0].z + normals[1].z + normals[2].z + normals[3].z + normals[4].z + normals[5].z + normals[6].z + normals[7].z;
			currentVertex->vertexNormal.x /= 8.0;
			currentVertex->vertexNormal.y /= 8.0;
			currentVertex->vertexNormal.z /= 8.0;

			gosASSERT(currentVertex->vertexNormal.z > 0.0);
		}

		currentVertex++;
	}
	
	Terrain::recalcShadows = false;
}	

//---------------------------------------------------------------------------
void MapData::clearShadows()
{
	//----------------------------------------
	// Let's calc the map dimensions...
	long height, width;
	height = width = Terrain::verticesBlockSide * Terrain::blocksMapSide;
	long totalVertices = height * width;
	PostcompVertexPtr currentVertex = blocks;
	for (long i=0;i<totalVertices;i++)
	{
		currentVertex->shadow = 0;

		currentVertex++;
	}
}

long sprayFrame = 1;
long sprayAdd = 1;
float SprayTextureNum = 0.0f;
//---------------------------------------------------------------------------
long MapData::update (void)
{
	long result = NO_ERR;

	if (!blankVertex)
	{
		ZoneScopedN("MapData::update initBlankVertex");
		blankVertex = new PostcompVertex;
		blankVertex->elevation = 33.0f;
		blankVertex->textureData = (41<<16) + 41;
	 			
		blankVertex->vertexNormal.x = 0.0;
		blankVertex->vertexNormal.y = 0.0;
		blankVertex->vertexNormal.z = 1.0;

		blankVertex->localRGBLight = 0xffffffff;
	}

	Stuff::Vector3D position;
	if (eye)
		position = eye->getPosition();
	else
		return NO_ERR;

	//-----------------------------------------------------
	// Calculate the topLeftVertex from Camera.
	// This is the closest vertex to the Physical topLeft
	// coordinate of the visible terrain blocks.
	{
		ZoneScopedN("MapData::update cameraWindow");
		topLeftVertex.x = (position.x - Terrain::mapTopLeft3d.x) * Terrain::oneOverWorldUnitsPerVertex;
		topLeftVertex.y = (Terrain::mapTopLeft3d.y - position.y) * Terrain::oneOverWorldUnitsPerVertex;
		
		long PVx = float2long(topLeftVertex.x+0.5f);
		long PVy = float2long(topLeftVertex.y+0.5f);

		float fTLVx = float(PVx) - (Terrain::visibleVerticesPerSide>>1);
		float fTLVy = float(PVy) - (Terrain::visibleVerticesPerSide>>1);
		
		long TLVx = float2long(fTLVx);
		long TLVy = float2long(fTLVy);
		
		topLeftVertex.x = TLVx;		//REAL ABS vertex now.  No longer sub classified to block.  OK if its out of range.  makeLists will handle.
		topLeftVertex.y = TLVy;
	}

	{
		ZoneScopedN("MapData::update scrollUV");
		ScrollUV1 += SCROLL_RATE;
		ScrollUV2 -= SCROLL_RATE;
		
		if (ScrollUV1 > 0.5)
		{
			SCROLL_RATE = -SCROLL_RATE;
		}
		
		if (ScrollUV1 < 0.02)
		{
			SCROLL_RATE = -SCROLL_RATE;
		}
	}

	if (Terrain::recalcLight && eye)
	{
		ZoneScopedN("MapData::update calcLight");
		calcLight();
	}

	//Used to scroll the water texture.
	{
		ZoneScopedN("MapData::update waterScroll");
		cloudScrollX += frameLength * cloudScrollSpeedX;
		if (cloudScrollX > 1.0f)
			cloudScrollX = 0.0f;
			
		cloudScrollY += frameLength * cloudScrollSpeedY;
		if (cloudScrollY > 1.0f)
			cloudScrollY = 0.0f;
	}

	{
		ZoneScopedN("MapData::update waterPhase");
		Terrain::frameAngle += Terrain::waterFreq * frameLength;
		if (Terrain::frameAngle >= 360.0f)
			Terrain::frameAngle = 0.0f;

		Terrain::frameCosAlpha = cos(Terrain::frameAngle * DEGREES_TO_RADS);
		Terrain::frameCos = Terrain::frameCosAlpha * Terrain::waterAmplitude;
	}

	{
		ZoneScopedN("MapData::update sprayFrame");
		SprayTextureNum += frameLength;
		if (!Terrain::terrainTextures2)
		{
			if ((0.0 != Terrain::terrainTextures->getDetailFrameRate(0))
				&& (SprayTextureNum > (1.0 / Terrain::terrainTextures->getDetailFrameRate(0))))
			{
				sprayFrame += sprayAdd;
				SprayTextureNum = 0.0f;
				if (sprayFrame == 31)
				{
					sprayAdd = -1;
	  			}
				else if (sprayFrame == 0)
				{
					sprayAdd = 1;
				}
			}
		}
		else
		{
			if (Terrain::terrainTextures2->getWaterDetailNumFrames() > 1)
			{
				if ((0.0 != Terrain::terrainTextures2->getWaterDetailFrameRate())
					&& (SprayTextureNum > (1.0 / Terrain::terrainTextures2->getWaterDetailFrameRate())))
				{
					sprayFrame += sprayAdd;
					SprayTextureNum = 0.0f;
					if (((int)(sprayFrame + sprayAdd)) >= (int)Terrain::terrainTextures2->getWaterDetailNumFrames())/*carefull of the signed/unsigned mismatch*/
					{
						sprayAdd = -1;
	  				}
					else if (sprayFrame <= 0)
					{
						sprayAdd = 1;
					}
				}
			}
			else if (1 == Terrain::terrainTextures2->getWaterDetailNumFrames())
			{
				sprayFrame = 0;
			}
		}
	}

	return(result);
}

//---------------------------------------------------------------------------
void MapData::makeLists (VertexPtr vertexList, long &numVerts, TerrainQuadPtr quadList, long &numQuads)
{
	ZoneScopedN("MapData::makeLists");
	long topLeftX = 0;
	long topLeftY = 0;
	{
		ZoneScopedN("MapData::makeLists topLeft");
		topLeftX = float2long(topLeftVertex.x);
		topLeftY = float2long(topLeftVertex.y);
	}

	PostcompVertexPtr Pvertex = NULL;
	VertexPtr currentVertex = vertexList;
	numVerts = 0;

	{
		ZoneScopedN("MapData::makeLists vertices");
		for (int y=0;y<Terrain::visibleVerticesPerSide;y++)
	{
		for (int x=0;x<Terrain::visibleVerticesPerSide;x++)
		{
			if ((topLeftX < 0) || (topLeftX >= Terrain::realVerticesMapSide) ||
				(topLeftY < 0) || (topLeftY >= Terrain::realVerticesMapSide))
			{
				//------------------------------------------------------------
				// Point this to the Blank Vertex
				Pvertex = blankVertex;
				currentVertex->vertexNum = -1;
			}
			else
			{
				Pvertex = &blocks[topLeftX + (topLeftY * Terrain::realVerticesMapSide)];
				currentVertex->vertexNum = topLeftX + (topLeftY * Terrain::realVerticesMapSide);
			}

			gosASSERT(Pvertex != NULL);
			currentVertex->pVertex = Pvertex;
			
			//------------------------------------------------
			// Must be calced from ABS positions now!
			long blockX = (topLeftX / Terrain::verticesBlockSide);
			long blockY = (topLeftY / Terrain::verticesBlockSide);

			long vertexX = topLeftX - (blockX * Terrain::verticesBlockSide);
			long vertexY = topLeftY - (blockY * Terrain::verticesBlockSide);

			currentVertex->blockVertex = ((blockX + (blockY * Terrain::blocksMapSide)) << 16) +
											(vertexX + (vertexY * Terrain::verticesBlockSide));
			
			//----------------------------------------------------------------------
			// From Blocks and vertex, calculate the World Position
			currentVertex->vx = float(topLeftX - Terrain::halfVerticesMapSide) * Terrain::worldUnitsPerVertex;
			currentVertex->vy = float(Terrain::halfVerticesMapSide - topLeftY) * Terrain::worldUnitsPerVertex;
			//----------------------------------------------------------------------
			
			long posTileR = topLeftY;
			long posTileC = topLeftX;
				
			currentVertex->posTile = (posTileR << 16) + (posTileC & 0x0000ffff);
			currentVertex->calcThisFrame = 0;
			currentVertex->px = currentVertex->py = -99999.0f;
			currentVertex->clipInfo = false;

			currentVertex++;
			numVerts++;

			topLeftX++;
		}

		topLeftX = float2long(topLeftVertex.x);
		topLeftY++;
		}
	}

	//---------------------------------------------------------------
	// Now that the vertex list is done, use it to create a tile
	// list which corresponds to the correct vertices.
	currentVertex = vertexList;

	TerrainQuadPtr currentQuad = quadList;
	numQuads = 0;

	{
		ZoneScopedN("MapData::makeLists quadSetup");
		topLeftY = float2long(topLeftVertex.y);
	}
	//------------------------------------------------------------------
	// The last row and last vertex in each row are only used to create
	// the previous tile.  They do not have a tile associated with the,
	// since they have no other vertices than their own to refer to.
	long maxX,maxY;
	maxX = maxY = Terrain::visibleVerticesPerSide-1;

	{
		ZoneScopedN("MapData::makeLists quads");
		for (long y=0;y<maxY;y++)
	{
		for (long x=0;x<maxX;x++)
		{
			VertexPtr v0, v1, v2 ,v3;

			v0 = currentVertex;
			v1 = currentVertex+1;
			v2 = currentVertex+Terrain::visibleVerticesPerSide+1;
			v3 = currentVertex+Terrain::visibleVerticesPerSide;

#ifdef _DEBUG
			v0->selected = false;
#endif

			{
				bool tlx = (topLeftX & 1);
				bool tly = (topLeftY & 1);

				bool yby2 = (y & 1) ^ (tly);
				bool xby2 = (x & 1) ^ (tlx);

				if (yby2)
				{
					if (xby2)
					{
						currentQuad->init(v0,v1,v2,v3);
						currentQuad->uvMode = BOTTOMRIGHT;
						currentQuad++;
						numQuads++;
					}
					else
					{
						currentQuad->init(v0,v1,v2,v3);
						currentQuad->uvMode = BOTTOMLEFT;
						currentQuad++;
						numQuads++;
					}
				}
				else
				{
					if (xby2)
					{
						currentQuad->init(v0,v1,v2,v3);
						currentQuad->uvMode = BOTTOMLEFT;
						currentQuad++;
						numQuads++;
					}
					else
					{
						currentQuad->init(v0,v1,v2,v3);
						currentQuad->uvMode = BOTTOMRIGHT;
						currentQuad++;
						numQuads++;
					}
				}
			}

			gosASSERT(currentVertex->pVertex->vertexNormal.z > 0.0);

			currentVertex++;
		}

		//----------------------------------------------------------------
		// We are pointing to the last vertex in the row.  Increment again
		// to point to first vertex in next row.
		currentVertex++;
		}
	}
}

//---------------------------------------------------------------------------
void MapData::setOverlayTile (long block, long vertex, long offset)
{
	long blockX = (block % Terrain::blocksMapSide);
	long blockY = (block / Terrain::blocksMapSide);

	long vertexX = (vertex % Terrain::verticesBlockSide);
	long vertexY = (vertex / Terrain::verticesBlockSide);

	long indexX = blockX * Terrain::verticesBlockSide + vertexX;
	long indexY = blockY * Terrain::verticesBlockSide + vertexY;

	long index = indexX + indexY * Terrain::realVerticesMapSide;

	PostcompVertexPtr ourBlock = &blocks[index];
	ourBlock[vertex].textureData += (offset<<16);
	
	setTerrain(indexY,indexX,-1);
}	

//---------------------------------------------------------------------------
void MapData::setOverlay( long indexY, long indexX, Overlays type, DWORD offset )
{
	long index = indexX + indexY * Terrain::realVerticesMapSide;

	PostcompVertexPtr ourBlock = &blocks[index];
	ourBlock->textureData &= 0x0000ffff;
	ourBlock->textureData |= Terrain::terrainTextures->getOverlayHandle( type, offset );
	
	setTerrain(indexY,indexX,-1);
}

//---------------------------------------------------------------------------
unsigned long MapData::getTexture( long indexY, long indexX )
{
	gosASSERT( indexX > -1 && indexX < Terrain::realVerticesMapSide );
	gosASSERT( indexY > -1 && indexY < Terrain::realVerticesMapSide );
	
	long index = indexX + indexY * Terrain::realVerticesMapSide;
	return blocks[index].textureData;

}

//---------------------------------------------------------------------------
float MapData::terrainElevation( long indexY, long indexX )
{
	gosASSERT( indexX > -1 && indexX < Terrain::realVerticesMapSide );
	gosASSERT( indexY > -1 && indexY < Terrain::realVerticesMapSide );
	
	long index = indexX + indexY * Terrain::realVerticesMapSide;
	return blocks[index].elevation;

}

//---------------------------------------------------------------------------
void MapData::setTerrain( long indexY, long indexX, int Type )
{
	long Vertices[4][2];

	int x = 0;
	int y = 1;

	Vertices[0][x] = indexX;
	Vertices[0][y] = indexY;
	Vertices[1][x] = indexX > 0 ? indexX - 1 : 0;
	Vertices[1][y] = indexY;
	Vertices[2][x] = indexX > 0 ? indexX - 1 : 0;
	Vertices[2][y] = indexY  > 0 ? indexY - 1 : 0;
	Vertices[3][x] = indexX;
	Vertices[3][y] = indexY > 0 ? indexY - 1 : 0;

	for ( int i = 0; i < 4; ++i )
	{
	
		if ( ( indexX > -1 && indexX < Terrain::realVerticesMapSide - 1 ) &&
			( indexY > -1 && indexY < Terrain::realVerticesMapSide - 1 ) )
		{
	
			long index = Vertices[i][x] + Vertices[i][y] * Terrain::realVerticesMapSide;

			PostcompVertexPtr currentVertex = &blocks[index];

			currentVertex->textureData &= 0xffff0000;

			if ( i == 0  && Type > 0)
				currentVertex->terrainType = Type;

			//-----------------------------------------------
			// Get the data needed to make this terrain quad
			PostcompVertex *pVertex1 = currentVertex;
			PostcompVertex *pVertex2 = currentVertex + 1;
			PostcompVertex *pVertex3 = currentVertex + Terrain::realVerticesMapSide + 1;
			PostcompVertex *pVertex4 = currentVertex + Terrain::realVerticesMapSide;
				
			//-------------------------------------------------------------------------------
			// Store texture in bottom part from TxmIndex provided by TerrainTextureManager
			DWORD terrainType = pVertex1->terrainType + 
								(pVertex2->terrainType << 8) +
								(pVertex3->terrainType << 16) +
								(pVertex4->terrainType << 24);

			DWORD overlayType = (pVertex1->textureData >> 16);
			if (overlayType < Terrain::terrainTextures->getFirstOverlay())
			{
				pVertex1->textureData = 0xffff0000;		//Erase the overlay, the numbers changed!
				overlayType = 0xffff;
			}
			
			//Insure Base Texture is zero.
			pVertex1->textureData &= (pVertex1->textureData & 0xffff0000);
			DWORD txmResult = Terrain::terrainTextures->setTexture(terrainType,overlayType);

			pVertex1->textureData += txmResult;
		}
	}

	// Mutations to blocks[].textureData invalidate the terrain face cache. Shape C
	// falls back to inline for ALL cache reads until buildTerrainFaceCache() is
	// explicitly called again (at next primeMissionTerrainCache). In-gameplay
	// setTerrain() calls (mines, scorch) leave cache NULL for the rest of that
	// mission -- acceptable since these events are rare.
	invalidateTerrainFaceCache();
}

//---------------------------------------------------------------------------
long MapData::getTerrain( long tileR, long tileC )
{
	
	gosASSERT( tileR < Terrain::realVerticesMapSide && tileR > -1 );
	gosASSERT( tileC < Terrain::realVerticesMapSide && tileC > -1 );

	long index = tileC + tileR * Terrain::realVerticesMapSide;

	return blocks[index].terrainType;


}

//---------------------------------------------------------------------------
void MapData::getOverlay( long tileR, long tileC, Overlays& type, DWORD& Offset )
{
	gosASSERT( tileR < Terrain::realVerticesMapSide && tileR > -1 );
	gosASSERT( tileC < Terrain::realVerticesMapSide && tileC > -1 );

	long index = tileC + tileR * Terrain::realVerticesMapSide;

	Terrain::terrainTextures->getOverlayInfoFromHandle( blocks[index].textureData, type, Offset );

}
//---------------------------------------------------------------------------
long MapData::getOverlayTile (long block, long vertex)
{
	long blockX = (block % Terrain::blocksMapSide);
	long blockY = (block / Terrain::blocksMapSide);

	long vertexX = (vertex % Terrain::verticesBlockSide);
	long vertexY = (vertex / Terrain::verticesBlockSide);

	long indexX = blockX * Terrain::verticesBlockSide + vertexX;
	long indexY = blockY * Terrain::verticesBlockSide + vertexY;

	long index = indexX + indexY * Terrain::realVerticesMapSide;

	PostcompVertexPtr ourBlock = &blocks[index];
	return (ourBlock[vertex].textureData >> 16);
}	

//---------------------------------------------------------------------------
float MapData::terrainAngle (const Stuff::Vector3D &position, Stuff::Vector3D* normal)
{
	//-------------------------------------------------------------------
	// Recoded for real 3D terrain on march 3, 1999.  Uses new triangle
	// method!
	Stuff::Vector3D triVert[3];
	Stuff::Vector2DOf<float> upperLeft;
	Stuff::Vector3D triPos;
	Stuff::Vector3D triangleVector[2];
	Stuff::Vector3D perpendicularVec;
	float result = 0.0;

	triVert[0].Zero();
	triVert[1].Zero();
	triVert[2].Zero();
	
	if (!Terrain::IsValidTerrainPosition(position))
	{
		normal->x = normal->y = 0.0f;
		normal->z = 1.0f;
		return(0.0f);
	}

	//--------------------------------------------------------
	// find closest vertex in UpperLeft direction to Position
	upperLeft.x = floor(position.x * Terrain::oneOverWorldUnitsPerVertex);
	upperLeft.x *= Terrain::worldUnitsPerVertex;

	upperLeft.y = floor(position.y * Terrain::oneOverWorldUnitsPerVertex);
	if (float(position.y * Terrain::oneOverWorldUnitsPerVertex) != (float)upperLeft.y)
		upperLeft.y += 1.0;
	upperLeft.y *= Terrain::worldUnitsPerVertex;

	Stuff::Vector2DOf<float> _upperLeft;
	_upperLeft.Multiply(upperLeft,float(Terrain::oneOverWorldUnitsPerVertex));
	
	Stuff::Vector2DOf<long> meshOffset;
	meshOffset.x = float2long(_upperLeft.x);
	meshOffset.y = float2long(_upperLeft.y);

	long verticesMapSide = Terrain::verticesBlockSide * Terrain::blocksMapSide;

	meshOffset.x += (verticesMapSide>>1);
	meshOffset.y = (verticesMapSide>>1) - meshOffset.y;

	//Make sure we have map data to return.  Otherwise, just make it full bright
	if (((meshOffset.x + 1) >= Terrain::realVerticesMapSide) ||
		((meshOffset.y + 1) >= Terrain::realVerticesMapSide) ||
		(meshOffset.x < 0) || 
		(meshOffset.y < 0))
		return(0.0f);

	PostcompVertexPtr pVertex1 = &blocks[meshOffset.x     + (meshOffset.y     * Terrain::realVerticesMapSide)];
	PostcompVertexPtr pVertex2 = &blocks[(meshOffset.x+1) + (meshOffset.y     * Terrain::realVerticesMapSide)];
	PostcompVertexPtr pVertex3 = &blocks[(meshOffset.x+1) + ((meshOffset.y+1) * Terrain::realVerticesMapSide)];
	PostcompVertexPtr pVertex4 = &blocks[meshOffset.x     + ((meshOffset.y+1) * Terrain::realVerticesMapSide)];

	triPos.x = Terrain::worldUnitsPerVertex * floor(_upperLeft.x);
	triPos.y = Terrain::worldUnitsPerVertex * floor(_upperLeft.y);
	triPos.z = pVertex1->elevation;
	
	bool tlx = (float2long(topLeftVertex.x) & 1);
	bool tly = (float2long(topLeftVertex.y) & 1);

	long x = meshOffset.x - float2long(topLeftVertex.x);
	long y = meshOffset.y - float2long(topLeftVertex.y);

	bool yby2 = (y & 1) ^ (tly);
	bool xby2 = (x & 1) ^ (tlx);

	long uvMode = 0;
	if (yby2)
	{
		if (xby2)
		{
			uvMode = BOTTOMRIGHT;
		}
		else
		{
			uvMode = BOTTOMLEFT;
		}
	}
	else
	{
		if (xby2)
		{
			uvMode = BOTTOMLEFT;
		}
		else
		{
			uvMode = BOTTOMRIGHT;
		}
	}

	float deltaX = 0.0f;
	float deltaY = 0.0f;

	if (uvMode == BOTTOMRIGHT)
	{
		deltaX = fabs(position.x - upperLeft.x);
		deltaY = fabs(upperLeft.y - position.y);

		//Calculate which triangle and return elevation
		//---------------------------------------------
		if (deltaX > deltaY)
		{
			//position is in Top Triangle
			//-------------------------
			triVert[0] = triPos;
		
			triVert[1].x = triVert[0].x + Terrain::worldUnitsPerVertex;
			triVert[1].y = triVert[0].y;
			triVert[1].z = pVertex2->elevation;
						   			
			triVert[2].x = triVert[1].x;
			triVert[2].y = triVert[0].y - Terrain::worldUnitsPerVertex;
			triVert[2].z = pVertex3->elevation;
		
			triangleVector[0].Subtract(triVert[1],triVert[0]);
			triangleVector[1].Subtract(triVert[2],triVert[0]);
		}
		else
		{
			//Position is in Bottom Triangle
			//----------------------------
			triVert[0] = triPos;
		
			triVert[1].x = triVert[0].x + Terrain::worldUnitsPerVertex;
			triVert[1].y = triVert[0].y - Terrain::worldUnitsPerVertex;
			triVert[1].z = pVertex3->elevation;
		
			triVert[2].x = triVert[0].x;
			triVert[2].y = triVert[1].y;
			triVert[2].z = pVertex4->elevation;
		
			triangleVector[0].Subtract(triVert[2],triVert[0]);
			triangleVector[1].Subtract(triVert[1],triVert[0]);
		}
	}
	else if (uvMode == BOTTOMLEFT)
	{
		deltaX = fabs((upperLeft.x + Terrain::worldUnitsPerVertex) - position.x);
		deltaY = fabs(upperLeft.y - position.y);

		//Calculate which triangle and return elevation
		//---------------------------------------------
		if (deltaX > deltaY)
		{
			//position is in Top Triangle
			//-------------------------
			triVert[1] = triPos;

			triVert[0].x = triPos.x + Terrain::worldUnitsPerVertex;
			triVert[0].y = triPos.y;
			triVert[0].z = pVertex2->elevation;
						   			
			triVert[2].x = triVert[1].x;
			triVert[2].y = triVert[1].y - Terrain::worldUnitsPerVertex;
			triVert[2].z = pVertex4->elevation;
		
			triangleVector[0].Subtract(triVert[1],triVert[0]);
			triangleVector[1].Subtract(triVert[2],triVert[0]);
		}
		else
		{
			//Position is in Bottom Triangle
			//----------------------------
			triVert[0].x = triPos.x + Terrain::worldUnitsPerVertex;
			triVert[0].y = triPos.y;
			triVert[0].z = pVertex2->elevation;

			triVert[1].x = triPos.x;
			triVert[1].y = triVert[0].y - Terrain::worldUnitsPerVertex;
			triVert[1].z = pVertex4->elevation;
		
			triVert[2].x = triVert[0].x;
			triVert[2].y = triVert[1].y;
			triVert[2].z = pVertex3->elevation;
		
			triangleVector[0].Subtract(triVert[2],triVert[0]);
			triangleVector[1].Subtract(triVert[1],triVert[0]);
		}

		deltaX = -deltaX;		//Must reverse for Bottom_left triangles
	}
	
	//----------------------------------------------
	//Calculate Normal vector to the Triangle Plane
	triangleVector[0].Normalize(triangleVector[0]);
	triangleVector[1].Normalize(triangleVector[1]);
	
	perpendicularVec.Cross(triangleVector[0],triangleVector[1]);
		
	//------------------------------
	//Calculate terrain angle
	gosASSERT(perpendicularVec.z != 0L);
	{
		Stuff::Vector3D worldK;
		worldK.x = 0.0;
		worldK.y = 0.0;
		worldK.z = 1.0;
	
		if (perpendicularVec.z < 0L)
		{
			perpendicularVec.Negate(perpendicularVec);
		}

		perpendicularVec.Normalize(perpendicularVec);

		if (normal)
			normal->Normalize(perpendicularVec);

		//We ONLY want PITCH!  No roll should be included!
		perpendicularVec.y = 0.0f;	
		
		result = perpendicularVec * worldK;
		result = acos(result) * RADS_TO_DEGREES;
	}

	return (result);
}

//---------------------------------------------------------------------------
float MapData::terrainLight (const Stuff::Vector3D &position)
{
	if (!Terrain::IsValidTerrainPosition(position))
		return(1.0f);

	//-------------------------------------------------------
	// Need pointer to block containing this vertex.
	float fTLVx = (position.x - Terrain::mapTopLeft3d.x) * Terrain::oneOverWorldUnitsPerVertex;
	float fTLVy = (Terrain::mapTopLeft3d.y - position.y) * Terrain::oneOverWorldUnitsPerVertex;
	
	long PVx = float2long(fTLVx);
	long PVy = float2long(fTLVy);

	//Make sure we have map data to return.  Otherwise, just make it full bright
	if (((PVx + 1) >= Terrain::realVerticesMapSide) ||
		((PVy + 1) >= Terrain::realVerticesMapSide) ||
		(PVx < 0) || 
		(PVy < 0))
		return 1.0f;

	PostcompVertexPtr pVertex1 = &blocks[PVx     + (PVy     * Terrain::realVerticesMapSide)];
	PostcompVertexPtr pVertex2 = &blocks[(PVx+1) + (PVy     * Terrain::realVerticesMapSide)];
	PostcompVertexPtr pVertex3 = &blocks[(PVx+1) + ((PVy+1) * Terrain::realVerticesMapSide)];
	PostcompVertexPtr pVertex4 = &blocks[PVx     + ((PVy+1) * Terrain::realVerticesMapSide)];
	
	Stuff::Vector3D vPos1,vPos2,vPos3,vPos4;
	vPos1.x = (PVx * Terrain::worldUnitsPerVertex) + Terrain::mapTopLeft3d.x;
	vPos1.y = Terrain::mapTopLeft3d.y - (PVy * Terrain::worldUnitsPerVertex);
	vPos1.z = terrainElevation(vPos1);
	
	vPos2.x = vPos1.x + Terrain::worldUnitsPerVertex;
	vPos2.y = vPos1.y;
	vPos2.z = terrainElevation(vPos2);
	
	vPos3.x = vPos2.x;
	vPos3.y = vPos1.y - Terrain::worldUnitsPerVertex;
	vPos3.z = terrainElevation(vPos3);
	
	vPos4.x = vPos1.x;
	vPos4.y = vPos3.y;
	vPos4.z = terrainElevation(vPos4);
	
	vPos1.Subtract(position,vPos1);
	vPos2.Subtract(position,vPos2);
	vPos3.Subtract(position,vPos3);
	vPos4.Subtract(position,vPos4);

	float dist1 = vPos1.GetLength();
	float dist2 = vPos2.GetLength();
	float dist3 = vPos3.GetLength();
	float dist4 = vPos4.GetLength();

	//---------------------------------------
	// CLOSEST vertex has GREATEST weight
	float maxDist = fmax(dist4,fmax(dist3,fmax(dist2,dist1)));
	dist1 = maxDist - dist1;
	dist2 = maxDist - dist2;
	dist3 = maxDist - dist3;
	dist4 = maxDist - dist4;

	float distWeight = dist1 + dist2 + dist3 + dist4;
	Stuff::Vector3D weightedNormal;

	weightedNormal.x = (pVertex1->vertexNormal.x * dist1) +
						(pVertex2->vertexNormal.x * dist2) +
						(pVertex3->vertexNormal.x * dist3) +
						(pVertex4->vertexNormal.x * dist4);
						
	weightedNormal.y = (pVertex1->vertexNormal.y * dist1) +
						(pVertex2->vertexNormal.y * dist2) +
						(pVertex3->vertexNormal.y * dist3) +
						(pVertex4->vertexNormal.y * dist4);
						
	weightedNormal.z = (pVertex1->vertexNormal.z * dist1) +
						(pVertex2->vertexNormal.z * dist2) +
						(pVertex3->vertexNormal.z * dist3) +
						(pVertex4->vertexNormal.z * dist4);


	if ( distWeight > Stuff::SMALL )
		weightedNormal /= distWeight;
	else
		weightedNormal = Stuff::Vector3D( 0.0, 0.0, 1.0 );
		
	float lightIntensity = weightedNormal * eye->lightDirection;

	return (lightIntensity);
}

//---------------------------------------------------------------------------
Stuff::Vector3D MapData::terrainNormal (const Stuff::Vector3D& position)
{
	//-------------------------------------------------------------------
	// Recoded for real 3D terrain on march 3, 1999.  Uses new triangle
	// method!
	Stuff::Vector3D triVert[3];
	Stuff::Vector2DOf<float> upperLeft;
	Stuff::Vector3D triPos;
	Stuff::Vector3D triangleVector[2];
	Stuff::Vector3D perpendicularVec;

	triVert[0].Zero();
	triVert[1].Zero();
	triVert[2].Zero();
	
	if (!Terrain::IsValidTerrainPosition(position))
		return(Stuff::Vector3D(0.0f,0.0f,1.0f));

	//--------------------------------------------------------
	// find closest vertex in UpperLeft direction to Position
	upperLeft.x = floor(position.x * Terrain::oneOverWorldUnitsPerVertex);
	upperLeft.x *= Terrain::worldUnitsPerVertex;

	upperLeft.y = floor(position.y * Terrain::oneOverWorldUnitsPerVertex);
	if (float(position.y * Terrain::oneOverWorldUnitsPerVertex) != (float)upperLeft.y)
		upperLeft.y += 1.0;
	upperLeft.y *= Terrain::worldUnitsPerVertex;

	Stuff::Vector2DOf<float> _upperLeft;
	_upperLeft.Multiply(upperLeft,float(Terrain::oneOverWorldUnitsPerVertex));
	
	Stuff::Vector2DOf<long> meshOffset;
	meshOffset.x = float2long(_upperLeft.x);
	meshOffset.y = float2long(_upperLeft.y);

	long verticesMapSide = Terrain::verticesBlockSide * Terrain::blocksMapSide;

	meshOffset.x += (verticesMapSide>>1);
	meshOffset.y = (verticesMapSide>>1) - meshOffset.y;

	//Make sure we have map data to return.  Otherwise, just make it full bright
	if (((meshOffset.x + 1) >= Terrain::realVerticesMapSide) ||
		((meshOffset.y + 1) >= Terrain::realVerticesMapSide) ||
		(meshOffset.x < 0) || 
		(meshOffset.y < 0))
		return(Stuff::Vector3D(0.0f,0.0f,1.0f));

	PostcompVertexPtr pVertex1 = &blocks[meshOffset.x     + (meshOffset.y     * Terrain::realVerticesMapSide)];
	PostcompVertexPtr pVertex2 = &blocks[(meshOffset.x+1) + (meshOffset.y     * Terrain::realVerticesMapSide)];
	PostcompVertexPtr pVertex3 = &blocks[(meshOffset.x+1) + ((meshOffset.y+1) * Terrain::realVerticesMapSide)];
	PostcompVertexPtr pVertex4 = &blocks[meshOffset.x     + ((meshOffset.y+1) * Terrain::realVerticesMapSide)];

	triPos.x = Terrain::worldUnitsPerVertex * floor(_upperLeft.x);
	triPos.y = Terrain::worldUnitsPerVertex * floor(_upperLeft.y);
	triPos.z = pVertex1->elevation;
	
	bool tlx = (float2long(topLeftVertex.x) & 1);
	bool tly = (float2long(topLeftVertex.y) & 1);

	long x = meshOffset.x - float2long(topLeftVertex.x);
	long y = meshOffset.y - float2long(topLeftVertex.y);

	bool yby2 = (y & 1) ^ (tly);
	bool xby2 = (x & 1) ^ (tlx);

	long uvMode = 0;
	if (yby2)
	{
		if (xby2)
		{
			uvMode = BOTTOMRIGHT;
		}
		else
		{
			uvMode = BOTTOMLEFT;
		}
	}
	else
	{
		if (xby2)
		{
			uvMode = BOTTOMLEFT;
		}
		else
		{
			uvMode = BOTTOMRIGHT;
		}
	}

	float deltaX = 0.0f;
	float deltaY = 0.0f;

	if (uvMode == BOTTOMRIGHT)
	{
		deltaX = fabs(position.x - upperLeft.x);
		deltaY = fabs(upperLeft.y - position.y);

		//Calculate which triangle and return elevation
		//---------------------------------------------
		if (deltaX > deltaY)
		{
			//position is in Top Triangle
			//-------------------------
			triVert[0] = triPos;
		
			triVert[1].x = triVert[0].x + Terrain::worldUnitsPerVertex;
			triVert[1].y = triVert[0].y;
			triVert[1].z = pVertex2->elevation;
						   			
			triVert[2].x = triVert[1].x;
			triVert[2].y = triVert[0].y - Terrain::worldUnitsPerVertex;
			triVert[2].z = pVertex3->elevation;
		
			triangleVector[0].Subtract(triVert[1],triVert[0]);
			triangleVector[1].Subtract(triVert[2],triVert[0]);
		}
		else
		{
			//Position is in Bottom Triangle
			//----------------------------
			triVert[0] = triPos;
		
			triVert[1].x = triVert[0].x + Terrain::worldUnitsPerVertex;
			triVert[1].y = triVert[0].y - Terrain::worldUnitsPerVertex;
			triVert[1].z = pVertex3->elevation;
		
			triVert[2].x = triVert[0].x;
			triVert[2].y = triVert[1].y;
			triVert[2].z = pVertex4->elevation;
		
			triangleVector[0].Subtract(triVert[2],triVert[0]);
			triangleVector[1].Subtract(triVert[1],triVert[0]);
		}
	}
	else if (uvMode == BOTTOMLEFT)
	{
		deltaX = fabs((upperLeft.x + Terrain::worldUnitsPerVertex) - position.x);
		deltaY = fabs(upperLeft.y - position.y);

		//Calculate which triangle and return elevation
		//---------------------------------------------
		if (deltaX > deltaY)
		{
			//position is in Top Triangle
			//-------------------------
			triVert[1] = triPos;

			triVert[0].x = triPos.x + Terrain::worldUnitsPerVertex;
			triVert[0].y = triPos.y;
			triVert[0].z = pVertex2->elevation;
						   			
			triVert[2].x = triVert[1].x;
			triVert[2].y = triVert[1].y - Terrain::worldUnitsPerVertex;
			triVert[2].z = pVertex4->elevation;
		
			triangleVector[0].Subtract(triVert[1],triVert[0]);
			triangleVector[1].Subtract(triVert[2],triVert[0]);
		}
		else
		{
			//Position is in Bottom Triangle
			//----------------------------
			triVert[0].x = triPos.x + Terrain::worldUnitsPerVertex;
			triVert[0].y = triPos.y;
			triVert[0].z = pVertex2->elevation;

			triVert[1].x = triPos.x;
			triVert[1].y = triVert[0].y - Terrain::worldUnitsPerVertex;
			triVert[1].z = pVertex4->elevation;
		
			triVert[2].x = triVert[0].x;
			triVert[2].y = triVert[1].y;
			triVert[2].z = pVertex3->elevation;
		
			triangleVector[0].Subtract(triVert[2],triVert[0]);
			triangleVector[1].Subtract(triVert[1],triVert[0]);
		}

		deltaX = -deltaX;		//Must reverse for Bottom_left triangles
	}
	
	//----------------------------------------------
	//Calculate Normal vector to the Triangle Plane
	triangleVector[0].Normalize(triangleVector[0]);
	triangleVector[1].Normalize(triangleVector[1]);
	
	perpendicularVec.Cross(triangleVector[0],triangleVector[1]);
		
	//------------------------------
	//Calculate terrain Normal
	if (perpendicularVec.z < 0L)
	{
		perpendicularVec.Negate(perpendicularVec);
	}

	perpendicularVec.Normalize(perpendicularVec);

	return (perpendicularVec);
}

//--- CPU-side terrain displacement helpers (match GPU TES) ---

// HSV conversion matching terrain_common.hglsl tc_rgb2hsv
static void cpu_rgb2hsv(float r, float g, float b, float& h, float& s, float& v) {
    float maxC = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
    float minC = (r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b);
    float d = maxC - minC;
    v = maxC;
    s = (maxC > 1e-10f) ? (d / maxC) : 0.0f;
    if (d < 1e-10f) { h = 0.0f; return; }
    if (maxC == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (maxC == g) h = (b - r) / d + 2.0f;
    else h = (r - g) / d + 4.0f;
    h /= 6.0f;
}

// smoothstep matching GLSL
static float cpu_smoothstep(float edge0, float edge1, float x) {
    float t = (x - edge0) / (edge1 - edge0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// Material weight classification matching terrain_common.hglsl tc_getColorWeights
// Returns dirt weight (w.z component) only, since that's all we need for TES displacement
static float cpu_getDirtWeight(float r, float g, float b) {
    float h, s, v;
    cpu_rgb2hsv(r, g, b, h, s, v);

    float grassW = cpu_smoothstep(0.14f, 0.17f, h) * cpu_smoothstep(0.15f, 0.28f, s);
    float dirtW = cpu_smoothstep(0.155f, 0.13f, h) * cpu_smoothstep(0.15f, 0.28f, s);
    float concreteW = cpu_smoothstep(0.18f, 0.10f, s) * cpu_smoothstep(0.50f, 0.62f, v);
    float rockW = cpu_smoothstep(0.18f, 0.10f, s) * cpu_smoothstep(0.45f, 0.30f, v);
    rockW += cpu_smoothstep(0.38f, 0.28f, v) * cpu_smoothstep(0.15f, 0.25f, s);

    float isWater = cpu_smoothstep(0.35f, 0.45f, h);
    rockW += isWater;
    grassW *= (1.0f - isWater);
    dirtW *= (1.0f - isWater);

    float total = rockW + grassW + dirtW + concreteW;
    if (total < 0.01f) return 1.0f; // default to dirt if unclassified
    return dirtW / total;
}

// Bilinear sample from unsigned char texture (wrapping)
static float cpu_sampleAlpha(const unsigned char* data, int size, float u, float v) {
    u = u - floorf(u);
    v = v - floorf(v);
    float fx = u * (float)size - 0.5f;
    float fy = v * (float)size - 0.5f;
    int x0 = (int)floorf(fx);
    int y0 = (int)floorf(fy);
    float dx = fx - (float)x0;
    float dy = fy - (float)y0;
    x0 = ((x0 % size) + size) % size;
    y0 = ((y0 % size) + size) % size;
    int x1 = (x0 + 1) % size;
    int y1 = (y0 + 1) % size;
    float s00 = data[y0 * size + x0] / 255.0f;
    float s10 = data[y0 * size + x1] / 255.0f;
    float s01 = data[y1 * size + x0] / 255.0f;
    float s11 = data[y1 * size + x1] / 255.0f;
    return (s00 * (1.0f-dx) + s10 * dx) * (1.0f-dy) +
           (s01 * (1.0f-dx) + s11 * dx) * dy;
}

// Sample colormap RGBA at UV (nearest neighbor)
// TGA layout is BGRA in memory
static void cpu_sampleColormap(const unsigned char* data, int size, float u, float v,
                               float& r, float& g, float& b) {
    u = u - floorf(u);
    v = v - floorf(v);
    int x = (int)(u * (float)size);
    int y = (int)(v * (float)size);
    if (x >= size) x = size - 1;
    if (y >= size) y = size - 1;
    int idx = (y * size + x) * 4;
    b = data[idx + 0] / 255.0f;
    g = data[idx + 1] / 255.0f;
    r = data[idx + 2] / 255.0f;
}

//---------------------------------------------------------------------------
float MapData::terrainElevation (const Stuff::Vector3D &position)
{
	//-------------------------------------------------------------------
	// Recoded for real 3D terrain on march 3, 1999.  Uses new triangle
	// method!
	Stuff::Vector3D triVert[3];
	Stuff::Vector2DOf<float> upperLeft;
	Stuff::Vector3D triPos;
	Stuff::Vector3D triangleVector[2];
	Stuff::Vector3D perpendicularVec;
	float result = 0.0;

	triVert[0].Zero();
	triVert[1].Zero();
	triVert[2].Zero();

	if (!Terrain::IsValidTerrainPosition(position))
		return(0.0f);
		
	//--------------------------------------------------------
	// find closest vertex in UpperLeft direction to Position
	upperLeft.x = floor(position.x * Terrain::oneOverWorldUnitsPerVertex);
	upperLeft.x *= Terrain::worldUnitsPerVertex;

	upperLeft.y = floor(position.y * Terrain::oneOverWorldUnitsPerVertex);
	if (float(position.y * Terrain::oneOverWorldUnitsPerVertex) != (float)upperLeft.y)
		upperLeft.y += 1.0;
	upperLeft.y *= Terrain::worldUnitsPerVertex;

	Stuff::Vector2DOf<float> _upperLeft;
	_upperLeft.Multiply(upperLeft,float(Terrain::oneOverWorldUnitsPerVertex));
	
	Stuff::Vector2DOf<long> meshOffset;
	meshOffset.x = floor(_upperLeft.x);
	meshOffset.y = floor(_upperLeft.y);

	if (long(floor(_upperLeft.x)) != float2long(_upperLeft.x))
		PAUSE(("Long != float2long  %d  ->  %d",long(floor(_upperLeft.x)),float2long(_upperLeft.x)));
		
	if (long(floor(_upperLeft.y)) != float2long(_upperLeft.y))
		PAUSE(("Long != float2long  %d  ->  %d",long(floor(_upperLeft.y)),float2long(_upperLeft.y)));

	long verticesMapSide = Terrain::verticesBlockSide * Terrain::blocksMapSide;

	meshOffset.x += (verticesMapSide>>1);
	meshOffset.y = (verticesMapSide>>1) - meshOffset.y;

	if ((meshOffset.x >= (verticesMapSide-1)))
		return 0.0f;

	if ((meshOffset.y >= (verticesMapSide-1)))
		return 0.0f;

	PostcompVertexPtr pVertex1 = &blocks[meshOffset.x     + (meshOffset.y     * Terrain::realVerticesMapSide)];
	PostcompVertexPtr pVertex2 = &blocks[(meshOffset.x+1) + (meshOffset.y     * Terrain::realVerticesMapSide)];
	PostcompVertexPtr pVertex3 = &blocks[(meshOffset.x+1) + ((meshOffset.y+1) * Terrain::realVerticesMapSide)];
	PostcompVertexPtr pVertex4 = &blocks[meshOffset.x     + ((meshOffset.y+1) * Terrain::realVerticesMapSide)];

	triPos.x = Terrain::worldUnitsPerVertex * floor(_upperLeft.x);
	triPos.y = Terrain::worldUnitsPerVertex * floor(_upperLeft.y);
	triPos.z = pVertex1->elevation;
	
	if (long(topLeftVertex.x) != float2long(topLeftVertex.x))
		PAUSE(("Long != float2long  %d  ->  %d",long(topLeftVertex.x),float2long(topLeftVertex.x)));
		
	if (long(topLeftVertex.y) != float2long(topLeftVertex.y))
		PAUSE(("Long != float2long  %d  ->  %d",long(topLeftVertex.y),float2long(topLeftVertex.y)));

	bool tlx = (long(topLeftVertex.x) & 1);
	bool tly = (long(topLeftVertex.y) & 1);

	long x = meshOffset.x - topLeftVertex.x;
	long y = meshOffset.y - topLeftVertex.y;

	bool yby2 = (y & 1) ^ (tly);
	bool xby2 = (x & 1) ^ (tlx);

	long uvMode = 0;
	if (yby2)
	{
		if (xby2)
		{
			uvMode = BOTTOMRIGHT;
		}
		else
		{
			uvMode = BOTTOMLEFT;
		}
	}
	else
	{
		if (xby2)
		{
			uvMode = BOTTOMLEFT;
		}
		else
		{
			uvMode = BOTTOMRIGHT;
		}
	}

	float deltaX = 0.0f;
	float deltaY = 0.0f;

	if (uvMode == BOTTOMRIGHT)
	{
		deltaX = fabs(position.x - upperLeft.x);
		deltaY = fabs(upperLeft.y - position.y);

		//Calculate which triangle and return elevation
		//---------------------------------------------
		if (deltaX > deltaY)
		{
			//position is in Top Triangle
			//-------------------------
			triVert[0] = triPos;
		
			triVert[1].x = triVert[0].x + Terrain::worldUnitsPerVertex;
			triVert[1].y = triVert[0].y;
			triVert[1].z = pVertex2->elevation;
						   			
			triVert[2].x = triVert[1].x;
			triVert[2].y = triVert[0].y - Terrain::worldUnitsPerVertex;
			triVert[2].z = pVertex3->elevation;
		
			triangleVector[0].Subtract(triVert[1],triVert[0]);
			triangleVector[1].Subtract(triVert[2],triVert[0]);
		}
		else
		{
			//Position is in Bottom Triangle
			//----------------------------
			triVert[0] = triPos;
		
			triVert[1].x = triVert[0].x + Terrain::worldUnitsPerVertex;
			triVert[1].y = triVert[0].y - Terrain::worldUnitsPerVertex;
			triVert[1].z = pVertex3->elevation;
		
			triVert[2].x = triVert[0].x;
			triVert[2].y = triVert[1].y;
			triVert[2].z = pVertex4->elevation;
		
			triangleVector[0].Subtract(triVert[2],triVert[0]);
			triangleVector[1].Subtract(triVert[1],triVert[0]);
		}
	}
	else if (uvMode == BOTTOMLEFT)
	{
		deltaX = fabs((upperLeft.x + Terrain::worldUnitsPerVertex) - position.x);
		deltaY = fabs(upperLeft.y - position.y);

		//Calculate which triangle and return elevation
		//---------------------------------------------
		if (deltaX > deltaY)
		{
			//position is in Top Triangle
			//-------------------------
			triVert[1] = triPos;

			triVert[0].x = triPos.x + Terrain::worldUnitsPerVertex;
			triVert[0].y = triPos.y;
			triVert[0].z = pVertex2->elevation;
						   			
			triVert[2].x = triVert[1].x;
			triVert[2].y = triVert[1].y - Terrain::worldUnitsPerVertex;
			triVert[2].z = pVertex4->elevation;
		
			triangleVector[0].Subtract(triVert[1],triVert[0]);
			triangleVector[1].Subtract(triVert[2],triVert[0]);
		}
		else
		{
			//Position is in Bottom Triangle
			//----------------------------
			triVert[0].x = triPos.x + Terrain::worldUnitsPerVertex;
			triVert[0].y = triPos.y;
			triVert[0].z = pVertex2->elevation;

			triVert[1].x = triPos.x;
			triVert[1].y = triVert[0].y - Terrain::worldUnitsPerVertex;
			triVert[1].z = pVertex4->elevation;
		
			triVert[2].x = triVert[0].x;
			triVert[2].y = triVert[1].y;
			triVert[2].z = pVertex3->elevation;
		
			triangleVector[0].Subtract(triVert[2],triVert[0]);
			triangleVector[1].Subtract(triVert[1],triVert[0]);
		}

		deltaX = -deltaX;		//Must reverse for Bottom_left triangles
	}
	
	//----------------------------------------------
	//Calculate Normal vector to the Triangle Plane
	triangleVector[0].Normalize(triangleVector[0]);
	triangleVector[1].Normalize(triangleVector[1]);
	
	perpendicularVec.Cross(triangleVector[0],triangleVector[1]);
	
	//------------------------------
	//Calculate terrain elevation
	if (perpendicularVec.z != 0L)
	{
		if (perpendicularVec.z < 0L)
		{
			perpendicularVec.Negate(perpendicularVec);
		}
			
		float perpendX = perpendicularVec.x / perpendicularVec.z;
		float perpendY = perpendicularVec.y / perpendicularVec.z;
				
		result = (deltaX*perpendX);
	
		result += ((-deltaY)*perpendY);
	
		result = -result;
	
		result += triVert[0].z;
	}

	// --- CPU-side terrain displacement (matching GPU TES) ---
	if (Terrain::terrainTextures2) {
		TerrainColorMap* tcm = Terrain::terrainTextures2;
		float phongAlpha = gos_GetTerrainPhongAlpha();
		float displaceScale = gos_GetTerrainDisplaceScale();
		float detailTiling = gos_GetTerrainDetailTiling();

		// Compute colormap UV from world position
		// Matches quad.cpp: u = (wx - mapTopLeft3d.x) / worldUnitsMapSide
		float cmapU = (position.x - Terrain::mapTopLeft3d.x) * Terrain::oneOverWorldUnitsMapSide;
		float cmapV = (Terrain::mapTopLeft3d.y - position.y) * Terrain::oneOverWorldUnitsMapSide;

		// Identify which PostcompVertices form our triangle (for normals)
		PostcompVertexPtr triPV[3];
		if (uvMode == BOTTOMRIGHT) {
			if (deltaX > deltaY) {
				triPV[0] = pVertex1; triPV[1] = pVertex2; triPV[2] = pVertex3;
			} else {
				triPV[0] = pVertex1; triPV[1] = pVertex3; triPV[2] = pVertex4;
			}
		} else { // BOTTOMLEFT
			if ((-deltaX) > deltaY) { // deltaX was negated for BOTTOMLEFT
				triPV[0] = pVertex2; triPV[1] = pVertex1; triPV[2] = pVertex4;
			} else {
				triPV[0] = pVertex2; triPV[1] = pVertex4; triPV[2] = pVertex3;
			}
		}

		// Compute barycentric coordinates
		float x0 = triVert[0].x, y0 = triVert[0].y;
		float x1 = triVert[1].x, y1 = triVert[1].y;
		float x2 = triVert[2].x, y2 = triVert[2].y;
		float det = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
		float bary0 = 0.333f, bary1 = 0.333f, bary2 = 0.333f;
		if (fabsf(det) > 1e-6f) {
			bary0 = ((y1 - y2) * (position.x - x2) + (x2 - x1) * (position.y - y2)) / det;
			bary1 = ((y2 - y0) * (position.x - x2) + (x0 - x2) * (position.y - y2)) / det;
			bary2 = 1.0f - bary0 - bary1;
		}

		// --- Phong smoothing (matching TES lines 77-84) ---
		if (phongAlpha > 0.0f) {
			// Phong projection: project interpolated pos onto tangent plane of each vertex
			// proj_i = pos - dot(pos - v_i, n_i) * n_i
			// We only need the Z component
			float px = position.x, py = position.y, pz = result;
			float projZ0, projZ1, projZ2;
			{
				Stuff::Vector3D& n = triPV[0]->vertexNormal;
				float dx2 = px - triVert[0].x, dy2 = py - triVert[0].y, dz2 = pz - triVert[0].z;
				float dot = dx2 * n.x + dy2 * n.y + dz2 * n.z;
				projZ0 = pz - dot * n.z;
			}
			{
				Stuff::Vector3D& n = triPV[1]->vertexNormal;
				float dx2 = px - triVert[1].x, dy2 = py - triVert[1].y, dz2 = pz - triVert[1].z;
				float dot = dx2 * n.x + dy2 * n.y + dz2 * n.z;
				projZ1 = pz - dot * n.z;
			}
			{
				Stuff::Vector3D& n = triPV[2]->vertexNormal;
				float dx2 = px - triVert[2].x, dy2 = py - triVert[2].y, dz2 = pz - triVert[2].z;
				float dot = dx2 * n.x + dy2 * n.y + dz2 * n.z;
				projZ2 = pz - dot * n.z;
			}

			float phongZ = bary0 * projZ0 + bary1 * projZ1 + bary2 * projZ2;
			result = result + phongAlpha * (phongZ - result);
		}

		// --- Dirt texture displacement (matching TES lines 87-99) ---
		if (displaceScale > 0.0f && tcm->cpuColorMap && tcm->cpuDispAlpha) {
			float cr, cg, cb;
			cpu_sampleColormap(tcm->cpuColorMap, tcm->cpuColorMapSize, cmapU, cmapV, cr, cg, cb);
			float dirtWeight = cpu_getDirtWeight(cr, cg, cb);

			if (dirtWeight > 0.01f) {
				// TC_MAT_TILING.z = 1.0 for dirt
				float dispU = cmapU * detailTiling * 1.0f;
				float dispV = cmapV * detailTiling * 1.0f;
				float disp = 1.0f - cpu_sampleAlpha(tcm->cpuDispAlpha, tcm->cpuDispAlphaSize, dispU, dispV);

				// Use face normal Z for displacement direction
				float len = sqrtf(perpendicularVec.x*perpendicularVec.x +
				                 perpendicularVec.y*perpendicularVec.y +
				                 perpendicularVec.z*perpendicularVec.z);
				float nz = (len > 1e-6f) ? fabsf(perpendicularVec.z) / len : 1.0f;

				result += nz * (disp - 0.5f) * displaceScale * dirtWeight;
			}
		}
	}

	return (result);
}

//---------------------------------------------------------------------------
void MapData::unselectAll()
{
	for ( int i = 0; i < Terrain::realVerticesMapSide * Terrain::realVerticesMapSide; ++i )
	{
		blocks[i].selected = false;
	}

	hasSelection = 0;
}

//---------------------------------------------------------------------------
void MapData::unhighlightAll()
{
	for ( int i = 0; i < Terrain::realVerticesMapSide * Terrain::realVerticesMapSide; ++i )
	{
		blocks[i].highlighted = false;
	}
}

//---------------------------------------------------------------------------
void MapData::selectVertex( unsigned long tileRow, unsigned long tileCol, bool bSelect, bool bToggle)
{
	//Just return.  Don't select anything!
	if ( tileRow >= Terrain::realVerticesMapSide )
		return;

	if ( tileCol >= Terrain::realVerticesMapSide )
		return;

	unsigned long index = tileRow * Terrain::realVerticesMapSide + tileCol;
	blocks[index].selected = bToggle ? !blocks[index].selected : bSelect;
	if ( blocks[index].selected )
		hasSelection ++;
	else
		hasSelection --;


	if ( hasSelection < 0 )
		hasSelection = 0;
}

//---------------------------------------------------------------------------
bool MapData::isVertexSelected( unsigned long tileRow, unsigned long tileCol )
{
	gosASSERT( tileRow < Terrain::realVerticesMapSide );
	gosASSERT( tileCol < Terrain::realVerticesMapSide );

	unsigned long index = tileRow * Terrain::realVerticesMapSide + tileCol;
	return blocks[index].selected ? true : false;

}

//---------------------------------------------------------------------------

