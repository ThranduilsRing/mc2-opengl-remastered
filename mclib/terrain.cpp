//---------------------------------------------------------------------------
//
// Terrain.cpp -- File contains calss definitions for the Terrain
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files
#ifndef TERRAIN_H
#include"terrain.h"
#endif

#ifndef VERTEX_H
#include"vertex.h"
#endif

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef TERRTXM_H
#include"terrtxm.h"
#include"tex_resolve_table.h"
#endif

#ifndef DBASEGUI_H
#include"dbasegui.h"
#endif

#include"../GameOS/gameos/gos_profiler.h"
#include"../GameOS/gameos/gos_terrain_water_stream.h"
#include"../GameOS/gameos/gos_terrain_indirect.h"
#include"../GameOS/gameos/gos_terrain_bridge.h"

#include <vector>
#include <cstdint>

// Externals from quad.cpp / mapdata.cpp / mechcmd2.cpp used by the water fast path.
extern float MaxMinUV;
extern float cloudScrollX;
extern float cloudScrollY;
extern long  sprayFrame;
extern bool  useWaterInterestTexture;

#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef PATHS_H
#include"paths.h"
#endif

#ifndef USERINPUT_H
#include"userinput.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#ifndef PACKET_H
#include"packet.h"
#endif

#ifndef INIFILE_H
#include"fitinifile.h"
#endif

#ifndef TGAINFO_H
#include"tgainfo.h"
#endif

//---------------------------------------------------------------------------
// Static Globals
float worldUnitsPerMeter = 5.01f;
float metersPerWorldUnit = 0.2f;
long terrainLineChanged = 0;

MapDataPtr					Terrain::mapData = NULL;
TerrainTexturesPtr			Terrain::terrainTextures = NULL;
TerrainColorMapPtr			Terrain::terrainTextures2 = NULL;

const long					Terrain::verticesBlockSide = 20;			//Changes for new terrain?
long						Terrain::blocksMapSide = 0;					//Calced during load.

long						Terrain::visibleVerticesPerSide = 0;		//Passed in.

const float					Terrain::worldUnitsPerVertex = 128.0;
const float					Terrain::worldUnitsPerCell = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
const float					Terrain::halfWorldUnitsPerCell = Terrain::worldUnitsPerCell / 2.0f;
const float					Terrain::metersPerCell = Terrain::worldUnitsPerCell * metersPerWorldUnit;
const float					Terrain::worldUnitsBlockSide = Terrain::worldUnitsPerVertex * Terrain::verticesBlockSide;
const float					Terrain::oneOverWorldUnitsPerVertex = 1.0f / Terrain::worldUnitsPerVertex;
const float					Terrain::oneOverWorldUnitsPerCell = 1.0f / Terrain::worldUnitsPerCell;
const float					Terrain::oneOverMetersPerCell = 1.0f / Terrain::metersPerCell;
const float					Terrain::oneOverVerticesBlockSide = 1.0f / Terrain::verticesBlockSide;

float						Terrain::worldUnitsMapSide = 0.0;		//Calced during load.
float						Terrain::oneOverWorldUnitsMapSide = 0.0f;
long						Terrain::halfVerticesMapSide = 0;
long						Terrain::realVerticesMapSide = 0;

Stuff::Vector3D				Terrain::mapTopLeft3d;					//Calced during load.

UserHeapPtr					Terrain::terrainHeap = NULL;			//Setup at load time.
char *						Terrain::terrainName = NULL;
char * 						Terrain::colorMapName = NULL;			

long		   				Terrain::numObjBlocks = 0;
ObjBlockInfo				*Terrain::objBlockInfo = NULL;
bool						*Terrain::objVertexActive = NULL;

float 						*Terrain::tileRowToWorldCoord = NULL;
float 						*Terrain::tileColToWorldCoord = NULL;
float 						*Terrain::cellToWorldCoord = NULL;
float 						*Terrain::cellColToWorldCoord = NULL;
float 						*Terrain::cellRowToWorldCoord = NULL;

float 						Terrain::waterElevation = 0.0f;
float						Terrain::frameAngle = 0.0f;
float 						Terrain::frameCos = 1.0f;
float						Terrain::frameCosAlpha = 1.0f;
DWORD						Terrain::alphaMiddle = 0xaf000000;
DWORD						Terrain::alphaEdge = 0x3f000000;
DWORD						Terrain::alphaDeep = 0xff000000;
float						Terrain::waterFreq = 4.0f;
float						Terrain::waterAmplitude = 10.0f;

long						Terrain::userMin = 0;
long						Terrain::userMax = 0;
unsigned long				Terrain::baseTerrain = 0;
unsigned char				Terrain::fractalThreshold = 1;
unsigned char				Terrain::fractalNoise = 0;
bool						Terrain::recalcShadows = false;
bool						Terrain::recalcLight = false;

Clouds						*Terrain::cloudLayer = NULL;

bool 						drawTerrainGrid = false;		//Override locally in editor so game don't come with these please!  Love -fs
bool						drawLOSGrid = false;
bool						drawTerrainTiles = true;
bool						drawTerrainOverlays = true;
bool						drawTerrainMines = true;
bool						renderObjects = true;
bool						renderTrees = true;

TerrainPtr					land = NULL;

long 						*usedBlockList;					//Used to determine what objects to deal with.
long 						*moverBlockList;

unsigned long 				blockMemSize = 0;				//Misc Flags.
bool 						useOldProject = FALSE;
bool 						projectAll = FALSE;
bool 						useClouds = false;
bool 						useFog = true;
bool 						useVertexLighting = true;
bool 						useFaceLighting = false;
extern bool					useRealLOS;

unsigned char 				godMode = 0;			//Can I simply see everything, enemy and friendly?

extern long 				DrawDebugCells;

#define						MAX_TERRAIN_HEAP_SIZE		1024000

long						visualRangeTable[256];
extern bool 				justResaveAllMaps;
//---------------------------------------------------------------------------
// These are used to determine what terrain objects to process.
// They date back to GenCon 1996!!
void addBlockToList (long blockNum)
{
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;
	for (long i=0;i<totalBlocks;i++)
	{
		if (usedBlockList[i] == blockNum)
		{
			return;
		}
		else if (usedBlockList[i] == -1)
		{
			usedBlockList[i] = blockNum;
			return;
		}
	}
}

//---------------------------------------------------------------------------
void addMoverToList (long blockNum)
{
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;
	for (long i=0;i<totalBlocks;i++)
	{
		if (moverBlockList[i] == blockNum)
		{
			return;
		}
		else if (moverBlockList[i] == -1)
		{
			moverBlockList[i] = blockNum;
			return;
		}
	}
}

//---------------------------------------------------------------------------
void clearList (void)
{
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;
	blockMemSize = totalBlocks * sizeof(long);
	
	if (usedBlockList)
		memset(usedBlockList,-1,blockMemSize);
}

//---------------------------------------------------------------------------
void clearMoverList (void)
{
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;
	blockMemSize = totalBlocks * sizeof(long);
	
	if (moverBlockList)
		memset(moverBlockList,-1,blockMemSize);
}

//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// class Terrain
void Terrain::init (void)
{
	vertexList = NULL;
	numberVertices = 0;
	
	quadList = NULL;
	numberQuads = 0;
}

//---------------------------------------------------------------------------
void Terrain::initMapCellArrays (void)
{
	if (!tileRowToWorldCoord)
	{
		tileRowToWorldCoord = (float *)terrainHeap->Malloc(sizeof(float) * realVerticesMapSide);
		gosASSERT(tileRowToWorldCoord != NULL);
	}

	if (!tileColToWorldCoord)
	{
		tileColToWorldCoord = (float *)terrainHeap->Malloc(sizeof(float) * realVerticesMapSide); 
		gosASSERT(tileColToWorldCoord != NULL);
	}

	if (!cellToWorldCoord)
	{
		cellToWorldCoord = (float *)terrainHeap->Malloc(sizeof(float) * MAPCELL_DIM); 
		gosASSERT(cellToWorldCoord != NULL);
	}

	if (!cellColToWorldCoord)
	{
		cellColToWorldCoord = (float *)terrainHeap->Malloc(sizeof(float) * realVerticesMapSide * MAPCELL_DIM); 
		gosASSERT(cellColToWorldCoord != NULL);
	}

	if (!cellRowToWorldCoord)
	{
		cellRowToWorldCoord = (float *)terrainHeap->Malloc(sizeof(float) * realVerticesMapSide * MAPCELL_DIM); 
		gosASSERT(cellRowToWorldCoord != NULL);
	}

	long i=0;

	long height = realVerticesMapSide, width = height;
	for (i = 0; i < height; i++)
		tileRowToWorldCoord[i] = (worldUnitsMapSide / 2.0) - (i * worldUnitsPerVertex);

	for (i = 0; i < width; i++)
		tileColToWorldCoord[i] = (i * worldUnitsPerVertex) - (worldUnitsMapSide / 2.0);

	for (i = 0; i < MAPCELL_DIM; i++)
		cellToWorldCoord[i] = (worldUnitsPerVertex / (float)MAPCELL_DIM) * i;

	long maxCell = height * MAPCELL_DIM;
	for (i = 0; i < maxCell; i++)
		cellRowToWorldCoord[i] = (worldUnitsMapSide / 2.0) - (i * worldUnitsPerCell);

	maxCell = width * MAPCELL_DIM;
	for (i = 0; i < maxCell; i++)
		cellColToWorldCoord[i] = (i * worldUnitsPerCell) - (worldUnitsMapSide / 2.0);
}	

