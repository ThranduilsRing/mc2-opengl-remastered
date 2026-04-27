//---------------------------------------------------------------------
//
//
// This class will manage the texture memory provided by GOS
// GOS gives me a maximum of 256 256x256 pixel texture pages.
// I want GOS to think I only use 256x256 textures.  This class
// will insure that GOS believes that completely and provided
// smaller texture surfaces out of the main surface if necessary
// as well as returning the necessary UVs to get to the other surface.
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef TXMMGR_H
#include"txmmgr.h"
#include "gos_crashbundle.h"
#endif
#include"tex_resolve_table.h"

#ifndef TGAINFO_H
#include"tgainfo.h"
#endif

#ifndef FILE_H
#include"file.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef LZ_H
#include"lz.h"
#endif

#ifndef CIDENT_H
#include"cident.h"
#endif

#include"../GameOS/gameos/gos_profiler.h"

#ifndef PATHS_H
#include"paths.h"
#endif

#include<gameos.hpp>
#include<mlr/mlr.hpp>
#include<gosfx/gosfxheaders.hpp>
#include <cstdlib>
#include <cstring>
#include <utils/gl_utils.h>
#include "gos_postprocess.h"
#include "gos_profiler.h"
#include "../GameOS/gameos/gos_static_prop_batcher.h"
#include "../GameOS/gameos/gos_validate.h"  // drainGLErrors (Tier-1 instr §4)

//---------------------------------------------------------------------------
// static globals
MC_TextureManager *mcTextureManager = NULL;
gos_VERTEXManager *MC_TextureManager::gvManager = NULL;
gos_RenderShapeManager<TG_RenderShape> *MC_TextureManager::rsManager = NULL;
MemoryPtr			MC_TextureManager::lzBuffer1 = NULL;
MemoryPtr			MC_TextureManager::lzBuffer2 = NULL;
int				MC_TextureManager::iBufferRefCount = 0;

bool MLRVertexLimitReached = false;
extern bool useFog;
extern DWORD BaseVertexColor;

// --- Shadow shape collection (file-scope global, no struct layout impact) ---
struct ShadowShapeEntry {
	HGOSBUFFER vb;
	HGOSBUFFER ib;
	HGOSVERTEXDECLARATION vdecl;
	float worldMatrix[16];
};
static const int MAX_SHADOW_SHAPES = 512;
static ShadowShapeEntry g_shadowShapes[MAX_SHADOW_SHAPES];
static int g_numShadowShapes = 0;

static bool isAllConcreteTerrainBatch(const gos_VERTEX* vertices, DWORD totalVertices)
{
	if (!vertices || totalVertices == 0)
		return false;

	for (DWORD vi = 0; vi < totalVertices; ++vi)
	{
		if ((vertices[vi].frgb & 0x000000ff) != 3)
			return false;
	}

	return true;
}

void addShadowShape(HGOSBUFFER vb, HGOSBUFFER ib, HGOSVERTEXDECLARATION vdecl, const float* worldEntries16) {
	if (g_numShadowShapes >= MAX_SHADOW_SHAPES) return;
	ShadowShapeEntry& ss = g_shadowShapes[g_numShadowShapes++];
	ss.vb = vb;
	ss.ib = ib;
	ss.vdecl = vdecl;
	memcpy(ss.worldMatrix, worldEntries16, 16 * sizeof(float));
}

void clearShadowShapes() {
	g_numShadowShapes = 0;
}

DWORD actualTextureSize = 0;
DWORD compressedTextureSize = 0;
static int64_t gTxmRealizedTotal = 0;

static const DWORD MC_TEXCACHE_FILE_LZ = 0xF0000000;
static const DWORD MC_TEXCACHE_FILE_RAW = 0xE0000000;
static const DWORD MC_TEXCACHE_MEM_RAW = 0xD0000000;
static const DWORD MC_TEXCACHE_SIZE_MASK = 0x0FFFFFFF;
static const long MC_TEXCACHE_RAW_THRESHOLD = 256 * 1024;

static bool tryReadTgaLogicalSize(File& textureFile, DWORD uvScale, DWORD& logicalWidth, DWORD& logicalHeight)
{
	logicalWidth = 0;
	logicalHeight = 0;

	if (textureFile.fileSize() < sizeof(TGAFileHeader))
		return false;

	TGAFileHeader header;
	if (textureFile.read((MemoryPtr)&header, sizeof(header)) != sizeof(header))
	{
		textureFile.seek(0);
		return false;
	}

	textureFile.seek(0);

	if (header.width <= 0 || header.height <= 0)
		return false;

	const DWORD scale = uvScale ? uvScale : 1;
	logicalWidth = static_cast<DWORD>(header.width) / scale;
	logicalHeight = static_cast<DWORD>(header.height) / scale;
	return logicalWidth && logicalHeight;
}

#define MAX_SENDDOWN		10002

//------------------------------------------------------
// Frees up gos_VERTEX manager memory
void MC_TextureManager::freeVertices(void)
{
	if (gvManager)
	{
		gvManager->destroy();
		delete gvManager;
		gvManager = NULL;
	}
}

void MC_TextureManager::freeShapes(void)
{
	if (rsManager)
	{
		rsManager->destroy();
		delete rsManager;
		rsManager = NULL;
	}
}
		
//------------------------------------------------------
// Creates gos_VERTEX Manager and allocates RAM.  Will not allocate if already done!
void MC_TextureManager::startVertices (long maxVertices)
{
	if (gvManager == NULL)
	{
		gvManager = new gos_VERTEXManager;
		gvManager->init(maxVertices);
		gvManager->reset();
	}
}

void MC_TextureManager::startShapes(uint32_t maxShapes)
{
	if (rsManager == NULL)
	{
		rsManager = new gos_RenderShapeManager<TG_RenderShape>;
		rsManager->init(maxShapes);
		rsManager->reset();
	}
}
	 
//----------------------------------------------------------------------
// Class MC_TextureManager
void MC_TextureManager::start (void)
{
	ZoneScopedN("MC_TextureManager::start");
	init();

	//------------------------------------------
	// Create nodes from systemHeap.
	long nodeRAM = MC_MAXTEXTURES * sizeof(MC_TextureNode);
	masterTextureNodes = (MC_TextureNode *)systemHeap->Malloc(nodeRAM);
	gosASSERT(masterTextureNodes != NULL);

	for (long i=0;i<MC_MAXTEXTURES;i++)
		masterTextureNodes[i].init();
		
	//-------------------------------------------
	// Create VertexNodes from systemHeap
	nodeRAM = MC_MAXTEXTURES * sizeof(MC_VertexArrayNode);
	masterVertexNodes = (MC_VertexArrayNode *)systemHeap->Malloc(nodeRAM);
	gosASSERT(masterVertexNodes != NULL);
	
	memset(masterVertexNodes,0,nodeRAM);

	nodeRAM = MC_MAXTEXTURES * sizeof(MC_HardwareVertexArrayNode);
	masterHardwareVertexNodes = (MC_HardwareVertexArrayNode *)systemHeap->Malloc(nodeRAM);
	gosASSERT(masterHardwareVertexNodes != NULL);
	
	memset(masterHardwareVertexNodes,0,nodeRAM);

	textureCacheHeap = new UserHeap;
	textureCacheHeap->init(TEXTURE_CACHE_SIZE,"TXMCache");
	textureCacheHeap->setMallocFatals(false);
	
	textureStringHeap = new UserHeap;
	textureStringHeap->init(512000,"TXMString");

	if (!textureManagerInstrumented)
	{
		StatisticFormat( "" );
		StatisticFormat( "MechCommander 2 Texture Manager" );
		StatisticFormat( "===============================" );
		StatisticFormat( "" );

		AddStatistic("Handles Used","Handles",gos_DWORD, &(currentUsedTextures), Stat_Total);

		AddStatistic("Cache Misses","",gos_DWORD, &(totalCacheMisses), Stat_Total);

		StatisticFormat( "" );
		StatisticFormat( "" );

		textureManagerInstrumented = true;
	}
	
	indexArray = (WORD *)systemHeap->Malloc(sizeof(WORD) * MC_MAXFACES);
	for (int i=0;i<MC_MAXFACES;i++)
		indexArray[i] = i;
		
	//Add an Empty Texture node for all untextured triangles to go down into.
	masterTextureNodes[0].gosTextureHandle = 0;
	masterTextureNodes[0].nodeName = NULL;
	masterTextureNodes[0].uniqueInstance = false;
	masterTextureNodes[0].neverFLUSH = 0x1;
	masterTextureNodes[0].numUsers = 0;
	masterTextureNodes[0].key = gos_Texture_Solid;
	masterTextureNodes[0].hints = 0; 
	masterTextureNodes[0].width = 0;
	masterTextureNodes[0].logicalWidth = 0;
	masterTextureNodes[0].logicalHeight = 0;
	masterTextureNodes[0].lastUsed = -1;
	masterTextureNodes[0].textureData = NULL;

    lightDataStructuresCapacity = 128;
    lightDataStructuresCount = 0;
    lightData_ = new TG_HWLightsData[lightDataStructuresCapacity];
	lightDataBuffer_ = gos_CreateBuffer(gosBUFFER_TYPE::UNIFORM, gosBUFFER_USAGE::STATIC_DRAW, sizeof(TG_HWLightsData) * lightDataStructuresCapacity, 1, NULL);
	gos_BindBufferBase(lightDataBuffer_, LIGHT_DATA_ATTACHMENT_SLOT);

    sceneData_ = new TG_HWSceneData;
	sceneDataBuffer_ = gos_CreateBuffer(gosBUFFER_TYPE::UNIFORM, gosBUFFER_USAGE::STATIC_DRAW, sizeof(TG_HWSceneData), 1, NULL);
	gos_BindBufferBase(sceneDataBuffer_, SCENE_DATA_ATTACHMENT_SLOT);

	initTexResolveTable();
}

