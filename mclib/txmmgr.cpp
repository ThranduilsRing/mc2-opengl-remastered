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
#include "../RenderCore/TextureService.h"
#include "../GameOS/gameos/utils/mc2log.h"

// NS3 boundary: effectStream belongs to the texture/effect subsystem, not
// to whatever game/tool main links it. Previously redefined in every main.
Stuff::MemoryStream *effectStream = nullptr;
static std::unique_ptr<Stuff::MemoryStream> s_effectStreamOwner;

// static globals
MC_TextureManager *mcTextureManager = nullptr;
gos_VERTEXManager *MC_TextureManager::gvManager = nullptr;
gos_RenderShapeManager<TG_RenderShape> *MC_TextureManager::rsManager = nullptr;
static std::unique_ptr<gos_VERTEXManager> s_gvManagerOwner;
static std::unique_ptr<gos_RenderShapeManager<TG_RenderShape>> s_rsManagerOwner;
MemoryPtr			MC_TextureManager::lzBuffer1 = nullptr;
MemoryPtr			MC_TextureManager::lzBuffer2 = nullptr;
int				MC_TextureManager::iBufferRefCount = 0;

bool MLRVertexLimitReached = false;

// CP-1: file-scope so a per-mission hook can re-prime the static terrain shadow
// accumulation for the new mission.  Previously a function-local static inside
// renderLists(); promoted here so mc_ResetTerrainShadowPrimed() can clear it.
static bool s_terrainShadowPrimed = false;
void mc_ResetTerrainShadowPrimed() { s_terrainShadowPrimed = false; }

// MC2_TEX_LIFECYCLE_TRACE=1 — diagnostic for the static-prop black-billboard bug
// under MC2_STATIC_UPDATE_SKIP=1. Logs lifecycle event types under a single
// schema (also emitted by msl.cpp, gos_static_prop_batcher.cpp,
// gos_static_prop_registry.cpp):
//   event=evict           — per cacheOut at MC_TextureManager::update / flushCache
//   event=evict_skipped   — per pinRefCount > 0 block at the four eviction sites
//   event=update_summary  — per call to MC_TextureManager::update
//   event=recache_multi   — per call to TG_TypeMultiShape::SetTextureHandle (msl.cpp)
//   event=draw_black      — per static-prop draw with invalid handle (batcher)
//   event=pin / event=unpin / event=pin_summary — registry-side pin lifecycle
// Cross-reference logs by `nodeIdx` to identify nodes that are evicted but never
// re-cached. See docs/superpowers/specs/2026-05-06-static-prop-texture-pin-fix.md
static const bool s_texLifecycleTrace = mc2::gate::get<"MC2_TEX_LIFECYCLE_TRACE">();