//---------------------------------------------------------------------------
long Terrain::init (PacketFile* pakFile, int whichPacket, unsigned long visibleVertices, volatile float& percent,
					float percentRange )
{
	clearList();
	clearMoverList();
	
	long result = pakFile->seekPacket( whichPacket );
	if (result != NO_ERR)
		STOP(("Unable to seek Packet %d in file %s",whichPacket,pakFile->getFilename()));
	
	int tmp = pakFile->getPacketSize();
	realVerticesMapSide = sqrt( float(tmp/ sizeof(PostcompVertex)));
	
	if (!justResaveAllMaps && 
		(realVerticesMapSide != 120) &&
		(realVerticesMapSide != 100) && 
		(realVerticesMapSide != 80) &&
		(realVerticesMapSide != 60))
	{
		PAUSE(("This map size NO longer supported %d.  Must be 120, 100, 80 or 60 now!  Can Continue, for NOW!!",realVerticesMapSide));
//		return -1;
	}
	
	init( realVerticesMapSide, pakFile, visibleVertices, percent, percentRange );	
	
	return(NO_ERR);
}

//---------------------------------------------------------------------------
void Terrain::getColorMapName (FitIniFile *file)
{
	if (file)
	{
		if (file->seekBlock("ColorMap") == NO_ERR)
		{
			char mapName[1024];
			if (file->readIdString("ColorMapName",mapName,1023) == NO_ERR)
			{
				colorMapName = new char[strlen(mapName)+1];
				strcpy(colorMapName,mapName);
				return;
			}
		}
	}

	colorMapName = NULL;
}

//---------------------------------------------------------------------------
void Terrain::setColorMapName (char *mapName)
{
	if (colorMapName)
	{
		delete [] colorMapName;
		colorMapName = NULL;
	}

	if (mapName)
	{
		colorMapName = new char [strlen(mapName)+1];
		strcpy(colorMapName,mapName);
	}
}

//---------------------------------------------------------------------------
void Terrain::saveColorMapName (FitIniFile *file)
{
	if (file && colorMapName)
	{
		file->writeBlock("ColorMap");
		file->writeIdString("ColorMapName",colorMapName);
	}
}

//---------------------------------------------------------------------------
long Terrain::init( unsigned long verticesPerMapSide, PacketFile* pakFile, unsigned long visibleVertices,
				   volatile float& percent,
					float percentRange)
{
	ZoneScopedN("Terrain::init");
	//Did we pass in the hi-res colormap?
	// If so, convert back to old verticesPerMapSide!
	if (verticesPerMapSide > 300)
		verticesPerMapSide /= 12.8;
		
	realVerticesMapSide = verticesPerMapSide;
	halfVerticesMapSide = realVerticesMapSide >> 1;
	blocksMapSide = realVerticesMapSide / verticesBlockSide;
	worldUnitsMapSide = realVerticesMapSide * worldUnitsPerVertex;
	if (worldUnitsMapSide > Stuff::SMALL)
		oneOverWorldUnitsMapSide = 1.0f / worldUnitsMapSide;
	else
		oneOverWorldUnitsMapSide = 0.0f;

	// Tell GameOS the map extent for static shadow projection
	gos_SetMapHalfExtent(worldUnitsMapSide * 0.5f);

	Terrain::numObjBlocks = blocksMapSide * blocksMapSide;
	visibleVerticesPerSide = visibleVertices;
	terrainHeapSize = MAX_TERRAIN_HEAP_SIZE;

	//-----------------------------------------------------------------
	// Startup to Terrain Heap
	if( !terrainHeap )
	{
		ZoneScopedN("Terrain::init terrainHeap");
		terrainHeap = new UserHeap;
		gosASSERT(terrainHeap != NULL);
		terrainHeap->init(terrainHeapSize,"TERRAIN");
	}

	percent += percentRange/5.f;
	//-----------------------------------------------------------------
	// Startup the Terrain Texture Maps
	if ( !terrainTextures )
	{
		ZoneScopedN("Terrain::init terrainTextures");
		char baseName[256];
		if (pakFile)
		{
			_splitpath(pakFile->getFilename(),NULL,NULL,baseName,NULL);
		}
		else
		{
			strcpy(baseName,"newmap");
		}

		terrainTextures = new TerrainTextures;
		terrainTextures->init("textures",baseName);
	}

	percent += percentRange/5.f;


	if ( !pakFile && !realVerticesMapSide )
		return NO_ERR;

	//-----------------------------------------------------------------
	// Startup the Terrain Color Map
	if ( !terrainTextures2 && pakFile)
	{
		ZoneScopedN("Terrain::init terrainColorMap");
		char name[1024];

		_splitpath(pakFile->getFilename(),NULL,NULL,name,NULL);
		terrainName = new char[strlen(name)+1];
		strcpy(terrainName,name);

		if (colorMapName)
			strcpy(name,colorMapName);

		FullPathFileName tgaColorMapName;
		tgaColorMapName.init(texturePath,name,".tga");
		
		FullPathFileName tgaColorMapBurninName;
		tgaColorMapBurninName.init(texturePath,name,".burnin.tga");

		FullPathFileName tgaColorMapJPGName;
		tgaColorMapJPGName.init(texturePath,name,".burnin.jpg");
				
		if (fileExists(tgaColorMapName) || fileExists(tgaColorMapBurninName) || fileExists(tgaColorMapJPGName))
		{
			terrainTextures2 = new TerrainColorMap;		//Otherwise, this will stay NULL and we know not to use them
		}
	}

	percent += percentRange/5.f;


	mapTopLeft3d.x = -worldUnitsMapSide / 2.0f;
	mapTopLeft3d.y = worldUnitsMapSide / 2.0f;

	percent += percentRange/5.f;


	//----------------------------------------------------------------------
	// Setup number of blocks
	long numberBlocks = blocksMapSide * blocksMapSide;
	
	numObjBlocks = numberBlocks;
	objBlockInfo = (ObjBlockInfo *)terrainHeap->Malloc(sizeof(ObjBlockInfo)*numObjBlocks);
	gosASSERT(objBlockInfo != NULL);
	
	memset(objBlockInfo,0,sizeof(ObjBlockInfo)*numObjBlocks);
	
	objVertexActive = (bool *)terrainHeap->Malloc(sizeof(bool) * realVerticesMapSide * realVerticesMapSide);
	gosASSERT(objVertexActive != NULL);
	
	memset(objVertexActive,0,sizeof(bool)*numObjBlocks);
	
	moverBlockList = (long *)terrainHeap->Malloc(sizeof(long) * numberBlocks);
	gosASSERT(moverBlockList != NULL);
	
	usedBlockList = (long *)terrainHeap->Malloc(sizeof(long) * numberBlocks);
	gosASSERT(usedBlockList != NULL);
	
	clearList();
	clearMoverList();

	//----------------------------------------------------------------------
	// Calculate size of each mapblock
	long blockSize = verticesBlockSide * verticesBlockSide;
	blockSize *= sizeof(PostcompVertex);

	//----------------------------------------------------------------------
	// Create the MapBlock Manager and allocate its RAM
	if ( !mapData )
	{
		mapData = new MapData;
		if ( pakFile )
			mapData->newInit( pakFile, realVerticesMapSide*realVerticesMapSide);
		else
			mapData->newInit( realVerticesMapSide*realVerticesMapSide );

		mapTopLeft3d.z = mapData->getTopLeftElevation();
	}

	percent += percentRange/5.f;

	
	//----------------------------------------------------------------------
	// Create the VertexList
	numberVertices = 0;
	vertexList = (VertexPtr)terrainHeap->Malloc(sizeof(Vertex) * visibleVertices * visibleVertices);
	gosASSERT(vertexList != NULL);
	memset(vertexList,0,sizeof(Vertex) * visibleVertices * visibleVertices);

	//----------------------------------------------------------------------
	// Create the QuadList
	numberQuads = 0;
	quadList = (TerrainQuadPtr)terrainHeap->Malloc(sizeof(TerrainQuad) * visibleVertices * visibleVertices);
	gosASSERT(quadList != NULL);
	memset(quadList,0,sizeof(TerrainQuad) * visibleVertices * visibleVertices);

	//-------------------------------------------------------------------
	initMapCellArrays();

	//-----------------------------------------------------------------
	// Startup the Terrain Color Map
	if ( terrainTextures2  && !(terrainTextures2->colorMapStarted))
	{
		if (colorMapName)
			terrainTextures2->init(colorMapName);
		else
			terrainTextures2->init(terrainName);
	}

	return NO_ERR;
}

void Terrain::resetVisibleVertices (long maxVisibleVertices)
{
	terrainHeap->Free(vertexList);
	vertexList = NULL;

	terrainHeap->Free(quadList);
	quadList = NULL;

	visibleVerticesPerSide = maxVisibleVertices;
	//----------------------------------------------------------------------
	// Create the VertexList
	numberVertices = 0;
	vertexList = (VertexPtr)terrainHeap->Malloc(sizeof(Vertex) * visibleVerticesPerSide * visibleVerticesPerSide);
	gosASSERT(vertexList != NULL);
	memset(vertexList,0,sizeof(Vertex) * visibleVerticesPerSide * visibleVerticesPerSide);

	//----------------------------------------------------------------------
	// Create the QuadList
	numberQuads = 0;
	quadList = (TerrainQuadPtr)terrainHeap->Malloc(sizeof(TerrainQuad) * visibleVerticesPerSide * visibleVerticesPerSide);
	gosASSERT(quadList != NULL);
	memset(quadList,0,sizeof(TerrainQuad) * visibleVerticesPerSide * visibleVerticesPerSide);

	
}

