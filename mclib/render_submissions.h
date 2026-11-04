// Copyright (C) Microsoft Corporation. All rights reserved.
// Geometry staging for the native Vulkan scene executor; source assets belong to TXM.
#ifndef MC2_RENDER_SUBMISSIONS_H
#define MC2_RENDER_SUBMISSIONS_H
#include <memory>

#ifndef DSTD_H
#include"dstd.h"
#endif

#ifndef HEAP_H
#include"heap.h"
#endif
#include<vector>
#include<unordered_map>
#include "tgl.h"
#include<string.h>
#include<gameos.hpp>
#include "texture_assets.h"
#include "vk_object_lights.h"
namespace mc2 { class VkSceneRenderer; }
#include<mutex>  // std::recursive_mutex — FRAME-JOBS-2 precondition (vertex node allocators)
#include<atomic> // TXMMGR-BOUNDS-HARDEN-1 exhaustion counters
#include<cstdio> // TXMMGR-BOUNDS-HARDEN-1 trace emit

// ---------------------------------------------------------------------------
// TXMMGR-BOUNDS-HARDEN-1 — pool/block exhaustion guards (defined in txmmgr.cpp).
// The vertex/shape pool allocators and the untextured addVertices/addRenderShape
// paths previously advanced their cursor past capacity and relied on a
// release-stripped gosASSERT — i.e. a silent OOB heap write on exhaustion. They
// now fail safe (return nullptr / skip the copy). Counters accumulate only on the
// rare overflow path; MC2_TXMMGR_BOUNDS_TRACE=1 emits a stderr line per event.
// ---------------------------------------------------------------------------
extern bool g_txmmgrBoundsTrace;
extern std::atomic<unsigned long long> g_txmmgr_vertex_block_exhausted;
extern std::atomic<unsigned long long> g_txmmgr_add_vertices_overflow_prevented;
//----------------------------------------------------------------------

// Per-frame submission budgets; source assets have no fixed slot pool.
inline constexpr int MC_MAX_RENDER_NODES = 4096;
inline constexpr int MC_MAXFACES = 16000000;

inline constexpr int MC2_ISTERRAIN = 1;
inline constexpr int MC2_DRAWSOLID = 2;
inline constexpr int MC2_DRAWALPHA = 4;
inline constexpr int MC2_ISSHADOWS = 8;
inline constexpr int MC2_ISEFFECTS = 16;
inline constexpr int MC2_DRAWONEIN = 32;
inline constexpr int MC2_ISCOMPASS = 128;
inline constexpr int MC2_ISSPOTLGT = 256;
inline constexpr int MC2_ISHUDLMNT = 512;
inline constexpr int MC2_ALPHATEST = 1024;
inline constexpr int MC2_ISWATER = 2048;
inline constexpr int MC2_ISWATERDETAIL = 4096;
inline constexpr int MC2_GPUOVERLAY = 8192;

//----------------------------------------------------------------------
// No More MC_BlockInfos.  gosTextureHandle is protected to enforce caching.
namespace mc2 { class RenderSubmissions; }

//---------------------------------------------------------------------------
struct _MC_VertexArrayNode
{

	public:
		DWORD				textureIndex;
		DWORD				flags;						//Marks texture render state and terrain or not, etc.
		long				numVertices;				//Number of vertices this texture will be used to draw this frame.
		gos_VERTEX			*currentVertex;				//CurrentVertex data being added.
		gos_VERTEX			*vertices;					//Pointer into the vertex Pool for this texture to draw.

	void init (void)
	{
		flags = 0;
		numVertices = 0;
		currentVertex = 0;
		vertices = nullptr;
		textureIndex = 0;
	}

}; using MC_VertexArrayNode = _MC_VertexArrayNode;

//----------------------------------------------------------------------
// Per-texture submission links. Source payloads belong to TextureService.
struct MC_RenderTextureBin {
    MC_VertexArrayNode *vertexData=nullptr, *vertexData2=nullptr, *vertexData3=nullptr;
};

class gos_VERTEXManager
{
	//Data Members
	//-------------
	protected:

		void* storage = nullptr;
		long						totalVertices;		//Total number of vertices in pool.
		long						currentVertex;		//Pointer to next available vertex in pool.

