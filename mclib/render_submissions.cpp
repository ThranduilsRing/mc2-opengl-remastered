// Copyright (C) Microsoft Corporation. All rights reserved.
// Per-frame geometry bins and pools feeding native Vulkan draws.
#include "render_submissions.h"
#include "gos_profiler.h"
bool MLRVertexLimitReached = false;
mc2::RenderSubmissions& mc2::renderSubmissions()
{
    static RenderSubmissions queue;
    return queue;
}
// Frees up gos_VERTEX manager memory
void mc2::RenderSubmissions::freeVertices() { gvManager.reset(); }

// Creates gos_VERTEX Manager and allocates RAM.  Will not allocate if already done!
void mc2::RenderSubmissions::startVertices (long maxVertices)
{
	if (gvManager == nullptr)
	{
		gvManager = std::make_unique<gos_VERTEXManager>();
		gvManager->init(maxVertices);
		gvManager->reset();
	}
}

void mc2::RenderSubmissions::start (void)
{
	ZoneScopedN("RenderSubmissions::start");
	init();

	// The node arrays live in vectors owned by the queue; the vertex node types are zero-filled by value-initialisation,
	// as the memsets they replace were.
	static_assert(std::is_trivially_default_constructible_v<MC_VertexArrayNode>,
		"value-initialisation must equal the zero fill of the old node arrays");
    textureBins_.clear();

	masterVertexNodeStore.assign(MC_MAX_RENDER_NODES, MC_VertexArrayNode{});
	masterVertexNodes = masterVertexNodeStore.data();

}

void mc2::RenderSubmissions::destroy()
{
    textureBins_.clear();
    freeVertices();
	// Release the node storage; every view into it ends here.
	masterVertexNodes = nullptr;
	std::vector<MC_VertexArrayNode>().swap(masterVertexNodeStore);

}
mc2::RenderSubmissions::~RenderSubmissions() { destroy(); }
