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
#include "render_contract.h"  // [RENDER_PASS v1] noteRenderPass
#include"tex_resolve_table.h"
#include "diagnostic_trace.h"

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

#include"terrain.h"   // VPL-#shadow Phase 1+2: Terrain::mapData for the full-map static-shadow build

#ifndef PATHS_H
#include"paths.h"
#endif

#include<gameos.hpp>
#include<mlr/mlr.hpp>
#include<gosfx/gosfxheaders.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <atomic>   // FRAME-JOBS-2 precondition: g_numShadowShapes
#include <mutex>    // FRAME-JOBS-2 precondition: s_lightDataMapMu
#include <chrono>   // [LIGHTBRIDGE v1] coarse per-frame populate sizing
#include <intrin.h> // __rdtsc — [SPFLUSH_COST_SPLIT v1]
#include <utils/gl_utils.h>
#include <GL/glew.h>                 // TEXMGR-COMPRESSED-UPLOAD-1: BPTC cap + GL_COMPRESSED_* enums
#include "../RenderCore/KtxLoader.h" // TEXMGR-COMPRESSED-UPLOAD-1: BC7 .ktx2 sidecar loader
#include "gos_postprocess.h"
#include "gos_profiler.h"
#include "../GameOS/gameos/gos_static_prop_batcher.h"
#include "../GameOS/gameos/render_frame_plan.h"  // RENDER-FRAME-PLAN-SCAFFOLD-1
#include "../GameOS/gameos/render_snapshot.h"  // getLastRenderSnapshot() for snap-cull (v2.3)
#include "../GameOS/gameos/gos_static_prop_registry.h"  // Stage 3.C: flush()
#include "../GameOS/gameos/gos_mech_batcher.h"
#include "../GameOS/gameos/gos_validate.h"  // drainGLErrors (Tier-1 instr §4)
#include "../GameOS/gameos/gos_terrain_patch_stream.h"
#include "../GameOS/gameos/gos_terrain_indirect.h"
#include "../GameOS/gameos/gos_render_pass_timer.h"
#include "../GameOS/gameos/gos_terrain_bridge.h"   // [TERRAIN_SURFACE] PR-2 surface validation draw
#include "../RenderCore/terrain_path_telemetry.h"  // LEGACY-MLR-DELETE-1: tripwire noteTerrainPath(LegacyMLR)
#include "../RenderCore/top_level_pass_executor.h"  // APPLY-STATE-MECHOPAQUE-1: findTopLevelStateDesc
#include "../GameOS/gameos/gos_terrain_mask_dispatch.h"  // B4 Stage 1b: mask-SOLID draw
#include "../GameOS/gameos/gpu_cull_compute.h"  // C1b: compute_dispatch() moved here from mission.cpp
#include "../GameOS/gameos/gpu_cull_substrate.h"
#include "dynamic_decal_ring.h"  // MC2_DYNAMIC_DECALS gather before gos_DrawDecals

// NS3 boundary: effectStream belongs to the texture/effect subsystem, not
// to whatever game/tool main links it. Previously redefined in every main.
Stuff::MemoryStream *effectStream = NULL;

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
extern uint32_t g_mc2FrameCounter;

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
static const bool s_texLifecycleTrace =
    (getenv("MC2_TEX_LIFECYCLE_TRACE") != nullptr);

// T1.15 [SPOT_DIAG v1] pack-probe state (GatherLightsParameters).
// First-shape always-on (one stderr line on the first call after process start).
// Per-summary every 600 GatherLightsParameters calls when env=1. `calls=N`
// because GatherLightsParameters runs once per submitMultiShape (many per frame).
static const bool s_spotDiagPackEnabled = (getenv("MC2_SPOT_DIAG") != nullptr);
static bool          s_spotDiagPackFirstHit  = false;
static unsigned long s_spotDiagPackCalls     = 0;
static unsigned long s_spotDiagPackActiveSum = 0;
static unsigned long s_spotDiagPackInactSum  = 0;
static unsigned long s_spotDiagPackPointSum  = 0;

// LIGHT-CLAMP-RAISE-STAGE1-1 — small env helper (default OFF; "1" only).
static inline bool lightClampEnvFlagDefaultOff(const char* name) {
    const char* v = getenv(name);
    return v && v[0] == '1' && v[1] == '\0';
}

// MC2_LIGHT_CLAMP_PROBE (default OFF) — pure observation. Tracks the per-object
// HIGH-WATER populated light count across all GatherLightsParameters calls and
// emits a summary line. Answers "do stock missions ever exceed 16 lights/object?"
// Does NOT change behavior when ON.
static const bool    s_lightClampProbe        = lightClampEnvFlagDefaultOff("MC2_LIGHT_CLAMP_PROBE");
static uint32_t      s_lightClampProbeHighWater = 0;
static unsigned long s_lightClampProbeCalls     = 0;

// MC2_LIGHT_CLAMP_FIXTURE (default OFF) — synthetic injector of deterministic
// extra point lights into slots [populated..32) to prove slots >16 ever render.
// NEVER writes past ABI cap 32 (clamps injection to slot 31). NEVER changes
// stock behavior unless explicitly enabled. Yells a clear log line when active.
static const bool    s_lightClampFixture      = lightClampEnvFlagDefaultOff("MC2_LIGHT_CLAMP_FIXTURE");
static bool          s_lightClampFixtureYelled = false;

// ---------------------------------------------------------------------------
// [SPFLUSH_COST_SPLIT v1] — txmmgr-side RDTSC accumulator.
// Measures the batcher_prepareBaseInstanceTable() + bucket-upload span each
// frame. Gate mirrors the registry-side gate (same env var). The consume
// function is called by the registry summary emit (extern declaration there).
// ---------------------------------------------------------------------------
static const bool s_spflushTxmEnabled = []() {
    const char* v = getenv("MC2_STATIC_PROP_FLUSH_COST_SPLIT");
    return v && v[0] == '1' && v[1] == '\0';
}();
static unsigned long long s_spflush_baseinstance_upload_cyc = 0;
// recipe_rebuilds: monotonic + window delta. light_index_writes==recipe_rebuilds (same site).
static unsigned long long s_spflush_recipe_rebuilds_txm      = 0;
static unsigned long long s_spflush_recipe_rebuilds_total     = 0;

// Exposed via extern in gos_static_prop_registry.cpp summary emit.
unsigned long long spflush_ConsumeBaseInstanceUploadCycles() {
    const unsigned long long v = s_spflush_baseinstance_upload_cyc;
    s_spflush_baseinstance_upload_cyc = 0;
    return v;
}
// Returns the window delta and the lifetime total for recipe_rebuilds.
// Delta is reset on call; total is monotonic.
unsigned long long spflush_ConsumeRecipeRebuildsDelta() {
    const unsigned long long v = s_spflush_recipe_rebuilds_txm;
    s_spflush_recipe_rebuilds_total += v;
    s_spflush_recipe_rebuilds_txm    = 0;
    return v;
}
unsigned long long spflush_GetRecipeRebuildTotal() {
    return s_spflush_recipe_rebuilds_total;
}