	//Member Functions
	//-----------------
	public:

		void init (void)
		{
			storage = nullptr;

			totalVertices = 0;
			currentVertex = 0;
		}

		gos_VERTEXManager (void)
		{
			init();
		}

		void destroy (void)
		{
			std::free(storage);
			storage = nullptr;
			reset();
			totalVertices = 0;
		}

		~gos_VERTEXManager (void)
		{
			destroy();
		}

		void init (long maxVertices)
		{
			totalVertices = maxVertices;
			DWORD heapSize = totalVertices * sizeof(gos_VERTEX);
			std::free(storage);
			storage = std::calloc(1, heapSize);
			gosASSERT(storage != nullptr);
			reset();
		}

		gos_VERTEX *getVertexBlock(long numVertices)
		{
			// TXMMGR-BOUNDS-HARDEN-1: check capacity BEFORE advancing the cursor.
			// Was: advance then gosASSERT (stripped in RelWithDebInfo) → returned a
			// pointer past the committed heap and kept incrementing → OOB write.
			if (numVertices < 0 || currentVertex + numVertices > totalVertices)
			{
				g_txmmgr_vertex_block_exhausted.fetch_add(1, std::memory_order_relaxed);
				if (g_txmmgrBoundsTrace)
					fprintf(stderr, "[TXMMGR_BOUNDS v1] vertex_block_exhausted need=%ld cur=%ld total=%ld\n",
							numVertices, currentVertex, totalVertices);
				return nullptr;
			}
			gos_VERTEX *start = reinterpret_cast<gos_VERTEX *>(storage);
			start = &(start[currentVertex]);
			currentVertex += numVertices;
			return start;
		}

		void reset (void)
		{
			currentVertex = 0;
		}
};
struct TG_HWLightsData;

//----------------------------------------------------------------------
namespace mc2 {
class RenderSubmissions
{
    friend class mc2::VkSceneRenderer;

	//Data Members
	//------------
	protected:
        std::unordered_map<DWORD, MC_RenderTextureBin> textureBins_;
        MC_RenderTextureBin& textureBin(DWORD asset) {
            std::lock_guard<std::recursive_mutex> guard(m_vertexNodeMu);
            return textureBins_[asset];
        }

		MC_VertexArrayNode 				*masterVertexNodes;			//Dynamically allocated from an MC Heap.
		long							nextAvailableVertexNode;	//index to next available vertex Node

		// FRAME-JOBS-2 precondition: protects nextAvailableVertexNode and
		// nextAvailableHardwareVertexNode slot allocations.
		// Atomics alone are NOT sufficient here: the allocation sequence is
		// (1) check !masterTextureNodes[nodeId].vertexDataX, then (2) take a
		// slot index, then (3) write the pointer into the shared per-texture-node
		// struct. Steps 1-3 are not atomic together, so a mutex is required to
		// prevent two threads from both observing a null pointer and then both
		// advancing the counter for the same texture node.
		// recursive_mutex because addTriangleBulk's untextured fallback path
		// calls addTriangle while holding the lock (same thread, same mutex).
		mutable std::recursive_mutex	m_vertexNodeMu;

		std::unique_ptr<gos_VERTEXManager> gvManager;					//Stores arrays of vertices for draw.

		// Owners of the node arrays and the vertex index table. The raw pointers named above are views into them: valid from
		// start() until destroy(), and the stores are sized once in start(), never grown while a view is held.
		std::vector<MC_VertexArrayNode>			masterVertexNodeStore;

																	//Upto four different kinds of untextured triangle!
		MC_VertexArrayNode 				*vertexData;				//This holds the vertex draw data for UNTEXTURED triangles!
		MC_VertexArrayNode				*vertexData2;				//This holds the vertex draw data for UNTEXTURED triangles!
		MC_VertexArrayNode 				*vertexData3;				//This holds the vertex draw data for UNTEXTURED triangles!
		MC_VertexArrayNode				*vertexData4;				//This holds the vertex draw data for UNTEXTURED triangles!
		MC_VertexArrayNode				*vertexData5;				//This holds the vertex draw data for UNTEXTURED triangles!