//---------------------------------------------------------------------------
void Terrain::primeMissionTerrainCache (volatile float& progress, float progressRange)
{
	if (!mapData || !terrainTextures2)
		return;

	const float buildRange = progressRange * 0.5f;
	const float warmRange = progressRange - buildRange;
	{
		ZoneScopedN("Terrain::primeMissionTerrainCache build");
		mapData->buildTerrainFaceCache(&progress, buildRange);
	}
	{
		ZoneScopedN("Terrain::primeMissionTerrainCache warm");
		mapData->warmTerrainFaceCacheResidency(&progress, warmRange);
	}

	// Stage 2 of the renderWater architectural slice (CPU→GPU offload):
	// build the static, map-keyed WaterRecipe array. Iterates MapData::blocks
	// directly (mission-immutable) — independent of quadList which is
	// camera-windowed and reshuffles each frame. Spec:
	// docs/superpowers/specs/2026-04-29-renderwater-fastpath-design.md.
	{
		ZoneScopedN("Terrain::primeMissionTerrainCache water_stream_build");
		WaterStream::Reset();
		WaterStream::Build();
	}

	// Stage 2 of the indirect-terrain SOLID-only PR1 (CPU→GPU offload):
	// build the dense TerrainQuadRecipe array (mapSide² × 144 B) indexed by
	// vertexNum. Called AFTER buildTerrainFaceCache (line 585) so the Shape C
	// cache is ready when buildRecipeSlot reads UV data from it.
	// Gated on IsEnabled() OR IsParityCheckEnabled() — no allocation when both
	// are unset.
	if (gos_terrain_indirect::IsEnabled() ||
	    gos_terrain_indirect::IsParityCheckEnabled()) {
		gos_terrain_indirect::ResetDenseRecipe();
		gos_terrain_indirect::BuildDenseRecipe();
	}
}

//---------------------------------------------------------------------------
bool Terrain::IsValidTerrainPosition (const Stuff::Vector3D pos)
{
	float metersCheck = (Terrain::worldUnitsMapSide / 2.0f);

	if ((pos.x > -metersCheck) &&
		(pos.x < metersCheck) &&
		(pos.y > -metersCheck) &&
		(pos.y < metersCheck))
	{
		return true;
	}

	return false;
}

//---------------------------------------------------------------------------
bool Terrain::IsEditorSelectTerrainPosition (const Stuff::Vector3D pos)
{
	float metersCheck = (Terrain::worldUnitsMapSide / 2.0f) - Terrain::worldUnitsPerVertex;

	if ((pos.x > -metersCheck) &&
		(pos.x < metersCheck) &&
		(pos.y > -metersCheck) &&
		(pos.y < metersCheck))
	{
		return true;
	}

	return false;
}

//---------------------------------------------------------------------------
bool Terrain::IsGameSelectTerrainPosition (const Stuff::Vector3D pos)
{
	float metersCheck = (Terrain::worldUnitsMapSide / 2.0f) - (Terrain::worldUnitsPerVertex * 2.0f);

	if ((pos.x > -metersCheck) &&
		(pos.x < metersCheck) &&
		(pos.y > -metersCheck) &&
		(pos.y < metersCheck))
	{
		return true;
	}

	return false;
}

//---------------------------------------------------------------------------
void Terrain::purgeTransitions (void)
{
	terrainTextures->purgeTransitions();
	mapData->calcTransitions();
}

//---------------------------------------------------------------------------
void Terrain::destroy (void)
{
	// Per-mission dense recipe teardown (Stage 2 indirect-terrain PR1).
	// Called from Mission::destroy → land->destroy() once per mission exit.
	// CPU-clears state; GL buffer is kept for reuse by next mission's Build.
	if (gos_terrain_indirect::IsEnabled() ||
	    gos_terrain_indirect::IsParityCheckEnabled()) {
		gos_terrain_indirect::ResetDenseRecipe();
	}

	if (terrainTextures)
	{
		terrainTextures->destroy();
		delete terrainTextures;
		terrainTextures = NULL;
	}

	if (terrainTextures2)
	{
		terrainTextures2->destroy();
		delete terrainTextures2;
		terrainTextures2 = NULL;
	}

	delete mapData;
	mapData = NULL;

	if (terrainName)
	{
		delete [] terrainName;
		terrainName = NULL;
	}

	if (colorMapName)
	{
		delete [] colorMapName;
		colorMapName = NULL;
	}

	if (tileRowToWorldCoord)
	{
		terrainHeap->Free(tileRowToWorldCoord);
		tileRowToWorldCoord = NULL;
	}

	if (tileColToWorldCoord)
	{
		terrainHeap->Free(tileColToWorldCoord); 
		tileColToWorldCoord = NULL;
	}

	if (cellToWorldCoord)
	{
		terrainHeap->Free(cellToWorldCoord); 
		cellToWorldCoord = NULL;
	}

	if (cellColToWorldCoord)
	{
		terrainHeap->Free(cellColToWorldCoord); 
		cellColToWorldCoord = NULL;
	}

	if (cellRowToWorldCoord)
	{
		terrainHeap->Free(cellRowToWorldCoord); 
		cellRowToWorldCoord = NULL;
	}

	if (moverBlockList)
	{
		terrainHeap->Free(moverBlockList);
		moverBlockList = NULL;
	}

	if (usedBlockList)
	{
		terrainHeap->Free(usedBlockList);
		usedBlockList = NULL;
	}

	if (vertexList)
	{
		terrainHeap->Free(vertexList);
		vertexList = NULL;
	}

	if (quadList)
	{
		terrainHeap->Free(quadList);
		quadList = NULL;
	}

	if (objBlockInfo)
	{
		terrainHeap->Free(objBlockInfo);
		objBlockInfo = NULL;
	}
	
	if (objVertexActive)
	{
		terrainHeap->Free(objVertexActive);
		objVertexActive = NULL;
	}
	
 	if (terrainHeap)
	{
		terrainHeap->destroy();
		delete terrainHeap;
		terrainHeap = NULL;
	}
	
	numberVertices =
	numberQuads =
	
	halfVerticesMapSide = 
	realVerticesMapSide = 
		
	visibleVerticesPerSide =
	blocksMapSide = 0;
	
	worldUnitsMapSide = 0.0f;
	
	mapTopLeft3d.Zero();
		
	numObjBlocks = 0;

	recalcShadows = 
	recalcLight = false;

	//Reset these.  This will fix the mine problem.
	TerrainQuad::rainLightLevel = 1.0f;
	TerrainQuad::lighteningLevel = 0;
	TerrainQuad::mineTextureHandle = 0xffffffff;
	TerrainQuad::blownTextureHandle = 0xffffffff;
}

extern float textureOffset;
//---------------------------------------------------------------------------
long Terrain::update (void)
{
	ZoneScopedN("Terrain::update");
	//-----------------------------------------------------------------
	// Startup the Terrain Color Map
	if ( terrainTextures2  && !(terrainTextures2->colorMapStarted))
	{
		ZoneScopedN("Terrain::update startColorMap");
		if (colorMapName)
			terrainTextures2->init(colorMapName);
		else
			terrainTextures2->init(terrainName);
	}

	//----------------------------------------------------------------
	// Nothing is ever visible.  We recalc every frame.  True LOS!
//	Terrain::VisibleBits->resetAll(0);
		
	if (godMode)	
	{
//		Terrain::VisibleBits->resetAll(0xff);
	}

	if (turn > terrainLineChanged+10)
	{
		ZoneScopedN("Terrain::update debugHotkeys");
		if (userInput->getKeyDown(KEY_UP) && userInput->ctrl() && userInput->alt() && !userInput->shift())
		{
			textureOffset += 0.1f;;
			terrainLineChanged = turn;
		}
		
		if (userInput->getKeyDown(KEY_DOWN) && userInput->ctrl() && userInput->alt() && !userInput->shift())
		{
			textureOffset -= 0.1f;;
			terrainLineChanged = turn;
		}
	}
	
 	//---------------------------------------------------------------------
	{
		ZoneScopedN("Terrain::update mapDataUpdate");
		Terrain::mapData->update();
	}
	{
		ZoneScopedN("Terrain::update makeLists");
		Terrain::mapData->makeLists(vertexList,numberVertices,quadList,numberQuads);
	}

	// Set terrain light direction for normal map shader
	if (eye)
	{
		ZoneScopedN("Terrain::update cameraParams");
		// Light direction now set from gamecam.cpp with proper MC2->GL swizzle
		// gos_SetTerrainLightDir(eye->lightDirection.x, eye->lightDirection.y, eye->lightDirection.z);

		// Pass camera world position in raw MC2 space (matching vs_WorldPos for TCS distance LOD)
		Stuff::Vector3D camOrigin = eye->getCameraOrigin();
		gos_SetTerrainCameraPos(camOrigin.x, camOrigin.y, camOrigin.z);

		// Pass camera look direction for POM (direction camera looks toward terrain)
		Stuff::Vector3D lookDir = eye->getLookVector();
		// Swizzle same as camera pos, then normalize
		float lx = -lookDir.x, ly = lookDir.z, lz = lookDir.y;
		float len = sqrtf(lx*lx + ly*ly + lz*lz);
		if (len > 0.001f) { lx /= len; ly /= len; lz /= len; }
		gos_SetTerrainViewDir(lx, ly, lz);
	}

	return TRUE;
}

