// This class will manage the texture memory provided by GOS
// GOS gives me a maximum of 256 256x256 pixel texture pages.
// I want GOS to think I only use 256x256 textures.  This class
// will insure that GOS believes that completely and provided
// smaller texture surfaces out of the main surface if necessary
// as well as returning the necessary UVs to get to the other surface.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include "txmmgr_internal.h"
#include "texture_ref.h"
#include "texture_assets.h"
#include "../RenderCore/TextureService.h"
#include "../GameOS/gameos/utils/mc2log.h"

// NS3 boundary: effectStream belongs to the texture/effect subsystem, not
// to whatever game/tool main links it. Previously redefined in every main.
Stuff::MemoryStream *effectStream = nullptr;

// static globals
MC_TextureManager *mcTextureManager = nullptr;
gos_VERTEXManager *MC_TextureManager::gvManager = nullptr;
gos_RenderShapeManager<TG_RenderShape> *MC_TextureManager::rsManager = nullptr;

bool MLRVertexLimitReached = false;

// CP-1: file-scope so a per-mission hook can re-prime the static terrain shadow
// accumulation for the new mission.  Previously a function-local static inside
// renderLists(); promoted here so mc_ResetTerrainShadowPrimed() can clear it.
static bool s_terrainShadowPrimed = false;
void mc_ResetTerrainShadowPrimed() { s_terrainShadowPrimed = false; }

// Frees up gos_VERTEX manager memory
void MC_TextureManager::freeVertices(void)
{
	if (gvManager)
	{
		gvManager->destroy();
		delete gvManager;
		gvManager = nullptr;
	}
}

void MC_TextureManager::freeShapes(void)
{
	if (rsManager)
	{
		rsManager->destroy();
		delete rsManager;
		rsManager = nullptr;
	}
}
		
// Creates gos_VERTEX Manager and allocates RAM.  Will not allocate if already done!
void MC_TextureManager::startVertices (long maxVertices)
{
	if (gvManager == nullptr)
	{
		gvManager = new gos_VERTEXManager;
		gvManager->init(maxVertices);
		gvManager->reset();
	}
}

void MC_TextureManager::startShapes(uint32_t maxShapes)
{
	if (rsManager == nullptr)
	{
		rsManager = new gos_RenderShapeManager<TG_RenderShape>;
		rsManager->init(maxShapes);
		rsManager->reset();
	}
}
	 
// Class MC_TextureManager
void MC_TextureManager::start (void)
{
	ZoneScopedN("MC_TextureManager::start");
	init();

	// The node arrays live in vectors owned by the manager; the vertex node types are zero-filled by value-initialisation,
	// as the memsets they replace were.
	static_assert(std::is_trivially_default_constructible_v<MC_VertexArrayNode> && std::is_trivially_default_constructible_v<MC_HardwareVertexArrayNode>,
		"value-initialisation must equal the zero fill of the old node arrays");
    textureBins_.clear();
    mc2::textures::startSources();

	masterVertexNodeStore.assign(MC_MAX_RENDER_NODES, MC_VertexArrayNode{});
	masterVertexNodes = masterVertexNodeStore.data();

	masterHardwareVertexNodeStore.assign(MC_MAX_RENDER_NODES, MC_HardwareVertexArrayNode{});
	masterHardwareVertexNodes = masterHardwareVertexNodeStore.data();

	indexArrayStore.resize(MC_MAXFACES);
	indexArray = indexArrayStore.data();
	for (int i=0;i<MC_MAXFACES;i++)
		indexArray[i] = i;
		
    lightDataStructuresCapacity = 128;
    lightDataStructuresCount = 0;
    lightData_.assign(lightDataStructuresCapacity, TG_HWLightsData());

	initTexResolveTable();
}