// ---------------------------------------------------------------------------
// [RENDERLISTS_COST v1] — TXMMGR-PERF-EASYWINS-1 coarse per-phase CPU cost
// split of MC_TextureManager::renderLists(). Smoke-visible complement of the
// Tracy zones (smoke runs cannot take user-driven Tracy captures). Pattern =
// SPFLUSH_COST_SPLIT: env-gated, window-averaged, default OFF (zero overhead
// beyond one cached-bool test per phase when unset; ~2 steady_clock reads per
// phase per frame when set — coarse spans only, per the 100ns-floor rule).
// Enable: MC2_RENDERLISTS_COST_SPLIT=1 -> one [RENDERLISTS_COST v1] stderr
// summary every 60 frames: per-frame mean µs per phase + node-table sizes.
// self_us = total minus the sum of instrumented phases (unattributed gaps).
// ---------------------------------------------------------------------------
namespace rlcost {
static const bool s_enabled = []() {
    const char* v = getenv("MC2_RENDERLISTS_COST_SPLIT");
    return v && v[0] != '0';
}();
enum Phase {
    kPreamble = 0, kLightUpload, kSceneData, kObj3d, kStateRestore,
    kStaticShadowBuild, kSpRegistryFlush, kDynShadow, kTerrainSolid,
    kGpuSpPrep, kSpBatcherFlush, kMechFlush, kOverlays, kWaterLoops,
    kNoUnderlayer, kShadowBlobs, kAlphaLoops, kVfxHud, kTotal, kPhaseCount
};
static const char* kName[kPhaseCount] = {
    "preamble", "light_upload", "scene_data", "obj3d", "state_restore",
    "static_shadow_build", "sp_registry_flush", "dyn_shadow", "terrain_solid",
    "gpusp_prep_cull", "sp_batcher_flush", "mech_flush", "overlays",
    "water_loops", "no_underlayer", "shadow_blobs", "alpha_loops",
    "vfx_hud", "total"
};
static unsigned long long s_ns[kPhaseCount] = {};
static int s_frames = 0;

static inline unsigned long long nowNs() {
    return static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Lexical phase span. Zero-cost (two branch-predicted tests) when disabled.
struct Span {
    int p; unsigned long long t0;
    explicit Span(int phase) : p(phase), t0(s_enabled ? nowNs() : 0ULL) {}
    ~Span() { if (s_enabled) s_ns[p] += nowNs() - t0; }
};

// Whole-function span + window emit. Construct FIRST in renderLists() so the
// destructor runs LAST: accumulates total, then emits every 60 frames.
struct TotalSpan {
    unsigned long long t0; long nodes_; long hwNodes_;
    TotalSpan(long nodes, long hwNodes)
        : t0(s_enabled ? nowNs() : 0ULL), nodes_(nodes), hwNodes_(hwNodes) {}
    ~TotalSpan() {
        if (!s_enabled) return;
        s_ns[kTotal] += nowNs() - t0;
        if (++s_frames < 60) return;
        const double wf = static_cast<double>(s_frames);
        double phaseSumUs = 0.0;
        char line[1400]; int off = 0;
        off += snprintf(line + off, sizeof(line) - off,
            "[RENDERLISTS_COST v1] event=summary frames=%d nodes=%ld hw_nodes=%ld",
            s_frames, nodes_, hwNodes_);
        for (int p = 0; p < kPhaseCount; ++p) {
            const double us = static_cast<double>(s_ns[p]) / 1000.0 / wf;
            if (p != kTotal) phaseSumUs += us;
            if (off < static_cast<int>(sizeof(line)))
                off += snprintf(line + off, sizeof(line) - off,
                                " %s_us=%.1f", kName[p], us);
        }
        const double selfUs =
            static_cast<double>(s_ns[kTotal]) / 1000.0 / wf - phaseSumUs;
        if (off < static_cast<int>(sizeof(line)))
            snprintf(line + off, sizeof(line) - off, " self_us=%.1f", selfUs);
        fprintf(stderr, "%s\n", line);
        fflush(stderr);
        memset(s_ns, 0, sizeof(s_ns));
        s_frames = 0;
    }
};
} // namespace rlcost

// ---------------------------------------------------------------------------
// [TXMMGR_BOUNDS v1] — TXMMGR-BOUNDS-HARDEN-1 exhaustion counters.
// Declared extern in txmmgr.h so the inline pool allocators + addVertices paths
// can bump them. atomic+relaxed because addVertices/addRenderShape run on
// frame-jobs workers; the increment only happens on the rare overflow path.
// A teardown dump surfaces any nonzero counter even with the trace gate off, so
// silent pool exhaustion can no longer pass unnoticed.
// ---------------------------------------------------------------------------
bool g_txmmgrBoundsTrace =
    []() { const char* v = getenv("MC2_TXMMGR_BOUNDS_TRACE"); return v && v[0] == '1'; }();
std::atomic<unsigned long long> g_txmmgr_vertex_block_exhausted{0};
std::atomic<unsigned long long> g_txmmgr_block_exhausted{0};
std::atomic<unsigned long long> g_txmmgr_add_vertices_overflow_prevented{0};
std::atomic<unsigned long long> g_txmmgr_add_shape_overflow_prevented{0};
namespace {
struct TxmmgrBoundsDump {
    ~TxmmgrBoundsDump() {
        const unsigned long long a = g_txmmgr_vertex_block_exhausted.load();
        const unsigned long long b = g_txmmgr_block_exhausted.load();
        const unsigned long long c = g_txmmgr_add_vertices_overflow_prevented.load();
        const unsigned long long d = g_txmmgr_add_shape_overflow_prevented.load();
        if (a | b | c | d)
            fprintf(stderr, "[TXMMGR_BOUNDS v1] SUMMARY vertex_block_exhausted=%llu block_exhausted=%llu "
                    "add_vertices_overflow_prevented=%llu add_shape_overflow_prevented=%llu\n", a, b, c, d);
    }
};
TxmmgrBoundsDump s_txmmgrBoundsDump;
} // namespace

#define TEX_LC(fmt, ...) \
    do { if (s_texLifecycleTrace) { printf("[TEX_LIFECYCLE v1] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)

// ---------------------------------------------------------------------------
// [TXMMGR_TEXTURE_AUDIT v1] — texture-node-slot leak instrumentation.
// Gate: MC2_TXM_LEAK_TRACE=1. Default OFF = byte-identical (no allocs, no emit).
// Goal: name the texture classes that accumulate node slots across mission
// unloads (the 4096 masterTextureNodes[] cap STOP at textureFromMemory /
// loadTexture / textureFromMemoryRaw). Emitted from flush() (the per-mission
// unload boundary, see Mission::destroy -> mcTextureManager->flush()).
//
//   [TXMMGR_TEXTURE_AUDIT] one line per mission unload with slot census.
//   [TXMMGR_FLUSH]         keep/free decision counts around the flush loop.
//   [TXMMGR_SURVIVOR]      top-10 surviving node-name prefixes after the flush.
//
// from_memory / anonymous heuristic: textureFromMemory + textureFromMemoryRaw
// both set nodeName=NULL; loadTexture always sets a real name. So a node with
// nodeName==NULL/empty is either a from-memory texture or the reserved empty
// node[0]. We report both buckets off the same NULL-name test.
// ---------------------------------------------------------------------------
static const bool s_txmLeakTrace =
    []() { const char* v = getenv("MC2_TXM_LEAK_TRACE"); return v && v[0] == '1'; }();
static long s_txmMissionIndex      = 0;   // ++ per mission unload (flush)
static long s_txmAllocThisMission  = 0;   // nodes created since last unload boundary

// Bump on every fresh node-slot creation (the three creation paths).
static inline void txmLeakNoteAlloc()
{
    if (s_txmLeakTrace) s_txmAllocThisMission++;
}

// Group a node name into a coarse leak key: take dir prefix (up to last
// separator) else the name with trailing digits stripped, so e.g.
// "data/tgl/128/foo01.tga" -> "data/tgl/128/", "cursor07" -> "cursor".
static void txmLeakKeyForName(const char* name, char* out, size_t outSz)
{
    if (!name || !name[0]) { snprintf(out, outSz, "<anon/from_memory>"); return; }
    const char* lastSep = NULL;
    for (const char* p = name; *p; ++p)
        if (*p == '/' || *p == '\\') lastSep = p;
    if (lastSep) {
        size_t n = (size_t)(lastSep - name) + 1;
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
#define EVICT_SKIPPED(nodeIdx, refcount, site)                                          \
    do {                                                                                \
        if (s_evictSkipShouldEmit((long)(nodeIdx), turn)) {                             \
            printf("[TEX_LIFECYCLE v1] event=evict_skipped reason=pinned "              \
                   "nodeIdx=%ld pinRefCount=%lu site=%s turn=%ld\n",                    \
                   (long)(nodeIdx), (unsigned long)(refcount), (site), (long)turn);     \
            fflush(stdout);                                                             \
        }                                                                               \
    } while (0)

// --- Shadow shape collection (file-scope global, no struct layout impact) ---
struct ShadowShapeEntry {
	HGOSBUFFER vb;
	HGOSBUFFER ib;
	HGOSVERTEXDECLARATION vdecl;
	float worldMatrix[16];
};
static const int MAX_SHADOW_SHAPES = 512;
static ShadowShapeEntry g_shadowShapes[MAX_SHADOW_SHAPES];
// FRAME-JOBS-2 precondition: atomic so worker threads can reserve shadow
// shape slots without a mutex. ShadowShapeEntry fields are fully independent
// per-slot (vb/ib/vdecl/worldMatrix — no aliased pointers), so fetch_add
// reservation + per-slot write is race-free between different callers.
static std::atomic<int> g_numShadowShapes{0};

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
	// Atomic reservation: fetch_add reserves an exclusive slot index.
	// ShadowShapeEntry fields are per-slot independent, so two threads
	// writing different indices never alias.
	// Do NOT store MAX_SHADOW_SHAPES back on overflow — that store is racy
	// (concurrent fetch_adds can push count past MAX before the store lands,
	// and the store itself races with other fetch_adds). Instead let the
	// counter freely overflow and clamp at all read sites via
	//   std::min(g_numShadowShapes.load(), MAX_SHADOW_SHAPES).
	int idx = g_numShadowShapes.fetch_add(1, std::memory_order_relaxed);
	if (idx >= MAX_SHADOW_SHAPES) {
		return;
	}
	ShadowShapeEntry& ss = g_shadowShapes[idx];
	ss.vb = vb;
	ss.ib = ib;
	ss.vdecl = vdecl;
	memcpy(ss.worldMatrix, worldEntries16, 16 * sizeof(float));
}

void clearShadowShapes() {
	g_numShadowShapes.store(0, std::memory_order_relaxed);
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

	// [UI-PHASE1] carried from mc2r_ui_phase1 (base->theirs hunk @txmmgr.cpp:202+).
	// PNG check first: the data/defs UI art is .png, and interpreting the
	// PNG signature bytes as a TGA header yields garbage width/height.
	// PNG IHDR stores big-endian width/height at offsets 16 and 20.
	unsigned char sniff[24] = {0};
	if (textureFile.read((MemoryPtr)sniff, sizeof(sniff)) == sizeof(sniff) &&
		sniff[0] == 0x89 && sniff[1] == 'P' && sniff[2] == 'N' && sniff[3] == 'G')
	{
		textureFile.seek(0);
		const DWORD pngWidth = (static_cast<DWORD>(sniff[16]) << 24) |
							   (static_cast<DWORD>(sniff[17]) << 16) |
							   (static_cast<DWORD>(sniff[18]) << 8) |
							   static_cast<DWORD>(sniff[19]);
		const DWORD pngHeight = (static_cast<DWORD>(sniff[20]) << 24) |
								(static_cast<DWORD>(sniff[21]) << 16) |
								(static_cast<DWORD>(sniff[22]) << 8) |
								static_cast<DWORD>(sniff[23]);
		if (!pngWidth || !pngHeight)
			return false;
		// PNG UI art is authored 1:1; do not apply the TGA uvScale shrink.
		logicalWidth = pngWidth;
		logicalHeight = pngHeight;
		return true;
	}
	textureFile.seek(0);

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
	// [LIGHTSSBO v1] EAGER create+bind here (not lazy). The old UBO was
	// created in this constructor so SSBO/UBO binding always had a valid
	// buffer for EVERY consumer regardless of frame phase — notably the
	// GPU static-prop/mech batcher, which reads LightsData via explicit
	// layout(binding=20) and runs in a different phase than the txmmgr
	// per-frame upload site. Lazy-create broke that lifetime invariant
	// (batcher drew before first upload -> binding 20 empty -> black
	// props). Allocate-once/update-many is the correct lifetime model.
	gos_LightDataSsbo_Upload(
		lightData_,
		(size_t)lightDataStructuresCapacity * sizeof(TG_HWLightsData));

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

	gos_LightDataSsbo_Destroy();  // [LIGHTSSBO v1]

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

		//-----------------------------------------------------
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
				if (masterTextureNodes[i].pinRefCount > 0) {
					EVICT_SKIPPED(i, masterTextureNodes[i].pinRefCount, "flushCache_turn30");
					continue;
				}
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
				if (masterTextureNodes[i].pinRefCount > 0) {
					EVICT_SKIPPED(i, masterTextureNodes[i].pinRefCount, "flushCache_turn1");
					continue;
				}
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

// PERF FIX 2026-05-07: hash-based dedup map. The previous linear-scan
// implementation walked all existing entries doing 900-byte memcmp per
// comparison. Tracy capture (728 trees, default config) showed 8.5 ms
// total per frame — every actor scanning every prior actor's terrain-
// light-scaled struct, all unique due to per-actor aRGB. Hash-first
// approach: O(1) average lookup; memcmp only on hash match (collision
// verify). Map is reset by resetLightData() at frame start to mirror
// lightDataStructuresCount=0 reset.
//
// Hash collisions: 64-bit FNV-1a over ~1KB struct. Birthday-paradox
// probability ~10⁻⁷ per 728-actor frame; even on collision, memcmp
// fails-safe by falling through to the append path. Logical duplicates
// from collisions are correct (same-data → same-result), just waste a
// UBO slot. Acceptable.
namespace {
    // FRAME-JOBS-2 precondition: single mutex protecting both dedup maps AND
    // the lightData_ grow path. One lock covers the whole light-slot allocation
    // sequence (map lookup -> grow-if-needed -> table write -> map insert) so
    // the check-then-act is atomic from the callers' perspective.
    // recursive_mutex: addLightDataStructureWithPerActorColor calls
    // addLightDataStructure on the same thread while holding this lock.
    static std::recursive_mutex s_lightDataMapMu;

    static std::unordered_map<uint64_t, uint32_t> s_lightDataDedupMap;

    struct CachedSceneLightTemplate {
        TG_HWLightsData data;
        uint32_t actorLightSlot;
    };

    static std::unordered_map<uint64_t, CachedSceneLightTemplate> s_sceneLightTemplateMap;
    static uint32_t s_sceneLightTemplateFrame = 0xFFFFFFFFu;

    static inline uint64_t fnv1a_64_bytes(uint64_t h, const void* data, size_t bytes) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) {
            h ^= p[i];
            h *= 0x100000001b3ULL;
        }
        return h;
    }

    static inline uint64_t fnv1a_64_struct(const void* data, size_t bytes) {
        // FNV-1a 64-bit. Walk in uint64_t chunks for fewer iterations
        // (~110 iters for ~900 bytes vs ~900 iters byte-at-a-time).
        const uint64_t* p64 = static_cast<const uint64_t*>(data);
        const size_t chunks = bytes / sizeof(uint64_t);
        const size_t tail   = bytes % sizeof(uint64_t);
        uint64_t h = 0xcbf29ce484222325ULL;  // FNV offset basis
        for (size_t i = 0; i < chunks; ++i) {
            h ^= p64[i];
            h *= 0x100000001b3ULL;            // FNV prime
        }
        if (tail) {
            const uint8_t* tailp = reinterpret_cast<const uint8_t*>(&p64[chunks]);
            for (size_t i = 0; i < tail; ++i) {
                h ^= tailp[i];
                h *= 0x100000001b3ULL;
            }
        }
        return h;
    }

    static inline uint64_t fnv1a_64_u32(uint64_t h, uint32_t v) {
        return fnv1a_64_bytes(h, &v, sizeof(v));
    }

    static inline uint64_t fnv1a_64_bool(uint64_t h, bool v) {
        const uint8_t b = v ? 1 : 0;
        return fnv1a_64_bytes(h, &b, sizeof(b));
    }

    static inline uint32_t decomposeFirstActiveLightColor(TG_HWLightsData* lights) {
        const TG_LightPtr* listOfLights = TG_Shape::s_listOfLights;
        const DWORD numLights = TG_Shape::s_numLights;

        for (DWORD iLight = 0; iLight < numLights; ++iLight) {
            if ((listOfLights[iLight] != NULL) && listOfLights[iLight]->active) {
                const DWORD startLight = listOfLights[iLight]->GetaRGB();
                lights->lightColor[0][0] = ((startLight >> 16) & 0x000000ff) / 255.0f;
                lights->lightColor[0][1] = ((startLight >> 8) & 0x000000ff) / 255.0f;
                lights->lightColor[0][2] = ((startLight) & 0x000000ff) / 255.0f;
                lights->lightColor[0][3] = 1.0f;
                return 0;
            }
        }

        return 0xFFFFFFFFu;
    }

    static inline uint32_t firstActiveLightSourceIndex() {
        const TG_LightPtr* listOfLights = TG_Shape::s_listOfLights;
        const DWORD numLights = TG_Shape::s_numLights;

        if (!listOfLights)
            return 0xFFFFFFFFu;

        for (DWORD iLight = 0; iLight < numLights; ++iLight) {
            if ((listOfLights[iLight] != NULL) && listOfLights[iLight]->active)
                return iLight;
        }
        return 0xFFFFFFFFu;
    }

    static uint64_t sceneLightTemplateKey(uint32_t actorLightSourceIndex) {
        const TG_LightPtr* listOfLights = TG_Shape::s_listOfLights;
        const DWORD numLights = TG_Shape::s_numLights;

        uint64_t h = 0xcbf29ce484222325ULL;
        h = fnv1a_64_u32(h, g_mc2FrameCounter);
        h = fnv1a_64_u32(h, numLights);

        const uintptr_t listPtr = reinterpret_cast<uintptr_t>(listOfLights);
        h = fnv1a_64_bytes(h, &listPtr, sizeof(listPtr));

        for (DWORD iLight = 0; iLight < numLights; ++iLight) {
            const TG_LightPtr light = listOfLights ? listOfLights[iLight] : NULL;
            h = fnv1a_64_bool(h, light != NULL);
            if (!light)
                continue;

            h = fnv1a_64_bool(h, light->active);
            if (!light->active)
                continue;

            h = fnv1a_64_u32(h, light->lightType);
            h = fnv1a_64_bytes(h, &light->lightToWorld, sizeof(light->lightToWorld));
            h = fnv1a_64_bytes(h, &light->closeDistance, sizeof(light->closeDistance));
            h = fnv1a_64_bytes(h, &light->farDistance, sizeof(light->farDistance));
            h = fnv1a_64_bytes(h, &light->oneOverDistance, sizeof(light->oneOverDistance));

            // Per-actor terrain scaling mutates the first active light color
            // before CacheGpuLightData(). Other active light colors remain part
            // of the scene key. If future gameplay makes more colors per-actor,
            // widen this patch/key rule instead of reusing the template.
            if (iLight != actorLightSourceIndex) {
                const DWORD argb = light->GetaRGB();
                h = fnv1a_64_u32(h, argb);
            }
        }

        return h;
    }

    // [LIGHTBRIDGE v1] C5/C6 populate sizing recon (env-gated, measure-only,
    // demote-not-delete). The handoff's prescribed shape_emit_ns counter was
    // grep-proven to wrap only the C1 legacy leaf (tgl.cpp:2602 scope, gated
    // !eligibleForGpuObjects) which is structurally dead for the GPU-batched
    // population this slice targets; the C5/C6 path
    // (addLightDataStructureWithPerActorColor, the sole caller route from
    // GatherGpuObjectLightDataOnly) had NO armed-path attribution. This is
    // that attribution: ONE std::chrono pair per call (NOT a per-call Tracy
    // zone / not nested -> ~30-50ns/call observer effect, << the claimed
    // multi-ms lever; the 6-nested-scope cost-split apparatus that inflated
    // terrain numbers is the anti-pattern this deliberately avoids), summed
    // per-frame, drained at resetLightData() (frame-start). Gated on the SAME
    // MC2_OBJECT_RECON_TRACY the handoff capture protocol already sets, so the
    // protocol is unchanged. tmpl_hit counts the template-cache-hit calls
    // whose trailing FNV+memcmp is the redundancy the slice retires.
    static bool     s_lbInit = false;
    static bool     s_lbEnabled = false;
    static uint64_t s_lbFrameNs = 0,    s_lbMonoNs = 0;
    static uint64_t s_lbFrameCalls = 0, s_lbMonoCalls = 0;
    static uint64_t s_lbFrameHit = 0,   s_lbMonoHit = 0;
    static uint64_t s_lbFrameMiss = 0,  s_lbMonoMiss = 0;
    static uint64_t s_lbFrameNo = 0,    s_lbMonoNo = 0;
    static uint32_t s_lbFirstDataFrame = 0;

    static inline void lbInitFromEnv() {
        if (s_lbInit) return;
        s_lbInit = true;
        const char* e = std::getenv("MC2_OBJECT_RECON_TRACY");
        s_lbEnabled = (e != nullptr && e[0] != '\0' && e[0] != '0');
        if (s_lbEnabled) {
            std::puts("[LIGHTBRIDGE v1] event=enabled note=c5c6_populate_sizing_active");
            std::fflush(stdout);
        }
    }

    struct LbScope {
        std::chrono::steady_clock::time_point t0;
        LbScope() : t0(std::chrono::steady_clock::now()) {}
        ~LbScope() {
            s_lbFrameNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count();
            ++s_lbFrameCalls;
        }
    };

    static void lbDrainPerFrame(uint32_t frame) {
        if (!s_lbInit) lbInitFromEnv();
        const bool hadData = (s_lbFrameCalls != 0);
        if (hadData && s_lbFirstDataFrame == 0) s_lbFirstDataFrame = frame;

        if (s_lbEnabled && hadData) {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "[LIGHTBRIDGE v1] frame=%u populate={ns:%llu,calls:%llu,"
                "tmpl_hit:%llu,tmpl_miss:%llu,no_actor_light:%llu}",
                (unsigned)frame,
                (unsigned long long)s_lbFrameNs,  (unsigned long long)s_lbFrameCalls,
                (unsigned long long)s_lbFrameHit, (unsigned long long)s_lbFrameMiss,
                (unsigned long long)s_lbFrameNo);
            std::puts(buf); crashbundle_append(buf); std::fflush(stdout);
        }

        s_lbMonoNs   += s_lbFrameNs;   s_lbMonoCalls += s_lbFrameCalls;
        s_lbMonoHit  += s_lbFrameHit;  s_lbMonoMiss  += s_lbFrameMiss;
        s_lbMonoNo   += s_lbFrameNo;
        s_lbFrameNs = s_lbFrameCalls = s_lbFrameHit = s_lbFrameMiss = s_lbFrameNo = 0;

        if (frame > 0 && (frame % 600) == 0 && s_lbMonoCalls != 0) {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "[LIGHTBRIDGE v1] summary=%u populate={ns:%llu,calls:%llu,"
                "tmpl_hit:%llu,tmpl_miss:%llu,no_actor_light:%llu} "
                "ns_per_call=%llu first_data_frame=%u",
                (unsigned)frame,
                (unsigned long long)s_lbMonoNs,  (unsigned long long)s_lbMonoCalls,
                (unsigned long long)s_lbMonoHit, (unsigned long long)s_lbMonoMiss,
                (unsigned long long)s_lbMonoNo,
                (unsigned long long)(s_lbMonoCalls ? s_lbMonoNs / s_lbMonoCalls : 0),
                (unsigned)s_lbFirstDataFrame);
            std::puts(buf); crashbundle_append(buf); std::fflush(stdout);
        }
    }

    // ---- [LIGHTBRIDGE v1] SUBSTITUTIVE REPOINT (slice, not recon) ----------
    // Retires the per-call 1792B fnv1a_64_struct + 1792B memcmp for the
    // C5/C6/C7 GPU-object populate. On the 99.3% template-cache-hit common
    // path the slot is resolved by a tiny (templateKey + per-actor-aRGB)
    // key instead. See docs/superpowers/plans/
    // 2026-05-17-addlightdatastructure-bridge-retirement.md. Kill-switch
    // MC2_LIGHTBRIDGE (default ON; =0 restores legacy FNV/memcmp bit-for-
    // bit). Slot cache is per-frame, cleared in resetLightData alongside
    // s_lightDataDedupMap (same frame-start slot-reset invariant).
    struct LightSlotEntry { uint64_t tmpl; uint32_t actorARGB; uint32_t slot; };
    static std::unordered_map<uint64_t, LightSlotEntry> s_lightSlotByActorKey;

    static bool s_lbRepointInit = false;
    static bool s_lbRepointEnabled = true;          // default ON
    static bool s_lbFirstPopulateLogged = false;
    static bool s_lbC6FastpathLogged = false;

    static inline void lbRepointInitFromEnv() {
        if (s_lbRepointInit) return;
        s_lbRepointInit = true;
        const char* e = std::getenv("MC2_LIGHTBRIDGE");
        // OFF only on explicit "0"; any other value (or unset) = ON.
        s_lbRepointEnabled = !(e != nullptr && e[0] == '0' && e[1] == '\0');
        std::printf("[LIGHTBRIDGE v1] event=enabled mode=%s\n",
                    s_lbRepointEnabled ? "repoint" : "legacy");
        std::fflush(stdout);
    }

    // ---- [LIGHTBAKE v1] static-actor mission-load lighting bake --------
    // Static bdactor bldg/tree actors have a mission-constant per-actor
    // light (position-derived getTerrainLight + frozen sun/nightFactor;
    // no dynamic emitters -- lighting_is_mission_load_static_no_dynamic_
    // emitters.md). Lazily bake the post-decompose TG_HWLightsData once
    // (keyed by monotonic-never-reused registry recipeIndex) then re-emit
    // the constant into a per-frame slot WITHOUT the per-frame
    // GatherLights+decompose+template recompute (the retired CPU zone).
    // Shape-C precedent: recompute dies, the O(1) per-frame slot WRITE
    // stays. Post-SSBO this is the full design (no window/partition --
    // see docs/superpowers/plans/2026-05-17-static-lighting-bake-SIMPLIFIED.md).
    // s_bakedStaticLight = mission-scoped struct SOURCE (recipeIndex ->
    // baked TG_HWLightsData; cleared on mission unload + per-recipe on
    // invalidate; NOT per-frame). [LIGHTBAKE v2] persistent static table:
    // each static recipe owns a PERMANENT lightData_ slot == recipeIndex
    // in [0..s_staticLightHighWater); written ONCE at bake (CPU mirror),
    // re-shipped idempotently by the unchanged per-frame whole-buffer
    // upload (no per-frame addLightDataStructure / FNV / memcmp). S =
    // max(recipeIndex)+1; resetLightData rebases the dynamic count to S
    // so dynamic appends never collide into [0..S).
    static std::unordered_map<int32_t, TG_HWLightsData> s_bakedStaticLight;
    static uint32_t                                      s_staticLightHighWater = 0;
    // [LIGHTSLOT v1] diagnostic-only counters (Task 0 cardinality gate). Pure
    // counting, no behavior change. Read via mc2LightSlot* accessors below.
    static uint64_t s_lightSlotDedupHits = 0;   // addLightDataStructure FNV+memcmp matches
    static uint64_t s_lightSlotActorKeyHits = 0; // per-actor-color slot-cache hits
    static bool s_bakeInit = false;
    static bool s_bakeEnabled = true;          // default ON
    static bool s_bakeFirstLogged = false;

    static inline void bakeInitFromEnv() {
        if (s_bakeInit) return;
        s_bakeInit = true;
        const char* e = std::getenv("MC2_LIGHTBAKE");
        s_bakeEnabled = !(e != nullptr && e[0] == '0' && e[1] == '\0');
        std::printf("[LIGHTBAKE v1] event=enabled mode=%s\n",
                    s_bakeEnabled ? "bake" : "passthrough");
        std::fflush(stdout);
        if (mc2_diag::tagEnabled("LIGHTBAKE_PROOF")) {
            char diag_buf[128];
            snprintf(diag_buf, sizeof(diag_buf),
                "{\"event\":\"enabled\",\"mode\":\"%s\"}",
                s_bakeEnabled ? "bake" : "passthrough");
            mc2_diag::writeEvent("LIGHTBAKE_PROOF", 1, 0, diag_buf);
        }
    }
}

// Cross-TU kill-switch view (C6 repoint lives in msl.cpp). Free function
// to avoid pulling txmmgr.h into msl.cpp. Lazy-inits the env read.
bool mc2LightBridgeRepointEnabled()
{
    lbRepointInitFromEnv();
    return s_lbRepointEnabled;
}

// ---- [LIGHTBAKE v1] cross-TU free fns (bdactor/msl/registry call these;
// free fns avoid pulling txmmgr.h into those TUs) ----
bool mc2LightBakeEnabled()
{
    bakeInitFromEnv();
    return s_bakeEnabled;
}

// [LIGHTBAKE-PROOF v1] read-only S accessor. s_staticLightHighWater lives in the
// anonymous namespace above but is visible throughout this TU (same pattern as
// mc2GetBakedStaticLight reading s_bakedStaticLight). Exposed as a cross-TU free
// fn for the stability trace below and the upload-split caller (Task 4) — no
// txmmgr.h coupling into the consuming TUs.
uint32_t mc2StaticLightHighWater()
{
    return s_staticLightHighWater;
}

// [LIGHTSSBO v2] Static prefix dirty flag. Set whenever the permanent prefix
// [0..S) changes: a new recipe bakes, a recipe re-bakes on invalidate, or the
// table is cleared on mission unload. Consumed (and cleared) once per frame by
// the LightDataUpload split path. Initial true → first frame uploads the prefix.
static bool s_staticLightPrefixDirty = true;

void mc2MarkStaticLightPrefixDirty() { s_staticLightPrefixDirty = true; }
bool mc2ConsumeStaticLightPrefixDirty()
{
    const bool d = s_staticLightPrefixDirty;
    s_staticLightPrefixDirty = false;
    return d;
}

// [LIGHTSSBO v2] Upload-split kill-switch. Default ON; MC2_STATIC_LIGHT_UPLOAD_SPLIT=0
// forces the legacy whole-buffer upload every frame (bit-identical GPU contents).
// parseEnvBoolWithDefault is not visible in this TU; use the explicit =0 idiom.
static bool mc2StaticLightUploadSplitEnabled()
{
    static const bool s_split = []{
        const char* e = std::getenv("MC2_STATIC_LIGHT_UPLOAD_SPLIT");
        return !(e != nullptr && e[0] == '0' && e[1] == '\0');   // default true; "0" disables
    }();
    return s_split;
}

// [LIGHTBAKE-PROOF v1] Stability trace: record each recipe's permanent slot
// index the first time it is observed, then verify it never changes across
// frames and stays < S (inPrefix). Env-gated (MC2_LIGHTBAKE_STABILITY),
// demote-not-delete. No behavior change. Per-recipe first/UNSTABLE lines are
// capped at 32 to bound log volume, but DETECTION runs for ALL recipes and an
// aggregate coverage line (recipes_tracked / max_index / out-of-prefix count)
// is emitted every 256 new recipes — so coverage is never silently 32-capped
// (M2 fix, adversarial review 2026-06-03).
static const bool s_lbStabilityTrace =
    (std::getenv("MC2_LIGHTBAKE_STABILITY") != nullptr);
static std::unordered_map<int32_t, uint32_t> s_lbFirstSeenIndex;   // recipeIndex -> first index
static uint64_t s_lbStabilityViolations = 0;   // cross-frame index changes
static uint64_t s_lbOutOfPrefix        = 0;    // first-seen index >= S (should stay 0)
static uint32_t s_lbMaxIndex           = 0;    // max permanent index observed

void mc2LightBakeStabilityObserve(int32_t recipeIndex, uint32_t lightDataIndex)
{
    if (!s_lbStabilityTrace || recipeIndex < 0) return;
    const uint32_t S = mc2StaticLightHighWater();
    auto it = s_lbFirstSeenIndex.find(recipeIndex);
    if (it == s_lbFirstSeenIndex.end()) {
        s_lbFirstSeenIndex.emplace(recipeIndex, lightDataIndex);
        if (lightDataIndex > s_lbMaxIndex) s_lbMaxIndex = lightDataIndex;
        if (lightDataIndex >= S)           ++s_lbOutOfPrefix;   // counted for ALL recipes
        if (s_lbFirstSeenIndex.size() <= 32) {
            std::fprintf(stderr,
                "[LIGHTBAKE-PROOF v1] event=first recipe=%d index=%u S=%u inPrefix=%d\n",
                recipeIndex, lightDataIndex, S, (lightDataIndex < S) ? 1 : 0);
            std::fflush(stderr);
            if (mc2_diag::tagEnabled("LIGHTBAKE_PROOF")) {
                char diag_buf[128];
                snprintf(diag_buf, sizeof(diag_buf),
                    "{\"event\":\"first\",\"recipe\":%d,\"index\":%u,\"S\":%u,\"inPrefix\":%d}",
                    recipeIndex, lightDataIndex, S, (lightDataIndex < S) ? 1 : 0);
                mc2_diag::writeEvent("LIGHTBAKE_PROOF", 1, 0, diag_buf);
            }
        }
        // Aggregate coverage so recipes beyond the 32 log-cap are accounted for.
        if ((s_lbFirstSeenIndex.size() % 256u) == 0u) {
            std::fprintf(stderr,
                "[LIGHTBAKE-PROOF v1] event=coverage recipes_tracked=%zu max_index=%u S=%u out_of_prefix=%llu\n",
                s_lbFirstSeenIndex.size(), s_lbMaxIndex, S,
                (unsigned long long)s_lbOutOfPrefix);
            std::fflush(stderr);
            if (mc2_diag::tagEnabled("LIGHTBAKE_PROOF")) {
                char diag_buf[192];
                snprintf(diag_buf, sizeof(diag_buf),
                    "{\"event\":\"coverage\",\"recipes_tracked\":%zu,\"max_index\":%u,"
                    "\"S\":%u,\"out_of_prefix\":%llu}",
                    s_lbFirstSeenIndex.size(), s_lbMaxIndex, S,
                    (unsigned long long)s_lbOutOfPrefix);
                mc2_diag::writeEvent("LIGHTBAKE_PROOF", 1, 0, diag_buf);
            }
        }
    } else if (it->second != lightDataIndex) {
        ++s_lbStabilityViolations;   // detection runs for ALL recipes (uncapped)
        if (s_lbStabilityViolations <= 32) {
            std::fprintf(stderr,
                "[LIGHTBAKE-PROOF v1] event=UNSTABLE recipe=%d was=%u now=%u S=%u\n",
                recipeIndex, it->second, lightDataIndex, S);
            std::fflush(stderr);
            if (mc2_diag::tagEnabled("LIGHTBAKE_PROOF")) {
                char diag_buf[128];
                snprintf(diag_buf, sizeof(diag_buf),
                    "{\"event\":\"UNSTABLE\",\"recipe\":%d,\"was\":%u,\"now\":%u,\"S\":%u}",
                    recipeIndex, it->second, lightDataIndex, S);
                mc2_diag::writeEvent("LIGHTBAKE_PROOF", 1, 0, diag_buf);
            }
        }
    }
}

bool mc2GetBakedStaticLight(int32_t recipeIndex, TG_HWLightsData& out)
{
    if (recipeIndex < 0) return false;
    auto it = s_bakedStaticLight.find(recipeIndex);
    if (it == s_bakedStaticLight.end()) return false;
    out = it->second;
    return true;
}

// [LIGHTBRIDGE-BAKED-PROBE-1] Presence-only probe — avoids 1792B struct copy
// when the callee (EmitBakedGpuLightData) only needs the key (recipeIndex).
// Use this in touch()/touchSerialCommit() hot paths instead of mc2GetBakedStaticLight.
std::atomic<int>       g_bakedProbeCalls{0};
std::atomic<int>       g_bakedCopyCalls{0};
std::atomic<long long> g_bakedCopyBytes{0};

bool mc2IsBakedStaticLightPresent(int32_t recipeIndex)
{
    if (recipeIndex < 0) return false;
    g_bakedProbeCalls.fetch_add(1, std::memory_order_relaxed);
    return s_bakedStaticLight.count(recipeIndex) != 0;
}

void mc2SetBakedStaticLight(int32_t recipeIndex, const TG_HWLightsData& in)
{
    if (recipeIndex < 0) return;
    s_bakedStaticLight[recipeIndex] = in;
    if (!s_bakeFirstLogged) {
        s_bakeFirstLogged = true;
        std::printf("[LIGHTBAKE v1] event=first_bake recipe=%d\n", recipeIndex);
        std::fflush(stdout);
    }
}

// Erased on destruction/LOD multi-swap (via invalidateStaticRegistration
// -> GpuStaticPropRegistry::invalidate) so the next CacheGpuLightData
// lazily re-bakes the same position-derived constant for the new multi.
void mc2EraseBakedStaticLight(int32_t recipeIndex)
{
    if (recipeIndex < 0) return;
    s_bakedStaticLight.erase(recipeIndex);
}

// Mission unload: recipeIndex restarts per mission -> a stale entry
// would alias a different actor. Drop the whole mission-scoped map.
void mc2ClearAllBakedStaticLight()
{
    // Co-located with GpuStaticPropRegistry::destroy s_recipeRanges.clear()
    // (gos_static_prop_registry.cpp) via mission.cpp -> next mission's
    // recipeIndex restarts at 0 against a fresh prefix.
    s_bakedStaticLight.clear();
    s_staticLightHighWater = 0;
    mc2MarkStaticLightPrefixDirty();   // [LIGHTSSBO v2] table reset → re-upload prefix next frame
}

// [G1-STATIC-EAGER-LIGHT v1] Probe helper: returns true when lightData_[recipeIndex]
// has been written by a real bake (numLights_ > 0). Default-constructed slots and
// the G1 ambient fallback both set numLights_=1, so this is specifically the check
// "has the slot ever been written by mc2WriteStaticLightSlot (static prefix only)".
// Used by MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE to count never-baked alive props.
// Checks the persistent static prefix [0..s_staticLightHighWater) only:
// slot beyond S means registerRecipe ran but bakeStaticLightSlot was never called.
bool mc2IsStaticLightSlotBaked(int32_t recipeIndex)
{
    if (recipeIndex < 0 || !mcTextureManager) return false;
    const uint32_t ri = static_cast<uint32_t>(recipeIndex);
    if (ri >= s_staticLightHighWater) return false;  // slot never written
    TG_HWLightsData slot{};
    if (!mcTextureManager->copyLightSlot(ri, slot)) return false;
    return slot.numLights_ > 0;
}

// [LIGHTBAKE v2] Persistent static slot write. Replaces the retired
// per-frame mc2SubmitBakedLightSlot (which re-ran addLightDataStructure
// -> 1792B FNV + 1792B memcmp every frame per recipe). Called ONCE per
// recipe at bake (and again only on invalidate re-bake): mirror the
// constant into the permanent CPU slot lightData_[recipeIndex] and
// advance S. The unchanged per-frame whole-buffer upload then ships it
// to the GPU every frame idempotently (resetLightData never memsets
// lightData_ contents). No GL call, no FNV, no memcmp.
void mc2WriteStaticLightSlot(int32_t recipeIndex, const TG_HWLightsData& baked)
{
    if (recipeIndex < 0 || !mcTextureManager) return;
    mcTextureManager->bakeStaticLightSlot(recipeIndex, baked);
}

// [LIGHTSLOT v1] Task 0 cardinality-gate accessors (diagnostic only).
// B = permanent baked static slots (high-water); H components = dedup +
// actor-key cache hits; U-source = total live light-table count.
uint32_t mc2LightSlotBakedHighWater()   { return s_staticLightHighWater; }
uint64_t mc2LightSlotDedupHits()         { return s_lightSlotDedupHits; }
uint64_t mc2LightSlotActorKeyHits()      { return s_lightSlotActorKeyHits; }
uint32_t mc2LightSlotTableCount()        { return mcTextureManager ? mcTextureManager->getLightStructCount() : 0u; }

uint32_t MC_TextureManager::addLightDataStructure(TG_HWLightsData* light_data)
{
    // Tracy zone retained — formerly named "scan", now wraps the whole
    // function so old captures remain comparable. Should drop from
    // ~12 µs/call (linear scan) to ~200-300 ns/call (hash + map ops).
    ZoneScopedN("addLightDataStructure scan");
    // FRAME-JOBS-2 precondition: lock covers map lookup + grow-if-needed +
    // table write + map insert as a single atomic sequence.
    std::lock_guard<std::recursive_mutex> _lk(s_lightDataMapMu);

    const uint64_t hash = fnv1a_64_struct(light_data, sizeof(TG_HWLightsData));
    auto it = s_lightDataDedupMap.find(hash);
    if (it != s_lightDataDedupMap.end()) {
        const uint32_t slot = it->second;
        // Verify with memcmp on hash match (collision safety).
        if (slot < lightDataStructuresCount &&
            0 == memcmp(lightData_ + slot, light_data, sizeof(TG_HWLightsData))) {
            ++s_lightSlotDedupHits;   // [LIGHTSLOT v1] content dedup matched an existing slot
            return slot;
        }
        // Hash collision (vanishingly rare) — fall through to append.
        // We don't update the map; future lookups of the colliding hash
        // will continue to find the existing slot via memcmp-verify.
    }

    // unique data passed, so add it
    if(lightDataStructuresCount + 1 >= lightDataStructuresCapacity)
    {
        TG_HWLightsData* new_lights_data = new TG_HWLightsData[lightDataStructuresCapacity + 128];
        memcpy(new_lights_data, lightData_, sizeof(TG_HWLightsData)*lightDataStructuresCount);
        delete[] lightData_;
        lightData_ = new_lights_data;
        lightDataStructuresCapacity += 128;
    }

    lightData_[lightDataStructuresCount] = *light_data;
    uint32_t rv = lightDataStructuresCount;
    lightDataStructuresCount++;
    s_lightDataDedupMap.emplace(hash, rv);  // O(1) avg insert

    // PERF DIAGNOSTIC 2026-05-06: log table growth periodically. 1 line per
    // 256 new entries. DEMOTED 2026-05-17 to env-gated (its own "demote
    // once the regression is closed" instruction): the dedup-growth
    // regression is closed (D2 + SSBO + static-bake shipped); it was
    // emitting ~6k lines/run and polluting frame-time captures.
    // Capability kept (debug_instrumentation_rule: demote-not-delete) --
    // set MC2_LIGHT_DEDUP_TRACE=1 to re-enable.
    static const bool s_lightDedupTrace =
        (std::getenv("MC2_LIGHT_DEDUP_TRACE") != nullptr);
    if (s_lightDedupTrace && (lightDataStructuresCount & 0xFF) == 0) {
        printf("[LIGHT_DEDUP v1] count=%u capacity=%u memcmp_per_call_bytes_max=%zu\n",
               lightDataStructuresCount,
               lightDataStructuresCapacity,
               (size_t)lightDataStructuresCount * sizeof(TG_HWLightsData));
        fflush(stdout);
    }
    return rv;
}

uint32_t MC_TextureManager::addLightDataStructureWithPerActorColor(TG_HWLightsData* light_data)
{
    gosASSERT(light_data);
    LbScope _lb_;  // [LIGHTBRIDGE v1] C5/C6 populate sizing (RAII, all return paths)
    // FRAME-JOBS-2 precondition: protects s_sceneLightTemplateMap, s_lightSlotByActorKey,
    // and (via the nested addLightDataStructure call) s_lightDataDedupMap + lightData_ grow.
    // recursive_mutex allows the nested call to re-acquire on the same thread.
    std::lock_guard<std::recursive_mutex> _lk(s_lightDataMapMu);

    if (s_sceneLightTemplateFrame != g_mc2FrameCounter) {
        s_sceneLightTemplateMap.clear();
        s_sceneLightTemplateFrame = g_mc2FrameCounter;
    }

    const uint32_t actorLightSource = firstActiveLightSourceIndex();
    if (actorLightSource == 0xFFFFFFFFu) {
        ++s_lbFrameNo;  // [LIGHTBRIDGE v1] no per-actor light (direct passthrough)
        GatherLightsParameters(light_data);
        return addLightDataStructure(light_data);
    }

    const uint64_t key = sceneLightTemplateKey(actorLightSource);
    auto it = s_sceneLightTemplateMap.find(key);
    if (it == s_sceneLightTemplateMap.end()) {
        ++s_lbFrameMiss;  // [LIGHTBRIDGE v1] template miss (GatherLightsParameters runs)
        CachedSceneLightTemplate entry;
        GatherLightsParameters(&entry.data);
        entry.actorLightSlot = decomposeFirstActiveLightColor(&entry.data);
        it = s_sceneLightTemplateMap.emplace(key, entry).first;
    } else {
        ++s_lbFrameHit;  // [LIGHTBRIDGE v1] template hit (trailing FNV+memcmp = retirable redundancy)
    }

    *light_data = it->second.data;
    const bool perActor = (it->second.actorLightSlot != 0xFFFFFFFFu);
    if (perActor)
        decomposeFirstActiveLightColor(light_data);

    // [LIGHTBRIDGE v1] substitutive repoint: on the (template + per-actor-
    // color) cache hit, return the resolved slot directly and SKIP the
    // 1792B fnv1a_64_struct + 1792B memcmp in addLightDataStructure.
    // Symmetry invariant (load-bearing — do not break without re-deriving
    // the key): actorLightSource == firstActiveLightSourceIndex() is the
    // SAME light decompose mutates into lightColor[0][0..3] AND the SAME
    // light sceneLightTemplateKey deliberately excludes from the template
    // key (txmmgr.cpp ~:1017-1020). actorARGB closes exactly that excluded
    // gap. perActor==false => no decompose => template key alone suffices
    // (actorARGB folds to 0, combined == key).
    if (!s_lbRepointInit) lbRepointInitFromEnv();
    if (s_lbRepointEnabled) {
        const uint32_t actorARGB = perActor
            ? (uint32_t)TG_Shape::s_listOfLights[actorLightSource]->GetaRGB()
            : 0u;
        const uint64_t combined =
            key ^ ((uint64_t)actorARGB * 0x9E3779B97F4A7C15ULL);
        auto sit = s_lightSlotByActorKey.find(combined);
        if (sit != s_lightSlotByActorKey.end() &&
            sit->second.tmpl == key && sit->second.actorARGB == actorARGB) {
            ++s_lightSlotActorKeyHits;  // [LIGHTSLOT v1] per-actor-color slot-cache hit
            return sit->second.slot;   // retired: no FNV, no memcmp
        }
        const uint32_t slot = addLightDataStructure(light_data);
        s_lightSlotByActorKey[combined] =
            LightSlotEntry{ key, actorARGB, slot };
        if (!s_lbFirstPopulateLogged) {
            s_lbFirstPopulateLogged = true;
            std::puts("[LIGHTBRIDGE v1] event=first_populate");
            std::fflush(stdout);
        }
        return slot;
    }

    return addLightDataStructure(light_data);
}

void MC_TextureManager::resetLightData()
{
    // [LIGHTBRIDGE v1] frame-start boundary: flush the just-completed frame's
    // C5/C6 populate sizing (same boundary the dedup-map reset relies on).
    lbDrainPerFrame(g_mc2FrameCounter);
    // FRAME-JOBS-2 precondition: lock during map clears and count rebase.
    std::lock_guard<std::recursive_mutex> _lk(s_lightDataMapMu);

    // [LIGHTBAKE v2] Rebase the DYNAMIC allocator base to S (the static
    // prefix high-water) when the bake is on, so dynamic appends start
    // above [0..S) and addLightDataStructure never returns a slot < S
    // (rv = count, count never < S -> the dedup maps below stay
    // dynamic-only by construction, exactly as before but rebased). With
    // MC2_LIGHTBAKE=0 the persistent table is off -> base 0 (else the
    // dynamic allocator would collide into a non-rebased prefix).
    lightDataStructuresCount = mc2LightBakeEnabled() ? s_staticLightHighWater : 0;
    // PERF FIX 2026-05-07: clear the dedup map alongside the count reset.
    // Both must reset together — dynamic slot indices restart from the
    // base (S or 0) each frame, so any stale hash→slot entries from the
    // prior frame are invalid. (Static [0..S) is NEVER cleared here:
    // s_bakedStaticLight + lightData_[0..S) persist across frames; that
    // is the whole point — resetLightData does not memset lightData_.)
    s_lightDataDedupMap.clear();
    s_sceneLightTemplateMap.clear();
    s_lightSlotByActorKey.clear();  // [LIGHTBRIDGE v1] per-frame slot cache
    s_sceneLightTemplateFrame = 0xFFFFFFFFu;
}

// [LIGHTBAKE v2] Persistent static slot writer (member: needs private
// lightData_/capacity access). Grow lightData_ so [recipeIndex] is
// addressable (preserving ALL existing contents -- static prefix AND any
// transient dynamic entries -- via the same realloc+memcpy pattern as
// the addLightDataStructure grow), mirror the baked constant into the
// permanent slot, advance S. Called once per recipe at bake / invalidate
// re-bake -- NOT per frame.
void MC_TextureManager::bakeStaticLightSlot(int32_t recipeIndex, const TG_HWLightsData& baked)
{
    if (recipeIndex < 0) return;
    const uint32_t ri = static_cast<uint32_t>(recipeIndex);
    if (ri + 1 >= lightDataStructuresCapacity)
    {
        uint32_t newCap = lightDataStructuresCapacity;
        while (ri + 1 >= newCap) newCap += 128;            // +128 chunks, like the dynamic grow
        TG_HWLightsData* grown = new TG_HWLightsData[newCap];
        // Preserve the FULL old array (static prefix is in [0..S); copying
        // only `count` would drop persisted static slots).
        memcpy(grown, lightData_, sizeof(TG_HWLightsData) * lightDataStructuresCapacity);
        delete[] lightData_;
        lightData_ = grown;
        lightDataStructuresCapacity = newCap;
    }
    lightData_[ri] = baked;                                 // CPU mirror (persists)
    mc2MarkStaticLightPrefixDirty();                        // [LIGHTSSBO v2] prefix content changed
    // 2A: the per-leaf permanent light index is NOT recipeIndex — it is the
    // owning RANGE's lightDataIndex (all leaves of a multi-leaf range share one
    // light slot, matching legacy flush). That propagation now lives in the
    // registry's markVisible() (range-aware). The earlier bake-time per-recipe
    // persist was wrong for multi-leaf ranges (compare-oracle caught it) and is
    // removed. The light-table SLOT content here is unchanged.
    if (ri + 1 > s_staticLightHighWater) s_staticLightHighWater = ri + 1;
    // [SPFLUSH_COST_SPLIT v1] recipe_rebuild counter. light_index_writes == recipe_rebuilds
    // (both happen at this bakeStaticLightSlot call — same site, same count; only one counter kept).
    if (s_spflushTxmEnabled) ++s_spflush_recipe_rebuilds_txm;
    // CRITICAL: the frame a recipe FIRST bakes, this-frame's count was
    // set to the OLD (smaller) S at frame-start resetLightData. Without
    // this bump, a later dynamic addLightDataStructure append THIS frame
    // could land on slot `ri` and clobber the just-written permanent
    // static slot (wrong-light, never re-baked). Raising the live count
    // to S keeps same-frame dynamic appends strictly above [0..S). Only
    // skips some low dynamic slots that frame (harmless -- dynamic is
    // per-frame ephemeral, rebuilt from S next resetLightData).
    if (lightDataStructuresCount < s_staticLightHighWater)
        lightDataStructuresCount = s_staticLightHighWater;
}

// Diagnostic body — declaration in txmmgr.h. See header for rationale.
MC_TextureManager::LightSlotPeek MC_TextureManager::peekLightSlot(uint32_t idx) const
{
    LightSlotPeek p = {-1, -1, 0.0f, 0.0f, 0.0f};
    if (idx >= lightDataStructuresCount || !lightData_) return p;
    const TG_HWLightsData& d = lightData_[idx];
    p.numLights = d.numLights_;
    if (d.numLights_ > 0) {
        // light_dir[i].w carries the light type (TG_LIGHT_AMBIENT=0, INFINITE=1,
        // INFINITEWITHFALLOFF=2, POINT=3, SPOT=4, TERRAIN=5). Mirrors GLSL
        // ObjectLights.light_dir[i].w in shaders/include/lighting.hglsl.
        p.firstType   = static_cast<int>(d.lightDir[0][3]);
        p.firstColorR = d.lightColor[0][0];
        p.firstColorG = d.lightColor[0][1];
        p.firstColorB = d.lightColor[0][2];
    }
    return p;
}

// [LIGHTBAKE-PROOF v1] read-only full-slot copy for the parity trace.
bool MC_TextureManager::copyLightSlot(uint32_t idx, TG_HWLightsData& out) const
{
    if (idx >= lightDataStructuresCount || !lightData_) return false;
    out = lightData_[idx];
    return true;
}

// [LIGHTBAKE-PROOF v1] Slot-write INTEGRITY check (NOT an independent A/B parity —
// M1 honesty fix, adversarial review 2026-06-03). Hashes the freshly-gathered leaf
// (the CacheGpuLightData MISS-path result == the legacy transient gather) against
// the permanent slot lightData_[recipeIndex] it was just memcpy'd into by
// mc2WriteStaticLightSlot. So match=1 proves the permanent slot FAITHFULLY STORES
// the legacy-gathered record (no corruption/truncation in the store) — combined
// with the stability trace (slot never changes), that gives "every frame the GPU
// sees the legacy-gathered value." It does NOT independently re-derive the legacy
// value (that would be tautological here — the leaf IS the gather source). The true
// baked-vs-transient A/B is the cross-run MC2_LIGHTBAKE=0 vs =1 comparison (validated
// when the bake shipped, 2db2a04). Env-gated (MC2_LIGHTBAKE_PARITY), capped at 32
// lines (+ always logs any mismatch). No behavior change.
static const bool s_lbParityTrace =
    (std::getenv("MC2_LIGHTBAKE_PARITY") != nullptr);
static uint64_t s_lbParityChecks   = 0;
static uint64_t s_lbParityMismatch = 0;

void mc2LightBakeParityCheck(int32_t recipeIndex,
                             const TG_HWLightsData* gatheredLeaf,
                             float wx, float wy, float wz,
                             const char* appearance)
{
    if (!s_lbParityTrace || recipeIndex < 0 || !gatheredLeaf || !mcTextureManager)
        return;
    const uint64_t leafHash = fnv1a_64_struct(gatheredLeaf, sizeof(TG_HWLightsData));
    TG_HWLightsData slot{};
    if (!mcTextureManager->copyLightSlot(static_cast<uint32_t>(recipeIndex), slot))
        return;  // slot not yet addressable; skip
    const uint64_t slotHash = fnv1a_64_struct(&slot, sizeof(TG_HWLightsData));
    ++s_lbParityChecks;
    const bool match = (leafHash == slotHash);
    if (!match) ++s_lbParityMismatch;
    if (s_lbParityChecks <= 32 || !match) {
        std::fprintf(stderr,
            "[LIGHTBAKE-PROOF v1] event=slot_write_integrity recipe=%d leafHash=%016llx slotHash=%016llx "
            "match=%d pos=(%.1f,%.1f,%.1f) appr=%s\n",
            recipeIndex, (unsigned long long)leafHash, (unsigned long long)slotHash,
            match ? 1 : 0, wx, wy, wz, appearance ? appearance : "?");
        std::fflush(stderr);
    }
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

	void render(HGOSBUFFER vb, HGOSBUFFER ib, HGOSVERTEXDECLARATION vdecl, DWORD texture_id, int light_index, bool isHudElement = false, const TG_RenderShape* renderShape = NULL)
	{
		gos_SetRenderState(gos_State_Texture, texture_id);
		gos_SetRenderViewport(viewport_[2], viewport_[3], viewport_[0], viewport_[1]);

		HGOSRENDERMATERIAL mat;
		const bool usePbrOverride = renderShape &&
			renderShape->programOverride_ &&
			renderShape->pbrNormalTexture_ &&
			renderShape->pbrOrmTexture_ &&
			renderShape->pbrMaterialSsbo_;
		if (usePbrOverride) {
			mat = renderShape->programOverride_;
		} else if (texture_id == 0) {
			static const HGOSRENDERMATERIAL s_matVertexLighted = gos_getRenderMaterial("gos_vertex_lighted");
			mat = s_matVertexLighted;
		} else {
			static const HGOSRENDERMATERIAL s_matTexVertexLighted = gos_getRenderMaterial("gos_tex_vertex_lighted");
			mat = s_matTexVertexLighted;
		}

		gos_SetRenderMaterialParameterMat4(mat, "world_", (const float*)*world_);
		//gos_SetRenderMaterialParameterMat4(mat, "view_", (const float*)*view_);
		gos_SetRenderMaterialParameterMat4(mat, "wvp_", (const float*)*wvp_);

        float ld[4] = { (float)light_index, 0.0f, 0.0f, 0.0f};
		gos_SetRenderMaterialParameterFloat4(mat, "light_offset_", ld);

		// GPU projection via terrainMVP (skip for HUD elements which need legacy viewport projection)
		gos_SetRenderMaterialParameterInt(mat, "gpuProjection", isHudElement ? 0 : 1);

		// [LIGHTSSBO v1] FORK-2: LightsData is now an SSBO; the UBO-
		// reflection bind below would silently no-op (SSBO blocks are not
		// in GL_ACTIVE_UNIFORM_BLOCKS) -> legacy lit meshes would render
		// garbage lighting. Bind the storage block explicitly instead.
		// SceneData stays a UBO.
		gos_SetRenderMaterialUniformBlockBindingPoint(mat, "SceneData", SCENE_DATA_ATTACHMENT_SLOT);

		// GLSTATE-SSBO-SLOT5-RESTORE-1: save prior slot 5 binding so endShadowPass
		// restores it correctly instead of hard-zeroing (NVIDIA may generate
		// GL_INVALID_OPERATION on a shader read from a zero-bound SSBO slot).
		GLuint savedSsbo5 = 0;
		if (usePbrOverride) {
			static int s_pbrRenderTrace = 0;
			if (getenv("MC2_BUILDING_PBR_TRACE") && s_pbrRenderTrace < 64) {
				++s_pbrRenderTrace;
				fprintf(stderr,
					"[BUILDING_PBR_TRACE] ShapeRenderer::render vb=%p ib=%p tex=%lu normal=%lu orm=%lu ssbo=%lu light=%d\n",
					(void*)vb, (void*)ib, (unsigned long)texture_id,
					(unsigned long)renderShape->pbrNormalTexture_,
					(unsigned long)renderShape->pbrOrmTexture_,
					(unsigned long)renderShape->pbrMaterialSsbo_,
					light_index);
				fflush(stderr);
			}
			float controls[4] = {
				renderShape->pbrTileScale_,
				renderShape->pbrRoughnessBias_,
				renderShape->pbrMetallicInfluence_,
				0.0f
			};
			gos_SetRenderMaterialParameterFloat4(mat, "u_buildingPbrControls", controls);
			gos_SetRenderMaterialSamplerUnit(mat, "tex1", 0);
			gos_SetRenderMaterialSamplerUnit(mat, "u_normalTex", 1);
			gos_SetRenderMaterialSamplerUnit(mat, "u_ormTex", 2);
			gos_SetRenderState(gos_State_Texture2, renderShape->pbrNormalTexture_);
			gos_SetRenderState(gos_State_Texture3, renderShape->pbrOrmTexture_);
			GLint q5 = 0;
			glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 5, &q5);
			savedSsbo5 = static_cast<GLuint>(q5);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, (GLuint)renderShape->pbrMaterialSsbo_);
			gos_SetRenderState(gos_State_Culling, gos_Cull_None);
		}

		gos_ApplyRenderMaterial(mat);
		gos_BindLightDataStorageBlock(mat);

		// Bind shadow maps + terrainMVP after apply() (requires active program)
		if (!isHudElement) {
			gos_SetupObjectShadows(mat);
		}

		gos_RenderIndexedArray(ib, vb, vdecl);

		if (usePbrOverride) {
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, savedSsbo5);
			gos_SetRenderState(gos_State_Texture2, 0);
			gos_SetRenderState(gos_State_Texture3, 0);
			gos_SetRenderState(gos_State_Culling, gos_Cull_CW);
		}

	}

};