//---------------------------------------------------------------------------
void Terrain::setOverlayTile (long block, long vertex, long offset)
{
	mapData->setOverlayTile(block,vertex,offset);
}	

//---------------------------------------------------------------------------
void Terrain::setOverlay( long tileR, long tileC, Overlays type, DWORD offset )
{
	mapData->setOverlay( tileR, tileC, type, offset );
}

//---------------------------------------------------------------------------
void Terrain::setTerrain( long tileR, long tileC, int terrainType )
{
	mapData->setTerrain( tileR, tileC, terrainType );
}

//---------------------------------------------------------------------------
int Terrain::getTerrain( long tileR, long tileC )
{
	return mapData->getTerrain( tileR, tileC );
}

//---------------------------------------------------------------------------
void Terrain::calcWater (float waterDepth, float waterShallowDepth, float waterAlphaDepth)
{
	mapData->calcWater(waterDepth, waterShallowDepth, waterAlphaDepth);
}	

//---------------------------------------------------------------------------
long Terrain::getOverlayTile (long block, long vertex)
{
	return (mapData->getOverlayTile(block,vertex));
}	

//---------------------------------------------------------------------------
void Terrain::getOverlay( long tileR, long tileC, enum Overlays& type, DWORD& Offset )
{
	mapData->getOverlay( tileR, tileC, type, Offset );
}

//---------------------------------------------------------------------------
void Terrain::setVertexHeight( int VertexIndex, float Val )
{
	if ( VertexIndex > -1 && VertexIndex < realVerticesMapSide * realVerticesMapSide )
		mapData->setVertexHeight( VertexIndex, Val );
}

//---------------------------------------------------------------------------
float Terrain::getVertexHeight( int VertexIndex )
{
	if ( VertexIndex > -1 && VertexIndex < realVerticesMapSide * realVerticesMapSide )
		return mapData->getVertexHeight(VertexIndex);

	return -1.f;
}

//---------------------------------------------------------------------------
void Terrain::render (void)
{
	//-----------------------------------
	// render the cloud layer
	if (Terrain::cloudLayer)
		Terrain::cloudLayer->render();
	
	//-----------------------------------
	// Draw resulting terrain quads. Loop split into 3 passes (draw / drawMine /
	// debugOverlays) so each gets its own Tracy zone — one zone per pass instead
	// of ~14K per-quad zones.
	DWORD fogColor = eye->fogColor;

	if (drawTerrainTiles)
	{
		ZoneScopedN("Terrain::render drawPass");
		TerrainQuadPtr currentQuad = quadList;
		for (long i = 0; i < numberQuads; i++)
		{
			// M2b loop-level pure-water hoist: skip the function call entirely for
			// quads with no base terrain, no overlay, and no detail handle. ~28K
			// quads/frame on water-heavy maps. Mirror of the in-draw() early-exit;
			// the in-function check is the fallback if useOverlayTexture /
			// useWaterInterestTexture globals get toggled at runtime.
			if (currentQuad->terrainHandle == 0
			    && currentQuad->overlayHandle == 0xffffffff
			    && currentQuad->terrainDetailHandle == 0xffffffff)
			{
				currentQuad++;
				continue;
			}
			currentQuad->draw();
			currentQuad++;
		}
	}

	if (drawTerrainTiles)
	{
		ZoneScopedN("Terrain::render minePass");
		TerrainQuadPtr currentQuad = quadList;
		for (long i = 0; i < numberQuads; i++)
		{
			currentQuad->drawMine();
			currentQuad++;
		}
	}

	if (drawTerrainGrid || DrawDebugCells || drawLOSGrid)
	{
		ZoneScopedN("Terrain::render debugOverlays");
		TerrainQuadPtr currentQuad = quadList;
		for (long i = 0; i < numberQuads; i++)
		{
			if (drawTerrainGrid)
			{
				if (useFog) gos_SetRenderState(gos_State_Fog, 0);
				currentQuad->drawLine();
				if (useFog) gos_SetRenderState(gos_State_Fog, fogColor);
			}
			else if (DrawDebugCells)
			{
				if (useFog) gos_SetRenderState(gos_State_Fog, 0);
				currentQuad->drawDebugCellLine();
				if (useFog) gos_SetRenderState(gos_State_Fog, fogColor);
			}
			else if (drawLOSGrid)
			{
				if (useFog) gos_SetRenderState(gos_State_Fog, 0);
				currentQuad->drawLOSLine();
				if (useFog) gos_SetRenderState(gos_State_Fog, fogColor);
			}
			currentQuad++;
		}
	}
}

//---------------------------------------------------------------------------
void Terrain::renderWater (void)
{
	ZoneScopedN("Terrain::renderWater");

	// MC2_WATER_DEBUG=1: post-warmup population recon for the renderWater slice.
	// Reports per-frame how many quads are pure-skip (waterHandle == 0xffffffff,
	// out-of-frustum or non-water by map data) vs handle-valid (the upper bound
	// on the actually-emitting subset). Mirrors the MC2_THIN_DEBUG pattern in
	// quad.cpp: prints 5 frames after a 1200-frame warmup hold-off, then dormant.
	static const bool s_waterDebugOn = (getenv("MC2_WATER_DEBUG") != nullptr);
	static uint32_t s_total = 0;
	static uint32_t s_handleValid = 0;
	static uint32_t s_detailEligibleByHandle = 0;
	static uint32_t s_framesPrinted = 0;
	static uint32_t s_frameCounter = 0;
	static uint64_t s_qpcFreq = 0;
	static uint64_t s_qpcStart = 0;
	constexpr uint32_t kWaterWarmupHoldoffFrames = 1200;
	if (s_waterDebugOn && s_qpcFreq == 0)
		QueryPerformanceFrequency((LARGE_INTEGER*)&s_qpcFreq);
	if (s_waterDebugOn)
		QueryPerformanceCounter((LARGE_INTEGER*)&s_qpcStart);

	// MC2_RENDER_WATER_FASTPATH=1: skip legacy water queueing entirely.
	// The actual draw runs from Terrain::renderWaterFastPath() AFTER
	// mcTextureManager->renderLists() has flushed terrain — otherwise
	// terrain would render OVER our water and overwrite it.
	static const bool s_fastPath =
	    (getenv("MC2_RENDER_WATER_FASTPATH") != nullptr);
	if (s_fastPath
	    && WaterStream::IsReady()
	    && WaterStream::GetRecipeCount() > 0
	    && Terrain::terrainTextures2 != nullptr)
	{
		// Skip legacy loop entirely; renderWaterFastPath() does the work.
		return;
	}

	//-----------------------------------
	// Draw resulting terrain quads
	TerrainQuadPtr currentQuad = quadList;

	const bool collect = s_waterDebugOn && (s_framesPrinted < 5)
	                     && (s_frameCounter >= kWaterWarmupHoldoffFrames);

	for (long i=0;i<numberQuads;i++)
	{
		if (collect)
		{
			++s_total;
			if (currentQuad->waterHandle != 0xffffffff)
			{
				++s_handleValid;
				if (currentQuad->waterDetailHandle != 0xffffffff)
					++s_detailEligibleByHandle;
			}
		}

		if (drawTerrainTiles)
			currentQuad->drawWater();

		currentQuad++;
	}

	if (s_waterDebugOn)
	{
		uint64_t qpcEnd = 0;
		QueryPerformanceCounter((LARGE_INTEGER*)&qpcEnd);
		const uint64_t elapsedTicks = qpcEnd - s_qpcStart;
		const double elapsedUs = (s_qpcFreq > 0)
		    ? (1000000.0 * (double)elapsedTicks / (double)s_qpcFreq)
		    : 0.0;
		if (s_framesPrinted < 5)
		{
			++s_frameCounter;
			if (s_frameCounter >= kWaterWarmupHoldoffFrames)
			{
				fprintf(stderr,
				        "[WATER_DEBUG v1] event=population frame=%u (post-warmup) "
				        "total=%u handle_valid=%u detail_eligible=%u "
				        "elapsed_us=%.1f\n",
				        s_framesPrinted, s_total, s_handleValid,
				        s_detailEligibleByHandle, elapsedUs);
				fflush(stderr);
				s_total = s_handleValid = s_detailEligibleByHandle = 0;
				++s_framesPrinted;
			}
		}
	}
}