extern Stuff::MemoryStream *effectStream;
extern MidLevelRenderer::MLRClipper * theClipper;
//----------------------------------------------------------------------
void MC_TextureManager::destroy (void)
{
	if (masterTextureNodes)
	{
		//-----------------------------------------------------
		// Traverses list of texture nodes and frees each one.
		long usedCount = 0;
		for (long i=0;i<MC_MAXTEXTURES;i++)
			masterTextureNodes[i].destroy();		// Destroy for nodes whacks GOS Handle
		
		currentUsedTextures = usedCount;			//Can this have been the damned bug all along!?
	}
	
	gos_PushCurrentHeap(MidLevelRenderer::Heap);

	delete MidLevelRenderer::MLRTexturePool::Instance;
	MidLevelRenderer::MLRTexturePool::Instance = NULL; 

	delete theClipper;
	theClipper = NULL;
	
	gos_PopCurrentHeap();

	//------------------------------------------------------
	// Shutdown the GOS FX and MLR.
	gos_PushCurrentHeap(gosFX::Heap);
	
	delete gosFX::EffectLibrary::Instance;
	gosFX::EffectLibrary::Instance = NULL;

	delete effectStream;
	effectStream = NULL;
	
	delete gosFX::LightManager::Instance;
	gosFX::LightManager::Instance = NULL;

	gos_PopCurrentHeap();

	//------------------------------------------
	// free SystemHeap Memory
	systemHeap->Free(masterTextureNodes);
	masterTextureNodes = NULL;
	
	systemHeap->Free(masterVertexNodes);
	masterVertexNodes = NULL;
	
	delete textureCacheHeap;
	textureCacheHeap = NULL;

	delete textureStringHeap;
	textureStringHeap = NULL;

	if(lightDataBuffer_)
		gos_DestroyBuffer(lightDataBuffer_);
	lightDataBuffer_ = nullptr;

    delete[] lightData_;
    lightData_ = nullptr;

	if(sceneDataBuffer_)
		gos_DestroyBuffer(sceneDataBuffer_);
	sceneDataBuffer_ = nullptr;

    delete sceneData_;
    sceneData_ = nullptr;
}

//----------------------------------------------------------------------
MC_TextureManager::~MC_TextureManager (void)
{
	MC_TextureManager::iBufferRefCount--;
	if (0 == MC_TextureManager::iBufferRefCount)
	{
		if (lzBuffer1)
		{
			gosASSERT(lzBuffer2 != NULL);
			if (textureCacheHeap)
			{
				textureCacheHeap->Free(lzBuffer1);
				textureCacheHeap->Free(lzBuffer2);
			}
			lzBuffer1 = NULL;
			lzBuffer2 = NULL;
		}
	}

	destroy();
}

//----------------------------------------------------------------------
void MC_TextureManager::flush (bool justTextures)
{
	if (masterTextureNodes)
	{
		{
			char _cbbuf[128];
			snprintf(_cbbuf, sizeof(_cbbuf),
				"[TXM v1] event=mission_unload peak_textures=%ld/%d",
				peakUsedTextures, MAX_MC2_GOS_TEXTURES);
			puts(_cbbuf);
			crashbundle_append(_cbbuf);
			fflush(stdout);
		}
		peakUsedTextures = 0;

		//-----------------------------------------------------
		// Traverses list of texture nodes and frees each one.
		long usedCount = 0;
		for (long i=0;i<MC_MAXTEXTURES;i++)
		{
			if (!masterTextureNodes[i].neverFLUSH)
				masterTextureNodes[i].destroy();		// Destroy for nodes whacks GOS Handle
		}
		
		currentUsedTextures = usedCount;				//Can this have been the damned bug all along!?
	}
	
	//If we just wanted to free up RAM, just return and let the MUNGA stuff go later.
	if (justTextures)
		return;

	gos_PushCurrentHeap(MidLevelRenderer::Heap);

	delete MidLevelRenderer::MLRTexturePool::Instance;
	MidLevelRenderer::MLRTexturePool::Instance = NULL; 

	delete theClipper;
	theClipper = NULL;
	
	gos_PopCurrentHeap();

	//------------------------------------------------------
	// Shutdown the GOS FX and MLR.
	gos_PushCurrentHeap(gosFX::Heap);
	
	delete gosFX::EffectLibrary::Instance;
	gosFX::EffectLibrary::Instance = NULL;

	delete effectStream;
	effectStream = NULL;
	
	delete gosFX::LightManager::Instance;
	gosFX::LightManager::Instance = NULL;

	gos_PopCurrentHeap();

	//------------------------------------------------------
	//Restart MLR and the GOSFx
	gos_PushCurrentHeap(MidLevelRenderer::Heap);

	MidLevelRenderer::TGAFilePool *pool = new MidLevelRenderer::TGAFilePool("data" PATH_SEPARATOR "tgl" PATH_SEPARATOR "128" PATH_SEPARATOR);
	MidLevelRenderer::MLRTexturePool::Instance = new MidLevelRenderer::MLRTexturePool(pool);

	MidLevelRenderer::MLRSortByOrder *cameraSorter = new MidLevelRenderer::MLRSortByOrder(MidLevelRenderer::MLRTexturePool::Instance);
	theClipper = new MidLevelRenderer::MLRClipper(0, cameraSorter);
	
	gos_PopCurrentHeap();

	//------------------------------------------------------
	// ReStart the GOS FX.
	gos_PushCurrentHeap(gosFX::Heap);
	
	gosFX::EffectLibrary::Instance = new gosFX::EffectLibrary();
	Check_Object(gosFX::EffectLibrary::Instance);

	FullPathFileName effectsName;
	effectsName.init(effectsPath,"mc2.fx","");

	File effectFile;
	long result = effectFile.open(effectsName);
	if (result != NO_ERR)
		STOP(("Could not find MC2.fx"));
		
	long effectsSize = effectFile.fileSize();
	MemoryPtr effectsData = (MemoryPtr)systemHeap->Malloc(effectsSize);
	effectFile.read(effectsData,effectsSize);
	effectFile.close();
	
	effectStream = new Stuff::MemoryStream(effectsData,effectsSize);
	gosFX::EffectLibrary::Instance->Load(effectStream);
	
	gosFX::LightManager::Instance = new gosFX::LightManager();

	gos_PopCurrentHeap();

	systemHeap->Free(effectsData);
}

//----------------------------------------------------------------------
void MC_TextureManager::removeTextureNode (DWORD textureNode)
{
	if (textureNode != 0xffffffff)
	{
		//-----------------------------------------------------------
		masterTextureNodes[textureNode].destroy();
		if (masterTextureNodes[textureNode].textureData)
		{
			textureCacheHeap->Free(masterTextureNodes[textureNode].textureData);
			masterTextureNodes[textureNode].textureData = NULL;

			if (masterTextureNodes[textureNode].nodeName)
			{
				textureStringHeap->Free(masterTextureNodes[textureNode].nodeName);
				masterTextureNodes[textureNode].nodeName = NULL;
			}
		}
	}
}

//----------------------------------------------------------------------
void MC_TextureManager::removeTexture (DWORD gosHandle)
{
	//-----------------------------------------------------------
    long i = 0;
	for (;i<MC_MAXTEXTURES;i++)
	{
		if (masterTextureNodes[i].gosTextureHandle == gosHandle)
		{
			masterTextureNodes[i].numUsers--;
			break;			
		}
	}
	
	if (i < MC_MAXTEXTURES && masterTextureNodes[i].numUsers == 0)
	{
		masterTextureNodes[i].destroy();
		if (masterTextureNodes[i].textureData)
		{
			textureCacheHeap->Free(masterTextureNodes[i].textureData);
			masterTextureNodes[i].textureData = NULL;

			if (masterTextureNodes[i].nodeName)
			{
				textureStringHeap->Free(masterTextureNodes[i].nodeName);
				masterTextureNodes[i].nodeName = NULL;
			}
		}
	}
}

#define cache_Threshold		150
//----------------------------------------------------------------------
bool MC_TextureManager::flushCache (void)
{
	bool cacheNotFull = false;
	totalCacheMisses++;
	currentUsedTextures = 0;
	int poolPinned = 0;
	int poolUnique = 0;
	int poolFlushableIdle = 0;

	//Count ACTUAL number of textures being used.
	// ALSO can't count on turn being right.  Logistics does not update unless simple Camera is up!!
	for (long i=0;i<MC_MAXTEXTURES;i++)
	{
		if ((masterTextureNodes[i].gosTextureHandle != CACHED_OUT_HANDLE) &&
			(masterTextureNodes[i].gosTextureHandle != 0xffffffff))
		{
			currentUsedTextures++;
			if (currentUsedTextures > peakUsedTextures) peakUsedTextures = currentUsedTextures;
			const bool pinned = (masterTextureNodes[i].neverFLUSH & 1) != 0;
			const bool unique = masterTextureNodes[i].uniqueInstance != 0;
			if (pinned) poolPinned++;
			if (unique) poolUnique++;
			if (!pinned && !unique && masterTextureNodes[i].lastUsed <= (turn - cache_Threshold))
				poolFlushableIdle++;
		}
	}

	TracyPlot("Txm pool used", int64_t(currentUsedTextures));
	TracyPlot("Txm pool pinned", int64_t(poolPinned));
	TracyPlot("Txm pool unique", int64_t(poolUnique));
	TracyPlot("Txm pool flushable idle", int64_t(poolFlushableIdle));

	//If we are now below the magic number, return that the cache is NOT full.
	if (currentUsedTextures < MAX_MC2_GOS_TEXTURES)
		return true;

	for (int i=0;i<MC_MAXTEXTURES;i++)
	{
		if ((masterTextureNodes[i].gosTextureHandle != CACHED_OUT_HANDLE) &&
			(masterTextureNodes[i].gosTextureHandle != 0xffffffff) &&
			(!masterTextureNodes[i].uniqueInstance))
		{
			if (masterTextureNodes[i].lastUsed <= (turn-cache_Threshold))
			{
				//----------------------------------------------------------------
				// Cache this badboy out.  Textures don't change.  Just Destroy!
				if (masterTextureNodes[i].gosTextureHandle)
					gos_DestroyTexture(masterTextureNodes[i].gosTextureHandle);

				masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;
				
				currentUsedTextures--;
				cacheNotFull = true;
				return cacheNotFull;
			}
		}
	}
	
	for (int i=0;i<MC_MAXTEXTURES;i++)
	{
		if ((masterTextureNodes[i].gosTextureHandle != CACHED_OUT_HANDLE) &&
			(masterTextureNodes[i].gosTextureHandle != 0xffffffff) &&
			(masterTextureNodes[i].gosTextureHandle) &&
			(!masterTextureNodes[i].uniqueInstance))
		{
			if (masterTextureNodes[i].lastUsed <= (turn-30))
			{
				//----------------------------------------------------------------
				// Cache this badboy out.  Textures don't change.  Just Destroy!
				if (masterTextureNodes[i].gosTextureHandle)
					gos_DestroyTexture(masterTextureNodes[i].gosTextureHandle);

				masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;
				
				currentUsedTextures--;
				cacheNotFull = true;
				return cacheNotFull;
			}
		}
	}
	
	for (int i=0;i<MC_MAXTEXTURES;i++)
	{
		if ((masterTextureNodes[i].gosTextureHandle != CACHED_OUT_HANDLE) &&
			(masterTextureNodes[i].gosTextureHandle != 0xffffffff) &&
			(!masterTextureNodes[i].uniqueInstance))
		{
			if (masterTextureNodes[i].lastUsed <= (turn-1))
			{
				//----------------------------------------------------------------
				// Cache this badboy out.  Textures don't change.  Just Destroy!
				if (masterTextureNodes[i].gosTextureHandle)
					gos_DestroyTexture(masterTextureNodes[i].gosTextureHandle);

				masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;
				
				currentUsedTextures--;
				cacheNotFull = true;
				return cacheNotFull;
			}
		}
	}
	
  	//gosASSERT(cacheNotFull);
	return cacheNotFull;
}