	//Member Functions
	//-----------------
	public:

		void init (void)
		{

			masterVertexNodes = nullptr;
			nextAvailableVertexNode = 0;

			vertexData = vertexData2 = vertexData3 = vertexData4 = vertexData5 = nullptr;

		}

		RenderSubmissions (void)
		{
			init();

		}

		void destroy (void);

		~RenderSubmissions (void);

		void start (void);

		bool active() const { return masterVertexNodes != nullptr; }
		void clearTextureBins() { textureBins_.clear(); }

		//------------------------------------------------------
		// Frees up gos_VERTEX manager memory
		void freeVertices (void);

		//------------------------------------------------------
		// Creates gos_VERTEX Manager and allocates RAM.  Will not allocate if already done!
		void startVertices (long maxVertices = 30000);

		void addTriangle (DWORD nodeId, DWORD flags)
		{
			std::lock_guard<std::recursive_mutex> _lk(m_vertexNodeMu);  // protects nextAvailableVertexNode + pointer writes
			if ((nodeId < mc2::textures::kTextureAssetLimit) && (nextAvailableVertexNode < MC_MAX_RENDER_NODES))
			{
                auto& textureNode = textureBin(nodeId);
				if (!textureNode.vertexData)
				{
					textureNode.vertexData = &(masterVertexNodes[nextAvailableVertexNode]);
					gosASSERT(textureNode.vertexData->numVertices == 0);
					gosASSERT(textureNode.vertexData->vertices == nullptr);

					nextAvailableVertexNode++;
					textureNode.vertexData->flags = flags;
					textureNode.vertexData->textureIndex = nodeId;
				}
				else if (textureNode.vertexData &&
					(textureNode.vertexData->flags != flags) &&
					!textureNode.vertexData2)
				{
					textureNode.vertexData2 = &(masterVertexNodes[nextAvailableVertexNode]);
					gosASSERT(textureNode.vertexData2->numVertices == 0);
					gosASSERT(textureNode.vertexData2->vertices == nullptr);

					nextAvailableVertexNode++;
					textureNode.vertexData2->flags = flags;
					textureNode.vertexData2->textureIndex = nodeId;
				}
				else if (textureNode.vertexData &&
						(textureNode.vertexData->flags != flags) &&
						textureNode.vertexData2 &&
						(textureNode.vertexData2->flags != flags) &&
						!textureNode.vertexData3)
				{
					textureNode.vertexData3 = &(masterVertexNodes[nextAvailableVertexNode]);
					gosASSERT(textureNode.vertexData3->numVertices == 0);
					gosASSERT(textureNode.vertexData3->vertices == nullptr);

					nextAvailableVertexNode++;
					textureNode.vertexData3->flags = flags;
					textureNode.vertexData3->textureIndex = nodeId;
				}

				if (textureNode.vertexData->flags == flags)
					textureNode.vertexData->numVertices += 3;
				else if (textureNode.vertexData2 &&
						textureNode.vertexData2->flags == flags)
					textureNode.vertexData2->numVertices += 3;
				else if (textureNode.vertexData3 &&
						textureNode.vertexData3->flags == flags)
					textureNode.vertexData3->numVertices += 3;
#ifdef _DEBUG
				else
					STOP(("Could not AddTriangles.  No flags match vertex data"));
#endif
			}
			else if (nextAvailableVertexNode < MC_MAX_RENDER_NODES)
			{
				//Add this one to the untextured vertexBuffers
				if (!vertexData)
				{
					vertexData = &(masterVertexNodes[nextAvailableVertexNode]);
					gosASSERT(vertexData->numVertices == 0);
					gosASSERT(vertexData->vertices == nullptr);

					nextAvailableVertexNode++;
					vertexData->flags = flags;
					vertexData->textureIndex = 0;
				}
				else if (vertexData && (vertexData->flags != flags) &&
						!vertexData2)
				{
					vertexData2 = &(masterVertexNodes[nextAvailableVertexNode]);
					gosASSERT(vertexData2->numVertices == 0);
					gosASSERT(vertexData2->vertices == nullptr);

					nextAvailableVertexNode++;
					vertexData2->flags = flags;
					vertexData2->textureIndex = 0;
				}
				else if (vertexData && (vertexData->flags != flags) &&
						vertexData2 && (vertexData2->flags != flags) &&
						!vertexData3)
				{
					vertexData3 = &(masterVertexNodes[nextAvailableVertexNode]);
					gosASSERT(vertexData3->numVertices == 0);
					gosASSERT(vertexData3->vertices == nullptr);

					nextAvailableVertexNode++;
					vertexData3->flags = flags;
					vertexData3->textureIndex = 0;
				}
				else if (vertexData && (vertexData->flags != flags) &&
						vertexData2 && (vertexData2->flags != flags) &&
						vertexData3 && (vertexData3->flags != flags) &&
						!vertexData4)
				{
					vertexData4 = &(masterVertexNodes[nextAvailableVertexNode]);
					gosASSERT(vertexData4->numVertices == 0);
					gosASSERT(vertexData4->vertices == nullptr);

					nextAvailableVertexNode++;
					vertexData4->flags = flags;
					vertexData4->textureIndex = 0;
				}
				else if (vertexData && (vertexData->flags != flags) &&
					vertexData2 && (vertexData2->flags != flags) &&
					vertexData3 && (vertexData3->flags != flags) &&
					vertexData4 && (vertexData4->flags != flags) &&
					!vertexData5)
				{
					vertexData5 = &(masterVertexNodes[nextAvailableVertexNode]);
					gosASSERT(vertexData5->numVertices == 0);
					gosASSERT(vertexData5->vertices == nullptr);

					nextAvailableVertexNode++;
					vertexData5->flags = flags;
					vertexData5->textureIndex = 0;
				}

				if (vertexData->flags == flags)
					vertexData->numVertices += 3;
				else if (vertexData2 && vertexData2->flags == flags)
					vertexData2->numVertices += 3;
				else if (vertexData3 && vertexData3->flags == flags)
					vertexData3->numVertices += 3;
				else if (vertexData4 && vertexData4->flags == flags)
					vertexData4->numVertices += 3;
				else if (vertexData5 && vertexData5->flags == flags)
					vertexData5->numVertices += 3;
#ifdef _DEBUG
				else
					PAUSE(("Could not AddTriangles.  Too many untextured triangle types"));
#endif
			}
		}

