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
// NOT ANY MORE
// Now we no longer coalesce textures.  Causes too many problems with rendering
// Now it simply checks for redundant filenames and can instance a texture instead of reloading it.
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//
//----------------------------------------------------------------------
#ifndef TXMMGR_H
#define TXMMGR_H
#include "../RenderCore/TextureService.h"

#ifndef DSTD_H
#include"dstd.h"
#endif

#ifndef HEAP_H
#include"heap.h"
#endif
#include<vector>
#include<unordered_map>
#include"tgl.h"   // TG_HWLightsData is stored by value in the manager
#include<string.h>
#include<gameos.hpp>
#include "texture_assets.h"
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
extern std::atomic<unsigned long long> g_txmmgr_block_exhausted;
extern std::atomic<unsigned long long> g_txmmgr_add_vertices_overflow_prevented;
extern std::atomic<unsigned long long> g_txmmgr_add_shape_overflow_prevented;
//----------------------------------------------------------------------
enum class MC_TextureKey
{
	MC_INVALIDTEXTUREKEY = 0,
	MC_Solid,
	MC_Keyed,
	MC_Alpha
};

//----------------------------------------------------------------------
// Per-frame submission budgets; source assets have no fixed slot pool.
#define MC_MAX_RENDER_NODES 4096
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
class MC_TextureManager;

//---------------------------------------------------------------------------
struct _MC_VertexArrayNode
{
	friend class MC_TextureManager;

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

class TG_RenderShape;
struct _MC_HardwareVertexArrayNode
{
	friend class MC_TextureManager;

public:
	DWORD			textureIndex;
	DWORD			flags;					//Marks texture render state and terrain or not, etc.
	uint32_t		numShapes;				//Number of vertices this texture will be used to draw this frame.
	TG_RenderShape	*currentShape;			//CurrentVertex data being added.
	TG_RenderShape	*shapes;				//Pointer into the vertex Pool for this texture to draw.

	void init(void)
	{
		flags = 0;
		numShapes = 0;
		currentShape = 0;
		shapes = nullptr;
		textureIndex = 0;
	}

	void destroy(void) {};							//Frees all blocks, free GOS_TextureHandle, blank all data.

}; using MC_HardwareVertexArrayNode = _MC_HardwareVertexArrayNode;

//----------------------------------------------------------------------
// Per-texture submission links. Source payloads belong to TextureService.
struct MC_RenderTextureBin {
    MC_VertexArrayNode *vertexData=nullptr, *vertexData2=nullptr, *vertexData3=nullptr;
    MC_HardwareVertexArrayNode *hardwareVertexData=nullptr, *hardwareVertexData2=nullptr, *hardwareVertexData3=nullptr;
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

//---------------------------------------------------------------------------
template<typename T>
class gos_RenderShapeManager
{
	//Data Members
	//-------------
protected:

	void* storage = nullptr;
	long						count;		//Total number of vertices in pool.
	long						current;	//Pointer to next available vertex in pool.

public:

	void init(void)
	{
		storage = nullptr;

		count = 0;
		current = 0;
	}

	gos_RenderShapeManager(void)
	{
		init();
	}

	void destroy(void)
	{
		std::free(storage);
		storage = nullptr;
		reset();
		count = 0;
	}

	~gos_RenderShapeManager(void)
	{
		destroy();
	}

	void init(long maxShapes)
	{
		count = maxShapes;
		DWORD heapSize = count * sizeof(T);
		std::free(storage);
		storage = std::calloc(1, heapSize);
		gosASSERT(storage != nullptr);
		reset();
	}

	T *getBlock(uint32_t num)
	{
		// TXMMGR-BOUNDS-HARDEN-1: check capacity BEFORE advancing the cursor
		// (see gos_VERTEXManager::getVertexBlock). Fail safe on exhaustion.
		if (current + static_cast<long>(num) > count)
		{
			g_txmmgr_block_exhausted.fetch_add(1, std::memory_order_relaxed);
			if (g_txmmgrBoundsTrace)
				fprintf(stderr, "[TXMMGR_BOUNDS v1] block_exhausted need=%u cur=%ld total=%ld\n",
						num, current, count);
			return nullptr;
		}
		T *start = reinterpret_cast<T*>(storage);
		start = &(start[current]);
		current += num;
		return start;
	}