void GatherLightsParameters(TG_HWLightsData* lights)
{
	gosASSERT(lights);

	// Stage 2.D.2 diagnostic: dump gathered lights when MC2_OBJECT_PARITY_TRACE=1.
	// Fired once per session to avoid per-frame spam. Shows what GPU UBO gets.
	static bool s_lightDumpDone = false;
	const bool doLightTrace = (!s_lightDumpDone && [](){
		const char* v = getenv("MC2_OBJECT_PARITY_TRACE");
		return v && v[0] == '1' && v[1] == '\0';
	}());

	uint32_t num_lights = 0;
	// LIGHT-CLAMP-RAISE-STAGE1-1: runtime population now reaches the full GPU ABI
	// cap (MAX_HW_LIGHTS_IN_WORLD = 32). Stage 0 widened the struct/stride to 32;
	// this slice "allows putting 32 in the box". Safe because every shader per-light
	// loop is min(numLights.x, MAX_LIGHTS_IN_WORLD)-bound (cost/reads follow the
	// populated count), and the break below (last write = slot 31) is the overrun
	// guard. The break loop is cap-agnostic — it just trips later now.
	static constexpr uint32_t kRuntimeLightClamp = MAX_HW_LIGHTS_IN_WORLD; // 32
	const uint32_t max_num_lights = kRuntimeLightClamp;

	const TG_LightPtr* listOfLights = TG_Shape::s_listOfLights;
	const DWORD numLights = TG_Shape::s_numLights;

	// T1.15 [SPOT_DIAG v1] per-call active/inactive/point_active tally.
	unsigned diagActiveLights = 0;
	unsigned diagInactiveLights = 0;
	unsigned diagPointActive = 0;

	for (uint32_t iLight = 0; iLight < numLights; iLight++)
	{
		if (num_lights == max_num_lights)
			break;

		// T1.15 [SPOT_DIAG v1] count active/inactive lights in the source list
		// before the active-filter culls them. Read `active` ONCE here; the
		// canonical filter below reads it again, but the field is plain DWORD
		// and not externally mutated between these two reads in this loop.
		if (listOfLights[iLight] != NULL) {
			const DWORD t = listOfLights[iLight]->lightType;
			if (listOfLights[iLight]->active) {
				++diagActiveLights;
				if (t == TG_LIGHT_POINT) ++diagPointActive;
			} else {
				++diagInactiveLights;
			}
		}

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

			// Slice 2 (object-offload) — Stage 2.C: per-light falloff fields.
			// GLSL `GetFalloff` reads .x=closeDistance, .y=farDistance,
			// .z=oneOverDistance. Source on the CPU side is
			// TG_Light::{closeDistance,farDistance,oneOverDistance} at
			// mclib/tgl.h:193-195. AMBIENT lights don't use distance falloff
			// (the GLSL kernel hits the AMBIENT case before reading falloff),
			// but populate the fields anyway for cache uniformity.
			lights->lightFalloff[num_lights][0] = listOfLights[iLight]->closeDistance;
			lights->lightFalloff[num_lights][1] = listOfLights[iLight]->farDistance;
			lights->lightFalloff[num_lights][2] = listOfLights[iLight]->oneOverDistance;
			lights->lightFalloff[num_lights][3] = 0.0f;

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

			if (doLightTrace) {
				std::fprintf(stderr,
					"[PARITY_DIAG v2] GatherLightsParameters iLight=%u type=%u "
					"aRGB=0x%08X dir=(%.4f,%.4f,%.4f) color=(%.4f,%.4f,%.4f)\n",
					num_lights,
					(unsigned)type,
					(unsigned)listOfLights[iLight]->GetaRGB(),
					lights->lightDir[num_lights][0],
					lights->lightDir[num_lights][1],
					lights->lightDir[num_lights][2],
					lights->lightColor[num_lights][0],
					lights->lightColor[num_lights][1],
					lights->lightColor[num_lights][2]);
				std::fflush(stderr);
			}

			num_lights++;
		}
	}

	if (doLightTrace) {
		std::fprintf(stderr,
			"[PARITY_DIAG v2] GatherLightsParameters numLights=%u\n",
			num_lights);
		std::fflush(stderr);
		s_lightDumpDone = true;
	}

	// MC2_LIGHT_CLAMP_FIXTURE (default OFF) — deterministic synthetic point lights
	// injected into slots [num_lights..32) to PROVE slots >16 ever populate AND
	// render. NEVER writes past slot 31 (ABI cap 32). NEVER fires unless the env
	// gate is "1". Deterministic: fixed color/dir/falloff derived from slot index.
	if (s_lightClampFixture && num_lights < MAX_HW_LIGHTS_IN_WORLD) {
		const uint32_t injectStart = num_lights;
		// Fill to the cap; the loop bound (< MAX_HW_LIGHTS_IN_WORLD) guarantees the
		// last written slot is 31 — never an overrun of the 32-wide arrays.
		while (num_lights < MAX_HW_LIGHTS_IN_WORLD) {
			const uint32_t s = num_lights;

			// Identity lightToWorld (point light at origin-relative offset baked
			// into dir is unused; point lights use position via lightToWorld).
			for (int c = 0; c < 16; ++c)
				lights->lightToWorld[s][c] = (c % 5 == 0) ? 1.0f : 0.0f; // identity

			// Deterministic direction (unused for POINT but populate for uniformity).
			lights->lightDir[s][0] = 0.0f;
			lights->lightDir[s][1] = -1.0f;
			lights->lightDir[s][2] = 0.0f;
			lights->lightDir[s][3] = (float)TG_LIGHT_POINT;

			// Deterministic bright color that cycles by slot so the extra lights
			// are visibly distinct when they contribute.
			lights->lightColor[s][0] = ((s & 1) ? 1.0f : 0.25f);
			lights->lightColor[s][1] = ((s & 2) ? 1.0f : 0.25f);
			lights->lightColor[s][2] = ((s & 4) ? 1.0f : 0.25f);
			lights->lightColor[s][3] = 1.0f;

			// Wide falloff so the synthetic lights actually reach the object.
			lights->lightFalloff[s][0] = 0.0f;       // closeDistance
			lights->lightFalloff[s][1] = 1000.0f;    // farDistance
			lights->lightFalloff[s][2] = 1.0f / 1000.0f; // oneOverDistance
			lights->lightFalloff[s][3] = 0.0f;

			++num_lights;
		}

		if (!s_lightClampFixtureYelled) {
			s_lightClampFixtureYelled = true;
			std::fprintf(stderr,
				"[LIGHT_CLAMP_FIXTURE] ON: injected %u synthetic point lights into "
				"object %p slots %u..%u (ABI cap %d). This DELIBERATELY changes "
				"rendered lighting — gate MC2_LIGHT_CLAMP_FIXTURE.\n",
				num_lights - injectStart, (void*)lights,
				injectStart, num_lights - 1, MAX_HW_LIGHTS_IN_WORLD);
			std::fflush(stderr);
		}
	}

	lights->numLights_ = num_lights;

	// MC2_LIGHT_CLAMP_PROBE (default OFF) — pure observation, no behavior change.
	// Track per-object high-water populated light count; emit on each new high
	// plus a periodic summary so the per-mission peak is visible in logs.
	if (s_lightClampProbe) {
		++s_lightClampProbeCalls;
		if (num_lights > s_lightClampProbeHighWater) {
			s_lightClampProbeHighWater = num_lights;
			std::fprintf(stderr,
				"[LIGHT_CLAMP_PROBE] new high-water populated lights/object = %u "
				"(ABI cap %d, runtime clamp %u) after %lu calls\n",
				s_lightClampProbeHighWater, MAX_HW_LIGHTS_IN_WORLD,
				(unsigned)max_num_lights, s_lightClampProbeCalls);
			std::fflush(stderr);
		}
		if ((s_lightClampProbeCalls % 2000) == 0) {
			std::fprintf(stderr,
				"[LIGHT_CLAMP_PROBE] summary: high-water=%u over %lu calls "
				"(exceeds_16=%s)\n",
				s_lightClampProbeHighWater, s_lightClampProbeCalls,
				(s_lightClampProbeHighWater > 16) ? "YES" : "no");
			std::fflush(stderr);
		}
	}

	// T1.15 [SPOT_DIAG v1] pack-probe emit. First-call always-on; summary
	// every 600 calls when env=1.
	++s_spotDiagPackCalls;
	s_spotDiagPackActiveSum += diagActiveLights;
	s_spotDiagPackInactSum  += diagInactiveLights;
	s_spotDiagPackPointSum  += diagPointActive;
	if (!s_spotDiagPackFirstHit) {
		s_spotDiagPackFirstHit = true;
		std::fprintf(stderr,
			"[SPOT_DIAG v1] event=pack_first_shape shape=%p active_lights=%u "
			"inactive_lights=%u point_lights_active=%u\n",
			(void*)lights, diagActiveLights, diagInactiveLights, diagPointActive);
		std::fflush(stderr);
	}
	if (s_spotDiagPackEnabled && (s_spotDiagPackCalls % 600) == 0) {
		double avgA = (double)s_spotDiagPackActiveSum / 600.0;
		double avgI = (double)s_spotDiagPackInactSum  / 600.0;
		double avgP = (double)s_spotDiagPackPointSum  / 600.0;
		std::fprintf(stderr,
			"[SPOT_DIAG v1] event=pack_summary calls=%lu "
			"avg_active_per_shape=%.3f avg_inactive_per_shape=%.3f "
			"avg_point_active_per_shape=%.3f\n",
			s_spotDiagPackCalls, avgA, avgI, avgP);
		std::fflush(stderr);
		s_spotDiagPackActiveSum = 0;
		s_spotDiagPackInactSum  = 0;
		s_spotDiagPackPointSum  = 0;
	}
}



