#include "txmmgr.h"
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
#include <format>

// NS3 boundary: effectStream belongs to the texture/effect subsystem, not
// to whatever game/tool main links it. Previously redefined in every main.
Stuff::MemoryStream *effectStream = nullptr;
static std::unique_ptr<Stuff::MemoryStream> s_effectStreamOwner;

void InstallEffectStream(const void *data, size_t size)
{
	s_effectStreamOwner = std::make_unique<Stuff::MemoryStream>(const_cast<void *>(data), static_cast<DWORD>(size));
	effectStream = s_effectStreamOwner.get();
}

void ReleaseEffectStream()
{
	s_effectStreamOwner.reset();
	effectStream = nullptr;
}

// static globals
MC_TextureManager *mcTextureManager = nullptr;

bool MLRVertexLimitReached = false;

// CP-1: file-scope so a per-mission hook can re-prime the static terrain shadow
// accumulation for the new mission.  Previously a function-local static inside
// renderLists(); promoted here so mc_ResetTerrainShadowPrimed() can clear it.
static bool s_terrainShadowPrimed = false;
void mc_ResetTerrainShadowPrimed() { s_terrainShadowPrimed = false; }

void MC_TextureManager::start()
{
    mc2::textures::startSources();
    mc2::renderSubmissions().start();
    mc2::vkLights::start();
    initTexResolveTable();
}

extern Stuff::MemoryStream *effectStream;
extern MidLevelRenderer::MLRClipper * theClipper;
void MC_TextureManager::destroy (void)
{
    mc2::textures::flushSources(true);
    mc2::renderSubmissions().clearTextureBins();

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

	effectStream = nullptr;
	s_effectStreamOwner.reset();
	
	delete gosFX::LightManager::Instance;
	gosFX::LightManager::Instance = nullptr;

	gos_PopCurrentHeap();

    mc2::renderSubmissions().destroy();
    mc2::vkLights::shutdown();

}

MC_TextureManager::~MC_TextureManager (void) { destroy(); }

void MC_TextureManager::flush (bool justTextures)
{
    mc2::textures::flushSources(false);
    mc2::renderSubmissions().clearTextureBins();
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

	effectStream = nullptr;
	s_effectStreamOwner.reset();
	
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
	
	InstallEffectStream(effectsData.data(), static_cast<size_t>(effectsSize));
	gosFX::EffectLibrary::Instance->Load(effectStream);
	
	gosFX::LightManager::Instance = new gosFX::LightManager();

	gos_PopCurrentHeap();
}
