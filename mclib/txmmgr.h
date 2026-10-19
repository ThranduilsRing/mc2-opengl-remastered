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

#ifndef DSTD_H
#include"dstd.h"
#endif

#ifndef HEAP_H
#include"heap.h"
#endif
#include<vector>
#include"tgl.h"   // TG_HWLightsData is stored by value in the manager
#include<string.h>
#include<gameos.hpp>
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
enum MC_TextureKey
{
	MC_INVALIDTEXTUREKEY = 0,
	MC_Solid,
	MC_Keyed,
	MC_Alpha
};

//----------------------------------------------------------------------
//Can store up to 1024 8x8 textures in 1 256x256.
#define MC_MAXTEXTURES				4096
inline constexpr unsigned int CACHED_OUT_HANDLE = 0xFFFFFACE;		//If this value is in gosTextureHandle, texture is cached out and must be cached in.
inline constexpr int MAX_MC2_GOS_TEXTURES = 3000;
#define TEXTURE_CACHE_SIZE			(128*1024*1024)	//Extra space here to facilitate editting (was 40MB; bumped for 2048^2 imported-mech TGAs)
#define MAX_CACHE_SIZE				(128*1024*1024)	//Actual amount map must run in! (was 40MB)
inline constexpr int MC_MAXFACES = 16000000;		// MODEL-OVERRIDE: sized for override forests (was 200000, CPU-era cap)
// Holds one whole uncompressed TGA in flight. 2048x2048 RGBA = 16MB, so 8MB
// overran on imported-mech textures (loadTexture memcpy READ violation). 32MB
// covers 2048^2 with headroom; stock textures (<=512^2) unaffected.
#define MAX_LZ_BUFFER_SIZE			((1024*1024*32) + 1024)

inline constexpr int MC2_ISTERRAIN = 1;
inline constexpr int MC2_DRAWSOLID = 2;
#define MC2_DRAWALPHA				4
inline constexpr int MC2_ISSHADOWS = 8;
inline constexpr int MC2_ISEFFECTS = 16;
inline constexpr int MC2_DRAWONEIN = 32;
inline constexpr int MC2_ISCOMPASS = 128;
inline constexpr int MC2_ISSPOTLGT = 256;
inline constexpr int MC2_ISHUDLMNT = 512;
inline constexpr int MC2_ALPHATEST = 1024;
inline constexpr int MC2_ISWATER = 2048;
inline constexpr int MC2_ISWATERDETAIL = 4096;
#define MC2_GPUOVERLAY				8192

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
struct MC_TextureNode
{
	friend class MC_TextureManager;

	protected:
		DWORD				gosTextureHandle;			//Handle returned by GOS
		
	public:
		char 				*nodeName;					//Used for Unique nodes so I can just return the handle!
		DWORD				uniqueInstance;				//Texture is modifiable.  DO NOT CACHE OUT!!!!!!
		DWORD				neverFLUSH;					//Textures used by Userinput, etc.  DO NOT CACHE OUT!!!!!!
		DWORD				pinRefCount;				//Registry-driven pin (refcount).  Excludes node from cacheout when > 0.
														//See docs/superpowers/specs/2026-05-06-static-prop-texture-pin-fix.md
		DWORD				numUsers;					//Pushed up for each user using.
														//Users can "free" a texture which will decrement the number and actually free it if number is 0
		gos_TextureFormat 	key;						//Used to recreate texture if cached out.
		DWORD 				hints;						//Used to recreate texture if cached out.
		DWORD				width;						//Used to recreate texture if cached out.
		DWORD				lzCompSize;					//Size of Compressed version.
		DWORD				uvScale;					//UV scale factor (4 for 4x upscaled overrides, 1 otherwise)
		DWORD				logicalWidth;				//Post-uvScale width for callers that only need metadata.
		DWORD				logicalHeight;				//Post-uvScale height for callers that only need metadata.
		long				lastUsed;					//Last Game turn texture was used.  Used to cache textures.
		DWORD 				*textureData;				//Raw texture data.  Texture is stored here in system RAM 
														//if we overrun the max number of GOS HAndles.
														//When the texture is needed, another least used GOS handle is
														//cached out and this one is copied in.
														//This means that all instances where the game refers
														//To gosTextureHandle must be replaced with a get_gosTextureHandle
														//Or texture may not be cached in and it will draw the texture
														//at this location instead.  Just like some video cards do!
														//This value can be NULL if texture is on card.  
														//Should only be alloced when we need to cache.
														//This will keep system memory usage to a minimum.
		MC_VertexArrayNode 	*vertexData;				//This holds the vertex draw data.  NULL if not used.
		MC_VertexArrayNode	*vertexData2;
		MC_VertexArrayNode	*vertexData3;