	void reset(void)
	{
		current = 0;
	}
};

struct TG_HWLightsData;

//----------------------------------------------------------------------
class MC_TextureManager
{
	
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

		MC_HardwareVertexArrayNode 		*masterHardwareVertexNodes;			//Dynamically allocated from an MC Heap.
		size_t							nextAvailableHardwareVertexNode;	//index to next available hardware vertex Node

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
													
		
		static gos_VERTEXManager		*gvManager;					//Stores arrays of vertices for draw.
		static gos_RenderShapeManager<TG_RenderShape>	*rsManager;					//Stores arrays of shapes for draw.


		
		WORD							*indexArray;				//Master Vertex Index array.

		// Owners of the node arrays and the vertex index table. The raw pointers named above are views into them: valid from
		// start() until destroy(), and the stores are sized once in start(), never grown while a view is held.
		std::vector<MC_VertexArrayNode>			masterVertexNodeStore;
		std::vector<MC_HardwareVertexArrayNode>	masterHardwareVertexNodeStore;
		std::vector<WORD>						indexArrayStore;

																	//Upto four different kinds of untextured triangle!
		MC_VertexArrayNode 				*vertexData;				//This holds the vertex draw data for UNTEXTURED triangles!
		MC_VertexArrayNode				*vertexData2;				//This holds the vertex draw data for UNTEXTURED triangles!
		MC_VertexArrayNode 				*vertexData3;				//This holds the vertex draw data for UNTEXTURED triangles!
		MC_VertexArrayNode				*vertexData4;				//This holds the vertex draw data for UNTEXTURED triangles!
		MC_VertexArrayNode				*vertexData5;				//This holds the vertex draw data for UNTEXTURED triangles!

		MC_HardwareVertexArrayNode 		*hardwareVertexData;		//This holds the vertex draw data for UNTEXTURED triangles!
		MC_HardwareVertexArrayNode 		*hardwareVertexData2;		//This holds the vertex draw data for UNTEXTURED triangles!
		MC_HardwareVertexArrayNode 		*hardwareVertexData3;		//This holds the vertex draw data for UNTEXTURED triangles!
		MC_HardwareVertexArrayNode 		*hardwareVertexData4;		//This holds the vertex draw data for UNTEXTURED triangles!
		MC_HardwareVertexArrayNode 		*hardwareVertexData5;		//This holds the vertex draw data for UNTEXTURED triangles!

        std::vector<TG_HWLightsData>    lightData_;               // CPU mirror of the light SSBO; size() == lightDataStructuresCapacity
        uint32_t                        lightDataStructuresCapacity;
        uint32_t                        lightDataStructuresCount;
		// [LIGHTSSBO v1] lightDataBuffer_ removed: LightsData is now an
		// SSBO owned by gameos_graphics.cpp (gos_LightDataSsbo_*).
		
	//Member Functions
	//-----------------
	public:



		void init (void)
		{

			indexArray = nullptr;
			
			masterVertexNodes = nullptr;
			nextAvailableVertexNode = 0;

			masterHardwareVertexNodes = nullptr;
			nextAvailableHardwareVertexNode = 0;
			
			vertexData = vertexData2 = vertexData3 = vertexData4 = vertexData5 = nullptr;
			hardwareVertexData = hardwareVertexData2 = hardwareVertexData3 = hardwareVertexData4 = hardwareVertexData5 = nullptr;

            lightDataStructuresCapacity = 0;
            lightDataStructuresCount = 0;

		}

		MC_TextureManager (void)
		{
			init();

		}

		void destroy (void);

		~MC_TextureManager (void);





		void start (void);					// This function creates the Blank Nodes from an MC Heap.