void MC_TextureManager::addRenderShape(DWORD nodeId, TG_RenderShape* render_shape, DWORD flags)
{
	//This function adds the actual vertex data to the texture Node.
	if (nodeId < MC_MAXTEXTURES)
	{
		if (masterTextureNodes[nodeId].hardwareVertexData &&
			masterTextureNodes[nodeId].hardwareVertexData->flags == flags)
		{
			TG_RenderShape* shapes = masterTextureNodes[nodeId].hardwareVertexData->currentShape;
			if (!shapes && !masterTextureNodes[nodeId].hardwareVertexData->shapes)
			{
				masterTextureNodes[nodeId].hardwareVertexData->currentShape =
					shapes =
					masterTextureNodes[nodeId].hardwareVertexData->shapes =
					rsManager->getBlock(masterTextureNodes[nodeId].hardwareVertexData->numShapes);
			}

			if (shapes < (masterTextureNodes[nodeId].hardwareVertexData->shapes + masterTextureNodes[nodeId].hardwareVertexData->numShapes))
			{
				*shapes = *render_shape;
				shapes++;
			}

			masterTextureNodes[nodeId].hardwareVertexData->currentShape = shapes;
		}
		else if (masterTextureNodes[nodeId].hardwareVertexData2 &&
			masterTextureNodes[nodeId].hardwareVertexData2->flags == flags)
		{
			TG_RenderShape* shapes = masterTextureNodes[nodeId].hardwareVertexData2->currentShape;

			//sebi: looks like assert may happen if more vertices added than was calculated on stage when addTriange was called. As one can see in (*) first time we go here, we allocate enough memory for all potential vertices, but if it is not enough this assert will trigger
#if defined( _DEBUG) || defined(_ARMOR)
			TG_RenderShape* oldShapes = shapes;
			TG_RenderShape* oldStart = (masterTextureNodes[nodeId].hardwareVertexData2->shapes + masterTextureNodes[nodeId].hardwareVertexData2->numShapes);
#endif
			gosASSERT(oldShapes < oldStart);

			// (*)
			if (!shapes && !masterTextureNodes[nodeId].hardwareVertexData2->shapes)
			{
				masterTextureNodes[nodeId].hardwareVertexData2->currentShape =
					shapes =
					masterTextureNodes[nodeId].hardwareVertexData2->shapes =
					rsManager->getBlock(masterTextureNodes[nodeId].hardwareVertexData2->numShapes);
			}

			if (shapes < (masterTextureNodes[nodeId].hardwareVertexData2->shapes + masterTextureNodes[nodeId].hardwareVertexData2->numShapes))
			{
				*shapes = *render_shape;
				shapes++;
			}

			masterTextureNodes[nodeId].hardwareVertexData2->currentShape = shapes;
		}
		else if (masterTextureNodes[nodeId].hardwareVertexData3 &&
			masterTextureNodes[nodeId].hardwareVertexData3->flags == flags)
		{
			TG_RenderShape * shapes = masterTextureNodes[nodeId].hardwareVertexData3->currentShape;

#if defined(_DEBUG) || defined(_ARMOR)
			TG_RenderShape * oldShapes = shapes;
			TG_RenderShape * oldStart = (masterTextureNodes[nodeId].hardwareVertexData3->shapes + masterTextureNodes[nodeId].hardwareVertexData3->numShapes);
#endif
			gosASSERT(oldShapes < oldStart);

			if (!shapes && !masterTextureNodes[nodeId].hardwareVertexData3->shapes)
			{
				masterTextureNodes[nodeId].hardwareVertexData3->currentShape =
					shapes =
					masterTextureNodes[nodeId].hardwareVertexData3->shapes =
					rsManager->getBlock(masterTextureNodes[nodeId].hardwareVertexData3->numShapes);
			}

			if (shapes < (masterTextureNodes[nodeId].hardwareVertexData3->shapes + masterTextureNodes[nodeId].hardwareVertexData3->numShapes))
			{
				*shapes = *render_shape;
				shapes++;
			}

			masterTextureNodes[nodeId].hardwareVertexData3->currentShape = shapes;
		}
		else	//If we got here, something is really wrong
		{
#ifdef _DEBUG
			SPEW(("GRAPHICS", "Flags do not match either set of render shapes Data\n"));
#endif
		}
	}
	else
	{
		if (hardwareVertexData && hardwareVertexData->flags == flags)
		{
			TG_RenderShape * shapes = hardwareVertexData->currentShape;
			if (!shapes && !hardwareVertexData->shapes)
			{
				hardwareVertexData->currentShape =
					shapes =
					hardwareVertexData->shapes =
					rsManager->getBlock(hardwareVertexData->numShapes);
			}

			if (shapes <= (hardwareVertexData->shapes + hardwareVertexData->numShapes))
			{
				*shapes = *render_shape;
				shapes ++;
			}

			hardwareVertexData->currentShape = shapes;
		}
		else if (hardwareVertexData2 && hardwareVertexData2->flags == flags)
		{
			TG_RenderShape * shapes = hardwareVertexData2->currentShape;
			if (!shapes && !hardwareVertexData2->shapes)
			{
				hardwareVertexData2->currentShape =
					shapes =
					hardwareVertexData2->shapes =
					rsManager->getBlock(hardwareVertexData2->numShapes);
			}

			if (shapes <= (hardwareVertexData2->shapes + hardwareVertexData2->numShapes))
			{
				*shapes = *render_shape;
				shapes ++;
			}

			hardwareVertexData2->currentShape = shapes;
		}
		else if (hardwareVertexData3 && hardwareVertexData3->flags == flags)
		{
			TG_RenderShape * shapes = hardwareVertexData3->currentShape;
			if (!shapes && !hardwareVertexData3->shapes)
			{
				hardwareVertexData3->currentShape =
					shapes =
					hardwareVertexData3->shapes =
					rsManager->getBlock(hardwareVertexData3->numShapes);
			}

			if (shapes <= (hardwareVertexData3->shapes + hardwareVertexData3->numShapes))
			{
				*shapes = *render_shape;
				shapes ++;
			}

			hardwareVertexData3->currentShape = shapes;
		}
		else if (hardwareVertexData4 && hardwareVertexData4->flags == flags)
		{
			TG_RenderShape * shapes = hardwareVertexData4->currentShape;
			if (!shapes && !hardwareVertexData4->shapes)
			{
				hardwareVertexData4->currentShape =
					shapes =
					hardwareVertexData4->shapes =
					rsManager->getBlock(hardwareVertexData4->numShapes);
			}

			if (shapes <= (hardwareVertexData4->shapes + hardwareVertexData4->numShapes))
			{
				*shapes = *render_shape;
				shapes ++;
			}

			hardwareVertexData4->currentShape = shapes;
		}
		else if (hardwareVertexData5 && hardwareVertexData5->flags == flags)
		{
			TG_RenderShape * shapes = hardwareVertexData5->currentShape;
			if (!shapes && !hardwareVertexData5->shapes)
			{
				hardwareVertexData5->currentShape =
					shapes =
					hardwareVertexData5->shapes =
					rsManager->getBlock(hardwareVertexData5->numShapes);
			}

			if (shapes <= (hardwareVertexData5->shapes + hardwareVertexData5->numShapes))
			{
				*shapes = *render_shape;
				shapes ++;
			}

			hardwareVertexData5->currentShape = shapes;
		}
		else	//If we got here, something is really wrong
		{
#ifdef _DEBUG
			SPEW(("GRAPHICS", "Flags do not match any set of untextured shapes\n"));
#endif
		}
	}
}

uint32_t MC_TextureManager::addLightDataStructure(TG_HWLightsData* light_data)
{
    for(uint32_t i = 0; i < lightDataStructuresCount; ++i)
    {
        if(0 == memcmp(lightData_ + i, light_data, sizeof(TG_HWLightsData)))
        {
            return i;
        }
    }

    // unique data passed, so add it

    if(lightDataStructuresCount + 1 >= lightDataStructuresCapacity)
    {
        TG_HWLightsData* new_lights_data = new TG_HWLightsData[lightDataStructuresCapacity + 128];
        memcpy(new_lights_data, lightData_, sizeof(TG_HWLightsData)*lightDataStructuresCount);
        delete[] lightData_;
        lightData_ = new_lights_data;
    }

    lightData_[lightDataStructuresCount] = *light_data;
	uint32_t rv = lightDataStructuresCount;
	lightDataStructuresCount++;
    return rv;
}

void MC_TextureManager::resetLightData()
{
    lightDataStructuresCount = 0;
}

mat4 gos2my(Stuff::Matrix4D& m)
{
	mat4 m2(
		m.entries[0], m.entries[1], m.entries[2], m.entries[3],
		m.entries[4], m.entries[5], m.entries[6], m.entries[7],
		m.entries[8], m.entries[9], m.entries[10], m.entries[11],
		m.entries[12], m.entries[13], m.entries[14], m.entries[15]);
	return m2;
}