//---------------------------------------------------------------------------
// Stage 2 of renderWater architectural slice. Called from gamecam.cpp AFTER
// mcTextureManager->renderLists() so terrain has been flushed and
// depth-written. This is required for the alpha-blend-on-top semantics:
// running fast-path INSIDE renderWater() (before renderLists) means terrain
// hasn't drawn yet and overwrites our water.
void Terrain::renderWaterFastPath (void)
{
	static const bool s_fastPath =
	    (getenv("MC2_RENDER_WATER_FASTPATH") != nullptr);
	if (!s_fastPath) return;
	if (!WaterStream::IsReady()) return;
	if (WaterStream::GetRecipeCount() == 0) return;
	if (!Terrain::terrainTextures2) return;

	ZoneScopedN("Terrain::renderWaterFastPath");

	// MC2_WATER_DEBUG=1 parallel timer (matches the legacy renderWater printer
	// style at terrain.cpp:1004-1077, post-warmup window of 5 frames). Lets
	// gate B Tracy/perf comparison run side-by-side with one env var.
	static const bool s_waterDebugOn = (getenv("MC2_WATER_DEBUG") != nullptr);
	static uint64_t s_qpcFreq2 = 0;
	static uint64_t s_qpcStart2 = 0;
	static uint32_t s_framesPrinted2 = 0;
	static uint32_t s_frameCounter2  = 0;
	constexpr uint32_t kFastWarmupHoldoffFrames = 1200;
	if (s_waterDebugOn && s_qpcFreq2 == 0)
		QueryPerformanceFrequency((LARGE_INTEGER*)&s_qpcFreq2);
	if (s_waterDebugOn)
		QueryPerformanceCounter((LARGE_INTEGER*)&s_qpcStart2);

	// getWater*Handle() returns mcTextureManager's textureIndex (master node
	// id), NOT the engine's gosTextureHandle. tex_resolve() chases the lazy
	// first-touch indirection — same pattern as M2d overlay at quad.cpp:2084.
	const DWORD waterTexIdx =
	    Terrain::terrainTextures2->getWaterTextureHandle();
	const DWORD waterDetailTexIdx =
	    Terrain::terrainTextures2->getWaterDetailHandle(sprayFrame);
	const DWORD waterTexHandle =
	    (waterTexIdx != 0xffffffff) ? tex_resolve(waterTexIdx) : 0u;
	const DWORD waterDetailTexHandle =
	    (waterDetailTexIdx != 0xffffffff) ? tex_resolve(waterDetailTexIdx) : 0xffffffffu;

	const float oneOverWaterTF =
	    Terrain::terrainTextures2->getWaterDetailTilingFactor()
	    / Terrain::worldUnitsMapSide;
	const float oneOverTF =
	    Terrain::terrainTextures2->getWaterTextureTilingFactor()
	    / Terrain::worldUnitsMapSide;

	const float cloudOffsetX =
	    cosf(360.0f * DEGREES_TO_RADS * 32.0f * cloudScrollX) * 0.1f;
	const float cloudOffsetY =
	    sinf(360.0f * DEGREES_TO_RADS * 32.0f * cloudScrollY) * 0.1f;
	const float sprayOffsetX = cloudScrollX * 10.0f;
	const float sprayOffsetY = cloudScrollY * 10.0f;

	{
		static bool s_dumped = false;
		if (!s_dumped && getenv("MC2_WATER_STREAM_DEBUG") != nullptr) {
			s_dumped = true;
			fprintf(stderr,
			        "[WATER_FAST v1] event=alpha_uniforms waterElevation=%.3f "
			        "alphaDepth=%.3f alphaEdgeByte=%u alphaMiddleByte=%u alphaDeepByte=%u\n",
			        (double)Terrain::waterElevation, (double)MapData::alphaDepth,
			        (unsigned)((Terrain::alphaEdge   >> 24) & 0xFFu),
			        (unsigned)((Terrain::alphaMiddle >> 24) & 0xFFu),
			        (unsigned)((Terrain::alphaDeep   >> 24) & 0xFFu));
			fflush(stderr);
		}
	}
	gos_terrain_bridge_renderWaterFast(
	    WaterStream::GetRecipeCount(),
	    (unsigned int)waterTexHandle,
	    (unsigned int)waterDetailTexHandle,
	    Terrain::waterElevation,
	    MapData::alphaDepth,
	    (unsigned int)((Terrain::alphaEdge   >> 24) & 0xFFu),
	    (unsigned int)((Terrain::alphaMiddle >> 24) & 0xFFu),
	    (unsigned int)((Terrain::alphaDeep   >> 24) & 0xFFu),
	    Terrain::mapTopLeft3d.x,
	    Terrain::mapTopLeft3d.y,
	    Terrain::frameCos,
	    Terrain::frameCosAlpha,
	    oneOverTF,
	    oneOverWaterTF,
	    cloudOffsetX,
	    cloudOffsetY,
	    sprayOffsetX,
	    sprayOffsetY,
	    MaxMinUV);

	// Stage 3 parity check (env-gated, silent on pass). Runs AFTER the bridge
	// so g_thinStaging is already populated by UploadAndBindThinRecords. The
	// check is CPU-only; it does not alter GPU state. See
	// `gos_terrain_water_stream.h` "Stage 3 parity check" doc-comment for
	// scope and field-level granularity.
	{
		WaterStream::ParityFrameUniforms pu;
		pu.waterElevation             = Terrain::waterElevation;
		pu.alphaDepth                 = MapData::alphaDepth;
		pu.alphaEdgeDword             = Terrain::alphaEdge;
		pu.alphaMiddleDword           = Terrain::alphaMiddle;
		pu.alphaDeepDword             = Terrain::alphaDeep;
		pu.mapTopLeftX                = Terrain::mapTopLeft3d.x;
		pu.mapTopLeftY                = Terrain::mapTopLeft3d.y;
		pu.frameCos                   = Terrain::frameCos;
		pu.frameCosAlpha              = Terrain::frameCosAlpha;
		pu.oneOverTF                  = oneOverTF;
		pu.oneOverWaterTF             = oneOverWaterTF;
		pu.cloudOffsetX               = cloudOffsetX;
		pu.cloudOffsetY               = cloudOffsetY;
		pu.sprayOffsetX               = sprayOffsetX;
		pu.sprayOffsetY               = sprayOffsetY;
		pu.maxMinUV                   = MaxMinUV;
		pu.useWaterInterestTexture    = useWaterInterestTexture;
		pu.waterDetailHandleSentinel  = (uint32_t)waterDetailTexHandle;
		pu.terrainTextures2Present    = (Terrain::terrainTextures2 != nullptr);
		WaterStream::CheckParityFrame(pu);
	}

	if (s_waterDebugOn)
	{
		uint64_t qpcEnd = 0;
		QueryPerformanceCounter((LARGE_INTEGER*)&qpcEnd);
		const uint64_t elapsedTicks = qpcEnd - s_qpcStart2;
		const double elapsedUs = (s_qpcFreq2 > 0)
		    ? (1000000.0 * (double)elapsedTicks / (double)s_qpcFreq2) : 0.0;
		++s_frameCounter2;
		if (s_frameCounter2 >= kFastWarmupHoldoffFrames && s_framesPrinted2 < 5)
		{
			fprintf(stderr,
			        "[WATER_FAST v1] event=elapsed frame=%u (post-warmup) "
			        "recipeCount=%u elapsed_us=%.1f\n",
			        s_framesPrinted2,
			        (unsigned)WaterStream::GetRecipeCount(),
			        elapsedUs);
			fflush(stderr);
			++s_framesPrinted2;
		}
	}
}

float cosineEyeHalfFOV = 0.0f;
#define MAX_CAMERA_RADIUS		(250.0f)
#define CLIP_THRESHOLD_DISTANCE	(768.0f)

//a full triangle.
#define VERTEX_EXTENT_RADIUS	(384.0f)

float leastZ = 1.0f,leastW = 1.0f;
float mostZ = -1.0f, mostW = -1.0;
float leastWY = 0.0f, mostWY = 0.0f;
extern bool InEditor;