//----------------------------------------------------------------------
// Draws all textures with isTerrain set that are solid first,
// then draws all alpha with isTerrain set.
void MC_TextureManager::renderLists (void)
{
	ZoneScopedN("textureManagerRenderLists");
	// [RENDERLISTS_COST v1] whole-function span; constructed first so its dtor
	// (accumulate + 60-frame emit) runs after every phase span has closed.
	rlcost::TotalSpan _rlTotal(nextAvailableVertexNode,
	                           static_cast<long>(nextAvailableHardwareVertexNode));
	static bool bSkip = true; // used across preamble and Render.3DObjects
	{
	ZoneScopedN("RenderLists.Preamble");
	rlcost::Span _rl(rlcost::kPreamble);
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

	gos_SetRenderState(gos_State_Culling, gos_Cull_CW);
	} // end RenderLists.Preamble

	{
	ZoneScopedN("RenderLists.LightDataUpload");
	rlcost::Span _rl(rlcost::kLightUpload);
    // copy global list of light data into GPU buffer

    // [LIGHTSSBO v1] Upload-size FLOOR retained (NOT a removable UBO-window
    // artifact — falsified 2026-05-17). The engine deliberately tolerates
    // transient over-count lightDataIndex for cull-stale actors whose
    // update() was skipped offscreen (see gos_static_prop_registry.cpp
    // comment "...points at a slot ... beyond the upload count"). The
    // floor guarantees those indices still read valid backing memory
    // instead of past-end (-> zero -> black props). std::max keeps the
    // old max(count, 64) semantics; lightData_ is capacity(128)-sized so
    // sourcing 64 entries is in-bounds.
    // [LIGHTSSBO v2] kLightUploadFloor=64 is PRESERVED: totalBytes stays
    // max(count,64)*stride. When count<64 the padded tail [count..64) lands in
    // the per-frame SUFFIX [prefixBytes..totalBytes) and is uploaded every frame,
    // so the deliberate tolerance of transient over-count lightDataIndex for
    // cull-stale offscreen actors still reads valid backing memory — identical
    // in-bounds guarantee to the legacy whole-buffer upload. The split only
    // changes WHICH bytes re-upload each frame, never the uploaded extent.
    constexpr uint32_t kLightUploadFloor = 64u;
    const size_t lightUploadCount =
        std::max<uint32_t>(lightDataStructuresCount, kLightUploadFloor);
    const size_t totalBytes = lightUploadCount * sizeof(TG_HWLightsData);
    // mc2StaticLightHighWater / mc2ConsumeStaticLightPrefixDirty / the split gate
    // are all file-scope fns defined earlier in this TU — call directly.
    if (mc2StaticLightUploadSplitEnabled() && mc2LightBakeEnabled()) {
        // Prefix = permanent static table [0..S), clamped to the floored count.
        const uint32_t S = mc2StaticLightHighWater();
        const size_t prefixCount = (S < lightUploadCount) ? S : lightUploadCount;
        const size_t prefixBytes = prefixCount * sizeof(TG_HWLightsData);
        const bool prefixDirty = mc2ConsumeStaticLightPrefixDirty();
        gos_LightDataSsbo_UploadSplit(lightData_, prefixBytes, totalBytes, prefixDirty);
    } else {
        gos_LightDataSsbo_Upload(lightData_, totalBytes);   // legacy whole-buffer
    }
	} // end RenderLists.LightDataUpload
    //

    // update scene data uniform buffer
    Stuff::Vector3D cp = eye->getCameraOrigin();
    {
        ZoneScopedN("Camera.SceneDataUpload");
        rlcost::Span _rl(rlcost::kSceneData);
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
    
    

	{
		ZoneScopedN("Render.3DObjects");
		TracyGpuZone("Render.3DObjects");
		rlcost::Span _rl(rlcost::kObj3d);
		gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Obj3d);
		// SAME-ORDER-EXECUTOR-SLICE-2: validate-only ownership of MechOpaque top-level
		// pass. Body sets its own state (no apply); this just validates ambient+FBO.
		render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::OpaqueObject,
		                                          "renderLists_Render.3DObjects");
	for (size_t i = 0; i<nextAvailableHardwareVertexNode; i++)
	{
		if ((masterHardwareVertexNodes[i].flags & MC2_DRAWSOLID) &&
			(masterHardwareVertexNodes[i].shapes))
		{
			gos_SetRenderState(gos_State_AlphaMode, gos_Alpha_OneZero);
			gos_SetRenderState(gos_State_AlphaTest,
				(masterHardwareVertexNodes[i].flags & MC2_ALPHATEST) ? 1 : 0);
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
					shape_renderer.render(rs->vb_, rs->ib_, rs->vdecl_, texture, rs->light_data_buffer_index_, rs->isHudElement_, rs);
				}

			}

			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterHardwareVertexNodes[i].currentShape = masterHardwareVertexNodes[i].shapes;
			//masterHardwareVertexNodes[i].numShapes = 0;
		}
	}
	gos_SetRenderState(gos_State_AlphaTest, 0);
	gos_render_pass_timer::End(gos_render_pass_timer::Pass_Obj3d);
	} // end Render.3DObjects zone
	drainGLErrors("objects_3d");

	{
	ZoneScopedN("RenderLists.PostObjectsStateRestore");
	rlcost::Span _rl(rlcost::kStateRestore);
	// [Moved in Phase 4 debug] flush() was originally here (after
	// Render.3DObjects). But Render.TerrainSolid runs AFTER us on line
	// ~1287, so terrain was overwriting our building pixels. Flush is
	// now relocated further down, after Render.TerrainSolid completes.

	// restore state as all old-style geometry is culled on CPU and all vertices are already pretransformed
	gos_SetRenderState(gos_State_Culling, gos_Cull_None);

	// restore viewport
	gos_SetRenderViewport(0, 0, Environment.drawableWidth, Environment.drawableHeight);
	} // end RenderLists.PostObjectsStateRestore

	// VPL-#shadow Phase 1+2 (arch-doc docs/plans/static-terrain-shadow-
	// architecture.md): build the static terrain shadow from the FULL map
	// ONCE. Was: a prime + a camera-windowed accumulate behind a >100u
	// camera-move gate -> the shadow FBO was fed only the ~110 visible
	// terrain nodes and never cleared -> a near-empty depth atlas ->
	// soft half-map shadow wash. Root cause is feed-scope and was
	// probe-proven: the world-fixed ortho light matrix is correct and
	// built-once ([SHADOWFRUSTUM v1] n=1 mapHalfExtent=6400
	// orthoHalf=9503.5; build & sample share getLightSpaceMatrix()).
	// Phase 1 retires the prime block, the camera-motion gate, and the
	// gos_*ShadowRebuild* API (this was their only caller). The build is
	// gated solely by the gos_StaticLightMatrixBuilt() latch, which
	// Terrain::destroy re-arms per mission (C-1) so mission 2+ rebuilds
	// against fresh blocks[]. The MapData full-map feed is stock-safe
	// (no-ops if blocks[] unallocated -> shadow simply absent, never a
	// crash, never worse than a missing shadow).
	if (gos_IsTerrainTessellationActive() && !gos_StaticLightMatrixBuilt() &&
	    Terrain::mapData) {
		ZoneScopedN("Shadow.StaticFullMapBuild");
		TracyGpuZone("Shadow.StaticFullMapBuild");
		rlcost::Span _rl(rlcost::kStaticShadowBuild);
		gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_ShadowStatic);

		gos_BuildStaticLightMatrix();   // world-fixed, camera-independent
		gos_MarkStaticLightMatrixBuilt();

		// Any valid terrain colormap: the shadow prepass is depth-only
		// (shadow_terrain.tese = plain lightSpaceMatrix*worldPos, not
		// sampled for depth), so the exact texture is irrelevant -- bind
		// the first terrain node's, else the solid default (idx 0).
		unsigned long shTex = masterTextureNodes[0].get_gosTextureHandle();
		for (long si = 0; si < nextAvailableVertexNode; si++) {
			if ((masterVertexNodes[si].flags & MC2_DRAWSOLID) &&
				(masterVertexNodes[si].flags & MC2_ISTERRAIN)) {
				shTex = masterTextureNodes[masterVertexNodes[si].textureIndex].get_gosTextureHandle();
				break;
			}
		}

		gos_BeginShadowPrePass(true);   // one-shot clear (no accumulate)
		Terrain::mapData->renderStaticTerrainShadowFullMap(indexArray, shTex);
		gos_EndShadowPrePass();

		// SHADOW-STATIC-BUILDINGS-2: append rigid BUILDING casters to the world-
		// fixed static shadow map, ONCE (same one-shot build block as terrain).
		// Source = the FULL registry (all buildings, visibility-independent, baked
		// at mission-load registerStatic), NOT per-frame visible buckets (Option B's
		// failure). Trees excluded by the Building population filter. Appends to the
		// terrain depth (gos_BeginShadowPrePass(false) = no clear). Gate
		// MC2_STATIC_PROP_BUILDING_SHADOW=1 (default OFF). Relies on C-pre min-combine
		// (7ea32b83) so a building in both this and the dynamic bounded-near map does
		// not double-darken terrain. Only runs when the prepass actually activates.
		{
			static const bool s_bldgStaticShadow =
				(getenv("MC2_STATIC_PROP_BUILDING_SHADOW") != nullptr &&
				 getenv("MC2_STATIC_PROP_BUILDING_SHADOW")[0] != '0');
			if (s_bldgStaticShadow) {
				const bool s_sbTxmTrace =
					(getenv("MC2_STATIC_PROP_BUILDING_SHADOW")[0] == '2');
				if (gos_BeginShadowPrePass(false)) {  // append, no clear
					// Local (not static): runs once per mission; getBuildingShadowInstances
					// clears+fills it. No need to retain capacity for the process lifetime.
					std::vector<GpuStaticPropInstance> bldgShadowInstances;
					GpuStaticPropRegistry::getBuildingShadowInstances(bldgShadowInstances);
					if (s_sbTxmTrace) {
						fprintf(stderr, "[SHADOW_STATIC_BLDG_TXM] prepass=1 instances=%zu\n",
							bldgShadowInstances.size());
						fflush(stderr);
					}
					GpuStaticPropBatcher::instance().drawStaticBuildingShadows(bldgShadowInstances);
					gos_EndShadowPrePass();
				} else if (s_sbTxmTrace) {
					fprintf(stderr, "[SHADOW_STATIC_BLDG_TXM] prepass=0 (shadows off, skipped)\n");
					fflush(stderr);
				}
			}
		}
		gos_render_pass_timer::End(gos_render_pass_timer::Pass_ShadowStatic);
	}
	// STATIC-PROP SHADOW-ORDER FIX (2026-05-29): inject static-registry instances
	// into batcher buckets BEFORE flushShadow() below. flushShadow() calls
	// uploadAllBucketsIfNeeded(), which uploads s_bucketsByType and LOCKS the
	// per-frame SSBO slot (s_lastUploadedSlot=s_frameSlot). If the registry flush
	// ran only at its old site (after this shadow block, before the main flush()),
	// terrain-object instances (trees/fences/prop-buildings) arrived AFTER the slot
	// was locked; the idempotency guard then skipped re-upload, so their typeIDs
	// were absent from s_typeRanges -> zero instances -> INVISIBLE whenever
	// MC2_SHADOW_ENABLE=1. (True buildings submit via submitMultiShape during
	// Render.3DObjects, so they were already in the buckets and unaffected.)
	// Running the registry flush here makes the single shadow-time upload include
	// terrain objects (they now also cast static shadows). Gate mirrors the
	// Render.GpuStaticProps block; runs regardless of tessellation/mech state.
	{
		ZoneScopedN("RenderLists.StaticPropRegistryFlush");
		rlcost::Span _rl(rlcost::kSpRegistryFlush);
		extern bool g_useGpuStaticProps;
		extern bool g_useGpuObjects;
		if (g_useGpuStaticProps || g_useGpuObjects) {
			// GPU-ORDERING-PROBE-1: glFinish() before static-prop flush to confirm
			// whether the flash is a pipeline ordering / missing-barrier issue.
			// If glFinish() here stops the flash → cause is a GPU sync gap introduced
			// by d65552ab's new GL_SHADER_STORAGE_BARRIER_BIT calls changing AMD
			// pipeline scheduling for s_indirectCmdBuf visibility on camera-move frames.
			// Kill this probe once root cause is identified.
			static const bool s_finishProbe = (getenv("MC2_FLUSH_PROBE") != nullptr);
			if (s_finishProbe) glFinish();
			GpuStaticPropRegistry::flush();
		}
	} // end RenderLists.StaticPropRegistryFlush
	// GPU-driven dynamic sun shadow (Phase 1): frustum-fit + flushShadow.
	// Runs BEFORE gpu_cull::compute_dispatch so the static-prop shadow uses the
	// full camera-visible per-type ranges (not the cull-narrowed indirect).
	// Casters = the camera-visible (inView) batched set (Phase 1 scope; the
	// off-screen-caster low-sun shadow is the documented Phase-2 gap).
	{
		ZoneScopedN("RenderLists.DynamicShadowPass");
		rlcost::Span _rl(rlcost::kDynShadow);
		gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_ShadowDyn);
		extern bool g_useGpuObjects;
		extern bool g_useGpuMechs;
		if (gos_IsTerrainTessellationActive() && (g_useGpuObjects || g_useGpuMechs)) {
			// Unproject 8 GL-NDC corners through the inverse of worldToClipGL().
			// SHADOW-DYNAMIC-PROJECTION-FIX-1: worldToClipGL() maps MC2 world (Z-up)
			//   = kAxisSwapMC2toGL * worldToCam * cameraToClipGL
			// -> GL clip (NDC xy[-1,1], z[0,1], w>0 in front), with the MC2->GL axis
			// swap (x'=-x, y'=z, z'=y) ALREADY BAKED IN. So inverting it maps a GL-NDC
			// corner straight back to MC2 world: NO manual (-x,z,y) swizzle and NO w<0
			// negate (this matrix emits positive-w-in-front).
			//
			// PRIOR BUG: the code inverted getWorldToClip() (D3D pixel-homogeneous,
			// Y-down, w<0 in front) but fed it the GL-NDC cube below. GL-NDC and that
			// native clip differ by kPixelHomogToGLNDC (Y-flip + x offset + w sign-flip,
			// camera.cpp:142-156), never applied -> the unprojected box was mirrored/
			// offset, only ~correct when the frustum was axis-aligned/centered. That is
			// why trees shadowed only at certain camera angles.
			static const float ndc[8][3] = {
				{-1.0f,-1.0f, 0.0f},{ 1.0f,-1.0f, 0.0f},
				{-1.0f, 1.0f, 0.0f},{ 1.0f, 1.0f, 0.0f},
				{-1.0f,-1.0f, 1.0f},{ 1.0f,-1.0f, 1.0f},
				{-1.0f, 1.0f, 1.0f},{ 1.0f, 1.0f, 1.0f}
			};
			float cornersMC2[8][3];
			// Invert(src) stores the inverse of src into *this (matrix.hpp:584).
			Stuff::Matrix4D clipToWorldGL;
			clipToWorldGL.Invert(eye->worldToClipGL());
			for (int c = 0; c < 8; ++c) {
				Stuff::Vector4D in, out;
				in.x = ndc[c][0]; in.y = ndc[c][1]; in.z = ndc[c][2]; in.w = 1.0f;
				out.Multiply(in, clipToWorldGL);      // row-vector * matrix; result is MC2-world homogeneous
				float inv = (fabsf(out.w) > 1e-6f) ? (1.0f / out.w) : 0.0f;
				cornersMC2[c][0] = out.x * inv;       // already MC2 (axis swap baked into worldToClipGL)
				cornersMC2[c][1] = out.y * inv;
				cornersMC2[c][2] = out.z * inv;
			}
			float lx, ly, lz;
			gos_GetTerrainLightDir(&lx, &ly, &lz);   // same accessor used by old shim
			// Focus source = the camera ORBIT TARGET (getPosition), not a
			// screen-center terrain raycast. The orbit target is yaw-invariant
			// MC2-world east/north (.x=east WU, .y=north WU; yaw is applied only
			// to the eye direction, never to position) and moves linearly with
			// panning, so the cascade no longer skews with the horizon or with
			// facing. Z = cameraShiftZ (terrain elevation at the target, already
			// clamped up to waterElevation), giving the correct water-clamped
			// look-at height in the same MC2 world space as cornersMC2
			// (x=east,y=north,z=elev). Always valid -- no raycast fallback needed.
			Stuff::Vector3D camTgt = eye->getPosition();        // MC2-world orbit target (east,north)
			float shadowCenterXYZ[3] = { camTgt.x, camTgt.y, eye->getCameraShiftZ() };
			bool scOk = true;                                   // camera target always valid
			gos_BuildDynamicLightMatrix(-lx, -ly, -lz, cornersMC2,
			                            shadowCenterXYZ, scOk);  // sign matches old shim
			// SAME-ORDER-EXECUTOR-SLICE-2: validate-only ownership of the Shadow top-level
			// pass (dynamic shadow only — static is once/mission, not wrapped here).
			// Body sets its own state (FBO bind, depth, lightSpaceMatrix); we just validate.
			render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::ShadowCaster,
			                                          "renderLists_gos_BeginDynamicShadowPass");
			gos_BeginDynamicShadowPass();             // no-op if shadowsEnabled_ false
			// Item 1 P1: caster set used by the CSM cascade replay (set in the
			// prop-caster path below; nullptr => CSM replay uses flushShadow).
			const std::vector<GpuStaticPropInstance>* csmCasterSet = nullptr;
			// SHADOW-STATIC-BUILDINGS-2: when buildings cast via the world-fixed
			// static map, skip them in the dynamic pass to avoid a redundant fuzzy
			// double-shadow on buildings (trees/mechs still cast dynamically).
			static const bool s_skipBldgInDynamic =
				(getenv("MC2_STATIC_PROP_BUILDING_SHADOW") != nullptr &&
				 getenv("MC2_STATIC_PROP_BUILDING_SHADOW")[0] != '0');
			// SHADOW-DYNAMIC-PROP-CASTERS-1 (gate, DEFAULT ON; =0 kill-switch): the camera-visible
			// flushShadow feed (s_typeRanges) only admits props the camera frustum
			// cull marked visible this frame, so most trees never cast into the (now
			// correctly camera-fit) dynamic map -- only the few nearest the camera.
			// When enabled, replay ALL registered NON-building props from the
			// registry (visibility-independent) into the dynamic map instead, so
			// every tree/prop in the light box casts. Buildings keep casting via the
			// world-fixed static map, so this pairs with MC2_STATIC_PROP_BUILDING_SHADOW
			// (with that OFF, buildings get no dynamic shadow -- intended for the
			// combined config). flushShadow is skipped entirely in this mode (it would
			// only double-draw the camera-visible subset). Mechs still cast via their
			// own batcher. Gate OFF -> byte-identical to prior behavior.
			static const bool s_dynPropCasters =
				!(getenv("MC2_SHADOW_DYNAMIC_PROP_CASTERS") != nullptr &&
				 getenv("MC2_SHADOW_DYNAMIC_PROP_CASTERS")[0] == '0');  // DEFAULT ON; =0 kill-switch
			if (s_dynPropCasters) {
				static std::vector<GpuStaticPropInstance> s_dynPropInsts; // reused; accessor clears
				// Include buildings in the dynamic feed ONLY when the static building
					// map is NOT active (else buildings would double-shadow). This keeps
					// buildings casting under bare MC2_SHADOW_ENABLE (no regression).
					const bool includeBldg = !s_skipBldgInDynamic;
					// SHADOW-DYNAMIC-PROP-DIRTY-ONLY-1 (gate, DEFAULT OFF; set env to enable):
					// The caster set (registry-indexed instances: modelMatrix + typeID) is
					// REGISTRY-STATIC — it changes only when a prop spawns, despawns, or has
					// an immutable-field write, all of which bump s_registryGeneration. The
					// shadow pass uses only modelMatrix+typeID from the instances (no lighting
					// data, no per-frame color offset), so the vector is safe to cache across
					// frames as long as the generation is unchanged.
					// Cache policy: rebuild when (a) generation changes, or (b) includeBldg
					// toggles (env-gated at startup — in practice stable per session, but
					// compare it anyway). On a clean scene getDynamicPropShadowInstances walks
					// the full s_recipeRanges vector (~14K recipes); skipping it saves ~120-
					// 150µs of cache-cold memory traffic per frame.
					// NOTE: includeBldg is captured from a static at the outer scope, so it
					// is also effectively constant per session; the extra compare is free.
					static const bool s_dynPropDirtyOnly = []() {
						// DEFAULT-ON since 2026-06-04 (smoke-clean mc2_24; proven
						// s_registryGeneration dirty-only pattern, shadow pass reads only
						// modelMatrix+typeID): only literal "0" opts out.
						const char* v = getenv("MC2_SHADOW_DYNAMIC_PROP_DIRTY_ONLY");
						return !(v && v[0] == '0' && v[1] == '\0');
					}();
					static uint64_t  s_dynPropInstsGeneration = UINT64_MAX; // sentinel: force first build
					static bool      s_dynPropInstsIncludeBldg = false;
					if (!s_dynPropDirtyOnly ||
					    GpuStaticPropRegistry::getRegistryGeneration() != s_dynPropInstsGeneration ||
					    includeBldg != s_dynPropInstsIncludeBldg) {
						GpuStaticPropRegistry::getDynamicPropShadowInstances(s_dynPropInsts, includeBldg);
						s_dynPropInstsGeneration  = GpuStaticPropRegistry::getRegistryGeneration();
						s_dynPropInstsIncludeBldg = includeBldg;
					}

					// SHADOW-CASTER-LIGHTBOX-CULL-1 (gate MC2_SHADOW_CASTER_LIGHTBOX_CULL,
					// DEFAULT OFF). The caster set above is the WHOLE registry (~14K props,
					// visibility-independent). The dynamic shadow map is camera-fit (small,
					// esp. under MC2_SHADOW_BOUNDED_NEAR_FIT), so thousands of off-map props
					// draw shadows that never land in the map = wasted GPU fill. When ON,
					// filter the casters to only those whose origin projects inside the
					// dynamic shadow frustum (+margin), using the EXACT same matrix the
					// shadow VS uses (getDynamicLightSpaceMatrix(), column-major, GL_FALSE).
					//
					// World-position frame: shaders/shadow_static_prop.vert computes
					//   worldStuff = vec4(a_position,1) * modelMatrix   (row-vector * M)
					//   worldMC2   = vec3(-worldStuff.x, worldStuff.z, worldStuff.y)
					//   gl_Position = lightSpaceMatrix * vec4(worldMC2,1)
					// For the local origin a_position=(0,0,0): worldStuff = (M[3],M[7],M[11])
					// in the column-major float[16] (v*M picks element c*4+3), so
					//   worldMC2 = (-M[3], M[11], M[7])
					// This is EXACTLY the registry's actorWorldCenter extraction
					// (gos_static_prop_registry.cpp:842-847) and the frame the light matrix
					// consumes — verified against both the VS and the SHADOWZRANGE probe.
					static const bool s_casterLightboxCull = []() {
						// DEFAULT ON since 2026-06-16 (render-hygiene-s1). Off-map props
						// never land their shadows on the playfield — filtering them saves
						// ~0.2-0.4ms GPU fill on prop-heavy missions (mc2_24). Kill: =0.
						const char* v = getenv("MC2_SHADOW_CASTER_LIGHTBOX_CULL");
						return !(v && v[0] == '0' && v[1] == '\0');
					}();
					const std::vector<GpuStaticPropInstance>* dynShadowSet = &s_dynPropInsts;
					if (s_casterLightboxCull) {
						ZoneScopedN("Shadow.CasterCull");
						static std::vector<GpuStaticPropInstance> s_culledDynPropInsts; // reused, no churn
						// SHADOW-CASTER-CULL-CACHE-1 (TXMMGR-PERF-EASYWINS-1, gate
						// MC2_SHADOW_CASTER_CULL_CACHE, DEFAULT OFF): the cull result is a
						// pure function of (caster set, light-space matrix, margin). The
						// caster set is generation-keyed (rebuilt above only on
						// s_registryGeneration change) and the matrix is camera-fit — on a
						// stationary camera its 16 floats are bit-identical across frames.
						// Reuse the previous culled vector when BOTH are unchanged; any
						// camera motion or registry mutation recomputes exactly as before.
						// margin + includeBldg are session-static (env-init'd) — no key part.
						static const bool s_cullCacheEnabled = []() {
							const char* v = getenv("MC2_SHADOW_CASTER_CULL_CACHE");
							return v && v[0] != '0';
						}();
						static uint64_t s_cullCacheGen = UINT64_MAX;   // sentinel: no cache
						static float    s_cullCacheM[16] = {};
						static const float s_cullMargin = []() {
							const char* v = getenv("MC2_SHADOW_CASTER_CULL_MARGIN");
							const float m = (v && v[0]) ? static_cast<float>(atof(v)) : 0.25f;
							return (m >= 0.0f) ? m : 0.25f;
						}();
						static const bool s_cullDebug =
							(getenv("MC2_SHADOW_CULL_DEBUG") != nullptr);
						gosPostProcess* ppCull = getGosPostProcess();
						const float* M = ppCull ? ppCull->getDynamicLightSpaceMatrix() : nullptr;
						if (!M) {
							// No matrix yet (early frames) -> keep everything (safe).
							dynShadowSet = &s_dynPropInsts;
						} else if (s_cullCacheEnabled &&
						           s_cullCacheGen == GpuStaticPropRegistry::getRegistryGeneration() &&
						           0 == memcmp(s_cullCacheM, M, sizeof(s_cullCacheM))) {
							// Cache HIT: same caster generation + bit-identical light matrix
							// -> the cull result is unchanged; reuse the retained vector.
							dynShadowSet = &s_culledDynPropInsts;
						} else {
							s_culledDynPropInsts.clear();
							s_culledDynPropInsts.reserve(s_dynPropInsts.size());
							const float lim = 1.0f + s_cullMargin;
							int dbgN = 0;
							for (const GpuStaticPropInstance& inst : s_dynPropInsts) {
								const float* mm = inst.modelMatrix;
								// world (MC2 frame the light matrix consumes)
								const float wx = -mm[3];
								const float wy =  mm[11];
								const float wz =  mm[7];
								// clip = M * (wx,wy,wz,1), M column-major: (row r,col c)=M[c*4+r]
								const float cx = M[0*4+0]*wx + M[1*4+0]*wy + M[2*4+0]*wz + M[3*4+0];
								const float cy = M[0*4+1]*wx + M[1*4+1]*wy + M[2*4+1]*wz + M[3*4+1];
								const float cw = M[0*4+3]*wx + M[1*4+3]*wy + M[2*4+3]*wz + M[3*4+3];
								bool keep = true;
								if (cw > 0.0f) {
									const float ndcx = cx / cw;
									const float ndcy = cy / cw;
									keep = (ndcx >= -lim && ndcx <= lim &&
									        ndcy >= -lim && ndcy <= lim);
								} else {
									// behind the light near plane: cannot land in the map.
									keep = false;
								}
								if (s_cullDebug && dbgN < 8) {
									++dbgN;
									const float ndcx = (cw != 0.0f) ? cx / cw : 0.0f;
									const float ndcy = (cw != 0.0f) ? cy / cw : 0.0f;
									fprintf(stderr,
										"[SHADOW_CULL_DBG] world=(%.1f,%.1f,%.1f) "
										"clipW=%.3f ndc=(%.3f,%.3f) keep=%d\n",
										wx, wy, wz, cw, ndcx, ndcy, keep ? 1 : 0);
									fflush(stderr);
								}
								if (keep) s_culledDynPropInsts.push_back(inst);
							}
							dynShadowSet = &s_culledDynPropInsts;
							if (s_cullCacheEnabled) {
								s_cullCacheGen = GpuStaticPropRegistry::getRegistryGeneration();
								memcpy(s_cullCacheM, M, sizeof(s_cullCacheM));
							}

							static bool s_cullLogged = false;
							if (!s_cullLogged) {
								s_cullLogged = true;
								const size_t total = s_dynPropInsts.size();
								const size_t kept  = s_culledDynPropInsts.size();
								const float pct = total ? (100.0f * (float)(total - kept) / (float)total) : 0.0f;
								fprintf(stderr,
									"[SHADOW_CULL] casters %zu -> %zu (%.0f%% culled, margin=%.2f)\n",
									total, kept, pct, s_cullMargin);
								fflush(stderr);
							}
						}
					}
				{
				// LANE-D measure-first: give the dynamic prop shadow CASTER draw its
				// own GPU timestamp so its cost is deconflated from GpuSP.BatcherFlush
				// (which previously absorbed it as first-GPU-zone self-time).
				ZoneScopedN("RenderLists.DynShadowDraw");
				TracyGpuZone("GpuSP.DynShadowDraw");
				GpuStaticPropBatcher::instance().drawDynamicPropShadows(*dynShadowSet);
			}
				csmCasterSet = dynShadowSet;   // Item 1 P1: remember for the cascade replay
			} else {
				GpuStaticPropBatcher::instance().flushShadow(s_skipBldgInDynamic);
			}
			GpuMechBatcher::instance().flushShadow();
			gos_EndDynamicShadowPass();
			render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::ShadowCaster,
			                                        "renderLists_gos_EndDynamicShadowPass");

			// Item 1 P1: replay casters into each CSM array layer. The legacy
			// single-map pass above is fully complete (props+mech+EndPass restored
			// state). Each beginDynamicShadowCascade binds the array layer + sets
			// the active cascade matrix (via getDynamicLightSpaceMatrix resolving to
			// the active cascade) and clears forward-Z. Skipped entirely (no GL) and
			// byte-identical when MC2_SHADOW_CSM is OFF. csmCasterSet is non-null in
			// the prop-caster path; otherwise we fall back to flushShadow per layer.
			if (mc2ShadowCsmEnabled()) {
				gosPostProcess* ppCsm = getGosPostProcess();
				if (ppCsm && ppCsm->getDynamicShadowArrayTexture()) {
					ZoneScopedN("RenderLists.DynShadowCSM");
					const int csmN = ppCsm->getDynamicShadowCascadeCount();
					for (int ci = 0; ci < csmN; ++ci) {
						if (!ppCsm->beginDynamicShadowCascade(ci)) continue;
						if (csmCasterSet)
							GpuStaticPropBatcher::instance().drawDynamicPropShadows(*csmCasterSet);
						else
							GpuStaticPropBatcher::instance().flushShadow(s_skipBldgInDynamic);
						GpuMechBatcher::instance().flushShadow();
						ppCsm->endDynamicShadowCascadePass();
					}
					// P5 headless log: layers + prop caster-set size fed to each
					// cascade (per-cascade containment is decided GPU-side in the
					// shader; this proves the array pass ran with casters).
					static bool s_csmLogged = false;
					if (!s_csmLogged) {
						s_csmLogged = true;
						const size_t nCasters = csmCasterSet ? csmCasterSet->size() : 0;
						fprintf(stderr, "[CSM] layers=%d casters_per_layer=%zu\n",
						        csmN, nCasters);
						fflush(stderr);
					}
				}
			}
		}
		gos_render_pass_timer::End(gos_render_pass_timer::Pass_ShadowDyn);
	}
	g_numShadowShapes.store(0, std::memory_order_relaxed);

	// No special depth state for DRAWSOLID terrain

	{
		ZoneScopedN("Render.TerrainSolid");
	rlcost::Span _rl(rlcost::kTerrainSolid);
		TracyGpuZone("Render.TerrainSolid");
		gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_TerrainSolid);

		// Modern path. flush() returns true on success and false on overflow
		// / not-ready / not-killswitched. On false we fall through to the
		// legacy loop for the WHOLE FRAME — never partial-frame. The legacy
		// ring data has been kept in sync by addVertices/fillTerrainExtra
		// running unconditionally in quad.cpp.
		// Bucket-census instrumentation (env-gated MC2_BUCKET_CENSUS=1).
		// Count legacy-eligible nodes BEFORE flush() runs (both the legacy
		// draw branch at ~1369 and the alternate reset branch at ~1382
		// zero currentVertex per node, so end-of-zone undercounts). The
		// filter mirrors the legacy DRAWSOLID|ISTERRAIN draw-emission
		// predicate so it is apples-to-apples with PatchStream's modern
		// scope.
		static const bool s_bucketCensusOn =
			(getenv("MC2_BUCKET_CENSUS") != NULL);
		uint32_t legacyEligible = 0;
		if (s_bucketCensusOn) {
			for (long ci = 0; ci < nextAvailableVertexNode; ++ci) {
				const DWORD cf = masterVertexNodes[ci].flags;
				if ((cf & MC2_DRAWSOLID) && (cf & MC2_ISTERRAIN) &&
				    masterVertexNodes[ci].vertices &&
				    masterVertexNodes[ci].currentVertex !=
				        masterVertexNodes[ci].vertices)
				{
					++legacyEligible;
				}
			}
		}

		bool modernHandled = false;

		// TERRAIN-CULL-STATE-PROBE-1 (MC2_TERRAIN_CULL_PROBE, default OFF): the
		// terrain-solid dispatch CHOKEPOINT — runs every frame terrain renders,
		// before the indirect/patch-stream/legacy branch. None of those paths set
		// cull, so the ambient cull/frontFace read HERE is what terrain renders
		// under. Read-only; first few frames then stops.
		{
			static const bool s_cullProbe = (std::getenv("MC2_TERRAIN_CULL_PROBE") != nullptr);
			static int s_cullProbeFrames = 0;
			if (s_cullProbe && s_cullProbeFrames < 8) {
				++s_cullProbeFrames;
				GLboolean cullOn = glIsEnabled(GL_CULL_FACE);
				GLint cullMode = 0, frontFace = 0;
				glGetIntegerv(GL_CULL_FACE_MODE, &cullMode);
				glGetIntegerv(GL_FRONT_FACE, &frontFace);
				std::fprintf(stderr,
					"[TERRAIN_CULL_PROBE] path=dispatch frame=%d cull=%s cullMode=%s frontFace=%s\n",
					s_cullProbeFrames,
					cullOn ? "ENABLED" : "disabled",
					cullMode == GL_BACK ? "BACK" : cullMode == GL_FRONT ? "FRONT" :
						cullMode == GL_FRONT_AND_BACK ? "FRONT_AND_BACK" : "?",
					frontFace == GL_CCW ? "CCW" : frontFace == GL_CW ? "CW" : "?");
				std::fflush(stderr);
			}
		}

		if (gos_terrain_indirect::IsFrameSolidArmed()) {
			// Indirect SOLID owns this frame. The SOLID gate-off in setupTextures()
			// already fired, so TerrainPatchStream has no SOLID records — do NOT fall
			// back to flush() when DrawIndirect returns false (plan v2 advisor
			// stop-the-line #1). A false return is a hard failure: logged, arming
			// disabled process-wide; operator advice in event=hard_failure line.
			//
			// Phase 7.5 LOD chunk coexistence: when MC2_TERRAIN_LOD_CHUNK=1 the chunk
			// path owns terrain rendering (flushDrawCommands in gamecam.cpp). Suppress
			// DrawIndirect to prevent depth/color conflicts — both paths write the same
			// world-space surface and the indirect path draws first (land->render),
			// causing the coarser LOD chunk mesh to fail the depth test.
			if (mc2TerrainLodChunkEnabled()) {
				modernHandled = true;   // chunks handle it (existing)
			} else {
				// INDIRECT-BRIDGE-RETIRE-1: indirect-bridge SOLID terrain draw retired (deprecated branch,
				// terrain_path.indirect proven 0 in default/smoke/capture/editor). LOD-chunk owns terrain.
				// Do NOT DrawIndirect(); fire the telemetry counter as a regression tripwire. modernHandled
				// MUST be true here: on armed frames setupTextures already fired SOLID gate-off so PatchStream
				// has no SOLID records — a false value would fall through to the (now terrain-skipping) shared
				// loop and could mis-fire the LegacyMLR tripwire. NOTE: DrawIndirect() itself is RETAINED
				// (RenderWaterReflectionPass calls it internally, gos_terrain_indirect.cpp:3817); only this
				// txmmgr SOLID-draw caller is retired.
				RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::IndirectBridge);
				modernHandled = true;
			}
		} else if (TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
			// PATCHSTREAM-THIN-RETIRE-1: patch-stream-thin terrain draw retired (deprecated branch,
			// terrain_path.patch_stream proven 0 in default/smoke/capture/editor). LOD-chunk owns
			// terrain. Do NOT flush()-draw; fire the telemetry counter as a regression tripwire so
			// patch_stream>0 surfaces if a reachable config ever needed it. modernHandled stays false
			// (no gate-off fired for un-armed frames) — matching the prior flush()-returns semantics
			// when it drew nothing.
			// NOTE: flush() side-effects audit — flush() does: (a) draw calls, (b) glFenceSync on
			// current slot, (c) markTerrainDrawn() on post-process, (d) noteTerrainPath(PatchStreamThin).
			// The ring slot advance is in beginFrame() NOT flush(), so skipping flush() does not break
			// the ring. The getLastFlush* snapshot getters read statics reset by beginFrame() each frame
			// — they will correctly read 0 when this branch is not flushing. markTerrainDrawn() is NOT
			// called here because no terrain was drawn. NO load-bearing non-draw side effects are lost.
			RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::PatchStreamThin);
		}

		// RENDER-FRAME-PLAN-SCAFFOLD-1: tattle WHICH terrain-solid branch drew this
		// frame. This is the rake-prevention trace — capture takes the legacy MLR
		// branch (modern bridge not armed), which bound TerrainSolid 0x and confused
		// the routing recon. Observe only; no behavior change.
		if (render_frame_plan::traceEnabled()) {
			using namespace render_frame_plan;
			const bool armed = gos_terrain_indirect::IsFrameSolidArmed();
			if (modernHandled && armed && !mc2TerrainLodChunkEnabled())
				trace(Phase::TerrainOpaque, "TerrainSolid", PathKind::ApplyPipeline, -1, "TerrainSolid");      // modern indirect bridge (routed)
			else if (modernHandled && armed)
				trace(Phase::TerrainOpaque, "TerrainSolidLODChunk", PathKind::ApplyPipeline, -1, "TerrainSolid");  // LOD-chunk, routed (ROUTING-1)
			else if (modernHandled)
				trace(Phase::TerrainOpaque, "TerrainSolidThin", PathKind::RawGL, -1, "None");                 // un-armed patch-stream flush
			else
				trace(Phase::TerrainOpaque, "TerrainLegacyMLR", PathKind::MLR, -1, "None");                   // legacy master-node fall-through
		}

		// [TERRAIN_SURFACE] PR-2 (Wave 1, ADDITIVE / DEFAULT-OFF / DELETES
		// NOTHING). Screen-agnostic continuous-surface VALIDATION draw: runs
		// on EVERY frame regardless of arming (design Convergence C-1 --
		// surface existence is decoupled from IsFrameSolidArmed). A no-op
		// unless MC2_TERRAIN_SURFACE is set (gos_terrain_surface::IsEnabled,
		// checked inside the bridge), so the default path is byte-for-byte
		// behaviour-neutral. When ON, the surface draws ON TOP of the still-
		// running legacy/indirect terrain above for visual validation of the
		// V-ssbo VS + Fork D clip-space pre-divide reverse-Z bias. NO legacy
		// kill site lands here -- the substitutive draw-kill is PR-4.
		gos_terrain_surface_bridge_draw();

		bool bSkip_DRAWSOLID = false;
		for (long i=0;i<nextAvailableVertexNode && !bSkip_DRAWSOLID;i++)
		{
				if ((masterVertexNodes[i].flags & MC2_DRAWSOLID) &&
					(masterVertexNodes[i].vertices))
				{
					// LEGACY-MLR-DELETE-1: legacy masterVertexNode terrain draw retired. Terrain solid is
					// owned by LOD-chunk (gamecam.cpp:508) / indirect bridge / patch-stream. A terrain node
					// must NOT draw via this shared MC2_DRAWSOLID loop. Always skip terrain here. If a terrain
					// node arrives with modernHandled==false, no modern path drew terrain this frame — a real
					// regression (terrain would be invisible). Fire the LegacyMLR telemetry counter as a loud
					// tripwire so it surfaces in terrain_path.legacy_mlr instead of silently vanishing.
					if (masterVertexNodes[i].flags & MC2_ISTERRAIN) {
						if (!modernHandled)
							RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::LegacyMLR);
						masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
						continue;
					}

					gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
					gos_SetRenderState( gos_State_Terrain, 0 );

					DWORD totalVertices = masterVertexNodes[i].numVertices;
					if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
					{
						totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
					}

					// LEGACY-MLR-DELETE-1: dead (terrain skipped above); non-terrain nodes never have terrain extras.
					gos_SetTerrainBatchExtras(NULL, 0);

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
		// Emit one [BUCKET_CENSUS v1] line per frame (env-gated). Runs
		// after flush() has populated the modern-side stats and after
		// either branch has reset the legacy ring; legacy_eligible was
		// captured pre-flush above. emitCensus() is a no-op when the
		// env var is unset.
		if (s_bucketCensusOn) {
			TerrainPatchStream::emitCensus(legacyEligible);
		}
		gos_render_pass_timer::End(gos_render_pass_timer::Pass_TerrainSolid);
	}   // end Render.TerrainSolid zone
	drainGLErrors("terrain");

	// Task 10 flush() — moved here from after Render.3DObjects because
	// terrain renders AFTER 3D objects in this codebase; placing our
	// flush earlier meant terrain overwrote our pixels. Running after
	// terrain but before overlays gives buildings the right layering:
	// on top of terrain, below decals/roads.
	{
		ZoneScopedN("Render.GpuStaticProps");
		TracyGpuZone("Render.GpuStaticProps");
		gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_SpColor);
		extern bool g_useGpuStaticProps;
		extern bool g_useGpuObjects;
		if (g_useGpuStaticProps || g_useGpuObjects) {
			// [RENDERLISTS_COST v1] prep+cull-dispatch only; the two flush units
			// below carry their own spans (no double-count vs total).
			rlcost::Span _rl(rlcost::kGpuSpPrep);
			// Stage 3.C registry flush MOVED earlier (before flushShadow, see
			// STATIC-PROP SHADOW-ORDER FIX 2026-05-29) so terrain-object instances
			// are in s_bucketsByType before the shadow-time SSBO upload locks the
			// slot. It injects static-registry instances into batcher buckets AND
			// appends Cat_StaticProp substrate records for the cull shader; both are
			// now done above, before this block. Do NOT re-add a flush() call here
			// (double-flush). compute_dispatch() below still runs after it.

			// Step 4.6 (global-pool slice 1): compute per-cmd baseInstance prefix-sum
			// and advance the coalesce ring slot BEFORE compute_dispatch() so the
			// patch shader can read baseInstanceByCmd[] in the same dispatch.
			// [SPFLUSH_COST_SPLIT v1] baseinstance_upload span (nifty) wrapped in
			// the GpuSP.PrepBaseInstance Tracy zone (override branch).
			{
				ZoneScopedN("GpuSP.PrepBaseInstance");
				const unsigned long long _t_bi0 = s_spflushTxmEnabled ? __rdtsc() : 0ULL;
				batcher_prepareBaseInstanceTable();
				if (s_spflushTxmEnabled) s_spflush_baseinstance_upload_cyc += __rdtsc() - _t_bi0;
			}

			// C1b GPU authority flip: compute_dispatch() is now called HERE
			// (moved from mission.cpp) so it processes BOTH dynamic actor records
			// (from substrate_flushUpload in objmgr::update) AND the static prop
			// records appended by GpuStaticPropRegistry::flush() above.
			// The patch shader then writes GPU-computed instanceCounts into the
			// indirect command buffer. GpuStaticPropBatcher::flush() below uses
			// glMultiDrawElementsIndirect which reads those GPU-authoritative counts.
#if defined(MC2_SUBSTRATE_COUNT_PARITY)
				gpu_cull::substrate_countParityCheck();
#endif
			// M1 FROZEN-STATIC-CULL-RECORDS: build + install the frozen static
			// cull-record prefix now that baseInstanceForType is valid (set by
			// batcher_prepareBaseInstanceTable above) and before compute_dispatch
			// consumes the records. No-op on clean frames / unless the gate is set.
			GpuStaticPropRegistry::buildStaticPrefixGolden();

			if (gpu_cull::compute_isEnabled()) {
				ZoneScopedN("GpuSP.CullDispatch");
				TracyGpuZone("GpuSP.CullDispatch");
				gpu_cull::compute_dispatch();
			}

			// CAMERA-MOVE-DIAG: log substrate record count + cull output before/after
			// dispatch to distinguish compute early-return vs frustum-all-fail.
			// Gate: MC2_CAMERA_MOVE_DIAG=1. Causes GL stalls — diagnostic only.
			{
				static const bool s_diagEnabled = (getenv("MC2_CAMERA_MOVE_DIAG") != nullptr);
				if (s_diagEnabled) {
					static float s_lastMvp[16] = {};
					static uint32_t s_frameN = 0;
					extern const float* gos_GetTerrainMVPMat4();
					const float* mvp = gos_GetTerrainMVPMat4();
					bool mvpChanged = mvp && (memcmp(mvp, s_lastMvp, 16 * sizeof(float)) != 0);
					if (mvp && mvpChanged) memcpy(s_lastMvp, mvp, 16 * sizeof(float));
					++s_frameN;
					// Log every camera-move frame; stationary every 120 frames
					bool doLog = mvpChanged || (s_frameN % 120 == 0);
					if (doLog) {
						const uint32_t subRec = gpu_cull::substrate_getCurrentRecordCount();
						const GLintptr  subOff = gpu_cull::substrate_getCurrentSlotOffset();
						glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
						const GLuint bktBuf = gpu_cull::compute_getBucketCountsBuf();
						const GLuint cmdBuf = gpu_cull::compute_getIndirectCmdBuf();
						uint32_t bkt[4] = {};
						if (bktBuf) glGetNamedBufferSubData(bktBuf, 0, sizeof(bkt), bkt);
						uint32_t inst0 = 0;
						if (cmdBuf) glGetNamedBufferSubData(cmdBuf, 4, sizeof(inst0), &inst0); // struct[0].instanceCount
						// Sum ALL bucket counts across all 221+ buckets.
						uint32_t bktTotal = 0;
						const uint32_t nBkts = gpu_cull::compute_getBucketCount();
						if (bktBuf && nBkts > 0) {
							std::vector<uint32_t> allBkt(nBkts + 1u);
							glGetNamedBufferSubData(bktBuf, 0, (nBkts + 1u) * sizeof(uint32_t), allBkt.data());
							for (uint32_t b = 0; b < nBkts; ++b) bktTotal += allBkt[b];
						}
						printf("[DIAG] %s subRec=%u | bkt0=%u bkt1=%u bkt2=%u bkt3=%u bktTOTAL=%u | inst0=%u | mvp[0..3]=[%.3f %.3f %.3f %.3f]\n",
							mvpChanged ? "MOVE  " : "STATIC",
							subRec,
							bkt[0], bkt[1], bkt[2], bkt[3], bktTotal, inst0,
							mvp ? mvp[0] : 0.f, mvp ? mvp[1] : 0.f,
							mvp ? mvp[2] : 0.f, mvp ? mvp[3] : 0.f);
						fflush(stdout);
					}
				}
			}

		}

		// MEASURED-REORDER-SPMECH-1 (tier-C reorder EXPERIMENT, default-OFF):
		// The two opaque flush UNITS below — StaticProp BatcherFlush (+Pass_SpColor
		// timer) and the Mech block (+Pass_Mechs timer) — are the one legal adjacent
		// swap the legal-reorder oracle proved. StaticProp PREP + compute_dispatch
		// (above, inside Render.GpuStaticProps) must stay BEFORE both flushes in
		// EITHER path; only the two flush units swap order. The OpaqueObject
		// begin(executorOwnBeginTopLevel @ Render.3DObjects)/end wrapper brackets
		// both regardless. Gate OFF -> today's order verbatim (byte-identical).
		// Gate ON -> mech flush, then static-prop flush.
		static const bool s_reorderSpMech =
			(getenv("MC2_FRAMEGRAPH_REORDER_SPMECH") != nullptr);

		// Unit A: static-prop batcher flush, scoped by Pass_SpColor. Captured pre-
		// flush eligibility lives in the extern flags read in Render.GpuStaticProps;
		// re-read here (same flags) so the flush still respects the GPU-path gate.
		auto runSpFlushUnit = []() {
			extern bool g_useGpuStaticProps;
			extern bool g_useGpuObjects;
			if (g_useGpuStaticProps || g_useGpuObjects) {
				ZoneScopedN("GpuSP.BatcherFlush");
				TracyGpuZone("GpuSP.BatcherFlush");
				rlcost::Span _rl(rlcost::kSpBatcherFlush);
				GpuStaticPropBatcher::instance().flush(getLastRenderSnapshot());
			}
			// Pass_SpColor ends after the static-prop flush: GL_TIME_ELAPSED
			// scopes cannot nest, so mechs get their own disjoint scope.
			gos_render_pass_timer::End(gos_render_pass_timer::Pass_SpColor);
		};

		// Unit B: GPU mech batcher Slice A flush — runs adjacent to the static-prop
		// flush, inside renderLists() so terrain has already been emitted by the
		// patch stream and the depth state is set up. Independent of
		// g_useGpuStaticProps; gated on its own MC2_GPU_MECHS env var inside the
		// flush itself. Pass_Mechs is a disjoint GL_TIME_ELAPSED scope.
		auto runMechFlushUnit = []() {
			ZoneScopedN("Render.GpuMechs");
			TracyGpuZone("Render.GpuMechs");
			rlcost::Span _rl(rlcost::kMechFlush);
			gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Mechs);
			// [RENDER_PASS v1] advisory telemetry (env-gated, rate-limited).
			// MECHOPAQUE-NOTE-RELOCATE-1: note fires at the real mech GPU draw
			// site (after shadow, after static-prop flush) — NOT at the preamble.
			// FBO/viewport facts here are draw-time (scene FBO + SceneGEqual depth
			// state established by terrain/static-prop passes above).
			render_contract::noteRenderPass(render_contract::PassIdentity::OpaqueObject,
			                               "GpuMechBatcher_flush(submit)");
			// APPLY-STATE-MECHOPAQUE-1: when MC2_FRAMEGRAPH_EXECUTOR is ON and a
			// kTopLevelStateDesc row exists, the executor pre-applies the MechOpaque
			// pipeline here (at the real flush call site, NOT the begin seam which wraps
			// non-mech draws too); flush()'s body then skips its own applyPipeline.
			// Gate OFF -> not called -> body applies -> byte-identical.
			if (render_contract::isTopLevelExecutorEnabled() &&
			    RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::MechOpaque) != nullptr) {
				GpuMechBatcher::instance().executorApplyMechOpaqueState();
			}
			GpuMechBatcher::instance().flush();
			gos_render_pass_timer::End(gos_render_pass_timer::Pass_Mechs);
		};

		if (s_reorderSpMech) {
			// EXPERIMENT (gate-ON): swap the two opaque flush units.
			runMechFlushUnit();
			runSpFlushUnit();
		} else {
			// Default path: today's order verbatim — static-prop flush, then mechs.
			runSpFlushUnit();
			runMechFlushUnit();
		}
		render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::OpaqueObject,
		                                        "renderLists_GpuMechBatcher_flush");
	}

	// DRAWSOLID done

	// B4 Slice Stage 1b — mask-SOLID dual-run dispatch.
	// Draws the same SOLID quads as the legacy drawPass (which is still active
	// in Stage 1b — both run; parity comparator validates the masks match).
	// Default-off: IsFrameMaskSolidArmed() returns false unless
	// MC2_TERRAIN_MASK_DISPATCH=1 AND MC2_TERRAIN_MASK_DISPATCH_SOLID != "0".
	// Pass_Overlays = one coarse scope spanning Render.TerrainMask.Solid
	// through Render.Decals (sub-zones stay Tracy-only).
	// [RENDERLISTS_COST v1] manual span: the overlay region is a run of sibling
	// blocks (mask-solid .. decals), not one lexical scope.
	const unsigned long long _rlOv0 = rlcost::s_enabled ? rlcost::nowNs() : 0ULL;
	gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Overlays);
	{
		ZoneScopedN("Render.TerrainMask.Solid");
		TracyGpuZone("Render.TerrainMask.Solid");
		if (gos_terrain_mask_dispatch::IsMaskDispatchReady()
		 && gos_terrain_mask_dispatch::IsFrameMaskSolidArmed()) {
			gos_terrain_mask_dispatch::DrawMaskSolid();
		}
	}

	// ── New world-space overlay batches ──────────────────────────────────────
	// These draw calls flush batches accumulated during land->render() and
	// craterManager->render().  They set their own GL state and restore it.
	// Render order matches design: terrain → cement overlays → decals → (old overlays).
	{
		ZoneScopedN("Render.TerrainOverlays");
		TracyGpuZone("Render.TerrainOverlays");
		gos_DrawTerrainOverlays();
	}
	// Slice A — cement-overlay static-bake draw. Mirrors the Render.Terrain
	// Mines hook below EXACTLY: gated on IsFrameOverlayArmed() (default OFF
	// unless MC2_TERRAIN_INDIRECT_OVERLAY=1). When armed, the per-quad M2d
	// gos_PushTerrainOverlay producer is skipped (quad.cpp gate-off) and
	// gos_DrawTerrainOverlays above flushes an empty batch (early-return);
	// DrawDecalStatic draws the persistent static bake instead. Placed right
	// after Render.TerrainOverlays so the static cement composites in the
	// same slot the per-frame batch used (before mines/decals/old overlays).
	{
		ZoneScopedN("Render.TerrainOverlaysStatic");
		TracyGpuZone("Render.TerrainOverlaysStatic");
		if (gos_terrain_indirect::IsFrameOverlayArmed()) {
			gos_terrain_indirect::DrawDecalStatic();
		}
	}
	// PR2c Stage 2c — mine static-bake draw. Hooks between TerrainOverlays
	// and Decals so mines composite ABOVE cement/road overlays and BENEATH
	// crater decals (state=2 blown-mine sprites coexist with crater decals).
	// Default-off: IsFrameMineArmed() returns false unless
	// MC2_TERRAIN_INDIRECT_MINE=1 AND the texture-array has been built.
	{
		ZoneScopedN("Render.TerrainMines");
		TracyGpuZone("Render.TerrainMines");
		if (gos_terrain_indirect::IsFrameMineArmed()) {
			gos_terrain_indirect::DrawMineStatic();
		}
	}
	{
		ZoneScopedN("Render.Decals");
		TracyGpuZone("Render.Decals");
		// MC2_DYNAMIC_DECALS: push dynamic impact decals into the batch before flushing.
		// Gate-off: gatherToDecalBatch() is a no-op; gate-on: pushes faded quads.
		// frameLength = duration of last frame in seconds (timing.h).
		DynDecal::gatherToDecalBatch(frameLength);
		gos_DrawDecals();
	}
	gos_render_pass_timer::End(gos_render_pass_timer::Pass_Overlays);
	if (rlcost::s_enabled) rlcost::s_ns[rlcost::kOverlays] += rlcost::nowNs() - _rlOv0;
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
	{
	ZoneScopedN("RenderLists.TerrainAlphaWaterLoops");
	rlcost::Span _rl(rlcost::kWaterLoops);
	gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Water);
    for(int states = 0; states < 2; ++states)
    {
        gos_SetRenderState( gos_State_AlphaTest, states);

        for (int i=0;i<nextAvailableVertexNode;i++)
        {
            if ((masterVertexNodes[i].flags & MC2_ISTERRAIN) &&
                    (masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
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
	gos_render_pass_timer::End(gos_render_pass_timer::Pass_Water);
	} // end RenderLists.TerrainAlphaWaterLoops


	//<< sebi: added this section to draw objects which do not have terrain underlayer (those are added in quad.cpp, see (*) there )
	{ ZoneScopedN("Render.NoUnderlayer");
	  TracyGpuZone("Render.NoUnderlayer");
	  rlcost::Span _rl(rlcost::kNoUnderlayer);
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

	{
	ZoneScopedN("RenderLists.ShadowBlobs");
	rlcost::Span _rl(rlcost::kShadowBlobs);
	gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Blobs);
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
	gos_render_pass_timer::End(gos_render_pass_timer::Pass_Blobs);
	} // end RenderLists.ShadowBlobs


	{
	ZoneScopedN("RenderLists.NonTerrainAlphaLoops");
	rlcost::Span _rl(rlcost::kAlphaLoops);
	gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_AlphaVfx);
	gos_SetRenderState( gos_State_ZCompare, 1);
	if (Environment.Renderer != 3)
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		// Blended alpha must not write depth: these are sorted-alpha nodes,
		// not the opaque/masked prepass. Applies to both the pure-blend
		// (states=0) and alpha-test cutout (states=1) loops, conservatively.
		gos_SetRenderState(	gos_State_ZWrite, 0);
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
	if (Environment.Renderer != 3)
	{
		// Restore legacy depth-write state for downstream passes (they
		// inherit render state from this block).
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}
	gos_render_pass_timer::End(gos_render_pass_timer::Pass_AlphaVfx);
	} // end RenderLists.NonTerrainAlphaLoops

	{
	ZoneScopedN("RenderLists.VfxHudSubmit");
	rlcost::Span _rl(rlcost::kVfxHud);
	gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Hud);
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
	
	// Spotlight cones are blended/additive: no depth writes during the draw.
	// Downstream state is unchanged -- the post-loop reset below already
	// sets ZWrite 0 (legacy behavior).
	gos_SetRenderState(	gos_State_ZWrite, 0);

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
	gos_render_pass_timer::End(gos_render_pass_timer::Pass_Hud);
	} // end RenderLists.VfxHudSubmit
	} // end Render.Overlays zone
}