mat4 gos2my(Stuff::LinearMatrix4D& m)
{
	mat4 m2(
		m.entries[0], m.entries[1], m.entries[2], m.entries[3],
		m.entries[4], m.entries[5], m.entries[6], m.entries[7],
		m.entries[8], m.entries[9], m.entries[10], m.entries[11],
		0.0f, 0.0f, 0.0f, 1.0f);
	return m2;
}


////////////////////////////////////////////////////////////////////////////////
class ShapeRenderer {

	mat4* world_;
	mat4* view_;
	mat4* wvp_;
	float* viewport_;
	HGOSBUFFER lights_data_;

public:

	void setup(mat4* world, mat4* view, mat4* wvp, float* viewport)
	{
		gosASSERT(world && view && wvp && viewport);
		world_ = world;
		view_ = view;
		wvp_ = wvp;
		viewport_ = viewport;
	}

	void set_lights_data(const HGOSBUFFER lights_data)
	{
		lights_data_ = lights_data;
	}

	void render(HGOSBUFFER vb, HGOSBUFFER ib, HGOSVERTEXDECLARATION vdecl, DWORD texture_id, int light_index, bool isHudElement = false)
	{
		gos_SetRenderState(gos_State_Texture, texture_id);
		gos_SetRenderViewport(viewport_[2], viewport_[3], viewport_[0], viewport_[1]);

		HGOSRENDERMATERIAL mat = texture_id == 0 ? gos_getRenderMaterial("gos_vertex_lighted") : gos_getRenderMaterial("gos_tex_vertex_lighted");

		gos_SetRenderMaterialParameterMat4(mat, "world_", (const float*)*world_);
		//gos_SetRenderMaterialParameterMat4(mat, "view_", (const float*)*view_);
		gos_SetRenderMaterialParameterMat4(mat, "wvp_", (const float*)*wvp_);

        float ld[4] = { (float)light_index, 0.0f, 0.0f, 0.0f};
		gos_SetRenderMaterialParameterFloat4(mat, "light_offset_", ld);

		// GPU projection via terrainMVP (skip for HUD elements which need legacy viewport projection)
		gos_SetRenderMaterialParameterInt(mat, "gpuProjection", isHudElement ? 0 : 1);

		gos_SetRenderMaterialUniformBlockBindingPoint(mat, "LightsData", LIGHT_DATA_ATTACHMENT_SLOT);
		gos_SetRenderMaterialUniformBlockBindingPoint(mat, "SceneData", SCENE_DATA_ATTACHMENT_SLOT);

		gos_ApplyRenderMaterial(mat);

		// Bind shadow maps + terrainMVP after apply() (requires active program)
		if (!isHudElement) {
			gos_SetupObjectShadows(mat);
		}

		gos_RenderIndexedArray(ib, vb, vdecl);

	}

};

void GatherLightsParameters(TG_HWLightsData* lights)
{
	gosASSERT(lights);

	uint32_t num_lights = 0;
	const uint32_t max_num_lights = MAX_HW_LIGHTS_IN_WORLD;

	const TG_LightPtr* listOfLights = TG_Shape::s_listOfLights;
	const DWORD numLights = TG_Shape::s_numLights;

	for (uint32_t iLight = 0; iLight < numLights; iLight++)
	{
		if (num_lights == max_num_lights)
			break;

		if ((listOfLights[iLight] != NULL) && (listOfLights[iLight]->active))
		{

			const DWORD type = listOfLights[iLight]->lightType;

			Stuff::LinearMatrix4D light2world;
			if (TG_LIGHT_AMBIENT != type)
				light2world = listOfLights[iLight]->lightToWorld;
			else
				light2world = Stuff::LinearMatrix4D::Identity;

			memcpy(lights->lightToWorld[num_lights], (const float*)light2world, 12*sizeof(float));
			lights->lightToWorld[num_lights][12] = lights->lightToWorld[num_lights][13] = lights->lightToWorld[num_lights][14] = 0.0f;
			lights->lightToWorld[num_lights][15] = 1.0f;

			Stuff::UnitVector3D uVec;
			light2world.GetLocalForwardInWorld(&uVec);
			lights->lightDir[num_lights][0] = uVec.x;
			lights->lightDir[num_lights][1] = uVec.y;
			lights->lightDir[num_lights][2] = uVec.z;

			lights->lightDir[num_lights][3] = (float)type;

			DWORD startLight = listOfLights[iLight]->GetaRGB();

			lights->lightColor[num_lights][0] = ((startLight >> 16) & 0x000000ff) / 255.0f;
			lights->lightColor[num_lights][1] = ((startLight >> 8) & 0x000000ff) / 255.0f;
			lights->lightColor[num_lights][2] = ((startLight) & 0x000000ff) / 255.0f;
			lights->lightColor[num_lights][3] = 1.0f;

			switch (type)
			{
			case TG_LIGHT_AMBIENT:
				break;
			case TG_LIGHT_INFINITE:
			case TG_LIGHT_INFINITEWITHFALLOFF:
				break;
			case TG_LIGHT_POINT:
				break;
			case TG_LIGHT_TERRAIN:
				break;
			case TG_LIGHT_SPOT:
				break;
			default:
				STOP(("Unknown light type id: %d", type));
			}

			num_lights++;
		}
	}

	lights->numLights_ = num_lights;
}