//---------------------------------------------------------------------------
// vertexProjectLoop fast-path (D1: CPU loop hoist + skip PROJECTZ_SITE +
// direct cull-cascade array writes). See:
//   docs/superpowers/cpu-to-gpu-offload-orchestrator.md (Status Board)
//   memory/cull_gates_are_load_bearing.md (why side effects can't move)
//
// MC2_VERTEX_PROJECT_FAST=1   — enables fast path (default off).
// MC2_VERTEX_PROJECT_PARITY=1 — runs legacy in parallel and byte-compares
//                               per-vertex outputs. Silent on pass; field-
//                               level mismatch printer (16/frame throttle)
//                               + 600-frame summary.
//---------------------------------------------------------------------------
namespace {
struct VPParitySnap {
	bool  clipInfo;
	float px, py, pz, pw;
	float hazeFactor;
};
}
//---------------------------------------------------------------------------
void Terrain::geometry (void)
{
	ZoneScopedN("Terrain::geometry");

	// Shape A (M0a) — per-frame texture-handle memoization. Initialized at
	// the EARLIEST terrain frame boundary because converted setup-time reads
	// in TerrainQuad::setupTextures, ensureTerrainFaceCacheEntryResident, and
	// terrtxm{,2}.h accessors fire during this function (mission-update phase),
	// before GameCamera::render. See
	// docs/superpowers/specs/2026-04-27-modern-terrain-tex-resolve-table-design.md.
	{
		static uint64_t s_texResolveFrameCounter = 0;
		beginFrameTexResolve(++s_texResolveFrameCounter);
	}

	//---------------------------------------------------------------------
	leastZ = 1.0f;leastW = 1.0f;
	mostZ = -1.0f; mostW = -1.0;
	leastWY = 0.0f; mostWY = 0.0f;

	//-----------------------------------
	// Transform entire list of vertices
	VertexPtr currentVertex = vertexList;

	Stuff::Vector3D cameraPos;
	cameraPos.x = -eye->getCameraOrigin().x;
	cameraPos.y = eye->getCameraOrigin().z;
	cameraPos.z = eye->getCameraOrigin().y;

	float vClipConstant = eye->verticalSphereClipConstant;
	float hClipConstant = eye->horizontalSphereClipConstant; 
	
	// vertexProjectLoop fast-path (D1) — see file-scope namespace block above.
	static const bool s_vpFast   = (getenv("MC2_VERTEX_PROJECT_FAST")   != nullptr);
	static const bool s_vpParity = (getenv("MC2_VERTEX_PROJECT_PARITY") != nullptr);
	static uint32_t   s_vpFrame  = 0;
	static uint64_t   s_vpVertsChecked  = 0;
	static uint64_t   s_vpVertsMismatch = 0;

	std::vector<VPParitySnap> vpFastSnap;
	if (s_vpFast && s_vpParity)
		vpFastSnap.resize(numberVertices);

	if (s_vpFast)
	{
		// Hoist invariants out of the hot loop.
		const bool  vp_usePersp        = eye->usePerspective;
		const bool  vp_isPerspRenderer = vp_usePersp && (Environment.Renderer != 3);
		const bool  vp_drawGrid        = drawTerrainGrid;
		const float vp_maxClipD        = Camera::MaxClipDistance;
		const float vp_minHazeD        = Camera::MinHazeDistance;
		const float vp_distFact        = Camera::DistanceFactor;
		const long  vp_numObjB         = numObjBlocks;
		const long  vp_numActiveVerts  = realVerticesMapSide * realVerticesMapSide;

		ZoneScopedN("Terrain::geometry vertexProjectLoop");
		VertexPtr cv = vertexList;
		for (long vi = 0; vi < numberVertices; ++vi, ++cv)
		{
			// Math byte-identical to legacy block at terrain.cpp:1298-1450.
			// Differences from legacy: PROJECTZ_SITE() is omitted (g_pzTrace is
			// debug-only); cull-cascade setters are inlined as direct array
			// writes so the compiler doesn't gamble on inlining setObjBlockActive.
			bool onScreen = false;
			float hazeFactor = 0.0f;

			if (vp_usePersp)
			{
				onScreen = true;

				Stuff::Vector3D vPosition;
				vPosition.x = cv->vx;
				vPosition.y = cv->vy;
				vPosition.z = cv->pVertex->elevation;

				Stuff::Vector3D objectCenter;
				objectCenter.Subtract(vPosition, cameraPos);
				Camera::cameraFrame.trans_to_frame(objectCenter);
				float distanceToEye = objectCenter.GetApproximateLength();

				Stuff::Vector3D clipVector = objectCenter;
				clipVector.z = 0.0f;
				float distanceToClip = clipVector.GetApproximateLength();
				float clip_distance = fabs(1.0f / objectCenter.y);

				if (distanceToClip > CLIP_THRESHOLD_DISTANCE)
				{
					float object_angle = fabs(objectCenter.z) * clip_distance;
					float extent_angle = VERTEX_EXTENT_RADIUS / distanceToEye;
					if (object_angle > (vClipConstant + extent_angle))
					{
						onScreen = false;
					}
					else
					{
						object_angle = fabs(objectCenter.x) * clip_distance;
						if (object_angle > (hClipConstant + extent_angle))
							onScreen = false;
					}
				}

				if (onScreen)
				{
					if (distanceToEye > vp_maxClipD)      hazeFactor = 1.0f;
					else if (distanceToEye > vp_minHazeD) hazeFactor = (distanceToEye - vp_minHazeD) * vp_distFact;
					else                                  hazeFactor = 0.0f;

					Stuff::Vector3D vPos(cv->vx, cv->vy, cv->pVertex->elevation);
					bool isVisible = Terrain::IsGameSelectTerrainPosition(vPos) || vp_drawGrid;
					if (!isVisible)
					{
						hazeFactor = 1.0f;
						onScreen = true;
					}
				}
				else
				{
					hazeFactor = 1.0f;
				}
			}
			else
			{
				hazeFactor = 0.0f;
				onScreen = true;
			}

			bool inView = false;
			Stuff::Vector4D screenPos(-10000.0f, -10000.0f, -10000.0f, -10000.0f);
			float pxL, pyL, pzL, pwL;
			float hazeL = hazeFactor;

			if (onScreen)
			{
				Stuff::Vector3D vertex3D(cv->vx, cv->vy, cv->pVertex->elevation);
				inView = eye->projectForTerrainAdmission(vertex3D, screenPos);
				pxL = screenPos.x;
				pyL = screenPos.y;
				pzL = screenPos.z;
				pwL = screenPos.w;
			}
			else
			{
				pxL = pyL = 10000.0f;
				pzL = -0.5f;
				pwL = 0.5f;
				hazeL = 0.0f;
			}

			const bool clipInfoFinal = vp_isPerspRenderer ? onScreen : inView;

			if (s_vpParity)
			{
				// Snapshot only — legacy will write live state below.
				VPParitySnap& s = vpFastSnap[vi];
				s.clipInfo   = clipInfoFinal;
				s.px         = pxL;
				s.py         = pyL;
				s.pz         = pzL;
				s.pw         = pwL;
				s.hazeFactor = hazeL;
			}
			else
			{
				// Live writes (cull cascade + accumulators).
				cv->hazeFactor = hazeL;
				cv->px = pxL;
				cv->py = pyL;
				cv->pz = pzL;
				cv->pw = pwL;
				cv->clipInfo = clipInfoFinal;

				if (clipInfoFinal)
				{
					const long blockNum = cv->getBlockNumber();
					if ((blockNum >= 0) && (blockNum < vp_numObjB))
						objBlockInfo[blockNum].active = true;

					const long vertNum = cv->vertexNum;
					if ((vertNum >= 0) && (vertNum < vp_numActiveVerts))
						objVertexActive[vertNum] = true;

					if (inView)
					{
						if (screenPos.z < leastZ) leastZ = screenPos.z;
						if (screenPos.z > mostZ)  mostZ  = screenPos.z;
						if (screenPos.w < leastW) { leastW = screenPos.w; leastWY = screenPos.y; }
						if (screenPos.w > mostW)  { mostW  = screenPos.w; mostWY  = screenPos.y; }
					}
				}
			}
		}
	}

	long i=0;
	if (!s_vpFast || s_vpParity)
	{
		ZoneScopedN("Terrain::geometry vertexProjectLoop");
		for (i=0;i<numberVertices;i++)
	{
		//----------------------------------------------------------------------------------------
		// Figure out if we are in front of camera or not.  Should be faster then actual project!
		// Should weed out VAST overwhelming majority of vertices!
		bool onScreen = false;
	
		//-----------------------------------------------------------------
		// Find angle between lookVector of Camera and vector from camPos
		// to Target.  If angle is less then halfFOV, object is visible.
		if (eye->usePerspective)
		{
			//-------------------------------------------------------------------
			//NEW METHOD from the WAY BACK Days
			onScreen = true;
			
			Stuff::Vector3D vPosition;
			vPosition.x = currentVertex->vx;
			vPosition.y = currentVertex->vy;
			vPosition.z = currentVertex->pVertex->elevation;
  
			Stuff::Vector3D objectCenter;
			objectCenter.Subtract(vPosition,cameraPos);
			Camera::cameraFrame.trans_to_frame(objectCenter);
			float distanceToEye = objectCenter.GetApproximateLength();

			Stuff::Vector3D clipVector = objectCenter;
			clipVector.z = 0.0f;
			float distanceToClip = clipVector.GetApproximateLength();
			float clip_distance = fabs(1.0f / objectCenter.y);
			
			if (distanceToClip > CLIP_THRESHOLD_DISTANCE)
			{
				//Is vertex on Screen OR close enough to screen that its triangle MAY be visible?
				// WE have removed the atans here by simply taking the tan of the angle we want above.
				float object_angle = fabs(objectCenter.z) * clip_distance;
				float extent_angle = VERTEX_EXTENT_RADIUS / distanceToEye;
				if (object_angle > (vClipConstant + extent_angle))
				{
					//In theory, we would return here.  Object is NOT on screen.
					onScreen = false;
				}
				else
				{
					object_angle = fabs(objectCenter.x) * clip_distance;
					if (object_angle > (hClipConstant + extent_angle))
					{
						//In theory, we would return here.  Object is NOT on screen.
						onScreen = false;
					}
				}
			}
			
			if (onScreen)
			{
				if (distanceToEye > Camera::MaxClipDistance)
				{
					currentVertex->hazeFactor = 1.0f;
				}
				else if (distanceToEye > Camera::MinHazeDistance)
				{
					currentVertex->hazeFactor = (distanceToEye - Camera::MinHazeDistance) * Camera::DistanceFactor;
				}
				else
				{
					currentVertex->hazeFactor = 0.0f;
				}
				
				//---------------------------------------
				// Vertex is at edge of world or beyond.
				Stuff::Vector3D vPos(currentVertex->vx,currentVertex->vy,currentVertex->pVertex->elevation);
				bool isVisible = Terrain::IsGameSelectTerrainPosition(vPos) || drawTerrainGrid;
				if (!isVisible)
				{
					currentVertex->hazeFactor = 1.0f;
					onScreen = true;
				}
			}
			else
			{
				currentVertex->hazeFactor = 1.0f;
			}
		}
		else
		{
			currentVertex->hazeFactor = 0.0f;
			onScreen = true;
		}

		bool inView = false;
		Stuff::Vector4D screenPos(-10000.0f,-10000.0f,-10000.0f,-10000.0f);
		if (onScreen)
		{
			Stuff::Vector3D vertex3D(currentVertex->vx,currentVertex->vy,currentVertex->pVertex->elevation);
			// [PROJECTZ:BoolAdmission id=terrain_cpu_vert_admit]
			PROJECTZ_SITE("terrain_cpu_vert_admit", "BoolAdmission");
			inView = eye->projectForTerrainAdmission(vertex3D,screenPos);
		
			currentVertex->px = screenPos.x;
			currentVertex->py = screenPos.y;
			currentVertex->pz = screenPos.z;
			currentVertex->pw = screenPos.w;
			
			//----------------------------------------------------------------------------------
			//We must transform these but should NOT draw any face where all three are fogged. 
//			if (currentVertex->hazeFactor == 1.0f)		
//				onScreen = false;
		}
		else
		{
			currentVertex->px = currentVertex->py = 10000.0f;
			currentVertex->pz = -0.5f;
			currentVertex->pw = 0.5f;
			currentVertex->hazeFactor = 0.0f;
		}	
		
		//------------------------------------------------------------
		// Fix clip.  Vertices can all be off screen and triangle
		// still needs to be drawn!
		if (eye->usePerspective && Environment.Renderer != 3)
		{
			currentVertex->clipInfo = onScreen;
		}
		else
			currentVertex->clipInfo = inView;
		
		if (currentVertex->clipInfo)				//ONLY set TRUE ones.  Otherwise we just reset the FLAG each vertex!
		{
			setObjBlockActive(currentVertex->getBlockNumber(), true);
			setObjVertexActive(currentVertex->vertexNum,true);
			
			if (inView)
			{
				if (screenPos.z < leastZ)
				{
					leastZ = screenPos.z;
				}
				
				if (screenPos.z > mostZ)
				{
					mostZ = screenPos.z;
				}
				
				if (screenPos.w < leastW)
				{
					leastW = screenPos.w;
					leastWY = screenPos.y;
				}
				
				if (screenPos.w > mostW)
				{
					mostW = screenPos.w;
					mostWY = screenPos.y;
				}
			}
		}

			currentVertex++;
		}
	}

	// vertexProjectLoop parity compare. Field-level mismatch printer
	// (16/frame throttle) + 600-frame summary. Silent on pass.
	// Comparison is on raw per-vertex output bytes — same CPU math both sides,
	// so any mismatch is a real bug, not FP drift.
	if (s_vpFast && s_vpParity)
	{
		++s_vpFrame;
		const int kMaxPrints = 16;
		int printsThisFrame = 0;
		VertexPtr cv = vertexList;
		for (long vi = 0; vi < numberVertices; ++vi, ++cv)
		{
			++s_vpVertsChecked;
			const VPParitySnap& s = vpFastSnap[vi];
			const bool match =
				((DWORD)s.clipInfo == cv->clipInfo) &&
				(s.px         == cv->px) &&
				(s.py         == cv->py) &&
				(s.pz         == cv->pz) &&
				(s.pw         == cv->pw) &&
				(s.hazeFactor == cv->hazeFactor);
			if (!match)
			{
				++s_vpVertsMismatch;
				if (printsThisFrame < kMaxPrints)
				{
					fprintf(stderr,
						"[VERTEX_PROJECT_PARITY v1] event=mismatch frame=%u vert=%ld "
						"clipInfo=(fast=%d/legacy=%d) "
						"px=(%a/%a) py=(%a/%a) pz=(%a/%a) pw=(%a/%a) haze=(%a/%a)\n",
						s_vpFrame, vi,
						(int)s.clipInfo, (int)cv->clipInfo,
						s.px, cv->px,
						s.py, cv->py,
						s.pz, cv->pz,
						s.pw, cv->pw,
						s.hazeFactor, cv->hazeFactor);
					fflush(stderr);
					++printsThisFrame;
				}
			}
		}
		if ((s_vpFrame % 600) == 0)
		{
			fprintf(stderr,
				"[VERTEX_PROJECT_PARITY v1] event=summary frames=%u "
				"verts_checked=%llu total_mismatches=%llu\n",
				s_vpFrame,
				(unsigned long long)s_vpVertsChecked,
				(unsigned long long)s_vpVertsMismatch);
			fflush(stderr);
		}
	}

	//-----------------------------------
	// setup terrain quad textures
	// Also sets up mine data.
	TerrainQuadPtr currentQuad = quadList;
	{
		ZoneScopedN("Terrain::geometry quadSetupTextures");
		// Stage 3: preflight arming — walks live quadList BEFORE the loop so
		// IsFrameSolidArmed() is stable for all setupTextures() calls.
		// On un-armed frames (recipe not ready, disabled, etc.) this returns
		// false with zero side-effects; setupTextures runs as normal.
		gos_terrain_indirect::ComputePreflight();
		for (i=0;i<numberQuads;i++)
		{
			currentQuad->setupTextures();
			currentQuad++;
		}
		// Stage 1 cost-split: roll per-frame nanosecond accumulators (no-op
		// when MC2_TERRAIN_COST_SPLIT unset). ParityFrameTick advances the
		// summary cadence; Stage 2 passes the actual quads-checked count.
		gos_terrain_indirect::CostSplit_RollFrame();
		{
			int quadsChecked = 0;
			if (gos_terrain_indirect::IsParityCheckEnabled())
				quadsChecked = gos_terrain_indirect::ParityCompareRecipeFrame();
			gos_terrain_indirect::ParityFrameTick(quadsChecked);
		}
	}

	float ywRange = 0.0f, yzRange = 0.0f;
	if (fabs(mostWY - leastWY) > Stuff::SMALL)
	{
		ywRange = (mostW - leastW) / (mostWY - leastWY);
		yzRange = (mostZ - leastZ) / (mostWY - leastWY);
	}

	eye->setInverseProject(mostZ,leastW,yzRange,ywRange);

	//-----------------------------------
	// update the cloud layer
	if (Terrain::cloudLayer)
	{
		ZoneScopedN("Terrain::geometry cloudUpdate");
		Terrain::cloudLayer->update();
	}
}