#define TEX_LC(fmt, ...) \
    do { if (s_texLifecycleTrace) { printf("[TEX_LIFECYCLE v1] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)

// [TXMMGR_TEXTURE_AUDIT v1] — texture-node-slot leak instrumentation.
// Gate: MC2_TXM_LEAK_TRACE=1. Default OFF = byte-identical (no allocs, no emit).
// Goal: name the texture classes that accumulate node slots across mission
// unloads (the 4096 masterTextureNodes[] cap STOP at textureFromMemory /
// loadTexture / textureFromMemoryRaw). Emitted from flush() (the per-mission
// unload boundary, see Mission::destroy -> mcTextureManager->flush()).
//   [TXMMGR_TEXTURE_AUDIT] one line per mission unload with slot census.
//   [TXMMGR_FLUSH]         keep/free decision counts around the flush loop.
//   [TXMMGR_SURVIVOR]      top-10 surviving node-name prefixes after the flush.
// from_memory / anonymous heuristic: textureFromMemory + textureFromMemoryRaw
// both set nodeName=NULL; loadTexture always sets a real name. So a node with
// nodeName==NULL/empty is either a from-memory texture or the reserved empty
// node[0]. We report both buckets off the same NULL-name test.
const bool s_txmLeakTrace = mc2::gate::get<"MC2_TXM_LEAK_TRACE">();
static long s_txmMissionIndex      = 0;   // ++ per mission unload (flush)
long s_txmAllocThisMission  = 0;   // nodes created since last unload boundary

// Group a node name into a coarse leak key: take dir prefix (up to last
// separator) else the name with trailing digits stripped, so e.g.
// "data/tgl/128/foo01.tga" -> "data/tgl/128/", "cursor07" -> "cursor".
static void txmLeakKeyForName(const char* name, char* out, size_t outSz)
{
    if (!name || !name[0]) { snprintf(out, outSz, "<anon/from_memory>"); return; }
    const char* lastSep = nullptr;
    for (const char* p = name; *p; ++p)
        if (*p == '/' || *p == '\\') lastSep = p;
    if (lastSep) {
        size_t n = static_cast<size_t>((lastSep - name)) + 1;
        if (n >= outSz) n = outSz - 1;
        memcpy(out, name, n);
        out[n] = 0;
        return;
    }
    // No dir: copy, then strip trailing digits.
    size_t len = strlen(name);
    if (len >= outSz) len = outSz - 1;
    memcpy(out, name, len);
    out[len] = 0;
    while (len > 0 && out[len-1] >= '0' && out[len-1] <= '9') out[--len] = 0;
    if (len == 0) snprintf(out, outSz, "<numeric>");
}

#define EVICT_SKIPPED(nodeIdx, refcount, site)                                          \
    do {                                                                                \
        if (s_evictSkipShouldEmit(static_cast<long>((nodeIdx)), turn)) {                             \
            printf("[TEX_LIFECYCLE v1] event=evict_skipped reason=pinned "              \
                   "nodeIdx=%ld pinRefCount=%lu site=%s turn=%ld\n",                    \
                   static_cast<long>((nodeIdx)), static_cast<unsigned long>((refcount)), (site), static_cast<long>(turn));     \
            fflush(stdout);                                                             \
        }                                                                               \
    } while (0)

// Shared per-process dedup for event=evict_skipped across the four eviction
// sites (one in MC_TextureManager::update, three in flushCache). Without
// dedup, ~thousands of pinned nodes × 60Hz eviction sweeps = unsustainable
// log volume. Key buckets per ~64-turn window so the same skip emits at
// most once per minute-ish.
static inline bool s_evictSkipShouldEmit(long nodeIdx, long curTurn) {
    if (!s_texLifecycleTrace) return false;
    static thread_local std::unordered_set<uint64_t> s_dedup;
    const uint64_t key = (uint64_t(curTurn >> 6) << 32) | uint64_t(uint32_t(nodeIdx));
    return s_dedup.insert(key).second;
}

// Frees up gos_VERTEX manager memory
void MC_TextureManager::freeVertices(void)
{
	if (gvManager)
	{
		gvManager->destroy();
		gvManager = nullptr;
		s_gvManagerOwner.reset();
	}
}

void MC_TextureManager::freeShapes(void)
{
	if (rsManager)
	{
		rsManager->destroy();
		rsManager = nullptr;
		s_rsManagerOwner.reset();
	}
}
		
// Creates gos_VERTEX Manager and allocates RAM.  Will not allocate if already done!
void MC_TextureManager::startVertices (long maxVertices)
{
	if (gvManager == nullptr)
	{
		s_gvManagerOwner = std::make_unique<gos_VERTEXManager>();
		gvManager = s_gvManagerOwner.get();
		gvManager->init(maxVertices);
		gvManager->reset();
	}
}

void MC_TextureManager::startShapes(uint32_t maxShapes)
{
	if (rsManager == nullptr)
	{
		s_rsManagerOwner = std::make_unique<gos_RenderShapeManager<TG_RenderShape>>();
		rsManager = s_rsManagerOwner.get();
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
	masterTextureNodeStore.assign(MC_MAXTEXTURES, MC_TextureNode{});
	masterTextureNodes = masterTextureNodeStore.data();

	for (long i=0;i<MC_MAXTEXTURES;i++)
		masterTextureNodes[i].init();
		
	masterVertexNodeStore.assign(MC_MAXTEXTURES, MC_VertexArrayNode{});
	masterVertexNodes = masterVertexNodeStore.data();

	masterHardwareVertexNodeStore.assign(MC_MAXTEXTURES, MC_HardwareVertexArrayNode{});
	masterHardwareVertexNodes = masterHardwareVertexNodeStore.data();

	

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
	
	indexArrayStore.resize(MC_MAXFACES);
	indexArray = indexArrayStore.data();
	for (int i=0;i<MC_MAXFACES;i++)
		indexArray[i] = i;
		
	//Add an Empty Texture node for all untextured triangles to go down into.
	masterTextureNodes[0].gosTextureHandle = 0;
	masterTextureNodes[0].nodeName = nullptr;
	masterTextureNodes[0].uniqueInstance = false;
	masterTextureNodes[0].neverFLUSH = 0x1;
	masterTextureNodes[0].numUsers = 0;
	masterTextureNodes[0].key = gos_Texture_Solid;
	masterTextureNodes[0].hints = 0; 
	masterTextureNodes[0].width = 0;
	masterTextureNodes[0].logicalWidth = 0;
	masterTextureNodes[0].logicalHeight = 0;
	masterTextureNodes[0].lastUsed = -1;
	masterTextureNodes[0].textureData = nullptr;

    lightDataStructuresCapacity = 128;
    lightDataStructuresCount = 0;
    lightData_.assign(lightDataStructuresCapacity, TG_HWLightsData());
	// [LIGHTSSBO v1] EAGER create+bind here (not lazy). The old UBO was
	// created in this constructor so SSBO/UBO binding always had a valid
	// buffer for EVERY consumer regardless of frame phase — notably the
	// GPU static-prop/mech batcher, which reads LightsData via explicit
	// layout(binding=20) and runs in a different phase than the txmmgr
	// per-frame upload site. Lazy-create broke that lifetime invariant
	// (batcher drew before first upload -> binding 20 empty -> black
	// props). Allocate-once/update-many is the correct lifetime model.
	gos_LightDataSsbo_Upload(
		lightData_.data(),
		static_cast<size_t>(lightDataStructuresCapacity) * sizeof(TG_HWLightsData));


	initTexResolveTable();
}

extern Stuff::MemoryStream *effectStream;
extern MidLevelRenderer::MLRClipper * theClipper;
void MC_TextureManager::destroy (void)
{
	if (masterTextureNodes)
	{
		// Traverses list of texture nodes and frees each one.
		long usedCount = 0;
		for (long i=0;i<MC_MAXTEXTURES;i++)
			masterTextureNodes[i].destroy();		// Destroy for nodes whacks GOS Handle
		
		currentUsedTextures = usedCount;			//Can this have been the damned bug all along!?
	}
	
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

	// Release the node storage; every view into it ends here.
	masterTextureNodes = nullptr;
	masterVertexNodes = nullptr;
	masterHardwareVertexNodes = nullptr;
	indexArray = nullptr;
	std::vector<MC_TextureNode>().swap(masterTextureNodeStore);
	std::vector<MC_VertexArrayNode>().swap(masterVertexNodeStore);
	std::vector<MC_HardwareVertexArrayNode>().swap(masterHardwareVertexNodeStore);
	std::vector<WORD>().swap(indexArrayStore);
	


	gos_LightDataSsbo_Destroy();  // [LIGHTSSBO v1]

    std::vector<TG_HWLightsData>().swap(lightData_);

}

MC_TextureManager::~MC_TextureManager (void)
{
	MC_TextureManager::iBufferRefCount--;
	if (0 == MC_TextureManager::iBufferRefCount)
	{
		if (lzBuffer1)
		{
			gosASSERT(lzBuffer2 != nullptr);

			{
				std::free(lzBuffer1);
				std::free(lzBuffer2);
			}
			lzBuffer1 = nullptr;
			lzBuffer2 = nullptr;
		}
	}

	destroy();
}

void MC_TextureManager::flush (bool justTextures)
{
	if (masterTextureNodes)
	{
		{
			char _cbbuf[128];
			snprintf(_cbbuf, sizeof(_cbbuf),
				"[TXM v1] event=mission_unload peak_textures=%ld/%d",
				peakUsedTextures, MAX_MC2_GOS_TEXTURES);
			MC2_DIAG_PUTS(_cbbuf);
			crashbundle_append(_cbbuf);
			fflush(stdout);
		}
		peakUsedTextures = 0;
#ifdef MC2_VULKAN_ISLAND
		// VK-PARITY-F1: texture node indices are reused by the next mission; drop the
		// legacy-shape island's copy-once texture cache.
		mc2_vk_legacy_shape_island_reset();
		mc2_vk_overlay_reset();   // VK-PARITY-OVERLAY-1: gos texture handles are reused too
#endif

		// [TXMMGR_TEXTURE_AUDIT v1] pre-flush census (slot occupancy before free).
		long auditTotalUsed = 0, auditNeverflush = 0, auditFlushable = 0;
		long auditGosLive = 0, auditAnon = 0;
		if (s_txmLeakTrace)
		{
			for (long i=0;i<MC_MAXTEXTURES;i++)
			{
				const DWORD h = masterTextureNodes[i].gosTextureHandle;
				const bool occupied = (h != 0xffffffff);
				if (!occupied) continue;
				auditTotalUsed++;
				if (masterTextureNodes[i].neverFLUSH) auditNeverflush++; else auditFlushable++;
				if (h != 0xffffffff && h != CACHED_OUT_HANDLE) auditGosLive++;
				const char* nm = masterTextureNodes[i].nodeName;
				if (!nm || !nm[0]) auditAnon++;   // from_memory / empty node
			}
		}

		// Traverses list of texture nodes and frees each one.
		long usedCount = 0;
		// [TXMMGR_FLUSH v1] keep/free decision tally.
		long flushBeforeUsed = auditTotalUsed, flushFreed = 0;
		long flushKeptNeverflush = 0, flushKeptOther = 0;
		for (long i=0;i<MC_MAXTEXTURES;i++)
		{
			if (!masterTextureNodes[i].neverFLUSH)
			{
				if (s_txmLeakTrace && masterTextureNodes[i].gosTextureHandle != 0xffffffff)
					flushFreed++;
				masterTextureNodes[i].destroy();		// Destroy for nodes whacks GOS Handle
			}
			else if (s_txmLeakTrace && masterTextureNodes[i].gosTextureHandle != 0xffffffff)
			{
				flushKeptNeverflush++;
			}
		}

		currentUsedTextures = usedCount;				//Can this have been the damned bug all along!?

		// [TXMMGR_TEXTURE_AUDIT v1] post-flush survivor census + emit.
		if (s_txmLeakTrace)
		{
			// Survivor name-prefix histogram (bounded fixed table; no heap).
			struct { char key[96]; long count; } buckets[64];
			int nBuckets = 0;
			long afterUsed = 0;
			for (long i=0;i<MC_MAXTEXTURES;i++)
			{
				if (masterTextureNodes[i].gosTextureHandle == 0xffffffff) continue;
				afterUsed++;
				if (!masterTextureNodes[i].neverFLUSH) flushKeptOther++;  // BUG: survived flush
				char key[96];
				txmLeakKeyForName(masterTextureNodes[i].nodeName, key, sizeof(key));
				int b = -1;
				for (int k=0;k<nBuckets;k++)
					if (strcmp(buckets[k].key, key) == 0) { b = k; break; }
				if (b < 0 && nBuckets < 64) { b = nBuckets++; strncpy(buckets[b].key, key, sizeof(buckets[b].key)-1); buckets[b].key[sizeof(buckets[b].key)-1]=0; buckets[b].count = 0; }
				if (b >= 0) buckets[b].count++;
			}

			s_txmMissionIndex++;
			printf("[TXMMGR_TEXTURE_AUDIT] mission_index=%ld total_nodes_used=%ld neverflush_nodes=%ld flushable_nodes=%ld gos_handles_live=%ld allocated_this_mission=%ld released_on_flush=%ld survived_flush=%ld anonymous_nodes=%ld from_memory_nodes=%ld\n",
				s_txmMissionIndex, auditTotalUsed, auditNeverflush, auditFlushable,
				auditGosLive, s_txmAllocThisMission, flushFreed, afterUsed,
				auditAnon, auditAnon);
			printf("[TXMMGR_FLUSH] mission_index=%ld before_flush_used=%ld freed_count=%ld kept_neverflush_count=%ld kept_other_count=%ld after_flush_used=%ld\n",
				s_txmMissionIndex, flushBeforeUsed, flushFreed, flushKeptNeverflush, flushKeptOther, afterUsed);

			// top_survivors[10]: selection sort over the bounded bucket table.
			for (int rank=0; rank<10 && rank<nBuckets; rank++)
			{
				int best = rank;
				for (int k=rank+1;k<nBuckets;k++)
					if (buckets[k].count > buckets[best].count) best = k;
				if (best != rank) { auto t = buckets[rank]; buckets[rank] = buckets[best]; buckets[best] = t; }
				printf("[TXMMGR_SURVIVOR] mission_index=%ld rank=%d count=%ld key=%s\n",
					s_txmMissionIndex, rank, buckets[rank].count, buckets[rank].key);
			}
			fflush(stdout);

			// Reset per-mission alloc counter at the unload boundary.
			s_txmAllocThisMission = 0;
		}
	}
	
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
	
	s_effectStreamOwner = std::make_unique<Stuff::MemoryStream>(effectsData.data(),effectsSize);
	effectStream = s_effectStreamOwner.get();
	gosFX::EffectLibrary::Instance->Load(effectStream);
	
	gosFX::LightManager::Instance = new gosFX::LightManager();

	gos_PopCurrentHeap();
}

void MC_TextureManager::releaseNode (DWORD textureNode)
{
	if (textureNode >= MC_MAXTEXTURES)
	{
		MC2_LOG_ONCE(Render, Warn, "[TEXTURE_RELEASE_BAD] node={} (not a texture node index; ignored)", textureNode);
		return;
	}

	MC_TextureNode& node = masterTextureNodes[textureNode];
	if (node.numUsers == 0)
	{
		MC2_LOG_ONCE(Render, Warn, "[TEXTURE_RELEASE_BAD] node={} (released with no reference counted; ignored)", textureNode);
		return;
	}

	RenderCore::textureService().dropRef(textureNode);   // P4a: mirrors the reference count in the identity service
	if (--node.numUsers == 0)
		node.destroy();
}

void mc2::ManagerTextureRelease::operator() (std::uint32_t node) const noexcept
{
	mc2::releaseThrough(mcTextureManager, node);
}

constexpr int cache_Threshold = 150;
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
			const bool refPinned = masterTextureNodes[i].pinRefCount > 0;
			if (pinned || refPinned) poolPinned++;
			if (unique) poolUnique++;
			if (!pinned && !refPinned && !unique && masterTextureNodes[i].lastUsed <= (turn - cache_Threshold))
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
				if (masterTextureNodes[i].pinRefCount > 0) {
					EVICT_SKIPPED(i, masterTextureNodes[i].pinRefCount, "flushCache_cacheThreshold");
					continue;
				}
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
				if (masterTextureNodes[i].pinRefCount > 0) {
					EVICT_SKIPPED(i, masterTextureNodes[i].pinRefCount, "flushCache_turn30");
					continue;
				}
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
				if (masterTextureNodes[i].pinRefCount > 0) {
					EVICT_SKIPPED(i, masterTextureNodes[i].pinRefCount, "flushCache_turn1");
					continue;
				}
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

			if (shapes && shapes < (masterTextureNodes[nodeId].hardwareVertexData->shapes + masterTextureNodes[nodeId].hardwareVertexData->numShapes))
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

			if (shapes && shapes < (masterTextureNodes[nodeId].hardwareVertexData2->shapes + masterTextureNodes[nodeId].hardwareVertexData2->numShapes))
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

			if (shapes && shapes < (masterTextureNodes[nodeId].hardwareVertexData3->shapes + masterTextureNodes[nodeId].hardwareVertexData3->numShapes))
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
void MC_TextureManager::pinNode (DWORD nodeIdx)
{
	gosASSERT(nodeIdx < static_cast<DWORD>(MC_MAXTEXTURES));
	gosASSERT(masterTextureNodes[nodeIdx].numUsers > 0);
	masterTextureNodes[nodeIdx].pinRefCount++;
}

void MC_TextureManager::unpinNode (DWORD nodeIdx)
{
	gosASSERT(nodeIdx < static_cast<DWORD>(MC_MAXTEXTURES));
	gosASSERT(masterTextureNodes[nodeIdx].pinRefCount > 0);
	masterTextureNodes[nodeIdx].pinRefCount--;
}

DWORD MC_TextureManager::getPinCount (DWORD nodeIdx) const
{
	if (nodeIdx >= static_cast<DWORD>(MC_MAXTEXTURES)) return 0;
	return masterTextureNodes[nodeIdx].pinRefCount;
}

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
					if (masterTextureNodes[i].pinRefCount > 0) {
						EVICT_SKIPPED(i, masterTextureNodes[i].pinRefCount, "update_turn60");
					} else {
						// Cache this badboy out.  Textures don't change.  Just Destroy!
						{
							ZoneScopedN("MC_TextureManager::update cacheOut");
							if (masterTextureNodes[i].gosTextureHandle)
								gos_DestroyTexture(masterTextureNodes[i].gosTextureHandle);
						}

						TEX_LC("event=evict nodeIdx=%ld turn=%ld lastUsed=%ld gosHandle=0x%08x",
						       i, turn, masterTextureNodes[i].lastUsed,
						       masterTextureNodes[i].gosTextureHandle);

						masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;
						numTexturesFreed++;
					}
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

	if (s_texLifecycleTrace) {
		TEX_LC("event=update_summary turn=%ld evicted=%lu currentUsed=%ld",
		       turn, static_cast<unsigned long>(numTexturesFreed), static_cast<long>(currentUsedTextures));
	}

	return numTexturesFreed;
}

void MC_TextureNode::destroy (void)
{
	if ((gosTextureHandle != CACHED_OUT_HANDLE) && (gosTextureHandle != 0xffffffff) && (gosTextureHandle != 0x0))
	{
		gos_DestroyTexture(gosTextureHandle);
	}
	
	std::free(nodeName);
	std::free(textureData);
	RenderCore::textureService().onNodeDestroyed(static_cast<uint32_t>(this - mcTextureManager->masterTextureNodes));   // P4a: the node's handle goes stale
	init();
}