//----------------------------------------------------------------------
// Draws all textures with isTerrain set that are solid first,
// then draws all alpha with isTerrain set.
void MC_TextureManager::renderLists (void)
{
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 0);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendDecal);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}
	else
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 0);
		gos_SetRenderState( gos_State_Perspective, 1);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 1);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}
		
	DWORD fogColor = eye->fogColor;
	//-----------------------------------------------------
	// FOG time.  Set Render state to FOG on!
	if (useFog)
	{
		//gos_SetRenderState( gos_State_Fog, (int)&fogColor);
		gos_SetRenderState( gos_State_Fog, fogColor); // sebi
	}
	else
	{
		gos_SetRenderState( gos_State_Fog, 0);
	}

	static bool bSkip = true;

	gos_SetRenderState(gos_State_Culling, gos_Cull_CW);

    // copy global list of light data into GPU buffer
    
	const uint32_t gpu_buf_size = gos_GetBufferSizeBytes(lightDataBuffer_);
    const uint32_t cpu_buf_size = lightDataStructuresCount*sizeof(TG_HWLightsData);
    if(gpu_buf_size < cpu_buf_size) {
        gos_DestroyBuffer(lightDataBuffer_);
        lightDataBuffer_ = gos_CreateBuffer(gosBUFFER_TYPE::UNIFORM, gosBUFFER_USAGE::STATIC_DRAW, cpu_buf_size, 1, lightData_);
	    gos_BindBufferBase(lightDataBuffer_, LIGHT_DATA_ATTACHMENT_SLOT);
    }
    else {
        gos_UpdateBuffer(lightDataBuffer_, lightData_, 0, cpu_buf_size);
	    gos_BindBufferBase(lightDataBuffer_, LIGHT_DATA_ATTACHMENT_SLOT);
    }
    //
    
    // update scene data uniform buffer
    Stuff::Vector3D cp = eye->getCameraOrigin();
    {
        ZoneScopedN("Camera.SceneDataUpload");
        sceneData_->fog_start = eye->fogStart;
        sceneData_->fog_end = eye->fogFull;
        sceneData_->min_haze_dist = Camera::MinHazeDistance;
        sceneData_->dist_factor = Camera::DistanceFactor;
        sceneData_->cam_pos[0] = cp.x;
        sceneData_->cam_pos[1] = cp.y;
        sceneData_->cam_pos[2] = cp.z;
        sceneData_->cam_pos[3] = 1.0f;
        vec4 fc = uint32_to_vec4(eye->fogColor);
        sceneData_->fog_color[0] = fc.z;
        sceneData_->fog_color[1] = fc.y;
        sceneData_->fog_color[2] = fc.x;
        sceneData_->fog_color[3] = fc.w;
        sceneData_->baseVertexColor = uint32_to_vec4(BaseVertexColor).zyxw();
        gos_UpdateBuffer(sceneDataBuffer_, sceneData_, 0, sizeof(TG_HWSceneData));
    }
    //gos_BindBufferBase(lightDataBuffer_, LIGHT_DATA_ATTACHMENT_SLOT);
    
    

	{
		ZoneScopedN("Render.3DObjects");
		TracyGpuZone("Render.3DObjects");
	for (size_t i = 0; i<nextAvailableHardwareVertexNode; i++)
	{
		if ((masterHardwareVertexNodes[i].flags & MC2_DRAWSOLID) &&
			(masterHardwareVertexNodes[i].shapes))
		{
			if (masterHardwareVertexNodes[i].flags & MC2_ISTERRAIN)
				gos_SetRenderState(gos_State_TextureAddress, gos_TextureClamp);
			else
				gos_SetRenderState(gos_State_TextureAddress, gos_TextureWrap);

			uint32_t totalShapes = masterHardwareVertexNodes[i].numShapes;
			// in case less shapes were addded in Render() that it was "promised" in Update(), generally etter to investigate and remove all such cases
			if (masterHardwareVertexNodes[i].currentShape != (masterHardwareVertexNodes[i].shapes + masterHardwareVertexNodes[i].numShapes))
			{
				totalShapes = masterHardwareVertexNodes[i].currentShape - masterHardwareVertexNodes[i].shapes;
			}
			for (uint32_t sh = 0; sh < totalShapes; ++sh)
			{
				DWORD textureIndex = masterHardwareVertexNodes[i].textureIndex;
				if (textureIndex == 1227 && bSkip)
					continue;

				static bool b_old_way = false;
				if (b_old_way)
				{
					gos_SetRenderState(gos_State_Texture, masterTextureNodes[textureIndex].get_gosTextureHandle());
					TG_RenderShape* rs = masterHardwareVertexNodes[i].shapes + sh;
					gos_SetRenderViewport(rs->viewport_[2], rs->viewport_[3], rs->viewport_[0], rs->viewport_[1]);


					//gos_SetRenderViewport(0, 0, Environment.drawableWidth, Environment.drawableHeight);
					// TODO: set mvp_ in a separate function, like gos_set_render_camera(mvp_)...
					gos_RenderIndexedArray(rs->ib_, rs->vb_, rs->vdecl_, (const float*)rs->mvp_);
				}
				else
				{
					DWORD texture = masterTextureNodes[textureIndex].get_gosTextureHandle();
					TG_RenderShape* rs = masterHardwareVertexNodes[i].shapes + sh;


					mat4 view_mat = gos2my(TG_Shape::s_worldToCamera);
					mat4 world_mat = gos2my(rs->mw_);
					mat4 wvp_mat = gos2my(rs->mvp_);

					ShapeRenderer shape_renderer;
					shape_renderer.setup(&world_mat, &view_mat, &wvp_mat, rs->viewport_);
					shape_renderer.render(rs->vb_, rs->ib_, rs->vdecl_, texture, rs->light_data_buffer_index_, rs->isHudElement_);
				}

			}

			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterHardwareVertexNodes[i].currentShape = masterHardwareVertexNodes[i].shapes;
			//masterHardwareVertexNodes[i].numShapes = 0;
		}
	}
	} // end Render.3DObjects zone
	drainGLErrors("objects_3d");

	// [Moved in Phase 4 debug] flush() was originally here (after
	// Render.3DObjects). But Render.TerrainSolid runs AFTER us on line
	// ~1287, so terrain was overwriting our building pixels. Flush is
	// now relocated further down, after Render.TerrainSolid completes.

	// restore state as all old-style geometry is culled on CPU and all vertices are already pretransformed
	gos_SetRenderState(gos_State_Culling, gos_Cull_None);

	// restore viewport
	gos_SetRenderViewport(0, 0, Environment.drawableWidth, Environment.drawableHeight);

	// Phase 4a: on the first frame that submits real terrain into
	// masterVertexNodes[], force one static shadow pass regardless of camera
	// position history. This guarantees the initial camera view is shadowed
	// from frame 1 instead of waiting for a >100-unit camera move.
	{
		static bool s_terrainShadowPrimed = false;
		if (!s_terrainShadowPrimed) {
			for (long si = 0; si < nextAvailableVertexNode; si++) {
				if ((masterVertexNodes[si].flags & MC2_DRAWSOLID) &&
					(masterVertexNodes[si].flags & MC2_ISTERRAIN) &&
					masterVertexNodes[si].vertices &&
					masterVertexNodes[si].currentVertex != masterVertexNodes[si].vertices) {
					gos_RequestFullShadowRebuild();
					s_terrainShadowPrimed = true;
					break;
				}
			}
		}
	}

	// Static shadow pass: accumulate terrain into shadow map over multiple frames.
	// World-fixed ortho projection covers entire map. First frame clears + builds matrix,
	// subsequent frames accumulate new terrain as camera reveals new areas.
	// Skip re-render when camera hasn't moved (same terrain already in shadow map).
	{
		static float lastShadowCamX = 1e9f, lastShadowCamY = 1e9f, lastShadowCamZ = 1e9f;
		float sdx = cp.x - lastShadowCamX;
		float sdy = cp.y - lastShadowCamY;
		float sdz = cp.z - lastShadowCamZ;
		float shadowCamDist = sdx*sdx + sdy*sdy + sdz*sdz;
		float shadowCacheThreshold = 100.0f;  // re-render when camera moves >100 units

	bool shadowRebuildForced = gos_ShadowRebuildPending();
	if (gos_IsTerrainTessellationActive() &&
		(shadowRebuildForced || shadowCamDist > shadowCacheThreshold * shadowCacheThreshold)) {
		ZoneScopedN("Shadow.StaticAccum");
		TracyGpuZone("Shadow.StaticAccum");

		// Forced passes must NOT advance the camera tracker — a subsequent
		// genuine >100-unit move must still trigger a normal accumulation update.
		if (!shadowRebuildForced) {
			lastShadowCamX = cp.x; lastShadowCamY = cp.y; lastShadowCamZ = cp.z;
		}

		bool firstFrame = !gos_StaticLightMatrixBuilt();
		if (firstFrame) {
			gos_BuildStaticLightMatrix();  // builds world-fixed light matrix (once)
			gos_MarkStaticLightMatrixBuilt();
		}

		gos_BeginShadowPrePass(firstFrame);  // clear only on first frame
		for (long si = 0; si < nextAvailableVertexNode; si++) {
			if ((masterVertexNodes[si].flags & MC2_DRAWSOLID) &&
				(masterVertexNodes[si].flags & MC2_ISTERRAIN) &&
				masterVertexNodes[si].vertices &&
				masterVertexNodes[si].extras) {

				DWORD totalVerts = masterVertexNodes[si].numVertices;
				if (masterVertexNodes[si].currentVertex !=
					(masterVertexNodes[si].vertices + masterVertexNodes[si].numVertices)) {
					totalVerts = masterVertexNodes[si].currentVertex - masterVertexNodes[si].vertices;
				}

				int extraCount = masterVertexNodes[si].currentExtra
					? (int)(masterVertexNodes[si].currentExtra - masterVertexNodes[si].extras)
					: 0;

				if (totalVerts > 0 && extraCount > 0) {
					gos_SetRenderState(gos_State_Texture, masterTextureNodes[masterVertexNodes[si].textureIndex].get_gosTextureHandle());

					gos_DrawShadowBatchTessellated(
						masterVertexNodes[si].vertices, totalVerts,
						indexArray, totalVerts,
						masterVertexNodes[si].extras, extraCount);
				}
			}
		}
		gos_EndShadowPrePass();

		if (shadowRebuildForced) {
			gos_ClearShadowRebuildPending();
		}
	}
	} // end shadow cache scope
	// Dynamic object shadow pass: render g_shadowShapes[] every frame
	if (gos_IsTerrainTessellationActive() && g_numShadowShapes > 0) {
		ZoneScopedN("Shadow.DynPass");
		TracyGpuZone("Shadow.DynPass");
		float lx = 0, ly = 0, lz = 0;
		gos_GetTerrainLightDir(&lx, &ly, &lz);

		// Ray-ground intersection to find where camera is actually looking.
		// Camera is in Stuff space (x=left, y=elevation, z=forward).
		// Ground plane is y=0 in Stuff space (z=0 in MC2 space).
		// Ray: P = cp + t * lookVector. Solve for cp.y + t * lv.y = 0.
		Stuff::Vector3D lv = eye->getLookVector();
		float focusX, focusZ;  // MC2 space: x=east, y=north
		if (fabsf(lv.y) > 0.001f) {
			float t = -cp.y / lv.y;
			// Pull center 20% back toward camera so bottom-of-screen mechs
			// aren't clipped. t=1.0 = exact screen center, t=0.0 = camera feet.
			t *= 0.80f;
			// Stuff hit point → MC2: MC2.x = -Stuff.x, MC2.y = Stuff.z
			focusX = -(cp.x + t * lv.x);
			focusZ = cp.z + t * lv.z;
		} else {
			// Camera looking horizontally — fall back to ground projection
			focusX = -cp.x;
			focusZ = cp.z;
		}
		gos_BuildDynamicLightMatrix(-lx, -ly, -lz, focusX, focusZ, 0.0f);
		gos_BeginDynamicShadowPass();
		for (int si = 0; si < g_numShadowShapes; si++)
			gos_DrawShadowObjectBatch(g_shadowShapes[si].vb, g_shadowShapes[si].ib,
				g_shadowShapes[si].vdecl, g_shadowShapes[si].worldMatrix);
		gos_EndDynamicShadowPass();
	}
	g_numShadowShapes = 0;

	// No special depth state for DRAWSOLID terrain

	{
		ZoneScopedN("Render.TerrainSolid");
		TracyGpuZone("Render.TerrainSolid");
	bool bSkip_DRAWSOLID = false;
	for (long i=0;i<nextAvailableVertexNode && !bSkip_DRAWSOLID;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_DRAWSOLID) &&
			(masterVertexNodes[i].vertices))
		{
			if (masterVertexNodes[i].flags & MC2_ISTERRAIN) {
				gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
				gos_SetRenderState( gos_State_Terrain, 1 );
			} else {
				gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
				gos_SetRenderState( gos_State_Terrain, 0 );
			}

			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}

			// Set per-node terrain extras for tessellation VBO alignment
			if ((masterVertexNodes[i].flags & MC2_ISTERRAIN) && masterVertexNodes[i].extras) {
				int extraCount = masterVertexNodes[i].currentExtra
					? (int)(masterVertexNodes[i].currentExtra - masterVertexNodes[i].extras)
					: 0;
				gos_SetTerrainBatchExtras(masterVertexNodes[i].extras, extraCount);
			} else {
				gos_SetTerrainBatchExtras(NULL, 0);
			}

			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, tex_resolve(masterVertexNodes[i].textureIndex));
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, tex_resolve(masterVertexNodes[i].textureIndex));

				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;

					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );

					currentVertices += tVertices;
				}
			}
			//Reset the list to zero length to avoid drawing more then once!			
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}
	} // end Render.TerrainSolid zone
	drainGLErrors("terrain");

	// Task 10 flush() — moved here from after Render.3DObjects because
	// terrain renders AFTER 3D objects in this codebase; placing our
	// flush earlier meant terrain overwrote our pixels. Running after
	// terrain but before overlays gives buildings the right layering:
	// on top of terrain, below decals/roads.
	{
		ZoneScopedN("Render.GpuStaticProps");
		TracyGpuZone("Render.GpuStaticProps");
		extern bool g_useGpuStaticProps;
		if (g_useGpuStaticProps) {
			GpuStaticPropBatcher::instance().flush();
		}
	}

	// DRAWSOLID done

	// ── New world-space overlay batches ──────────────────────────────────────
	// These draw calls flush batches accumulated during land->render() and
	// craterManager->render().  They set their own GL state and restore it.
	// Render order matches design: terrain → cement overlays → decals → (old overlays).
	{
		ZoneScopedN("Render.TerrainOverlays");
		TracyGpuZone("Render.TerrainOverlays");
		gos_DrawTerrainOverlays();
	}
	{
		ZoneScopedN("Render.Decals");
		TracyGpuZone("Render.Decals");
		gos_DrawDecals();
	}
	// ── End new world-space overlay batches ───────────────────────────────────

	{
		ZoneScopedN("Render.Overlays");
		TracyGpuZone("Render.Overlays");
	if (Environment.Renderer == 3)
	{
		//Do NOT draw the water as transparent in software
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
	}
	else
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
	}
	
    // sebi: split in 2 parts, first draw objects which have alpha test off, then with alpha test on
    for(int states = 0; states < 2; ++states) 
    {   
        gos_SetRenderState( gos_State_AlphaTest, states);

        for (int i=0;i<nextAvailableVertexNode;i++)
        {
            if ((masterVertexNodes[i].flags & MC2_ISTERRAIN) &&
                    (masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
                    !(masterVertexNodes[i].flags & MC2_ISCRATERS) &&
                    (masterVertexNodes[i].flags & MC2_ALPHATEST)==states*MC2_ALPHATEST &&
                    (masterVertexNodes[i].vertices))
            {
                // The legacy non-water terrain alpha/detail layer sits on the original
                // flat terrain plane and shows through displaced tess terrain as the
                // dark striped under-pattern. Keep water passes, but drop this layer.
                if (!(masterVertexNodes[i].flags & MC2_ISWATER) &&
                    !(masterVertexNodes[i].flags & MC2_ISWATERDETAIL)) {
                    masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
                    continue;
                }

                {
                    int waterMode = 0;
                    if (masterVertexNodes[i].flags & MC2_ISWATER) waterMode = 1;
                    else if (masterVertexNodes[i].flags & MC2_ISWATERDETAIL) waterMode = 2;
                    gos_SetRenderState(gos_State_Water, waterMode);
                }

                DWORD totalVertices = masterVertexNodes[i].numVertices;
                if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
                {
                    totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
                }

                if (totalVertices && (totalVertices < MAX_SENDDOWN))
                {
                    gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
                    gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
                }
                else if (totalVertices > MAX_SENDDOWN)
                {
                    gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());

                    //Must divide up vertices into batches of 10,000 each to send down.
                    // Somewhere around 20000 to 30000 it really gets screwy!!!
                    long currentVertices = 0;
                    while (currentVertices < totalVertices)
                    {
                        gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
                        long tVertices = totalVertices - currentVertices;
                        if (tVertices > MAX_SENDDOWN)
                            tVertices = MAX_SENDDOWN;

                        gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );

                        currentVertices += tVertices;
                    }
                }

                //Reset the list to zero length to avoid drawing more then once!
                //Also comes in handy if gameLogic is not called.
                masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
            }
        }
    }
    //reset alpha test at the end
    gos_SetRenderState( gos_State_AlphaTest, 0);


	//<< sebi: added this section to draw objects which do not have terrain underlayer (those are added in quad.cpp, see (*) there )
	{ ZoneScopedN("Render.NoUnderlayer");
	  TracyGpuZone("Render.NoUnderlayer");
	if (Environment.Renderer != 3)
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}

	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISTERRAIN) &&
			!(masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
			(masterVertexNodes[i].flags & MC2_GPUOVERLAY) &&
			(masterVertexNodes[i].vertices))
		{
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
	
			gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
			gos_SetRenderState( gos_State_ZCompare, 0);
			gos_SetRenderState(gos_State_Overlay, 1);
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
			gos_SetRenderState(gos_State_Overlay, 0);
			gos_SetRenderState( gos_State_ZCompare, 1);
			gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
			
			//Reset the list to zero length to avoid drawing more then once!			
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}
	} // end ZoneScopedN("Render.NoUnderlayer")
	//<< sebi: end of added block

	// Cement overlays (MC2_ISCRATERS|MC2_ISTERRAIN) and decals (!MC2_ISTERRAIN|MC2_ISCRATERS)
	// are now drawn by gos_DrawTerrainOverlays() and gos_DrawDecals() before Render.Overlays.
	// The old Render.CraterOverlays and non-terrain crater loops are removed.

	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState(	gos_State_ZWrite, 1);
		gos_SetRenderState( gos_State_ZCompare, 2);
	}
	else
	{
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Specular, 1);
		// sebi: shadows do not draw in depth, we do not want z-fighting
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_ZCompare, 2);
	}

	//NEVER draw shadows in Software.
	if (Environment.Renderer != 3)
	{
		for (int i=0;i<nextAvailableVertexNode;i++)
		{
			 if	((masterVertexNodes[i].flags & MC2_ISSHADOWS) &&
				(masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
				(masterVertexNodes[i].vertices))
			{
				DWORD totalVertices = masterVertexNodes[i].numVertices;
				if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
				{
					totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
				}
			
				if (totalVertices && (totalVertices < MAX_SENDDOWN))
				{
					gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
					gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
				}
				else if (totalVertices > MAX_SENDDOWN)
				{
					gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
					
					//Must divide up vertices into batches of 10,000 each to send down.
					// Somewhere around 20000 to 30000 it really gets screwy!!!
					long currentVertices = 0;
					while (currentVertices < totalVertices)
					{
						gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
						long tVertices = totalVertices - currentVertices;
						if (tVertices > MAX_SENDDOWN)
							tVertices = MAX_SENDDOWN;
						
						gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
						
						currentVertices += tVertices;
					}
				}
				
				//Reset the list to zero length to avoid drawing more then once!
				//Also comes in handy if gameLogic is not called.
				masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
			}
		}
	}


	gos_SetRenderState( gos_State_ZCompare, 1);
	if (Environment.Renderer != 3)
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}
	
    // sebi: split in 2 parts, first draw objects which have alpha test off, then with alpha test on
    for(int states = 0; states < 2; ++states) 
    {   
        gos_SetRenderState( gos_State_AlphaTest, states);
        for (int i=0;i<nextAvailableVertexNode;i++)
        {
            if (!(masterVertexNodes[i].flags & MC2_ISTERRAIN) &&
                    !(masterVertexNodes[i].flags & MC2_ISSHADOWS) &&
                    !(masterVertexNodes[i].flags & MC2_ISCOMPASS) &&
                    !(masterVertexNodes[i].flags & MC2_ISCRATERS) &&
                    (masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
                    (masterVertexNodes[i].flags & MC2_ALPHATEST)==states*MC2_ALPHATEST &&
                    (masterVertexNodes[i].vertices))
            {
                DWORD totalVertices = masterVertexNodes[i].numVertices;
                if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
                {
                    totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
                }

                if (totalVertices && (totalVertices < MAX_SENDDOWN))
                {
                    gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
                    gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
                }
                else if (totalVertices > MAX_SENDDOWN)
                {
                    gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());

                    //Must divide up vertices into batches of 10,000 each to send down.
                    // Somewhere around 20000 to 30000 it really gets screwy!!!
                    long currentVertices = 0;
                    while (currentVertices < totalVertices)
                    {
                        gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
                        long tVertices = totalVertices - currentVertices;
                        if (tVertices > MAX_SENDDOWN)
                            tVertices = MAX_SENDDOWN;

                        gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );

                        currentVertices += tVertices;
                    }
                }

                //Reset the list to zero length to avoid drawing more then once!
                //Also comes in handy if gameLogic is not called.
                masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
            }
        }
    }
    //reset alpha test at the end
    gos_SetRenderState( gos_State_AlphaTest, 0);

	
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState( gos_State_Fog, 0);
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneOne);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_MonoEnable, 1);
	}
	else
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_Perspective, 1);
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState( gos_State_Fog, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneOne);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_MonoEnable, 0);
	}
				
	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISEFFECTS) &&
			(masterVertexNodes[i].vertices))
		{
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
			
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
	
			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}
	
	gos_SetRenderState(	gos_State_ZWrite, 1);
	
	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISSPOTLGT) &&
			(masterVertexNodes[i].vertices))
		{
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
			
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
	
			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}
	
	gos_SetRenderState( gos_State_ZWrite, 0);
	gos_SetRenderState( gos_State_ZCompare, 0);
	gos_SetRenderState( gos_State_Perspective, 1);
 	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
	gos_SetRenderState( gos_State_AlphaTest, 1);
	
 	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISCOMPASS) &&
			(masterVertexNodes[i].vertices))
		{
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
			
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
	
			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}
	
	gos_SetRenderState( gos_State_Filter, gos_FilterNone);
	
 	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISHUDLMNT) &&
			(masterVertexNodes[i].vertices))
		{
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
			
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
	
			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}

	//Must turn zCompare back on for FXs
	gos_SetRenderState( gos_State_ZCompare, 1 );

	// Reset terrain extra buffer after rendering — will be re-filled during next frame's TerrainQuad::draw() calls
	gos_TerrainExtraReset();
	} // end Render.Overlays zone
}