//---------------------------------------------------------------------------
float Terrain::getTerrainElevation (const Stuff::Vector3D &position)
{
	float result = mapData->terrainElevation(position);
	return(result);
}

//---------------------------------------------------------------------------
float Terrain::getTerrainElevation( long tileR, long tileC )
{
	return mapData->terrainElevation( tileR, tileC );
}

//---------------------------------------------------------------------------
unsigned long Terrain::getTexture( long tileR, long tileC )
{
	return mapData->getTexture( tileR, tileC );
}

//---------------------------------------------------------------------------
float Terrain::getTerrainAngle (const Stuff::Vector3D &position, Stuff::Vector3D* normal)
{
	float result = mapData->terrainAngle(position, normal);
	return(result);
}

//---------------------------------------------------------------------------
float Terrain::getTerrainLight (const Stuff::Vector3D &position)
{
	float result = mapData->terrainLight(position);
	return(result);
}

//---------------------------------------------------------------------------
Stuff::Vector3D Terrain::getTerrainNormal (const Stuff::Vector3D &position)
{
	Stuff::Vector3D result = Terrain::mapData->terrainNormal(position);
	return(result);
}

//---------------------------------------------------------------------------
// Uses a simple value to mark radius.  It never changes now!!
// First value in range table!!
void Terrain::markSeen (const Stuff::Vector3D &looker, byte who, float specialUnitExpand)
{
	return;

	/*		Not needed anymore.  Real LOS now.
	//-----------------------------------------------------------
	// This function marks vertices has being seen by a given side.
	Stuff::Vector3D position = looker;
	position.x -= mapTopLeft3d.x;
	position.y = mapTopLeft3d.y - looker.y;
	
	Stuff::Vector2DOf<float> upperLeft;
	upperLeft.x = floor(position.x * oneOverWorldUnitsPerVertex);
	upperLeft.y = floor(position.y * oneOverWorldUnitsPerVertex);

	Stuff::Vector2DOf<long> meshOffset;
	meshOffset.x = float2long(upperLeft.x);
	meshOffset.y = float2long(upperLeft.y);

	unsigned long xCenter = meshOffset.x;
	unsigned long yCenter = meshOffset.y;

	//Figure out altitude above minimum terrain altitude and look up in table.
	float baseElevation = MapData::waterDepth;
	if (MapData::waterDepth < Terrain::userMin)
		baseElevation = Terrain::userMin;

	float altitude = position.z - baseElevation;
	float altitudeIntegerRange = (Terrain::userMax - baseElevation) * 0.00390625f;
	long altLevel = 0;
	if (altitudeIntegerRange > Stuff::SMALL)
		altLevel = altitude / altitudeIntegerRange;
	
	if (altLevel < 0)
		altLevel = 0;

	if (altLevel > 255)
		altLevel = 255;

	float radius = visualRangeTable[altLevel];
	
	radius += (radius * specialUnitExpand);

	if (radius <= 0.0f)
		return;

	//-----------------------------------------------------
	// Who is the shift value to create the mask
	BYTE wer = (1 << who);

	VisibleBits->setCircle(xCenter,yCenter,float2long(radius),wer);
	*/
}