extern Stuff::MemoryStream *effectStream;
extern MidLevelRenderer::MLRClipper * theClipper;
void MC_TextureManager::destroy (void)
{
    mc2::textures::flushSources(true);
    textureBins_.clear();

	gos_PushCurrentHeap(MidLevelRenderer::Heap);

	delete MidLevelRenderer::MLRTexturePool::Instance;
	MidLevelRenderer::MLRTexturePool::Instance = nullptr; 

	delete theClipper;
	theClipper = nullptr;
	
	gos_PopCurrentHeap();

	// Shutdown the GOS FX and MLR.
	gos_PushCurrentHeap(gosFX::Heap);
	
	delete gosFX::EffectLibrary::Instance;
	gosFX::EffectLibrary::Instance = nullptr;

	delete effectStream;
	effectStream = nullptr;
	
	delete gosFX::LightManager::Instance;
	gosFX::LightManager::Instance = nullptr;

	gos_PopCurrentHeap();

	// Release the node storage; every view into it ends here.
	masterVertexNodes = nullptr;
	masterHardwareVertexNodes = nullptr;
	indexArray = nullptr;
	std::vector<MC_VertexArrayNode>().swap(masterVertexNodeStore);
	std::vector<MC_HardwareVertexArrayNode>().swap(masterHardwareVertexNodeStore);
	std::vector<WORD>().swap(indexArrayStore);
	



    std::vector<TG_HWLightsData>().swap(lightData_);

}

MC_TextureManager::~MC_TextureManager (void) { destroy(); }

void MC_TextureManager::flush (bool justTextures)
{
    mc2::textures::flushSources(false);
    textureBins_.clear();
    invalidateTexResolveTable();
#ifdef MC2_VULKAN_ISLAND
    mc2_vk_legacy_shape_island_reset();
    mc2_vk_overlay_reset();
#endif

	//If we just wanted to free up RAM, just return and let the MUNGA stuff go later.
	if (justTextures)
		return;

	gos_PushCurrentHeap(MidLevelRenderer::Heap);

	delete MidLevelRenderer::MLRTexturePool::Instance;
	MidLevelRenderer::MLRTexturePool::Instance = nullptr; 

	delete theClipper;
	theClipper = nullptr;
	
	gos_PopCurrentHeap();

	// Shutdown the GOS FX and MLR.
	gos_PushCurrentHeap(gosFX::Heap);
	
	delete gosFX::EffectLibrary::Instance;
	gosFX::EffectLibrary::Instance = nullptr;

	delete effectStream;
	effectStream = nullptr;
	
	delete gosFX::LightManager::Instance;
	gosFX::LightManager::Instance = nullptr;

	gos_PopCurrentHeap();

	//Restart MLR and the GOSFx
	gos_PushCurrentHeap(MidLevelRenderer::Heap);

	MidLevelRenderer::TGAFilePool *pool = new MidLevelRenderer::TGAFilePool("data" PATH_SEPARATOR "tgl" PATH_SEPARATOR "128" PATH_SEPARATOR);
	MidLevelRenderer::MLRTexturePool::Instance = new MidLevelRenderer::MLRTexturePool(pool);

	MidLevelRenderer::MLRSortByOrder *cameraSorter = new MidLevelRenderer::MLRSortByOrder(MidLevelRenderer::MLRTexturePool::Instance);
	theClipper = new MidLevelRenderer::MLRClipper(nullptr, cameraSorter);
	
	gos_PopCurrentHeap();

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
	std::vector<unsigned char> effectsData(static_cast<size_t>(effectsSize));
	effectFile.read(effectsData.data(),effectsSize);
	effectFile.close();
	
	effectStream = new Stuff::MemoryStream(effectsData.data(),effectsSize);
	gosFX::EffectLibrary::Instance->Load(effectStream);
	
	gosFX::LightManager::Instance = new gosFX::LightManager();

	gos_PopCurrentHeap();
}