//----------------------------------------------------------------------
DWORD MC_TextureManager::update (void)
{
	ZoneScopedN("MC_TextureManager::update");
	DWORD numTexturesFreed = 0;
	currentUsedTextures = 0;
	
	{
		ZoneScopedN("MC_TextureManager::update scanNodes");
		for (long i=0;i<MC_MAXTEXTURES;i++)
	{
		if ((masterTextureNodes[i].gosTextureHandle != CACHED_OUT_HANDLE) &&
			(masterTextureNodes[i].gosTextureHandle != 0xffffffff))
		{
			if (!masterTextureNodes[i].uniqueInstance &&
				!(masterTextureNodes[i].neverFLUSH & 1))		//Only uncachable if BIT 1 is set, otherwise, cache 'em out!
			{
				if (masterTextureNodes[i].lastUsed <= (turn-60))
				{
					//----------------------------------------------------------------
					// Cache this badboy out.  Textures don't change.  Just Destroy!
					{
						ZoneScopedN("MC_TextureManager::update cacheOut");
						if (masterTextureNodes[i].gosTextureHandle)
							gos_DestroyTexture(masterTextureNodes[i].gosTextureHandle);
					}

					masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;
					numTexturesFreed++;
				}
			}

			//Count ACTUAL number of textures being used.
			// ALSO can't count on turn being right.  Logistics does not update unless simple Camera is up!!
			if (masterTextureNodes[i].gosTextureHandle != CACHED_OUT_HANDLE)
			{
				currentUsedTextures++;
				if (currentUsedTextures > peakUsedTextures) peakUsedTextures = currentUsedTextures;
			}
		}
		}
	}
	
	return numTexturesFreed;
}