		MC_HardwareVertexArrayNode	*hardwareVertexData;
		MC_HardwareVertexArrayNode	*hardwareVertexData2;
		MC_HardwareVertexArrayNode	*hardwareVertexData3;

	void init (void)
	{
		gosTextureHandle = 0xffffffff;
		numUsers = 0;
		nodeName = nullptr;
		textureData = nullptr;
		lastUsed = -1;							//NEVER been used.
		key = gos_Texture_Solid;
		hints = gosHint_DisableMipmap;
		width = 0;
		uniqueInstance = 0x0;
		neverFLUSH = false;
		pinRefCount = 0;
		vertexData = nullptr;
		vertexData2 = nullptr;
		vertexData3 = nullptr;
		lzCompSize = 0xffffffff;
		uvScale = 1;
		logicalWidth = 0;
		logicalHeight = 0;

		hardwareVertexData = nullptr;
		hardwareVertexData2 = nullptr;
		hardwareVertexData3 = nullptr;
	}

	DWORD findFirstAvailableBlock (void);

	void destroy (void);							//Frees all blocks, free GOS_TextureHandle, blank all data.

	void removeBlock (DWORD blockNum);				//Just free one block.  DO NOT FREE GOS_TextureHandle.

	void markBlock (DWORD blockNum);
	
	DWORD get_gosTextureHandle (void);				//If texture is not in VidRAM, cache a texture out and cache this one in.

};

//---------------------------------------------------------------------------
class gos_VERTEXManager : public HeapManager
{
	//Data Members
	//-------------
	protected:
	
		long						totalVertices;		//Total number of vertices in pool.
		long						currentVertex;		//Pointer to next available vertex in pool.

	//Member Functions
	//-----------------
	public:

		void init (void)
		{
			HeapManager::init();

			totalVertices = 0;
			currentVertex = 0;
		}

		gos_VERTEXManager (void) : HeapManager()
		{
			init();
		}

