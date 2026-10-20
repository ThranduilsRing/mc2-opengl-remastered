// Copyright (C) Microsoft Corporation. All rights reserved.
// Per-frame geometry bins and pools feeding native Vulkan draws.
#include "render_submissions.h"
#include "gos_profiler.h"
mc2::RenderSubmissions& mc2::renderSubmissions()
{
    static RenderSubmissions queue;
    return queue;
}
// Frees up gos_VERTEX manager memory
void mc2::RenderSubmissions::freeVertices() { gvManager.reset(); }

void mc2::RenderSubmissions::freeShapes() { rsManager.reset(); }

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

void mc2::RenderSubmissions::startShapes(uint32_t maxShapes)
{
	if (rsManager == nullptr)
	{
		rsManager = std::make_unique<gos_RenderShapeManager<TG_RenderShape>>();
		rsManager->init(maxShapes);
		rsManager->reset();
	}
}

void mc2::RenderSubmissions::start (void)
{
	ZoneScopedN("RenderSubmissions::start");
	init();

	// The node arrays live in vectors owned by the queue; the vertex node types are zero-filled by value-initialisation,
	// as the memsets they replace were.
	static_assert(std::is_trivially_default_constructible_v<MC_VertexArrayNode> && std::is_trivially_default_constructible_v<MC_HardwareVertexArrayNode>,
		"value-initialisation must equal the zero fill of the old node arrays");
    textureBins_.clear();

	masterVertexNodeStore.assign(MC_MAX_RENDER_NODES, MC_VertexArrayNode{});
	masterVertexNodes = masterVertexNodeStore.data();

	masterHardwareVertexNodeStore.assign(MC_MAX_RENDER_NODES, MC_HardwareVertexArrayNode{});
	masterHardwareVertexNodes = masterHardwareVertexNodeStore.data();

}

void mc2::RenderSubmissions::destroy()
{
    textureBins_.clear();
    freeVertices();
    freeShapes();
	// Release the node storage; every view into it ends here.
	masterVertexNodes = nullptr;
	masterHardwareVertexNodes = nullptr;
	std::vector<MC_VertexArrayNode>().swap(masterVertexNodeStore);
	std::vector<MC_HardwareVertexArrayNode>().swap(masterHardwareVertexNodeStore);

}
mc2::RenderSubmissions::~RenderSubmissions() { destroy(); }
void mc2::RenderSubmissions::addRenderShape(DWORD nodeId, TG_RenderShape* render_shape, DWORD flags)
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