//----------------------------------------------------------------------
// Registry-driven texture pinning. See
// docs/superpowers/specs/2026-05-06-static-prop-texture-pin-fix.md
//
// pinNode    asserts the slot is in-range AND has a live texture allocation
//            (numUsers > 0). Pinning a free slot is a bug — the texture might
//            be reallocated to a different consumer before unpinNode runs.
// unpinNode  asserts pinRefCount > 0 before decrement to catch
//            unpaired-release / double-release.
// getPinCount is non-mutating; usable from logging paths.
void MC_TextureManager::pinNode (DWORD nodeIdx)
{
	gosASSERT(nodeIdx < (DWORD)MC_MAXTEXTURES);
	gosASSERT(masterTextureNodes[nodeIdx].numUsers > 0);
	masterTextureNodes[nodeIdx].pinRefCount++;
}

void MC_TextureManager::unpinNode (DWORD nodeIdx)
{
	gosASSERT(nodeIdx < (DWORD)MC_MAXTEXTURES);
	gosASSERT(masterTextureNodes[nodeIdx].pinRefCount > 0);
	masterTextureNodes[nodeIdx].pinRefCount--;
}

DWORD MC_TextureManager::getPinCount (DWORD nodeIdx) const
{
	if (nodeIdx >= (DWORD)MC_MAXTEXTURES) return 0;
	return masterTextureNodes[nodeIdx].pinRefCount;
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
					if (masterTextureNodes[i].pinRefCount > 0) {
						EVICT_SKIPPED(i, masterTextureNodes[i].pinRefCount, "update_turn60");
					} else {
						//----------------------------------------------------------------
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
		       turn, (unsigned long)numTexturesFreed, (long)currentUsedTextures);
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
	txmLeakNoteAlloc();  // [TXMMGR_TEXTURE_AUDIT v1] from_memory path

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

	// Residency ground-truth trace (MC2_TEXMGR_LOAD_TRACE=1, default-off,
	// zero-cost when unset): logs every loadTexture call's uniqueInstance +
	// logical name. uniqueInstance != 0 == "modifiable / CPU-locked" (paint;
	// txmmgr.h:136) -> the texture is CPU_RGBA_REQUIRED and its .tga must NOT be
	// slimmed. Consumed by tools/residency_slim.py to derive the keep/drop set
	// from ground truth (mech *rgb AND plain-named vehicle bodies) instead of
	// fragile filename heuristics.
	{
		static const bool s_loadTrace = (getenv("MC2_TEXMGR_LOAD_TRACE") != nullptr);
		if (s_loadTrace)
		{
			fprintf(stderr, "[TEXLOAD] uniq=%lu name=%s\n",
				(unsigned long)uniqueInstance, textureFullPathName ? textureFullPathName : "<null>");
			fflush(stderr);
		}
	}

	//--------------------------------------
	// Is this texture already Loaded?
	for (i=0;i<MC_MAXTEXTURES;i++)
	{
		if (masterTextureNodes[i].nodeName && (S_stricmp(masterTextureNodes[i].nodeName,textureFullPathName) == 0))
		{
			if (uniqueInstance == masterTextureNodes[i].uniqueInstance)
			{
				if (getenv("MC2_LOG_MECH_ICON") && strstr(textureFullPathName, "mechicon"))
					printf("[MECHICON] loadTexture CACHE-HIT id=%ld: %s\n", i, textureFullPathName);
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

 	if (getenv("MC2_LOG_MECH_ICON") && strstr(textureFullPathName, "mechicon"))
		printf("[MECHICON] loadTexture FRESH id=%ld: %s\n", i, textureFullPathName);
	//--------------------------------------------------------
	// New Method.  Just store memory footprint of texture.
	// DO NOT create GOS handle until we need it.
 	masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;
	txmLeakNoteAlloc();  // [TXMMGR_TEXTURE_AUDIT v1] named loadTexture path
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
	long textureFileOpenResult = textureFile.open(textureFullPathName);

	// ROUTE-2 (MC2_TEXMGR_KTX_PRIMARY, default mode 1): if the .tga cannot be
	// resolved (loose + base-strip + fastfile all miss -- e.g. the redundant
	// /128 .tga was deleted), source dims + RGBA8 from the BC7 .ktx2 sidecar
	// via the CPU decoder and build a MEM_RAW node (cf. textureFromMemoryRaw).
	// Proves the decoder integration; lets a .ktx2 stand alone without a .tga.
	// MC2_TEXMGR_KTX_PRIMARY mode: 0=off, 1=fallback (DEFAULT -- decode the BC7
	// .ktx2 when the .tga is absent; the .tga is the fallback for textures with
	// no .ktx2), 2=force (prefer the .ktx2 even when the .tga exists -- A/B test).
	// Default 1 is safe: for a full deploy (every .tga present) it never fires
	// (open succeeds -> unchanged path, GPU BC7 sidecar VRAM win preserved); for
	// a slim deploy (redundant /128 .tgas removed from loose + tgl.fst via
	// tools/fst_repack_drop.py) the .ktx2 is decoded on demand. Set =0 to force
	// the legacy "tga or bust" behavior.
	static const int s_ktxPrimaryMode = [](){
		const char* v = getenv("MC2_TEXMGR_KTX_PRIMARY");
		return (v && v[0]) ? atoi(v) : 1;
	}();
	if (s_ktxPrimaryMode == 2 || (s_ktxPrimaryMode == 1 && textureFileOpenResult != NO_ERR))
	{
		{
			char sidecar[1024];
			strncpy(sidecar, textureFullPathName, sizeof(sidecar) - 1);
			sidecar[sizeof(sidecar) - 1] = 0;
			char* dot = strrchr(sidecar, '.');
			char* slash = strrchr(sidecar, '/');
			if (dot && (!slash || dot > slash)) *dot = 0;
			if (strlen(sidecar) + 6 < sizeof(sidecar)) strcat(sidecar, ".ktx2");
			RenderCore::KtxImage img;
			std::vector<uint8_t> rgba;
			int kw = 0, kh = 0;
			if (RenderCore::ktxLoadRgba8(sidecar, img) && img.isCompressed &&
				(img.vkFormat == 145 || img.vkFormat == 146) && img.width == img.height &&
				RenderCore::ktxDecodeBc7ToRgba8(img, 0, rgba, &kw, &kh) && kw == kh && kw > 0)
			{
				const DWORD txmSize = (DWORD)(kw * kh * 4);
				masterTextureNodes[i].uvScale = 4;
				masterTextureNodes[i].logicalWidth = (DWORD)kw;
				masterTextureNodes[i].logicalHeight = (DWORD)kh;
				masterTextureNodes[i].hints = hints | gosHint_Try32bpp;
				masterTextureNodes[i].width = MC_TEXCACHE_MEM_RAW + txmSize;
				masterTextureNodes[i].lzCompSize = txmSize;
				actualTextureSize += txmSize;
				compressedTextureSize += txmSize;
				masterTextureNodes[i].textureData = (DWORD *)textureCacheHeap->Malloc(txmSize);
				if (masterTextureNodes[i].textureData)
				{
					// ktxDecodeBc7ToRgba8 emits RGBA8; the gos MEM_RAW upload expects
					// BGRA8 (matching the .tga decode path), so swap R<->B per pixel.
					// Without this, red content renders blue (blue-tinted scene).
					const uint8_t* src = rgba.data();
					uint8_t* dst = (uint8_t*)masterTextureNodes[i].textureData;
					for (DWORD px = 0; px + 4 <= txmSize; px += 4)
					{
						dst[px + 0] = src[px + 2]; // B
						dst[px + 1] = src[px + 1]; // G
						dst[px + 2] = src[px + 0]; // R
						dst[px + 3] = src[px + 3]; // A
					}
				}
				else
					masterTextureNodes[i].gosTextureHandle = 0;
				printf("[TEXMGR_KTX_PRIMARY] %s -> RGBA8 %dx%d (ktx2 CPU decode)\n", sidecar, kw, kh); fflush(stdout);
				return(i);
			}
		}
	}
	gosASSERT(textureFileOpenResult == NO_ERR);

	// ENCYCLO-3D-2 diagnostic: for TGL preview textures, record where the
	// bytes actually came from at load time (loose disk vs fastfile), the
	// open result, and a few header bytes — separates "loaded garbage/zeros"
	// from "loaded real pixels but never uploaded".
	if ( getenv("MC2_LOG_PREVIEW") && strstr(textureFullPathName, "tgl") )
	{
		if ( FILE* f = fopen("preview_debug.log","a") )
		{
			unsigned char hdr[18] = {0};
			long fsz = (textureFileOpenResult == NO_ERR) ? textureFile.fileSize() : -1;
			if ( textureFileOpenResult == NO_ERR && fsz >= (long)sizeof(hdr) )
			{
				textureFile.read(hdr, sizeof(hdr));
				textureFile.seek(0);
			}
			fprintf(f,"[PREVIEW-LOAD] open=%ld size=%ld disk=%d name=%s tgaType=%u dims=%ux%u bpp=%u\n",
				textureFileOpenResult, fsz,
				(int)((textureFileOpenResult == NO_ERR) ? textureFile.isLoadedFromDisk() : -1),
				textureFullPathName,
				hdr[2], hdr[12] | (hdr[13]<<8), hdr[14] | (hdr[15]<<8), hdr[16]);
			fclose(f);
		}
	}

	if (textureFile.isLoadedFromDisk())
	{
		// Disk TGAs are 4x-upscaled gameplay textures (logical = physical/4). But some
		// UI atlases are NATIVE-resolution (NOT upscaled), and their FIT UV coords are
		// authored in physical-pixel space -> logical must equal physical (uvScale=1):
		//  - mcui_gn_mechicons.tga (mech-bay roster): blanket uvScale=4 made initIcon's
		//    logical V divisor physical/4 so vehicle indices 118-142 sampled off-atlas.
		//  - cursors*.tga (mouse-cursor sheets): a 128x128 sheet -> logical 32, while the
		//    cursor FIT UNormal (e.g. 48) is physical -> U=48/32=1.5 off-texture ->
		//    INVISIBLE cursor (notably MC2X at the 800-logical cursorsa.fit tier).
		//  - mcl_mc_*.tga (mech-lab component icons: weapons/armor/heatsinks/engines):
		//    native-res UI icons whose FIT/list UVs are physical-pixel. Stock ones
		//    load from the FST (never hit this disk path, so stay uvScale=1), but a
		//    MOD ships them as loose disk TGAs -> blanket uvScale=4 made logical =
		//    physical/4 -> the icon sampled a 1/4 sub-rect and rendered tiny in the
		//    corner. They are NOT 4x-upscaled gameplay art, so force uvScale=1.
		const bool isNativeResUiAtlas = textureFullPathName &&
			(strstr(textureFullPathName, "mcui_gn_mechicons") != nullptr ||
			 strstr(textureFullPathName, "cursors") != nullptr ||
			 strstr(textureFullPathName, "mcl_mc_") != nullptr);
		masterTextureNodes[i].uvScale = isNativeResUiAtlas ? 1 : 4;
	}

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
// DEV-SHELL texture_refresh (see txmmgr.h). Mirrors the loadTexture disk
// read/compress path byte-for-byte so the refreshed cache entry is exactly
// what a fresh loadTexture of the same file would have produced; the node's
// key/hints/uvScale/logical dims are untouched (same-format, same-size
// replacement). GL work happens at the next get_gosTextureHandle() cache-in,
// so this is safe anywhere on the main thread.
long MC_TextureManager::refreshTexturesByName (const char *substring, long &skippedNoFile)
{
	skippedNoFile = 0;
	if (!substring || !*substring)
		return 0;

	// case-insensitive substring match (nodeNames are stored lowercase, but
	// don't rely on it)
	auto containsNoCase = [](const char *hay, const char *needle) -> bool {
		const size_t nlen = strlen(needle);
		for (const char *p = hay; *p; ++p)
		{
			size_t k = 0;
			while (k < nlen && p[k] && tolower((unsigned char)p[k]) == tolower((unsigned char)needle[k]))
				++k;
			if (k == nlen) return true;
		}
		return false;
	};

	long refreshed = 0;
	for (long i = 0; i < MC_MAXTEXTURES; i++)
	{
		MC_TextureNode &node = masterTextureNodes[i];
		if (!node.nodeName || node.gosTextureHandle == 0xffffffff)
			continue;
		if (!containsNoCase(node.nodeName, substring))
			continue;

		File textureFile;
		if (textureFile.open(node.nodeName) != NO_ERR || !textureFile.isLoadedFromDisk())
		{
			// FST-only (or unopenable): no loose file to refresh from.
			skippedNoFile++;
			continue;
		}

		const long txmSize = textureFile.fileSize();
		if (txmSize <= 0 || txmSize > MAX_LZ_BUFFER_SIZE)
		{
			skippedNoFile++;
			textureFile.close();
			continue;
		}

		if (!lzBuffer1)
		{
			lzBuffer1 = (MemoryPtr)textureCacheHeap->Malloc(MAX_LZ_BUFFER_SIZE);
			gosASSERT(lzBuffer1 != NULL);
			lzBuffer2 = (MemoryPtr)textureCacheHeap->Malloc(MAX_LZ_BUFFER_SIZE);
			gosASSERT(lzBuffer2 != NULL);
		}

		textureFile.read(lzBuffer1, txmSize);
		textureFile.close();

		const bool storeRawFileData = (txmSize >= MC_TEXCACHE_RAW_THRESHOLD);
		const DWORD cacheBytes = storeRawFileData
			? (DWORD)txmSize
			: LZCompress(lzBuffer2, lzBuffer1, txmSize);

		DWORD *fresh = (DWORD *)textureCacheHeap->Malloc(cacheBytes);
		if (!fresh)
		{
			skippedNoFile++;
			continue;	// leave the old copy live rather than corrupt the node
		}
		memcpy(fresh, storeRawFileData ? lzBuffer1 : lzBuffer2, cacheBytes);

		if (node.textureData)
			textureCacheHeap->Free(node.textureData);
		node.textureData = fresh;
		node.lzCompSize = cacheBytes;
		node.width = (storeRawFileData ? MC_TEXCACHE_FILE_RAW : MC_TEXCACHE_FILE_LZ) + txmSize;

		// Force re-upload: destroy the resident gos texture (same cache-out
		// dance as flushCache) so the next get_gosTextureHandle() re-creates
		// it from the fresh bytes.
		if (node.gosTextureHandle != CACHED_OUT_HANDLE && node.gosTextureHandle != 0)
		{
			gos_DestroyTexture(node.gosTextureHandle);
			if (currentUsedTextures > 0)
				currentUsedTextures--;
		}
		node.gosTextureHandle = CACHED_OUT_HANDLE;
		refreshed++;
	}
	return refreshed;
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
	txmLeakNoteAlloc();  // [TXMMGR_TEXTURE_AUDIT v1] from_memory raw path
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
	// PERF 2026-05-07: stripped MC_TextureNode::get_gosTextureHandle Tracy
	// scopes/plots from this hot accessor; cache-miss work remains unchanged.
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
					gosTextureHandle = gos_NewEmptyTexture(key,nodeName,logicalWidth ? logicalWidth : width,hints);
				}
				TEXTUREPTR pTextureData;
				{
					gos_LockTexture(gosTextureHandle, 0, 0, &pTextureData);
				}
				{
					memcpy(pTextureData.pTexture, textureBytes, originalSize);
				}
				{
					gos_UnLockTexture(gosTextureHandle);
				}
			}
			else
			{
				// TEXMGR-COMPRESSED-UPLOAD-1: gated BC7 .ktx2 sidecar upload.
				// Default-OFF; when the gate is set AND a same-stem .ktx2 sidecar
				// exists on disk AND it is a stored BC7 (vkFormat 145/146) image,
				// upload mip 0 via glCompressedTexImage2D and skip the normal
				// gos_NewTextureFromMemory call. Any failure (no sidecar, not BC7,
				// load fail, no BPTC support) falls through unchanged.
				// Default-ON (opt out with MC2_TEXMGR_COMPRESSED_UPLOAD=0): prefer the
				// BC7 .ktx2 sidecar for GPU upload when present; any failure (no sidecar,
				// not BC7, no BPTC support) falls through to the .tga unchanged.
				static const bool s_texmgrCompressedUpload = [](){
					const char* v = getenv("MC2_TEXMGR_COMPRESSED_UPLOAD");
					return (!v || !v[0]) ? true : (v[0] != '0');
				}();
				bool uploadedCompressed = false;

				// Exclude MODIFIABLE textures (uniqueInstance != 0): the mech/vehicle
				// paint-scheme system gos_LockTexture()s these and rewrites the R/G/B
				// team-color mask in place (mech3d/gvactor setPaintScheme). A BC7
				// texture is GPU-immutable / not CPU-lockable, so compressing it leaves
				// the raw mask on screen (base blue + red/green, no team colors). Static
				// shared textures (uniqueInstance == 0: terrain, trees, buildings) are
				// never locked and keep the BC7 upload. See txmmgr.h:136 ("Texture is
				// modifiable. DO NOT CACHE OUT").
				if (s_texmgrCompressedUpload && nodeName && *nodeName && uniqueInstance == 0)
				{
					if (!GLEW_ARB_texture_compression_bptc)
					{
						static bool s_warnedNoBptc = false;
						if (!s_warnedNoBptc)
						{
							s_warnedNoBptc = true;
							printf("[TEXMGR_BC7] skip: GLEW_ARB_texture_compression_bptc unsupported; compressed upload disabled\n");
						}
					}
					else
					{
						// Derive sidecar path: replace extension with .ktx2.
						char sidecar[1024];
						strncpy(sidecar, nodeName, sizeof(sidecar) - 1);
						sidecar[sizeof(sidecar) - 1] = '\0';
						char* dot = strrchr(sidecar, '.');
						char* slash = strrchr(sidecar, '/');
						char* bslash = strrchr(sidecar, '\\');
						char* lastSep = slash > bslash ? slash : bslash;
						bool haveRoom = false;
						if (dot && dot > lastSep)
						{
							// truncate at the extension dot, then append .ktx2
							size_t base = (size_t)(dot - sidecar);
							if (base + 6 < sizeof(sidecar)) { strcpy(sidecar + base, ".ktx2"); haveRoom = true; }
						}
						else
						{
							size_t len = strlen(sidecar);
							if (len + 6 < sizeof(sidecar)) { strcpy(sidecar + len, ".ktx2"); haveRoom = true; }
						}

						if (haveRoom)
						{
							// Disk-only probe (std::fopen, NOT File/FastFile).
							FILE* probe = std::fopen(sidecar, "rb");
							if (!probe)
							{
								// No sidecar — common; quiet fall-through.
							}
							else
							{
								std::fclose(probe);
								RenderCore::KtxImage img;
								if (RenderCore::ktxLoadRgba8(sidecar, img) &&
									img.isCompressed &&
									(img.vkFormat == 145 || img.vkFormat == 146))
								{
									GLenum glIF = (img.vkFormat == 146)
										? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
										: GL_COMPRESSED_RGBA_BPTC_UNORM;
									// Upload MIP 0 only (single level).
									size_t mip0Bytes = (img.mipCount > 1 && img.mipByteOffsets.size() > 1)
										? (size_t)img.mipByteOffsets[1]
										: img.pixels.size();
									DWORD h = gos_NewCompressedTexture2D(
										(uint32_t)glIF, img.width, img.height,
										img.pixels.data(), mip0Bytes, nodeName);
									if (h != 0 /*INVALID_TEXTURE_ID*/ && h != 0xffffffff)
									{
										gosTextureHandle = h;
										uploadedCompressed = true;
										printf("[TEXMGR_BC7] loaded name=%s %dx%d fmt=0x%x bytes=%zu\n",
											nodeName, img.width, img.height, (unsigned)glIF, mip0Bytes);
									}
									else
									{
										printf("[TEXMGR_BC7] upload failed name=%s; fallthrough RGBA8\n", nodeName);
									}
								}
								else
								{
									printf("[TEXMGR_BC7] sidecar not BC7/loadable name=%s; fallthrough RGBA8\n", sidecar);
								}
							}
						}
					}
				}

				if (!uploadedCompressed)
				{
					gosTextureHandle = gos_NewTextureFromMemory(key,nodeName,textureBytes,originalSize,hints);
				}
			}
			mcTextureManager->currentUsedTextures++;
			if (mcTextureManager->currentUsedTextures > mcTextureManager->peakUsedTextures)
				mcTextureManager->peakUsedTextures = mcTextureManager->currentUsedTextures;
			++gTxmRealizedTotal;
			lastUsed = turn;

			return gosTextureHandle;
		}
		else
		{
			{
				gosTextureHandle = gos_NewEmptyTexture(key,nodeName,width,hints);
			}
			mcTextureManager->currentUsedTextures++;
			if (mcTextureManager->currentUsedTextures > mcTextureManager->peakUsedTextures)
				mcTextureManager->peakUsedTextures = mcTextureManager->currentUsedTextures;
			++gTxmRealizedTotal;

			//------------------------------------------
			// Cache this badboy IN.
			TEXTUREPTR pTextureData;
			{
				gos_LockTexture(gosTextureHandle, 0, 0, &pTextureData);
			}
		 
			//-------------------------------------------------------
			// Create a block of cache memory to hold this texture.
			DWORD txmSize = pTextureData.Height * pTextureData.Height * sizeof(DWORD);
			gosASSERT(textureData);

			{
				LZDecomp(MC_TextureManager::lzBuffer2,(MemoryPtr)textureData,lzCompSize,MAX_LZ_BUFFER_SIZE);
			}
			{
				memcpy(pTextureData.pTexture,MC_TextureManager::lzBuffer2,txmSize);
			}

			//------------------------
			// Unlock the texture
			{
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