		void destroy (void)
		{
			HeapManager::destroy();
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
			createHeap(heapSize);
			commitHeap();
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
			gos_VERTEX *start = reinterpret_cast<gos_VERTEX *>(getHeapPtr());
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
class gos_RenderShapeManager : public HeapManager
{
	//Data Members
	//-------------
protected:

	long						count;		//Total number of vertices in pool.
	long						current;	//Pointer to next available vertex in pool.

public:

	void init(void)
	{
		HeapManager::init();

		count = 0;
		current = 0;
	}

	gos_RenderShapeManager(void) : HeapManager()
	{
		init();
	}

	void destroy(void)
	{
		HeapManager::destroy();
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
		createHeap(heapSize);
		commitHeap();
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
		T *start = reinterpret_cast<T*>(getHeapPtr());
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

struct TG_HWSceneData {
    float fog_start;
    float fog_end;
    float min_haze_dist;
    float dist_factor;
    float cam_pos[4];
    float fog_color[4];
    vec4 baseVertexColor;
};

using TG_HWSceneDataPtr = TG_HWSceneData*;

//----------------------------------------------------------------------
class MC_TextureManager
{
	friend struct MC_TextureNode;
	
	//Data Members
	//------------
	protected:

		MC_TextureNode					*masterTextureNodes;		//Dynamically allocated from an MC Heap.
		long							currentUsedTextures;		//Number of textures on video card.
		long							peakUsedTextures;			//Mission high-water mark for currentUsedTextures.
													
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
													
		UserHeapPtr						textureCacheHeap;			//Heap used to cache textures from vidCard to system RAM.
		UserHeapPtr						textureStringHeap;			//Heap used to store filenames of textures so no dupes.
		bool 							textureManagerInstrumented;	//Texture Manager Instrumented.
		long							totalCacheMisses;			//NUmber of times flush has been called.
		
		static gos_VERTEXManager		*gvManager;					//Stores arrays of vertices for draw.
		static gos_RenderShapeManager<TG_RenderShape>	*rsManager;					//Stores arrays of shapes for draw.

		static MemoryPtr				lzBuffer1;					//Used to compress/decompress textures from cache.
		static MemoryPtr				lzBuffer2;					//Used to compress/decompress textures from cache.
		/* iBufferRefCount is used to help determine if lzBuffer1&2 are valid. The assumption
		is that if there are no valid MC_TextureManagers then lzBuffer1&2 are not valid. */
		static int				iBufferRefCount;
		
		WORD							*indexArray;				//Master Vertex Index array.

		// Owners of the node arrays and the vertex index table. The raw pointers named above are views into them: valid from
		// start() until destroy(), and the stores are sized once in start(), never grown while a view is held.
		std::vector<MC_TextureNode>				masterTextureNodeStore;
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
        TG_HWSceneData*                 sceneData_;
		gosBuffer						*sceneDataBuffer_;
		
	//Member Functions
	//-----------------
	public:

		long getPeakUsedTextures (void) const { return peakUsedTextures; }

		void init (void)
		{
			masterTextureNodes = nullptr;

			textureCacheHeap = nullptr;
			textureStringHeap = nullptr;
			textureManagerInstrumented = false;
			totalCacheMisses = 0;
			currentUsedTextures = 0;
			peakUsedTextures = 0;
			indexArray = nullptr;
			
			masterVertexNodes = nullptr;
			nextAvailableVertexNode = 0;

			masterHardwareVertexNodes = nullptr;
			nextAvailableHardwareVertexNode = 0;
			
			vertexData = vertexData2 = vertexData3 = vertexData4 = vertexData5 = nullptr;
			hardwareVertexData = hardwareVertexData2 = hardwareVertexData3 = hardwareVertexData4 = hardwareVertexData5 = nullptr;

            lightDataStructuresCapacity = 0;
            lightDataStructuresCount = 0;

			sceneData_ = nullptr;
            sceneDataBuffer_ = nullptr;
		}

		MC_TextureManager (void)
		{
			init();

			MC_TextureManager::iBufferRefCount++;
		}

		void destroy (void);

		~MC_TextureManager (void);

		DWORD getUVScale(DWORD nodeId) {
			if (nodeId < MC_MAXTEXTURES)
				return masterTextureNodes[nodeId].uvScale;
			return 1;
		}

		bool tryGetTextureLogicalSize(DWORD nodeId, DWORD& logicalWidth, DWORD& logicalHeight) const
		{
			if (nodeId < MC_MAXTEXTURES)
			{
				const MC_TextureNode& node = masterTextureNodes[nodeId];
				if (node.logicalWidth && node.logicalHeight)
				{
					logicalWidth = node.logicalWidth;
					logicalHeight = node.logicalHeight;
					return true;
				}
			}

			logicalWidth = 0;
			logicalHeight = 0;
			return false;
		}

		void start (void);					// This function creates the Blank Nodes from an MC Heap.

		//-----------------------------------------------------------------------------
		// Returns whether or not loadTexture() would return a handle to a texture instance
		// that was already loaded in memory. This function was motivated by the fact that
		// that an existing texture instance can been modified in memory after it's loaded, and
		// thus be different the from an instance that would be loaded from disk.
		DWORD textureInstanceExists (const char *textureFullPathName, gos_TextureFormat key, DWORD hints, DWORD uniqueInstance = 0x0, DWORD nFlush = 0x0);

		//-----------------------------------------------------------------------------
		// Returns the TextureNode Id based on what you asked for.
		//
		// uniqueInstance is an ID for the instance of the texture. If its value matches that of
		// an already existing instance of the texture, the handle of the existing instance will
		// be returned. Used for Mech Coloration possibly, damage states, etc.
		DWORD loadTexture (const char *textureFullPathName, gos_TextureFormat key, DWORD hints, DWORD uniqueInstance = 0x0, DWORD nFlush = 0x0);

		long saveTexture (DWORD textureIndex, const char *textureFullPathName);

		//-----------------------------------------------------------------------------
		// Returns the TextureNode Id based on what you asked for.
		DWORD textureFromMemory (DWORD *data, gos_TextureFormat key, DWORD hints, DWORD width, DWORD bitDepth = 4);
		DWORD textureFromMemoryRaw (DWORD *data, gos_TextureFormat key, DWORD hints, DWORD width, DWORD bitDepth = 4);

		// increments the ref count
		DWORD copyTexture( DWORD texNodeID );

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
		void releaseNode (DWORD textureNode);

		//------------------------------------------------------
		// DEV-SHELL texture_refresh: re-read the source file from disk for
		// every named node whose nodeName contains `substring`
		// (case-insensitive), replace the system-RAM copy, and destroy the
		// resident gos texture so the next get_gosTextureHandle() re-uploads
		// the fresh pixels. Node INDEX is stable (widgets keep their ids).
		// FST-only textures (no loose disk file) are skipped and counted in
		// skippedNoFile. Returns number of nodes refreshed.
		long refreshTexturesByName (const char *substring, long &skippedNoFile);

		
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
		DWORD get_gosTextureHandle (DWORD nodeId)
		{
			if (nodeId != 0xffffffff)
				return masterTextureNodes[nodeId].get_gosTextureHandle();
			else
				return nodeId;
		}

        const char* getTextureName(DWORD nodeId) const
        {
            if (nodeId < MC_MAXTEXTURES)
                return masterTextureNodes[nodeId].nodeName;
            return nullptr;
        }

		bool isTextureNodeResident (DWORD nodeId) const
		{
			if ((nodeId == 0xffffffff) || (nodeId == 0))
				return true;

			return masterTextureNodes[nodeId].gosTextureHandle != CACHED_OUT_HANDLE &&
				masterTextureNodes[nodeId].gosTextureHandle != 0xffffffff &&
				masterTextureNodes[nodeId].gosTextureHandle != 0;
		}

		void setTextureNeverFlush (DWORD nodeId, DWORD nFlush)
		{
			if (nodeId < MC_MAXTEXTURES)
				masterTextureNodes[nodeId].neverFLUSH = nFlush;
		}

		void addRenderShape(DWORD nodeId, DWORD flags)
		{
			std::lock_guard<std::recursive_mutex> _lk(m_vertexNodeMu);  // protects nextAvailableHardwareVertexNode + pointer writes
			if ((nodeId < MC_MAXTEXTURES) && (nextAvailableHardwareVertexNode < MC_MAXTEXTURES))
			{
				if (!masterTextureNodes[nodeId].hardwareVertexData)
				{
					masterTextureNodes[nodeId].hardwareVertexData = &(masterHardwareVertexNodes[nextAvailableHardwareVertexNode]);
					gosASSERT(masterTextureNodes[nodeId].hardwareVertexData->numShapes == 0);
					gosASSERT(masterTextureNodes[nodeId].hardwareVertexData->shapes == nullptr);

					nextAvailableHardwareVertexNode++;
					masterTextureNodes[nodeId].hardwareVertexData->flags = flags;
					masterTextureNodes[nodeId].hardwareVertexData->textureIndex = nodeId;
				}
				else if (masterTextureNodes[nodeId].hardwareVertexData &&
					(masterTextureNodes[nodeId].hardwareVertexData->flags != flags) &&
					!masterTextureNodes[nodeId].hardwareVertexData2)
				{
					masterTextureNodes[nodeId].hardwareVertexData2 = &(masterHardwareVertexNodes[nextAvailableHardwareVertexNode]);
					gosASSERT(masterTextureNodes[nodeId].hardwareVertexData2->numShapes == 0);
					gosASSERT(masterTextureNodes[nodeId].hardwareVertexData2->shapes == nullptr);

					nextAvailableHardwareVertexNode++;
					masterTextureNodes[nodeId].hardwareVertexData2->flags = flags;
					masterTextureNodes[nodeId].hardwareVertexData2->textureIndex = nodeId;
				}
				// TXMMGR-HARDWARE-FLAGS-FIX-1: was masterTextureNodes[nodeId].vertexData
				// (the SOFTWARE gos_VERTEX node) — a cross-wire into the hardware ladder.
				// The 3rd-slot presence guard must test the hardware node (always non-null
				// here, created in the first-slot branch above); the software node is
				// unrelated and mis-gated 3rd-slot creation, under-counting the reserve.
				else if (masterTextureNodes[nodeId].hardwareVertexData &&
					(masterTextureNodes[nodeId].hardwareVertexData->flags != flags) &&
					masterTextureNodes[nodeId].hardwareVertexData2 &&
					(masterTextureNodes[nodeId].hardwareVertexData2->flags != flags) &&
					!masterTextureNodes[nodeId].hardwareVertexData3)
				{
					masterTextureNodes[nodeId].hardwareVertexData3 = &(masterHardwareVertexNodes[nextAvailableHardwareVertexNode]);
					gosASSERT(masterTextureNodes[nodeId].hardwareVertexData3->numShapes == 0);
					gosASSERT(masterTextureNodes[nodeId].hardwareVertexData3->shapes == nullptr);

					nextAvailableHardwareVertexNode++;
					masterTextureNodes[nodeId].hardwareVertexData3->flags = flags;
					masterTextureNodes[nodeId].hardwareVertexData3->textureIndex = nodeId;
				}

				if (masterTextureNodes[nodeId].hardwareVertexData->flags == flags)
					masterTextureNodes[nodeId].hardwareVertexData->numShapes += 1;
				else if (masterTextureNodes[nodeId].hardwareVertexData2 &&
					masterTextureNodes[nodeId].hardwareVertexData2->flags == flags)
					masterTextureNodes[nodeId].hardwareVertexData2->numShapes += 1;
				else if (masterTextureNodes[nodeId].hardwareVertexData3 &&
					masterTextureNodes[nodeId].hardwareVertexData3->flags == flags)
					masterTextureNodes[nodeId].hardwareVertexData3->numShapes += 1;
#ifdef _DEBUG
				else
					STOP(("Could not AddTriangles.  No flags match vertex data"));
#endif
			}
			else if (nextAvailableHardwareVertexNode < MC_MAXTEXTURES)
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
			if ((nodeId < MC_MAXTEXTURES) && (nextAvailableVertexNode < MC_MAXTEXTURES))
			{
				if (!masterTextureNodes[nodeId].vertexData)
				{
					masterTextureNodes[nodeId].vertexData = &(masterVertexNodes[nextAvailableVertexNode]);
					gosASSERT(masterTextureNodes[nodeId].vertexData->numVertices == 0);
					gosASSERT(masterTextureNodes[nodeId].vertexData->vertices == nullptr);

					nextAvailableVertexNode++;
					masterTextureNodes[nodeId].vertexData->flags = flags;
					masterTextureNodes[nodeId].vertexData->textureIndex = nodeId;
				}
				else if (masterTextureNodes[nodeId].vertexData &&
					(masterTextureNodes[nodeId].vertexData->flags != flags) &&
					!masterTextureNodes[nodeId].vertexData2)
				{
					masterTextureNodes[nodeId].vertexData2 = &(masterVertexNodes[nextAvailableVertexNode]);
					gosASSERT(masterTextureNodes[nodeId].vertexData2->numVertices == 0);
					gosASSERT(masterTextureNodes[nodeId].vertexData2->vertices == nullptr);

					nextAvailableVertexNode++;
					masterTextureNodes[nodeId].vertexData2->flags = flags;
					masterTextureNodes[nodeId].vertexData2->textureIndex = nodeId;
				}
				else if (masterTextureNodes[nodeId].vertexData &&
						(masterTextureNodes[nodeId].vertexData->flags != flags) &&
						masterTextureNodes[nodeId].vertexData2 &&
						(masterTextureNodes[nodeId].vertexData2->flags != flags) &&
						!masterTextureNodes[nodeId].vertexData3)
				{
					masterTextureNodes[nodeId].vertexData3 = &(masterVertexNodes[nextAvailableVertexNode]);
					gosASSERT(masterTextureNodes[nodeId].vertexData3->numVertices == 0);
					gosASSERT(masterTextureNodes[nodeId].vertexData3->vertices == nullptr);

					nextAvailableVertexNode++;
					masterTextureNodes[nodeId].vertexData3->flags = flags;
					masterTextureNodes[nodeId].vertexData3->textureIndex = nodeId;
				}
					
				if (masterTextureNodes[nodeId].vertexData->flags == flags)
					masterTextureNodes[nodeId].vertexData->numVertices += 3;
				else if (masterTextureNodes[nodeId].vertexData2 &&
						masterTextureNodes[nodeId].vertexData2->flags == flags)
					masterTextureNodes[nodeId].vertexData2->numVertices += 3;
				else if (masterTextureNodes[nodeId].vertexData3 &&
						masterTextureNodes[nodeId].vertexData3->flags == flags)
					masterTextureNodes[nodeId].vertexData3->numVertices += 3;
#ifdef _DEBUG
				else
					STOP(("Could not AddTriangles.  No flags match vertex data"));
#endif
			}
			else if (nextAvailableVertexNode < MC_MAXTEXTURES)
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

			if ((nodeId < MC_MAXTEXTURES) && (nextAvailableVertexNode < MC_MAXTEXTURES))
			{
				if (!masterTextureNodes[nodeId].vertexData)
				{
					masterTextureNodes[nodeId].vertexData = &(masterVertexNodes[nextAvailableVertexNode]);
					gosASSERT(masterTextureNodes[nodeId].vertexData->numVertices == 0);
					gosASSERT(masterTextureNodes[nodeId].vertexData->vertices == nullptr);

					nextAvailableVertexNode++;
					masterTextureNodes[nodeId].vertexData->flags = flags;
					masterTextureNodes[nodeId].vertexData->textureIndex = nodeId;
				}
				else if (masterTextureNodes[nodeId].vertexData &&
					(masterTextureNodes[nodeId].vertexData->flags != flags) &&
					!masterTextureNodes[nodeId].vertexData2)
				{
					masterTextureNodes[nodeId].vertexData2 = &(masterVertexNodes[nextAvailableVertexNode]);
					gosASSERT(masterTextureNodes[nodeId].vertexData2->numVertices == 0);
					gosASSERT(masterTextureNodes[nodeId].vertexData2->vertices == nullptr);

					nextAvailableVertexNode++;
					masterTextureNodes[nodeId].vertexData2->flags = flags;
					masterTextureNodes[nodeId].vertexData2->textureIndex = nodeId;
				}
				else if (masterTextureNodes[nodeId].vertexData &&
						(masterTextureNodes[nodeId].vertexData->flags != flags) &&
						masterTextureNodes[nodeId].vertexData2 &&
						(masterTextureNodes[nodeId].vertexData2->flags != flags) &&
						!masterTextureNodes[nodeId].vertexData3)
				{
					masterTextureNodes[nodeId].vertexData3 = &(masterVertexNodes[nextAvailableVertexNode]);
					gosASSERT(masterTextureNodes[nodeId].vertexData3->numVertices == 0);
					gosASSERT(masterTextureNodes[nodeId].vertexData3->vertices == nullptr);

					nextAvailableVertexNode++;
					masterTextureNodes[nodeId].vertexData3->flags = flags;
					masterTextureNodes[nodeId].vertexData3->textureIndex = nodeId;
				}

				const int delta = 3 * triCount;
				if (masterTextureNodes[nodeId].vertexData->flags == flags)
					masterTextureNodes[nodeId].vertexData->numVertices += delta;
				else if (masterTextureNodes[nodeId].vertexData2 &&
						masterTextureNodes[nodeId].vertexData2->flags == flags)
					masterTextureNodes[nodeId].vertexData2->numVertices += delta;
				else if (masterTextureNodes[nodeId].vertexData3 &&
						masterTextureNodes[nodeId].vertexData3->flags == flags)
					masterTextureNodes[nodeId].vertexData3->numVertices += delta;
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
			if (nodeId < MC_MAXTEXTURES)
			{
				if (masterTextureNodes[nodeId].vertexData && 
					masterTextureNodes[nodeId].vertexData->flags == flags)
				{
					gos_VERTEX * vertices = masterTextureNodes[nodeId].vertexData->currentVertex;
					if (!vertices && !masterTextureNodes[nodeId].vertexData->vertices)
					{
						masterTextureNodes[nodeId].vertexData->currentVertex =
						vertices = 
						masterTextureNodes[nodeId].vertexData->vertices =
						gvManager->getVertexBlock(masterTextureNodes[nodeId].vertexData->numVertices);
					}
	
					if (vertices && vertices < (masterTextureNodes[nodeId].vertexData->vertices + masterTextureNodes[nodeId].vertexData->numVertices))
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
					
					masterTextureNodes[nodeId].vertexData->currentVertex = vertices;
				}
				else if (masterTextureNodes[nodeId].vertexData2 &&
						 masterTextureNodes[nodeId].vertexData2->flags == flags)
				{
					gos_VERTEX * vertices = masterTextureNodes[nodeId].vertexData2->currentVertex;
					
                    //sebi: looks like assert may happen if more vertices added than was calculated on stage when addTriange was called. As one can see in (*) first time we go here, we allocate enough memory for all potential vertices, but if it is not enough this assert will trigger
					#if defined( _DEBUG) || defined(_ARMOR)
					gos_VERTEX * oldVertices = vertices;
					gos_VERTEX * oldStart = (masterTextureNodes[nodeId].vertexData2->vertices + masterTextureNodes[nodeId].vertexData2->numVertices);
					#endif
					gosASSERT(oldVertices < oldStart);

                    // (*)
					if (!vertices && !masterTextureNodes[nodeId].vertexData2->vertices)
					{
						masterTextureNodes[nodeId].vertexData2->currentVertex =
						vertices = 
						masterTextureNodes[nodeId].vertexData2->vertices =
						gvManager->getVertexBlock(masterTextureNodes[nodeId].vertexData2->numVertices);
					}
	
					if (vertices && vertices < (masterTextureNodes[nodeId].vertexData2->vertices + masterTextureNodes[nodeId].vertexData2->numVertices))
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
					
					masterTextureNodes[nodeId].vertexData2->currentVertex = vertices;
				}
				else if (masterTextureNodes[nodeId].vertexData3 &&
						 masterTextureNodes[nodeId].vertexData3->flags == flags)
				{
					gos_VERTEX * vertices = masterTextureNodes[nodeId].vertexData3->currentVertex;
					
					#if defined(_DEBUG) || defined(_ARMOR)
					gos_VERTEX * oldVertices = vertices;
					gos_VERTEX * oldStart = (masterTextureNodes[nodeId].vertexData3->vertices + masterTextureNodes[nodeId].vertexData3->numVertices);
					#endif
					gosASSERT(oldVertices < oldStart);

					if (!vertices && !masterTextureNodes[nodeId].vertexData3->vertices)
					{
						masterTextureNodes[nodeId].vertexData3->currentVertex =
						vertices = 
						masterTextureNodes[nodeId].vertexData3->vertices =
						gvManager->getVertexBlock(masterTextureNodes[nodeId].vertexData3->numVertices);
					}
	
					if (vertices && vertices < (masterTextureNodes[nodeId].vertexData3->vertices + masterTextureNodes[nodeId].vertexData3->numVertices))
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
					
					masterTextureNodes[nodeId].vertexData3->currentVertex = vertices;
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
			for (long i=0;i<MC_MAXTEXTURES;i++)
			{
				masterTextureNodes[i].vertexData = nullptr;
				masterTextureNodes[i].vertexData2 = nullptr;
				masterTextureNodes[i].vertexData3 = nullptr;

				masterTextureNodes[i].hardwareVertexData = nullptr;
				masterTextureNodes[i].hardwareVertexData2 = nullptr;
				masterTextureNodes[i].hardwareVertexData3 = nullptr;
			}

			vertexData = vertexData2 = vertexData3 = vertexData4 = vertexData5 = nullptr;
			hardwareVertexData = hardwareVertexData2 = hardwareVertexData3 = hardwareVertexData4 = hardwareVertexData5 = nullptr;

			memset(masterVertexNodes,0,sizeof(MC_VertexArrayNode)*MC_MAXTEXTURES);
			memset(masterHardwareVertexNodes,0,sizeof(MC_HardwareVertexArrayNode)*MC_MAXTEXTURES);
			
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
		struct RenderListsFrame;
		void renderListsPreamble (void);
		void renderListsLightDataUpload (void);
		void renderListsSceneDataUpload (void);
		void renderListsObjects3D (void);
		void renderListsPostObjectsStateRestore (void);
		void renderListsStaticShadowBuild (void);
		void renderListsStaticPropRegistryFlush (void);
		void renderListsDynamicShadowPass (void);
		void renderListsTerrainSolid (const RenderListsFrame& st);
		void renderListsGpuStaticProps (void);
		void renderListsOverlayBatches (void);
		void renderListsOverlaysWaterLoops (void);
		void renderListsOverlaysNoUnderlayer (void);
		void renderListsOverlaysShadowBlobs (const RenderListsFrame& st);
		void renderListsOverlaysNonTerrainAlpha (const RenderListsFrame& st);
		void renderListsOverlaysVfxHudSubmit (const RenderListsFrame& st);
		void renderListsOverlaysOld (const RenderListsFrame& st);
		// VK legacy-shape island queue / run (defined only under MC2_VULKAN_ISLAND).
		void vkLegacySubmitNode (long i, uint32_t pass, uint32_t alphaTest);
		void vkLegacyRun (void);
	public:

		DWORD getWidth( DWORD nodeId )
		{
			if (nodeId != 0xffffffff)
				return masterTextureNodes[nodeId].width;
			else
				return nodeId;
		}
		
		//----------------------------------------------------------------------------
		// FlushCache copies a single flushable texture from VidRAM to system Memory
		// No real logic yet, just find the first unused texture and swap it.
		// DO NOT want to implement a priority system at all.
		// Returns FALSE if no texture can be cached out this frame. 
		bool flushCache (void);
		
		//-----------------------------------------------------------------------
		// This routine will run through the TXM Cache on a regular basis and free
		// up GOS Handles which haven't been used in some time.  Some Time TBD
		// more accurately with time.
		DWORD update (void);

		//-----------------------------------------------------------------------
		// Registry-driven texture pinning.  Excludes the master node at nodeIdx
		// from cacheout while pinRefCount > 0.  Refcounted so multiple registrants
		// can share the same node (static-prop typeIDs share underlying textures —
		// confirmed by trace).  See
		// docs/superpowers/specs/2026-05-06-static-prop-texture-pin-fix.md
		//
		// pinNode  asserts nodeIdx < MC_MAXTEXTURES AND numUsers > 0 (orphan-pin
		//          guard — slot must have a live texture allocation).
		// unpinNode asserts pinRefCount > 0 before decrement.
		void  pinNode    (DWORD nodeIdx);
		void  unpinNode  (DWORD nodeIdx);
		DWORD getPinCount(DWORD nodeIdx) const;

		bool checkCacheHeap (void)
		{
			if (textureCacheHeap->totalCoreLeft() <= (TEXTURE_CACHE_SIZE - MAX_CACHE_SIZE))
			{
				return false;
			}
			
			return true;
		}
		
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