		// Bulk variant of addTriangle: reserves `triCount` triangles at once for a given
		// (nodeId, flags) pair, doing the slot allocation + flag-match traversal exactly
		// once instead of `triCount` times. Mathematically identical numVertices output to
		// `for (i=0; i<triCount; ++i) addTriangle(nodeId, flags)`. Hot-path optimization
		// for callers that emit paired/batched triangles (terrain solid+detail layers,
		// mine pairs, water pairs) — see mclib/quad.cpp.
		void addTriangleBulk (DWORD nodeId, DWORD flags, int triCount)
		{
			// Bounds-check triCount: negative caught by <= 0; upper cap guards against
			// runaway callers sending corrupt counts into the untextured fallback loop
			// (for (i=0; i<triCount; ++i) addTriangle) which has no independent limit.
			// MC_MAXFACES is the total face budget for the entire scene — no single
			// bulk call can legitimately exceed it.
			if (triCount <= 0 || triCount > static_cast<int>(MC_MAXFACES)) return;
			// recursive_mutex: the untextured fallback path below calls addTriangle
			// which also acquires m_vertexNodeMu on the same thread — recursive is required.
			std::lock_guard<std::recursive_mutex> _lk(m_vertexNodeMu);

			if ((nodeId < mc2::textures::kTextureAssetLimit) && (nextAvailableVertexNode < MC_MAX_RENDER_NODES))
			{
                auto& textureNode = textureBin(nodeId);
				if (!textureNode.vertexData)
				{
					textureNode.vertexData = &(masterVertexNodes[nextAvailableVertexNode]);
					gosASSERT(textureNode.vertexData->numVertices == 0);
					gosASSERT(textureNode.vertexData->vertices == nullptr);

					nextAvailableVertexNode++;
					textureNode.vertexData->flags = flags;
					textureNode.vertexData->textureIndex = nodeId;
				}
				else if (textureNode.vertexData &&
					(textureNode.vertexData->flags != flags) &&
					!textureNode.vertexData2)
				{
					textureNode.vertexData2 = &(masterVertexNodes[nextAvailableVertexNode]);
					gosASSERT(textureNode.vertexData2->numVertices == 0);
					gosASSERT(textureNode.vertexData2->vertices == nullptr);

					nextAvailableVertexNode++;
					textureNode.vertexData2->flags = flags;
					textureNode.vertexData2->textureIndex = nodeId;
				}
				else if (textureNode.vertexData &&
						(textureNode.vertexData->flags != flags) &&
						textureNode.vertexData2 &&
						(textureNode.vertexData2->flags != flags) &&
						!textureNode.vertexData3)
				{
					textureNode.vertexData3 = &(masterVertexNodes[nextAvailableVertexNode]);
					gosASSERT(textureNode.vertexData3->numVertices == 0);
					gosASSERT(textureNode.vertexData3->vertices == nullptr);

					nextAvailableVertexNode++;
					textureNode.vertexData3->flags = flags;
					textureNode.vertexData3->textureIndex = nodeId;
				}

				const int delta = 3 * triCount;
				if (textureNode.vertexData->flags == flags)
					textureNode.vertexData->numVertices += delta;
				else if (textureNode.vertexData2 &&
						textureNode.vertexData2->flags == flags)
					textureNode.vertexData2->numVertices += delta;
				else if (textureNode.vertexData3 &&
						textureNode.vertexData3->flags == flags)
					textureNode.vertexData3->numVertices += delta;
#ifdef _DEBUG
				else
					STOP(("Could not AddTrianglesBulk.  No flags match vertex data"));
#endif
			}
			else
			{
				// Untextured path is rare/never for terrain reservations; fall back to
				// per-triangle to avoid duplicating the 5-slot logic from addTriangle.
				for (int i = 0; i < triCount; ++i) addTriangle(nodeId, flags);
			}
		}

