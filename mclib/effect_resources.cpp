// Copyright (C) Microsoft Corporation. All rights reserved.
// Effect stream ownership and the existing MLR/gosFX lifecycle hooks.
#include "render_scene_support.h"
#include "effect_resources.h"
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

extern MidLevelRenderer::MLRClipper* theClipper;
void mc2::effects::shutdown()
{
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

}
void mc2::effects::reload()
{
    shutdown();

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