//---------------------------------------------------------------------------
// Uses dist passed in as radius.
void Terrain::markRadiusSeen (const Stuff::Vector3D &looker, float dist, byte who)
{
	return;

	//Not needed.  Real LOS now!
	/*
	if (dist <= 0.0f)
		return;

	//-----------------------------------------------------------
	// This function marks vertices has being seen by
	// a given side.
	dist *= worldUnitsPerMeter;
	dist *= Terrain::oneOverWorldUnitsPerVertex;
	
	Stuff::Vector3D position = looker;
	position.x -= mapTopLeft3d.x;
	position.y = mapTopLeft3d.y - looker.y;
	
	Stuff::Vector2DOf<float> upperLeft;
	upperLeft.x = floor(position.x * oneOverWorldUnitsPerVertex);
	upperLeft.y = floor(position.y * oneOverWorldUnitsPerVertex);

	Stuff::Vector2DOf<long> meshOffset;
	meshOffset.x = floor(upperLeft.x);
	meshOffset.y = floor(upperLeft.y);

	unsigned long xCenter = meshOffset.x;
	unsigned long yCenter = meshOffset.y;

	//-----------------------------------------------------
	// Who is the shift value to create the mask
	BYTE wer = (1 << who);

	VisibleBits->setCircle(xCenter,yCenter,dist,wer);
	*/
}

//---------------------------------------------------------------------------
void Terrain::setObjBlockActive (long blockNum, bool active)
{
	if ((blockNum >= 0) && (blockNum < numObjBlocks))
		objBlockInfo[blockNum].active = active;	
}	

//---------------------------------------------------------------------------
void Terrain::clearObjBlocksActive (void)
{
	for (long i = 0; i < numObjBlocks; i++)
		setObjBlockActive(i, false);
}	

//---------------------------------------------------------------------------
void Terrain::setObjVertexActive (long vertexNum, bool active)
{
	if ( (vertexNum >= 0) && (vertexNum < (realVerticesMapSide * realVerticesMapSide)) )
		objVertexActive[vertexNum] = active;	
}	

//---------------------------------------------------------------------------
void Terrain::clearObjVerticesActive (void)
{
	memset(objVertexActive,0,sizeof(bool) * realVerticesMapSide * realVerticesMapSide);
}

//---------------------------------------------------------------------------
long Terrain::save( PacketFile* fileName, int whichPacket, bool quickSave )
{ 
	if (!quickSave)
	{
		recalcShadows = true;
		mapData->calcLight();
	}
	else
	{
		recalcShadows = false;
	}
		
	return mapData->save( fileName, whichPacket ); 
}


//-----------------------------------------------------
bool Terrain::save( FitIniFile* fitFile )
{
	// write out the water info
#ifdef _DEBUG
	long result = 
#endif
	fitFile->writeBlock( "Water" );
	gosASSERT( result > 0 );


	fitFile->writeIdFloat( "Elevation", mapData->waterDepth );
	fitFile->writeIdFloat( "Frequency", waterFreq );
	fitFile->writeIdFloat( "Ampliture", waterAmplitude );
	fitFile->writeIdULong( "AlphaShallow", alphaEdge );
	fitFile->writeIdULong( "AlphaMiddle", alphaMiddle );
	fitFile->writeIdULong( "AlphaDeep", alphaDeep );
	fitFile->writeIdFloat( "AlphaDepth", mapData->alphaDepth );
	fitFile->writeIdFloat( "ShallowDepth", mapData->shallowDepth );

	fitFile->writeBlock( "Terrain" );
	fitFile->writeIdLong( "UserMin", userMin );
	fitFile->writeIdLong( "UserMax", userMax );
	fitFile->writeIdFloat( "TerrainMinX", tileColToWorldCoord[0] );
	fitFile->writeIdFloat( "TerrainMinY", tileRowToWorldCoord[0] );
	fitFile->writeIdUChar( "Noise", fractalNoise);
	fitFile->writeIdUChar( "Threshold", fractalThreshold);

	if (terrainTextures2)
	{
		terrainTextures2->saveTilingFactors(fitFile);
	}
	return true;
}

bool Terrain::load( FitIniFile* fitFile )
{
	// write out the water info
	long result = fitFile->seekBlock( "Water" );
	gosASSERT( result == NO_ERR );

	result = fitFile->readIdFloat( "Elevation", mapData->waterDepth );
	gosASSERT( result == NO_ERR );
	waterElevation = mapData->waterDepth;
	result = fitFile->readIdFloat( "Frequency", waterFreq );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdFloat( "Ampliture", waterAmplitude );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdULong( "AlphaShallow", alphaEdge );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdULong( "AlphaMiddle", alphaMiddle );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdULong( "AlphaDeep", alphaDeep );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdFloat( "AlphaDepth", mapData->alphaDepth );
	gosASSERT( result == NO_ERR );
	result = fitFile->readIdFloat( "ShallowDepth", mapData->shallowDepth );
	gosASSERT( result == NO_ERR );

	fitFile->seekBlock( "Terrain" );
	fitFile->readIdLong( "UserMin", userMin );
	fitFile->readIdLong( "UserMax", userMax );

	fitFile->readIdUChar( "Noise", fractalNoise);
	fitFile->readIdUChar( "Threshold", fractalThreshold);

	return true;

}

//---------------------------------------------------------------------------
void Terrain::unselectAll()
{
	mapData->unselectAll();
}

//---------------------------------------------------------------------------
void Terrain::selectVerticesInRect( const Stuff::Vector4D& topLeft, const Stuff::Vector4D& bottomRight, bool bToggle )
{
	Stuff::Vector3D worldPos;
	Stuff::Vector4D screenPos;

	int xMin, xMax;
	int yMin, yMax;

	if ( topLeft.x < bottomRight.x )
	{
		xMin = topLeft.x;
		xMax = bottomRight.x;
	}
	else
	{
		xMin = bottomRight.x;
		xMax = topLeft.x;
	}

	if ( topLeft.y < bottomRight.y )
	{
		yMin = topLeft.y;
		yMax = bottomRight.y;
	}
	else
	{
		yMin = bottomRight.y;
		yMax = topLeft.y;
	}
	
	for ( int i = 0; i < realVerticesMapSide; ++i )
	{
		for ( int j = 0; j < realVerticesMapSide; ++j )
		{
			worldPos.y = tileRowToWorldCoord[j];
			worldPos.x = tileColToWorldCoord[i];
			worldPos.z = mapData->terrainElevation( j, i );

			// [PROJECTZ:SelectionPicking id=picking_terrain_rect_select]
			PROJECTZ_SITE("picking_terrain_rect_select", "SelectionPicking");
			eye->projectForSelectionPicking( worldPos, screenPos );

			if ( screenPos.x >= xMin && screenPos.x <= xMax &&
				 screenPos.y >= yMin && screenPos.y <= yMax )
			{
				mapData->selectVertex( j, i, true, bToggle );		
			}
		}
	}
}

//---------------------------------------------------------------------------
bool Terrain::hasSelection()
{
	return mapData->selection();
}

//---------------------------------------------------------------------------
bool Terrain::isVertexSelected( long tileR, long tileC )
{
	return mapData->isVertexSelected( tileR, tileC );
}

//---------------------------------------------------------------------------
bool Terrain::selectVertex( long tileR, long tileC, bool bSelect )
{
	//We never use the return value so just send back false.
	if ( (tileR <= -1) || (tileR >= realVerticesMapSide) )
		return false;

	if ( (tileC <= -1) || (tileC >= realVerticesMapSide) )
		return false;

	mapData->selectVertex( tileR, tileC, bSelect, 0 );
	return true;
}

//---------------------------------------------------------------------------
float Terrain::getHighestVertex( long& tileR, long& tileC )
{
	float highest = -9999999.; // an absurdly small number
	for ( int i = 0; i < realVerticesMapSide * realVerticesMapSide; ++i )
	{
		float tmp = getVertexHeight( i );
		if ( tmp > highest )
		{
			highest = tmp;
			tileR = i/realVerticesMapSide;
			tileC = i % realVerticesMapSide;
		}
	}

	return highest;
}

//---------------------------------------------------------------------------
float Terrain::getLowestVertex(  long& tileR, long& tileC )
{
	float lowest = 9999999.; // an absurdly big number
	for ( int i = 0; i < realVerticesMapSide * realVerticesMapSide; ++i )
	{
		float tmp = getVertexHeight( i );
		if ( tmp < lowest )
		{
			lowest = tmp;
			tileR = i/realVerticesMapSide;
			tileC = i % realVerticesMapSide;
		}
	}

	return lowest;
}

//---------------------------------------------------------------------------
void  Terrain::setUserSettings( long min, long max, int terrainType )
{
	userMin = min;
	userMax = max;
	baseTerrain = terrainType;
}

//---------------------------------------------------------------------------
void Terrain::getUserSettings( long& min, long& max, int& terrainType )
{
	min = userMin;
	max = userMax;
	terrainType = baseTerrain;
}

//---------------------------------------------------------------------------
void Terrain::recalcWater()
{
	mapData->recalcWater();
}

//---------------------------------------------------------------------------
void Terrain::reCalcLight(bool doShadows)
{
	recalcLight = true;
	recalcShadows = doShadows;
	
	//Do a new burnin for the colormap
	if (terrainTextures2)
	{
		if (colorMapName)
			terrainTextures2->recalcLight(colorMapName);
		else
			terrainTextures2->recalcLight(terrainName);
	}
}

//---------------------------------------------------------------------------
void Terrain::clearShadows()
{
	mapData->clearShadows();
}

//---------------------------------------------------------------------------

long Terrain::getWater (const Stuff::Vector3D& worldPos) {
	//-------------------------------------------------
	// Get elevation at this point and compare to deep
	// water altitude for this map.
	float elevation = getTerrainElevation(worldPos);
	
	if (elevation < (waterElevation - MapData::shallowDepth))
		return(2);
	if (elevation < waterElevation)
		return(1);
	return(0);
}

//---------------------------------------------------------------------------