		//-----------------------------------------------------------------------------
		// Returns whether or not loadTexture() would return a handle to a texture instance
		// that was already loaded in memory. This function was motivated by the fact that
		// that an existing texture instance can been modified in memory after it's loaded, and
		// thus be different the from an instance that would be loaded from disk.

		//-----------------------------------------------------------------------------
		// Returns the TextureNode Id based on what you asked for.
		//
		// uniqueInstance is an ID for the instance of the texture. If its value matches that of
		// an already existing instance of the texture, the handle of the existing instance will
		// be returned. Used for Mech Coloration possibly, damage states, etc.


		//-----------------------------------------------------------------------------
		// Returns the TextureNode Id based on what you asked for.

		// increments the ref count

		//------------------------------------------------------
		// Tosses ALL of the textureNodes and frees GOS Handles
		void flush (bool justTextures=false);

		//------------------------------------------------------
		// Frees up gos_VERTEX manager memory
		void freeVertices (void);

		//------------------------------------------------------
		// Frees up TG_RenderShape manager memory
		void freeShapes (void);
		
		//------------------------------------------------------
		// Creates gos_VERTEX Manager and allocates RAM.  Will not allocate if already done!
		void startVertices (long maxVertices = 30000);

		//------------------------------------------------------
		// Creates TG_RenderShape Manager and allocates RAM.  Will not allocate if already done!
		void startShapes (uint32_t maxShapes = 3000);
		
		//------------------------------------------------------
		// Gives back one reference to a texture node (the count loadTexture / textureFromMemory took for the caller). The node, its GOS
		// texture and its RAM copy are freed when the last reference goes. This is the release behind mc2::TextureRef.

		//------------------------------------------------------
		// DEV-SHELL texture_refresh: re-read the source file from disk for
		// every named node whose nodeName contains `substring`
		// (case-insensitive), replace the system-RAM copy, and destroy the
		// resident gos texture so the next get_gosTextureHandle() re-uploads
		// the fresh pixels. Node INDEX is stable (widgets keep their ids).
		// FST-only textures (no loose disk file) are skipped and counted in
		// skippedNoFile. Returns number of nodes refreshed.

		
        void resetLightData();

        // [LIGHTBAKE v2] persistent static-light table: write the baked
        // constant into the permanent CPU slot lightData_[recipeIndex]
        // (grow-preserving) + advance the static high-water S. Called
        // once per recipe at bake / invalidate re-bake, NOT per frame.
        void bakeStaticLightSlot(int32_t recipeIndex, const struct TG_HWLightsData& baked);

        // Diagnostic accessors for the static-prop registry's flush trace:
        // read-only views of the per-frame light dedup table.
        // LightSlotPeek includes the first light's type (lightDir[0].w) and
        // color (lightColor[0].rgb) so registry::flush() can identify the
        // H2 hypothesis: slot has numLights=1 but lightColor=(0,0,0) → black.
        struct LightSlotPeek {
            int   numLights;    // -1 if out of range
            int   firstType;    // -1 if numLights==0; else int(lightDir[0].w)
            float firstColorR;
            float firstColorG;
            float firstColorB;
        };
        uint32_t getLightStructCount() const { return lightDataStructuresCount; }
        LightSlotPeek peekLightSlot(uint32_t idx) const;
        // [LIGHTBAKE-PROOF v1] copy a light slot's full struct (parity trace).
        // Returns false if idx is beyond the allocated count.
        bool copyLightSlot(uint32_t idx, TG_HWLightsData& out) const;
        // VK-MECH-FEED-NATIVE-1: read-only view of the CPU LightsData array
        // (the byte source of the GL SSBO upload). Defined in txmmgr.cpp
        // (needs complete TG_HWLightsData for sizeof).
        const void* getLightDataPtr() const;
        uint64_t    getLightDataBytes() const;

 		//-----------------------------------------------------------------
		// Gets gosTextureHandle for Node ID.  Does all caching necessary.