		void addVertices (DWORD nodeId, gos_VERTEX *data, DWORD flags)
		{
			//This function adds the actual vertex data to the texture Node.
			if (nodeId < mc2::textures::kTextureAssetLimit)
			{
                auto& textureNode = textureBin(nodeId);
				if (textureNode.vertexData &&
					textureNode.vertexData->flags == flags)
				{
					gos_VERTEX * vertices = textureNode.vertexData->currentVertex;
					if (!vertices && !textureNode.vertexData->vertices)
					{
						textureNode.vertexData->currentVertex =
						vertices =
						textureNode.vertexData->vertices =
						gvManager->getVertexBlock(textureNode.vertexData->numVertices);
					}

					if (vertices && vertices < (textureNode.vertexData->vertices + textureNode.vertexData->numVertices))
					{
						#if 0
						if ((data[0].u > 64.0f) ||
							(data[0].u < -64.0f) ||
							(data[1].u > 64.0f) ||
							(data[1].u < -64.0f) ||
							(data[2].u > 64.0f) ||
							(data[2].u < -64.0f))
						{
							PAUSE(("Vertex U Out of range"));
						}

						if ((data[0].v > 64.0f) ||
							(data[0].v < -64.0f) ||
							(data[1].v > 64.0f) ||
							(data[1].v < -64.0f) ||
							(data[2].v > 64.0f) ||
							(data[2].v < -64.0f))
						{
							PAUSE(("Vertex V Out of range"));
						}
						#endif

						memcpy(vertices,data,sizeof(gos_VERTEX) * 3);
						vertices += 3;
					}

					textureNode.vertexData->currentVertex = vertices;
				}
				else if (textureNode.vertexData2 &&
						 textureNode.vertexData2->flags == flags)
				{
					gos_VERTEX * vertices = textureNode.vertexData2->currentVertex;

                    //sebi: looks like assert may happen if more vertices added than was calculated on stage when addTriange was called. As one can see in (*) first time we go here, we allocate enough memory for all potential vertices, but if it is not enough this assert will trigger
					#if defined( _DEBUG) || defined(_ARMOR)
					gos_VERTEX * oldVertices = vertices;
					gos_VERTEX * oldStart = (textureNode.vertexData2->vertices + textureNode.vertexData2->numVertices);
					#endif
					gosASSERT(oldVertices < oldStart);

                    // (*)
					if (!vertices && !textureNode.vertexData2->vertices)
					{
						textureNode.vertexData2->currentVertex =
						vertices =
						textureNode.vertexData2->vertices =
						gvManager->getVertexBlock(textureNode.vertexData2->numVertices);
					}

					if (vertices && vertices < (textureNode.vertexData2->vertices + textureNode.vertexData2->numVertices))
					{
						#if 0
						if ((data[0].u > 64.0f) ||
							(data[0].u < -64.0f) ||
							(data[1].u > 64.0f) ||
							(data[1].u < -64.0f) ||
							(data[2].u > 64.0f) ||
							(data[2].u < -64.0f))
						{
							PAUSE(("Vertex U Out of range"));
						}

						if ((data[0].v > 64.0f) ||
							(data[0].v < -64.0f) ||
							(data[1].v > 64.0f) ||
							(data[1].v < -64.0f) ||
							(data[2].v > 64.0f) ||
							(data[2].v < -64.0f))
						{
							PAUSE(("Vertex V Out of range"));
						}
						#endif

						memcpy(vertices,data,sizeof(gos_VERTEX) * 3);
						vertices += 3;
					}

					textureNode.vertexData2->currentVertex = vertices;
				}
				else if (textureNode.vertexData3 &&
						 textureNode.vertexData3->flags == flags)
				{
					gos_VERTEX * vertices = textureNode.vertexData3->currentVertex;

					#if defined(_DEBUG) || defined(_ARMOR)
					gos_VERTEX * oldVertices = vertices;
					gos_VERTEX * oldStart = (textureNode.vertexData3->vertices + textureNode.vertexData3->numVertices);
					#endif
					gosASSERT(oldVertices < oldStart);

					if (!vertices && !textureNode.vertexData3->vertices)
					{
						textureNode.vertexData3->currentVertex =
						vertices =
						textureNode.vertexData3->vertices =
						gvManager->getVertexBlock(textureNode.vertexData3->numVertices);
					}

					if (vertices && vertices < (textureNode.vertexData3->vertices + textureNode.vertexData3->numVertices))
					{
						#if 0
						if ((data[0].u > 64.0f) ||
							(data[0].u < -64.0f) ||
							(data[1].u > 64.0f) ||
							(data[1].u < -64.0f) ||
							(data[2].u > 64.0f) ||
							(data[2].u < -64.0f))
						{
							PAUSE(("Vertex U Out of range"));
						}

						if ((data[0].v > 64.0f) ||
							(data[0].v < -64.0f) ||
							(data[1].v > 64.0f) ||
							(data[1].v < -64.0f) ||
							(data[2].v > 64.0f) ||
							(data[2].v < -64.0f))
						{
							PAUSE(("Vertex V Out of range"));
						}
						#endif

						memcpy(vertices,data,sizeof(gos_VERTEX) * 3);
						vertices += 3;
					}

					textureNode.vertexData3->currentVertex = vertices;
				}
				else	//If we got here, something is really wrong
				{
#ifdef _DEBUG
					SPEW(("GRAPHICS", "Flags do not match either set of vertex Data\n"));
#endif
				}
			}
			else
			{
				if (vertexData && vertexData->flags == flags)
				{
					gos_VERTEX * vertices = vertexData->currentVertex;
					if (!vertices && !vertexData->vertices)
					{
						vertexData->currentVertex =
						vertices =
						vertexData->vertices =
						gvManager->getVertexBlock(vertexData->numVertices);
					}

					if (vertices && vertices < (vertexData->vertices + vertexData->numVertices))
					{
						memcpy(vertices,data,sizeof(gos_VERTEX) * 3);
						vertices += 3;
					}
					else if (vertices)
						g_txmmgr_add_vertices_overflow_prevented.fetch_add(1, std::memory_order_relaxed);

					vertexData->currentVertex = vertices;
				}
				else if (vertexData2 && vertexData2->flags == flags)
				{
					gos_VERTEX * vertices = vertexData2->currentVertex;
					if (!vertices && !vertexData2->vertices)
					{
						vertexData2->currentVertex =
						vertices =
						vertexData2->vertices =
						gvManager->getVertexBlock(vertexData2->numVertices);
					}

					if (vertices && vertices < (vertexData2->vertices + vertexData2->numVertices))
					{
						memcpy(vertices,data,sizeof(gos_VERTEX) * 3);
						vertices += 3;
					}
					else if (vertices)
						g_txmmgr_add_vertices_overflow_prevented.fetch_add(1, std::memory_order_relaxed);

					vertexData2->currentVertex = vertices;
				}
				else if (vertexData3 && vertexData3->flags == flags)
				{
					gos_VERTEX * vertices = vertexData3->currentVertex;
					if (!vertices && !vertexData3->vertices)
					{
						vertexData3->currentVertex =
						vertices =
						vertexData3->vertices =
						gvManager->getVertexBlock(vertexData3->numVertices);
					}

					if (vertices && vertices < (vertexData3->vertices + vertexData3->numVertices))
					{
						memcpy(vertices,data,sizeof(gos_VERTEX) * 3);
						vertices += 3;
					}
					else if (vertices)
						g_txmmgr_add_vertices_overflow_prevented.fetch_add(1, std::memory_order_relaxed);

					vertexData3->currentVertex = vertices;
				}
				else if (vertexData4 && vertexData4->flags == flags)
				{
					gos_VERTEX * vertices = vertexData4->currentVertex;
					if (!vertices && !vertexData4->vertices)
					{
						vertexData4->currentVertex =
						vertices =
						vertexData4->vertices =
						gvManager->getVertexBlock(vertexData4->numVertices);
					}

					if (vertices && vertices < (vertexData4->vertices + vertexData4->numVertices))
					{
						memcpy(vertices,data,sizeof(gos_VERTEX) * 3);
						vertices += 3;
					}
					else if (vertices)
						g_txmmgr_add_vertices_overflow_prevented.fetch_add(1, std::memory_order_relaxed);

					vertexData4->currentVertex = vertices;
				}
				else if (vertexData5 && vertexData5->flags == flags)
				{
					gos_VERTEX * vertices = vertexData5->currentVertex;
					if (!vertices && !vertexData5->vertices)
					{
						vertexData5->currentVertex =
							vertices =
							vertexData5->vertices =
							gvManager->getVertexBlock(vertexData5->numVertices);
					}

					if (vertices && vertices < (vertexData5->vertices + vertexData5->numVertices))
					{
						memcpy(vertices, data, sizeof(gos_VERTEX) * 3);
						vertices += 3;
					}
					else if (vertices)
						g_txmmgr_add_vertices_overflow_prevented.fetch_add(1, std::memory_order_relaxed);

					vertexData5->currentVertex = vertices;
				}
				else	//If we got here, something is really wrong
				{
#ifdef _DEBUG
					SPEW(("GRAPHICS", "Flags do not match any set of untextured vertex Data\n"));
#endif
				}
			}
		}

		void clearArrays (void)
		{
            for (auto& bin : textureBins_) bin.second = MC_RenderTextureBin{};

			vertexData = vertexData2 = vertexData3 = vertexData4 = vertexData5 = nullptr;

			memset(masterVertexNodes,0,sizeof(MC_VertexArrayNode)*MC_MAX_RENDER_NODES);

			nextAvailableVertexNode = 0;

			gvManager->reset();

		}

};
RenderSubmissions& renderSubmissions();
} // namespace mc2

void GatherLightsParameters(TG_HWLightsData* lights);

// RENDERER-INPUT-READBACK-1: the probe wrapper around the function above is
// declared in mclib/tgl.h, not here -- its signature needs TG_LightPtr, whose
// underlying TG_Light is an anonymous-struct typedef (tgl.h:300) and so cannot
// be forward-declared in this header.

//----------------------------------------------------------------------

// Shadow shape collection — populated during TG_Shape::Render(), consumed in renderLists()
void addShadowShape(HGOSBUFFER vb, HGOSBUFFER ib, HGOSVERTEXDECLARATION vdecl, const float* worldEntries16);


//----------------------------------------------------------------------
#endif