void MC_TextureManager::addRenderShape(DWORD nodeId, TG_RenderShape* render_shape, DWORD flags)
{
	//This function adds the actual vertex data to the texture Node.
	if (nodeId < mc2::textures::kTextureAssetLimit)
	{
        auto& textureNode = textureBin(nodeId);
		if (textureNode.hardwareVertexData &&
			textureNode.hardwareVertexData->flags == flags)
		{
			TG_RenderShape* shapes = textureNode.hardwareVertexData->currentShape;
			if (!shapes && !textureNode.hardwareVertexData->shapes)
			{
				textureNode.hardwareVertexData->currentShape =
					shapes =
					textureNode.hardwareVertexData->shapes =
					rsManager->getBlock(textureNode.hardwareVertexData->numShapes);
			}

			if (shapes && shapes < (textureNode.hardwareVertexData->shapes + textureNode.hardwareVertexData->numShapes))
			{
				*shapes = *render_shape;
				shapes++;
			}

			textureNode.hardwareVertexData->currentShape = shapes;
		}
		else if (textureNode.hardwareVertexData2 &&
			textureNode.hardwareVertexData2->flags == flags)
		{
			TG_RenderShape* shapes = textureNode.hardwareVertexData2->currentShape;

			//sebi: looks like assert may happen if more vertices added than was calculated on stage when addTriange was called. As one can see in (*) first time we go here, we allocate enough memory for all potential vertices, but if it is not enough this assert will trigger
#if defined( _DEBUG) || defined(_ARMOR)
			TG_RenderShape* oldShapes = shapes;
			TG_RenderShape* oldStart = (textureNode.hardwareVertexData2->shapes + textureNode.hardwareVertexData2->numShapes);
#endif
			gosASSERT(oldShapes < oldStart);

			// (*)
			if (!shapes && !textureNode.hardwareVertexData2->shapes)
			{
				textureNode.hardwareVertexData2->currentShape =
					shapes =
					textureNode.hardwareVertexData2->shapes =
					rsManager->getBlock(textureNode.hardwareVertexData2->numShapes);
			}

			if (shapes && shapes < (textureNode.hardwareVertexData2->shapes + textureNode.hardwareVertexData2->numShapes))
			{
				*shapes = *render_shape;
				shapes++;
			}

			textureNode.hardwareVertexData2->currentShape = shapes;
		}
		else if (textureNode.hardwareVertexData3 &&
			textureNode.hardwareVertexData3->flags == flags)
		{
			TG_RenderShape * shapes = textureNode.hardwareVertexData3->currentShape;

#if defined(_DEBUG) || defined(_ARMOR)
			TG_RenderShape * oldShapes = shapes;
			TG_RenderShape * oldStart = (textureNode.hardwareVertexData3->shapes + textureNode.hardwareVertexData3->numShapes);
#endif
			gosASSERT(oldShapes < oldStart);

			if (!shapes && !textureNode.hardwareVertexData3->shapes)
			{
				textureNode.hardwareVertexData3->currentShape =
					shapes =
					textureNode.hardwareVertexData3->shapes =
					rsManager->getBlock(textureNode.hardwareVertexData3->numShapes);
			}

			if (shapes && shapes < (textureNode.hardwareVertexData3->shapes + textureNode.hardwareVertexData3->numShapes))
			{
				*shapes = *render_shape;
				shapes++;
			}

			textureNode.hardwareVertexData3->currentShape = shapes;
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

			if (shapes && shapes < (hardwareVertexData->shapes + hardwareVertexData->numShapes))
			{
				*shapes = *render_shape;
				shapes ++;
			}
			else if (shapes)
				g_txmmgr_add_shape_overflow_prevented.fetch_add(1, std::memory_order_relaxed);

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

			if (shapes && shapes < (hardwareVertexData2->shapes + hardwareVertexData2->numShapes))
			{
				*shapes = *render_shape;
				shapes ++;
			}
			else if (shapes)
				g_txmmgr_add_shape_overflow_prevented.fetch_add(1, std::memory_order_relaxed);

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

			if (shapes && shapes < (hardwareVertexData3->shapes + hardwareVertexData3->numShapes))
			{
				*shapes = *render_shape;
				shapes ++;
			}
			else if (shapes)
				g_txmmgr_add_shape_overflow_prevented.fetch_add(1, std::memory_order_relaxed);

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

			if (shapes && shapes < (hardwareVertexData4->shapes + hardwareVertexData4->numShapes))
			{
				*shapes = *render_shape;
				shapes ++;
			}
			else if (shapes)
				g_txmmgr_add_shape_overflow_prevented.fetch_add(1, std::memory_order_relaxed);

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

			if (shapes && shapes < (hardwareVertexData5->shapes + hardwareVertexData5->numShapes))
			{
				*shapes = *render_shape;
				shapes ++;
			}
			else if (shapes)
				g_txmmgr_add_shape_overflow_prevented.fetch_add(1, std::memory_order_relaxed);

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

// Registry-driven texture pinning. See
// docs/superpowers/specs/2026-05-06-static-prop-texture-pin-fix.md
// pinNode    asserts the slot is in-range AND has a live texture allocation
//            (numUsers > 0). Pinning a free slot is a bug — the texture might
//            be reallocated to a different consumer before unpinNode runs.
// unpinNode  asserts pinRefCount > 0 before decrement to catch
//            unpaired-release / double-release.
// getPinCount is non-mutating; usable from logging paths.