//----------------------------------------------------------------------
DWORD MC_TextureManager::textureFromMemory (DWORD *data, gos_TextureFormat key, DWORD hints, DWORD width, DWORD bitDepth)
{
	ZoneScopedN("MC_TextureManager::textureFromMemory");
	long i=0;

	//--------------------------------------------------------
	// If we called this, we KNOW the texture is NOT loaded!
	//
	// Find first empty NODE
	{
		ZoneScopedN("MC_TextureManager::textureFromMemory findSlot");
		for (i=0;i<MC_MAXTEXTURES;i++)
		{
			if (masterTextureNodes[i].gosTextureHandle == 0xffffffff)
			{
				break;
			}
		}
	}

	if (i == MC_MAXTEXTURES)
		STOP(("TOO Many textures in game.  We have exceeded 4096 game handles"));
		
	//--------------------------------------------------------
	// New Method.  Just store memory footprint of texture.
	// DO NOT create GOS handle until we need it.
 	masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;
	masterTextureNodes[i].nodeName = NULL;

	masterTextureNodes[i].numUsers = 1;
	masterTextureNodes[i].key = key;
	masterTextureNodes[i].hints = hints;
	masterTextureNodes[i].logicalWidth = width;
	masterTextureNodes[i].logicalHeight = width;

	//------------------------------------------
	// Find and store the width.
	masterTextureNodes[i].width = width;
	long txmSize = width * width * bitDepth;
	
	if (!lzBuffer1)
	{
		ZoneScopedN("MC_TextureManager::textureFromMemory lzBuffers");
		lzBuffer1 = (MemoryPtr)textureCacheHeap->Malloc(MAX_LZ_BUFFER_SIZE);
		gosASSERT(lzBuffer1 != NULL);
		
		lzBuffer2 = (MemoryPtr)textureCacheHeap->Malloc(MAX_LZ_BUFFER_SIZE);
		gosASSERT(lzBuffer2 != NULL);
	}
	
	actualTextureSize += txmSize;
	DWORD txmCompressSize;
	{
		ZoneScopedN("MC_TextureManager::textureFromMemory LZCompress");
		txmCompressSize = LZCompress(lzBuffer2,(MemoryPtr)data,txmSize);
	}
	compressedTextureSize += txmCompressSize;
	
 	//-------------------------------------------------------
	// Create a block of cache memory to hold this texture.
	if (!masterTextureNodes[i].textureData )
	{
		ZoneScopedN("MC_TextureManager::textureFromMemory cacheAlloc");
		masterTextureNodes[i].textureData = (DWORD *)textureCacheHeap->Malloc(txmCompressSize);
	}
	
	//No More RAM.  Do not display this texture anymore.
	if (masterTextureNodes[i].textureData == NULL)
		masterTextureNodes[i].gosTextureHandle = 0;
	else
	{
		ZoneScopedN("MC_TextureManager::textureFromMemory cacheCopy");
		memcpy(masterTextureNodes[i].textureData,lzBuffer2,txmCompressSize);
		masterTextureNodes[i].lzCompSize = txmCompressSize;
	}
	
	//------------------	
	return(i);
}

//----------------------------------------------------------------------
DWORD MC_TextureManager::textureInstanceExists (const char *textureFullPathName, gos_TextureFormat key, DWORD hints, DWORD uniqueInstance, DWORD nFlush)
{
	long i=0;

	//--------------------------------------
	// Is this texture already Loaded?
	for (i=0;i<MC_MAXTEXTURES;i++)
	{
		if (masterTextureNodes[i].nodeName)
		{
			if (S_stricmp(masterTextureNodes[i].nodeName,textureFullPathName) == 0)
			{
				if (uniqueInstance == masterTextureNodes[i].uniqueInstance)
				{
					masterTextureNodes[i].numUsers++;
					return(i);							//Return the texture Node Id Now.
				}
				else
				{
					//------------------------------------------------
					// Copy the texture from old Handle to a new one.
					// Return the NEW handle.
					//
					// There should be no code here!!!
				}
			}
		}
	}
	return 0;
}

//----------------------------------------------------------------------
DWORD MC_TextureManager::loadTexture (const char *textureFullPathName, gos_TextureFormat key, DWORD hints, DWORD uniqueInstance, DWORD nFlush)
{
	ZoneScopedN("MC_TextureManager::loadTexture");
	long i=0;

	//--------------------------------------
	// Is this texture already Loaded?
	for (i=0;i<MC_MAXTEXTURES;i++)
	{
		if (masterTextureNodes[i].nodeName && (S_stricmp(masterTextureNodes[i].nodeName,textureFullPathName) == 0))
		{
			if (uniqueInstance == masterTextureNodes[i].uniqueInstance)
			{
				masterTextureNodes[i].numUsers++;
				return(i);							//Return the texture Node Id Now.
			}
			else
			{
				//------------------------------------------------
				// Copy the texture from old Handle to a new one.
				// Return the NEW handle.
				//
				// There should be no code here!!!
			}
		}
	}

	//--------------------------------------------------
	// If we get here, texture has not been loaded yet.
	// Load it now!
	//
	// Find first empty NODE
	for (i=0;i<MC_MAXTEXTURES;i++)
	{
		if (masterTextureNodes[i].gosTextureHandle == 0xffffffff)
		{
			break;
		}
	}

	if (i == MC_MAXTEXTURES)
		STOP(("TOO Many textures in game.  We have exceeded 4096 game handles"));
		
	if (key == gos_Texture_Alpha && Environment.Renderer == 3)
	{
		key = gos_Texture_Keyed;
	}

 	//--------------------------------------------------------
	// New Method.  Just store memory footprint of texture.
	// DO NOT create GOS handle until we need it.
 	masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;
	masterTextureNodes[i].nodeName = (char *)textureStringHeap->Malloc(strlen(textureFullPathName) + 1);
	gosASSERT(masterTextureNodes[i].nodeName != NULL);

	strcpy(masterTextureNodes[i].nodeName,textureFullPathName);
	masterTextureNodes[i].numUsers = 1;
	masterTextureNodes[i].key = key;
	masterTextureNodes[i].hints = hints;
	masterTextureNodes[i].uniqueInstance = uniqueInstance;
	masterTextureNodes[i].neverFLUSH = nFlush;
	masterTextureNodes[i].logicalWidth = 0;
	masterTextureNodes[i].logicalHeight = 0;

	//----------------------------------------------------------------------------------------------
	// Store a cache-format marker and fileSize in width so that cache knows to create new texture from memory.
	// This way, we never need to know anything about the texture AND we can store PMGs
	// in memory instead of TGAs which use WAY less RAM!
	File textureFile;
#ifdef _DEBUG
	long textureFileOpenResult = 
#endif
		textureFile.open(textureFullPathName);
	gosASSERT(textureFileOpenResult == NO_ERR);

	if (textureFile.isLoadedFromDisk())
		masterTextureNodes[i].uvScale = 4;

	tryReadTgaLogicalSize(textureFile, masterTextureNodes[i].uvScale,
		masterTextureNodes[i].logicalWidth, masterTextureNodes[i].logicalHeight);

	long txmSize = textureFile.fileSize();
	
	if (!lzBuffer1)
	{
		ZoneScopedN("MC_TextureManager::loadTexture lzBuffers");
		lzBuffer1 = (MemoryPtr)textureCacheHeap->Malloc(MAX_LZ_BUFFER_SIZE);
		gosASSERT(lzBuffer1 != NULL);
		
		lzBuffer2 = (MemoryPtr)textureCacheHeap->Malloc(MAX_LZ_BUFFER_SIZE);
		gosASSERT(lzBuffer2 != NULL);
	}

	//Try reading the RAW data out of the fastFile.
	// If it succeeds, we just saved a complete compress, decompress and two memcpys!!
	//
	long result;
	{
		ZoneScopedN("MC_TextureManager::loadTexture readRAW");
		result = textureFile.readRAW(masterTextureNodes[i].textureData,textureCacheHeap);
	}
	if (!result)
	{
		gosASSERT(txmSize <= MAX_LZ_BUFFER_SIZE);
		{
			ZoneScopedN("MC_TextureManager::loadTexture fileRead");
			textureFile.read(lzBuffer1,txmSize);
		}

		textureFile.close();

		actualTextureSize += txmSize;
		const bool storeRawFileData = (txmSize >= MC_TEXCACHE_RAW_THRESHOLD);
		const DWORD cacheBytes = storeRawFileData ? txmSize : [&]() -> DWORD {
			DWORD txmCompressSize;
			{
				ZoneScopedN("MC_TextureManager::loadTexture LZCompress");
				txmCompressSize = LZCompress(lzBuffer2,lzBuffer1,txmSize);
			}
			compressedTextureSize += txmCompressSize;
			return txmCompressSize;
		}();

		{
			ZoneScopedN("MC_TextureManager::loadTexture cacheAlloc");
			masterTextureNodes[i].textureData = (DWORD *)textureCacheHeap->Malloc(cacheBytes);
		}
		if (masterTextureNodes[i].textureData == NULL)
			masterTextureNodes[i].gosTextureHandle = 0;
		else
		{
			ZoneScopedN("MC_TextureManager::loadTexture cacheCopy");
			memcpy(masterTextureNodes[i].textureData, storeRawFileData ? lzBuffer1 : lzBuffer2, cacheBytes);
			}

		masterTextureNodes[i].lzCompSize = cacheBytes;
		masterTextureNodes[i].width = (storeRawFileData ? MC_TEXCACHE_FILE_RAW : MC_TEXCACHE_FILE_LZ) + txmSize;
	}
	else
	{
		masterTextureNodes[i].lzCompSize = result;
		masterTextureNodes[i].width = MC_TEXCACHE_FILE_LZ + txmSize;
	}

 	//-------------------
	return(i);
}

//----------------------------------------------------------------------
DWORD MC_TextureManager::textureFromMemoryRaw (DWORD *data, gos_TextureFormat key, DWORD hints, DWORD width, DWORD bitDepth)
{
	ZoneScopedN("MC_TextureManager::textureFromMemoryRaw");
	long i=0;

	{
		ZoneScopedN("MC_TextureManager::textureFromMemoryRaw findSlot");
		for (i=0;i<MC_MAXTEXTURES;i++)
		{
			if (masterTextureNodes[i].gosTextureHandle == 0xffffffff)
			{
				break;
			}
		}
	}

	if (i == MC_MAXTEXTURES)
		STOP(("TOO Many textures in game.  We have exceeded 4096 game handles"));

	masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;
	masterTextureNodes[i].nodeName = NULL;
	masterTextureNodes[i].numUsers = 1;
	masterTextureNodes[i].key = key;
	masterTextureNodes[i].hints = hints;
	masterTextureNodes[i].logicalWidth = width;
	masterTextureNodes[i].logicalHeight = width;

	const DWORD txmSize = width * width * bitDepth;
	masterTextureNodes[i].width = MC_TEXCACHE_MEM_RAW + txmSize;
	masterTextureNodes[i].lzCompSize = txmSize;
	actualTextureSize += txmSize;
	compressedTextureSize += txmSize;

	{
		ZoneScopedN("MC_TextureManager::textureFromMemoryRaw cacheAlloc");
		masterTextureNodes[i].textureData = (DWORD *)textureCacheHeap->Malloc(txmSize);
	}

	if (masterTextureNodes[i].textureData == NULL)
		masterTextureNodes[i].gosTextureHandle = 0;
	else
	{
		ZoneScopedN("MC_TextureManager::textureFromMemoryRaw cacheCopy");
		memcpy(masterTextureNodes[i].textureData, data, txmSize);
	}

	return(i);
}