		void addRenderShape(DWORD nodeId, DWORD flags)
		{
			std::lock_guard<std::recursive_mutex> _lk(m_vertexNodeMu);  // protects nextAvailableHardwareVertexNode + pointer writes
			if ((nodeId < mc2::textures::kTextureAssetLimit) && (nextAvailableHardwareVertexNode < MC_MAX_RENDER_NODES))
			{
                auto& textureNode = textureBin(nodeId);
				if (!textureNode.hardwareVertexData)
				{
					textureNode.hardwareVertexData = &(masterHardwareVertexNodes[nextAvailableHardwareVertexNode]);
					gosASSERT(textureNode.hardwareVertexData->numShapes == 0);
					gosASSERT(textureNode.hardwareVertexData->shapes == nullptr);

					nextAvailableHardwareVertexNode++;
					textureNode.hardwareVertexData->flags = flags;
					textureNode.hardwareVertexData->textureIndex = nodeId;
				}
				else if (textureNode.hardwareVertexData &&
					(textureNode.hardwareVertexData->flags != flags) &&
					!textureNode.hardwareVertexData2)
				{
					textureNode.hardwareVertexData2 = &(masterHardwareVertexNodes[nextAvailableHardwareVertexNode]);
					gosASSERT(textureNode.hardwareVertexData2->numShapes == 0);
					gosASSERT(textureNode.hardwareVertexData2->shapes == nullptr);

					nextAvailableHardwareVertexNode++;
					textureNode.hardwareVertexData2->flags = flags;
					textureNode.hardwareVertexData2->textureIndex = nodeId;
				}
				// TXMMGR-HARDWARE-FLAGS-FIX-1: was textureNode.vertexData
				// (the SOFTWARE gos_VERTEX node) — a cross-wire into the hardware ladder.
				// The 3rd-slot presence guard must test the hardware node (always non-null
				// here, created in the first-slot branch above); the software node is
				// unrelated and mis-gated 3rd-slot creation, under-counting the reserve.
				else if (textureNode.hardwareVertexData &&
					(textureNode.hardwareVertexData->flags != flags) &&
					textureNode.hardwareVertexData2 &&
					(textureNode.hardwareVertexData2->flags != flags) &&
					!textureNode.hardwareVertexData3)
				{
					textureNode.hardwareVertexData3 = &(masterHardwareVertexNodes[nextAvailableHardwareVertexNode]);
					gosASSERT(textureNode.hardwareVertexData3->numShapes == 0);
					gosASSERT(textureNode.hardwareVertexData3->shapes == nullptr);

					nextAvailableHardwareVertexNode++;
					textureNode.hardwareVertexData3->flags = flags;
					textureNode.hardwareVertexData3->textureIndex = nodeId;
				}

				if (textureNode.hardwareVertexData->flags == flags)
					textureNode.hardwareVertexData->numShapes += 1;
				else if (textureNode.hardwareVertexData2 &&
					textureNode.hardwareVertexData2->flags == flags)
					textureNode.hardwareVertexData2->numShapes += 1;
				else if (textureNode.hardwareVertexData3 &&
					textureNode.hardwareVertexData3->flags == flags)
					textureNode.hardwareVertexData3->numShapes += 1;
#ifdef _DEBUG
				else
					STOP(("Could not AddTriangles.  No flags match vertex data"));
#endif
			}
			else if (nextAvailableHardwareVertexNode < MC_MAX_RENDER_NODES)
			{
				//Add this one to the untextured vertexBuffers
				if (!hardwareVertexData)
				{
					hardwareVertexData = &(masterHardwareVertexNodes[nextAvailableHardwareVertexNode]);
					gosASSERT(hardwareVertexData->numShapes == 0);
					gosASSERT(hardwareVertexData->shapes == nullptr);

					nextAvailableHardwareVertexNode++;
					hardwareVertexData->flags = flags;
					hardwareVertexData->textureIndex = 0;
				}
				else if (hardwareVertexData && (hardwareVertexData->flags != flags) &&
					!hardwareVertexData2)
				{
					hardwareVertexData2 = &(masterHardwareVertexNodes[nextAvailableHardwareVertexNode]);
					gosASSERT(hardwareVertexData2->numShapes == 0);
					gosASSERT(hardwareVertexData2->shapes == nullptr);

					nextAvailableHardwareVertexNode++;
					hardwareVertexData2->flags = flags;
					hardwareVertexData2->textureIndex = 0;
				}
				else if (hardwareVertexData && (hardwareVertexData->flags != flags) &&
					hardwareVertexData2 && (hardwareVertexData2->flags != flags) &&
					!hardwareVertexData3)
				{
					hardwareVertexData3 = &(masterHardwareVertexNodes[nextAvailableHardwareVertexNode]);
					gosASSERT(hardwareVertexData3->numShapes == 0);
					gosASSERT(hardwareVertexData3->shapes == nullptr);

					nextAvailableHardwareVertexNode++;
					hardwareVertexData3->flags = flags;
					hardwareVertexData3->textureIndex = 0;
				}
				else if (hardwareVertexData && (hardwareVertexData->flags != flags) &&
					hardwareVertexData2 && (hardwareVertexData2->flags != flags) &&
					hardwareVertexData3 && (hardwareVertexData3->flags != flags) &&
					!hardwareVertexData4)
				{
					hardwareVertexData4 = &(masterHardwareVertexNodes[nextAvailableHardwareVertexNode]);
					gosASSERT(hardwareVertexData4->numShapes == 0);
					gosASSERT(hardwareVertexData4->shapes == nullptr);

					nextAvailableHardwareVertexNode++;
					hardwareVertexData4->flags = flags;
					hardwareVertexData4->textureIndex = 0;
				}
				else if (hardwareVertexData && (hardwareVertexData->flags != flags) &&
					hardwareVertexData2 && (hardwareVertexData2->flags != flags) &&
					hardwareVertexData3 && (hardwareVertexData3->flags != flags) &&
					hardwareVertexData4 && (hardwareVertexData4->flags != flags) &&
					!hardwareVertexData5)
				{
					hardwareVertexData5 = &(masterHardwareVertexNodes[nextAvailableHardwareVertexNode]);
					gosASSERT(hardwareVertexData5->numShapes == 0);
					gosASSERT(hardwareVertexData5->shapes == nullptr);

					nextAvailableHardwareVertexNode++;
					hardwareVertexData5->flags = flags;
					hardwareVertexData5->textureIndex = 0;
				}

				if (hardwareVertexData->flags == flags)
					hardwareVertexData->numShapes += 1;
				else if (hardwareVertexData2 && hardwareVertexData2->flags == flags)
					hardwareVertexData2->numShapes += 1;
				else if (hardwareVertexData3 && hardwareVertexData3->flags == flags)
					hardwareVertexData3->numShapes += 1;
				else if (hardwareVertexData4 && hardwareVertexData4->flags == flags)
					hardwareVertexData4->numShapes += 1;
				else if (hardwareVertexData5 && hardwareVertexData5->flags == flags)
					hardwareVertexData5->numShapes += 1;
#ifdef _DEBUG
				else
					PAUSE(("Could not AddRenderShape.  Too many untextured render shapes"));
#endif
			}
		}

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