//----------------------------------------------------------------------
long MC_TextureManager::saveTexture (DWORD textureIndex, const char *textureFullPathName)
{
	if ((MC_MAXTEXTURES <= textureIndex) || (NULL == masterTextureNodes[textureIndex].textureData))
	{
		return (~NO_ERR);
	}
	File textureFile;
	long textureFileOpenResult = textureFile.create(textureFullPathName);
	if (NO_ERR != textureFileOpenResult)
	{
		textureFile.close();
		return textureFileOpenResult;
	}

	{
		if (masterTextureNodes[textureIndex].width == 0)
		{
			textureFile.close();
			return (~NO_ERR);		//These faces have no texture!!
		}

		{
			//------------------------------------------
			const DWORD cacheFormat = masterTextureNodes[textureIndex].width & 0xF0000000;
			const DWORD originalSize = masterTextureNodes[textureIndex].width & MC_TEXCACHE_SIZE_MASK;
			if (cacheFormat == MC_TEXCACHE_FILE_RAW || cacheFormat == MC_TEXCACHE_MEM_RAW)
			{
				textureFile.write((MemoryPtr)masterTextureNodes[textureIndex].textureData, originalSize);
			}
			else
			{
				// Badboys are now LZ Compressed in texture cache.
				long origSize = LZDecomp(MC_TextureManager::lzBuffer2,(MemoryPtr)masterTextureNodes[textureIndex].textureData,masterTextureNodes[textureIndex].lzCompSize,MAX_LZ_BUFFER_SIZE);
				if (origSize != originalSize)
					STOP(("Decompressed to different size from original!  Txm:%s  Width:%d  DecompSize:%d",masterTextureNodes[textureIndex].nodeName,originalSize,origSize));

				if (origSize >= MAX_LZ_BUFFER_SIZE)
					STOP(("Texture TOO large: %s",masterTextureNodes[textureIndex].nodeName));

				textureFile.write(MC_TextureManager::lzBuffer2, origSize);
			}
		}
		textureFile.close();
	}

	return NO_ERR;
}

DWORD MC_TextureManager::copyTexture( DWORD texNodeID )
{
	gosASSERT( texNodeID < MC_MAXTEXTURES );
	if ( masterTextureNodes[texNodeID].gosTextureHandle != -1 )
	{
		masterTextureNodes[texNodeID].numUsers++;
		return texNodeID;
	}
	else
	{
		STOP(( "tried to copy an invalid texture" ));
	}

	return -1;

}
//----------------------------------------------------------------------
// MC_TextureNode
DWORD MC_TextureNode::get_gosTextureHandle (void)	//If texture is not in VidRAM, cache a texture out and cache this one in.
{
	ZoneScopedN("MC_TextureNode::get_gosTextureHandle");
	if (gosTextureHandle == 0xffffffff)
	{
		//Somehow this texture is bad.  Probably we are using a handle which got purged between missions.
		// Just send back, NO TEXTURE and we should be able to debug from there because the tri will have no texture!!
		PAUSE(("txmmgr: Bad texture handle!"));
		return 0x0;
	}
	
	if (gosTextureHandle != CACHED_OUT_HANDLE)
	{
		lastUsed = turn;
		return gosTextureHandle;
	}
	else
	{
		if ((mcTextureManager->currentUsedTextures >= MAX_MC2_GOS_TEXTURES) && !mcTextureManager->flushCache())
		{
			PAUSE(("txmmgr: Out of texture handles!"));
			TracyMessageL("txmmgr: Out of texture handles!");
			return 0x0;		//No texture!
		}
	   
		if (width == 0)
		{
			{
				char _cbbuf[256];
				snprintf(_cbbuf, sizeof(_cbbuf),
					"[TXM] zero-width texture node: nodeName=%s handle=%u lzCompSize=%u",
					nodeName ? nodeName : "<null>",
					(unsigned)gosTextureHandle, (unsigned)lzCompSize);
				puts(_cbbuf); fflush(stdout); crashbundle_append(_cbbuf);
			}
			PAUSE(("txmmgr: Textur has zero width!"));
			return 0;		//These faces have no texture!!
		}

		if (!textureData)
		{
			PAUSE(("txmmgr: Cache is out of RAM!"));
			return 0x0;		//No Texture.  Cache is out of RAM!!
		}

		const DWORD cacheFormat = width & 0xF0000000;
		if ((cacheFormat == MC_TEXCACHE_FILE_LZ) || (cacheFormat == MC_TEXCACHE_FILE_RAW) || (cacheFormat == MC_TEXCACHE_MEM_RAW))
		{
			const DWORD originalSize = width & MC_TEXCACHE_SIZE_MASK;
			BYTE* textureBytes = (BYTE*)textureData;
			{
				if (cacheFormat == MC_TEXCACHE_FILE_LZ)
				{
					//------------------------------------------
					// Cache this badboy IN.
					// Badboys are now LZ Compressed in texture cache.
					long origSize;
					{
						ZoneScopedN("MC_TextureNode::get_gosTextureHandle LZDecomp");
						origSize = LZDecomp(MC_TextureManager::lzBuffer2,(MemoryPtr)textureData,lzCompSize,MAX_LZ_BUFFER_SIZE);
					}
					if (origSize != originalSize)
						STOP(("Decompressed to different size from original!  Txm:%s  Width:%d  DecompSize:%d",nodeName,originalSize,origSize));

					if (origSize >= MAX_LZ_BUFFER_SIZE)
						STOP(("Texture TOO large: %s",nodeName));
					textureBytes = (BYTE*)MC_TextureManager::lzBuffer2;
				}
				else if (cacheFormat == MC_TEXCACHE_MEM_RAW)
				{
					textureBytes = (BYTE*)textureData;
				}
			}

			if (cacheFormat == MC_TEXCACHE_MEM_RAW)
			{
				{
					ZoneScopedN("MC_TextureNode::get_gosTextureHandle gos_NewEmptyTexture");
					gosTextureHandle = gos_NewEmptyTexture(key,nodeName,logicalWidth ? logicalWidth : width,hints);
				}
				TEXTUREPTR pTextureData;
				{
					ZoneScopedN("MC_TextureNode::get_gosTextureHandle gos_LockTexture");
					gos_LockTexture(gosTextureHandle, 0, 0, &pTextureData);
				}
				{
					ZoneScopedN("MC_TextureNode::get_gosTextureHandle textureMemcpy");
					memcpy(pTextureData.pTexture, textureBytes, originalSize);
				}
				{
					ZoneScopedN("MC_TextureNode::get_gosTextureHandle gos_UnLockTexture");
					gos_UnLockTexture(gosTextureHandle);
				}
			}
			else
			{
				{
					ZoneScopedN("MC_TextureNode::get_gosTextureHandle gos_NewTextureFromMemory");
					gosTextureHandle = gos_NewTextureFromMemory(key,nodeName,textureBytes,originalSize,hints);
				}
			}
			mcTextureManager->currentUsedTextures++;
			if (mcTextureManager->currentUsedTextures > mcTextureManager->peakUsedTextures)
				mcTextureManager->peakUsedTextures = mcTextureManager->currentUsedTextures;
			++gTxmRealizedTotal;
			TracyPlot("Txm realized total", gTxmRealizedTotal);
			lastUsed = turn;

			return gosTextureHandle;
		}
		else
		{
			{
				ZoneScopedN("MC_TextureNode::get_gosTextureHandle gos_NewEmptyTexture");
				gosTextureHandle = gos_NewEmptyTexture(key,nodeName,width,hints);
			}
			mcTextureManager->currentUsedTextures++;
			if (mcTextureManager->currentUsedTextures > mcTextureManager->peakUsedTextures)
				mcTextureManager->peakUsedTextures = mcTextureManager->currentUsedTextures;
			++gTxmRealizedTotal;
			TracyPlot("Txm realized total", gTxmRealizedTotal);
			
			//------------------------------------------
			// Cache this badboy IN.
			TEXTUREPTR pTextureData;
			{
				ZoneScopedN("MC_TextureNode::get_gosTextureHandle gos_LockTexture");
				gos_LockTexture(gosTextureHandle, 0, 0, &pTextureData);
			}
		 
			//-------------------------------------------------------
			// Create a block of cache memory to hold this texture.
			DWORD txmSize = pTextureData.Height * pTextureData.Height * sizeof(DWORD);
			gosASSERT(textureData);
			
			{
				ZoneScopedN("MC_TextureNode::get_gosTextureHandle LZDecomp");
				LZDecomp(MC_TextureManager::lzBuffer2,(MemoryPtr)textureData,lzCompSize,MAX_LZ_BUFFER_SIZE);
			}
			{
				ZoneScopedN("MC_TextureNode::get_gosTextureHandle textureMemcpy");
				memcpy(pTextureData.pTexture,MC_TextureManager::lzBuffer2,txmSize);
			}
			 
			//------------------------
			// Unlock the texture
			{
				ZoneScopedN("MC_TextureNode::get_gosTextureHandle gos_UnLockTexture");
				gos_UnLockTexture(gosTextureHandle);
			}
			 
			lastUsed = turn;
			return gosTextureHandle;
		}
	}
}

//----------------------------------------------------------------------
void MC_TextureNode::destroy (void)
{
	if ((gosTextureHandle != CACHED_OUT_HANDLE) && (gosTextureHandle != 0xffffffff) && (gosTextureHandle != 0x0))
	{
		gos_DestroyTexture(gosTextureHandle);
	}
	
	mcTextureManager->textureStringHeap->Free(nodeName);
	mcTextureManager->textureCacheHeap->Free(textureData);
	init();
}

//----------------------------------------------------------------------