		void addRenderShape(DWORD nodeId, TG_RenderShape* render_shape, DWORD flags);

        // returns index at which this light structure was added
        // ( if same one was found, then that index is returned and nothing is added)
        uint32_t addLightDataStructure(TG_HWLightsData* lightData);
        uint32_t addLightDataStructureWithPerActorColor(TG_HWLightsData* lightData);

		void clearArrays (void)
		{
            for (auto& bin : textureBins_) bin.second = MC_RenderTextureBin{};

			vertexData = vertexData2 = vertexData3 = vertexData4 = vertexData5 = nullptr;
			hardwareVertexData = hardwareVertexData2 = hardwareVertexData3 = hardwareVertexData4 = hardwareVertexData5 = nullptr;

			memset(masterVertexNodes,0,sizeof(MC_VertexArrayNode)*MC_MAX_RENDER_NODES);
			memset(masterHardwareVertexNodes,0,sizeof(MC_HardwareVertexArrayNode)*MC_MAX_RENDER_NODES);
			
			nextAvailableVertexNode = 0;
			nextAvailableHardwareVertexNode = 0;

			gvManager->reset();
			rsManager->reset();

            resetLightData();
		}
		
		//Sends down the triangle lists
		void renderLists (void);

	private:
		// renderLists phases, in execution order (txmmgr_render.cpp).
		void renderListsPreamble (void);
		void renderListsLightDataUpload (void);
		void renderListsSceneDataUpload (void);
		void renderListsObjects3D (void);
		void renderListsPostObjectsStateRestore (void);
		void renderListsStaticShadowBuild (void);
		void renderListsStaticPropRegistryFlush (void);
		void renderListsDynamicShadowPass (void);
		void renderListsTerrainSolid (void);
		void renderListsGpuStaticProps (void);
		void renderListsOverlayBatches (void);
		void renderListsOverlaysWaterLoops (void);
		void renderListsOverlaysNoUnderlayer (void);
		void renderListsOverlaysShadowBlobs (void);
		void renderListsOverlaysNonTerrainAlpha (void);
		void renderListsOverlaysVfxHudSubmit (void);
		void renderListsOverlaysOld (void);
		// VK legacy-shape island queue / run (defined only under MC2_VULKAN_ISLAND).
		void vkLegacySubmitNode (long i, uint32_t pass, uint32_t alphaTest);
		void vkLegacyRun (void);
	public:


};

void GatherLightsParameters(TG_HWLightsData* lights);

//----------------------------------------------------------------------
// LIGHT-ROW-PURE-1 (ARCH-LIGHTING slice 1) — declare the per-class light-row
// policy instead of leaving it implicit in the order of mutations.
//
// An appearance class calls this immediately beside the legacy
// eye->setLightColor()/->active mutation it is about to perform, naming the
// SAME effect declaratively, and calls mc2ClearObjectLightPolicy() once its
// gather is done. `classPolicy` is an object_light_row::ClassPolicy value
// (mclib/object_light_row.h); it is passed as uint32_t so this header stays
// free of that include.
//
// Publishing is INERT on its own: it mutates no light, changes no row and
// moves no pixel. It is consumed only by the pure producer behind
// MC2_LIGHT_ROW_PURE, which reproduces the mutation exactly, and by
// MC2_LIGHT_ROW_PURE_VERIFY, which continuously checks that what a class
// DECLARES still matches what it DOES.
//----------------------------------------------------------------------
void mc2PublishObjectLightPolicy(uint32_t classPolicy,
                                 bool sunRecolour, uint32_t sunARGB,
                                 bool sunDisabled,
                                 bool ambientRecolour, uint32_t ambientARGB,
                                 float posX, float posY, float posZ,
                                 float terrainSample);
void mc2ClearObjectLightPolicy(void);

// RENDERER-INPUT-READBACK-1: the probe wrapper around the function above is
// declared in mclib/tgl.h, not here -- its signature needs TG_LightPtr, whose
// underlying TG_Light is an anonymous-struct typedef (tgl.h:300) and so cannot
// be forward-declared in this header.

//----------------------------------------------------------------------
extern MC_TextureManager *mcTextureManager;

// Shadow shape collection — populated during TG_Shape::Render(), consumed in renderLists()
void addShadowShape(HGOSBUFFER vb, HGOSBUFFER ib, HGOSVERTEXDECLARATION vdecl, const float* worldEntries16);

// CP-1: per-mission reset of the static terrain shadow priming flag.
// Call alongside gos_ResetStaticShadowPriming() at the per-mission init
// chokepoint so each mission re-primes its static terrain shadow.
void mc_ResetTerrainShadowPrimed();

//----------------------------------------------------------------------
#endif
