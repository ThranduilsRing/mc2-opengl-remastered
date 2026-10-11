//-------------------------------------------------------------------------------
//
// Tiny Geometry Layer
//
// For MechCommander 2 -- Copyright (c) 1999 Microsoft
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include "tgl_internal.h"
#include "Gates/gate.h"
#include "mc2fmt.h"

//-------------------------------------------------------------------------------
// Statics
Stuff::LinearMatrix4D 	*TG_Shape::s_cameraOrigin = nullptr;
Stuff::Matrix4D			*TG_Shape::s_cameraToClip = nullptr;
Stuff::LinearMatrix4D	TG_Shape::s_worldToCamera;
Stuff::Matrix4D			TG_Shape::s_worldToClip;

float					TG_Shape::viewMulX = 0.0;
float					TG_Shape::viewAddX = 0.0;
float					TG_Shape::viewMulY = 0.0;
float					TG_Shape::viewAddY = 0.0;

DWORD					TG_Shape::fogColor = 0xffffffff;
float					TG_Shape::fogStart = 0.0f;
float					TG_Shape::fogFull = 0.0f;

TG_LightPtr				*TG_Shape::s_listOfLights = nullptr;
DWORD					TG_Shape::s_numLights = 0;

Stuff::LinearMatrix4D 	TG_Shape::s_lightToShape[MAX_LIGHTS_IN_WORLD];
Stuff::Vector3D			TG_Shape::s_lightDir[MAX_LIGHTS_IN_WORLD];
Stuff::Vector3D			TG_Shape::s_rootLightDir[MAX_LIGHTS_IN_WORLD];
Stuff::Vector3D			TG_Shape::s_spotDir[MAX_LIGHTS_IN_WORLD];

UserHeapPtr 			TG_Shape::tglHeap = nullptr;

DWORD					TG_Shape::lighteningLevel = 0;

TG_VertexPool 			*colorPool = nullptr;
TG_GOSVertexPool 		*vertexPool = nullptr;
TG_DWORDPool			*facePool = nullptr;
TG_ShadowPool			*shadowPool = nullptr;
TG_TrianglePool			*trianglePool = nullptr;

// Encyclopedia 3D-preview auto-framing: when armed, the software vertex
// transform accumulates the projected screen-space bounding box of the
// geometry it draws. This is the *real* rendered extent (rotation- and
// geometry-correct), used by SimpleCamera to fit the object to the panel.
// Disarmed by default; zero cost on the normal in-mission path.
// [UI-PHASE1] carried from mc2r_ui_phase1 (base->theirs hunk @tgl.cpp:90-105);
// consumed by code/simplecamera.cpp + code/mechlopedia.cpp (Mechlopedia/Mechbay preview).
bool  g_previewCaptureBounds = false;
float g_previewBoundsMinX =  1e9f;
float g_previewBoundsMaxX = -1e9f;
float g_previewBoundsMinY =  1e9f;
float g_previewBoundsMaxY = -1e9f;
// Diagnostic: triangles actually emitted to the draw list by TG_Shape::Render.
// Reset and read by SimpleCamera::render to tell "transformed but not drawn"
// (render gate / cull) apart from "not transformed".
int   g_previewDrawTris = 0;
bool drawOldWay = false;
bool useLocalShadows = false;

bool renderTGLShapes = true;

bool silentMode = false;		//Used for automated builds to keep errors from popping up.

//sebi: uncomment this to also enable path of rendering shapes using lighting in shaders
// instead of calculating everything on the CPU
// (will render them offset a bit to check that they are rendered exactly same as old way)
// for now everything seems to be correct thoug, so could enable it by default
// look into gos_tex_vertex_lighted.vert for where offset is defined
// MERGE-CONFLICT-UI-PHASE1: theirs (mc2r_ui_phase1) flips this default false->true,
// apparently so the Mechlopedia/Mechbay preview's hardware-vertex shapes take the
// shader-lighting path. NOT applied: this flag also gates the live
// `eligibleForGpuObjects(this)`-interlocked addRenderShape branch in
// TG_Shape::MultiTransformShape (tgl.cpp, ~line 2698+17) used by the slice-2
// GPU-object-offload work — flipping the *global* default changes normal
// in-mission rendering behavior, not just the preview panel. Needs a scoped
// per-preview override (e.g. save/restore around the encyclopedia render call)
// instead of a global default flip. Left at ours' value; resolve before ship.
// RESOLVED 2026-07-05: the motivation for theirs' flip is GONE — the
// Mechlopedia/Mech Bay previews render correctly through the drawOldWay +
// fixed-800x600-FBO composite path (ENCYCLO-3D-2 / PREVIEW-FBO-FIXED),
// which never needed the shader-lighting path. Default stays false; the
// GPU-object-offload interlock is unaffected. No per-preview override needed.
bool bShadersDrawPathEnabled = false;

// Slice 2 (object-offload) — forward declaration from gos_static_prop_batcher.h.
// Cheap, side-effect-free check used by the addRenderShape gate at the
// `bShadersDrawPathEnabled` block to prevent double-draw when slice 2's
// GPU-object path picks up an actor. Under default `MC2_GPU_OBJECTS=0`
// (g_useGpuObjects==false) returns false unconditionally, so the legacy gate
// is byte-for-byte unchanged.
bool eligibleForGpuObjects(class TG_Shape* shape);

//-------------------------------------------------------------------------------
// Tier-1 instrumentation (stability spec §2.3-§2.5)
//
// Per-frame line is gated on MC2_TGL_POOL_TRACE; monotonic summary every 600
// frames is unconditional. Pools are file-scope globals (see tgl.h:1306+ and
// code/mission.cpp:3117). All calls happen on the render thread inside the
// frame-end window — no locks needed.
//-------------------------------------------------------------------------------
static const bool s_tglPoolTrace = mc2::gate::get<"MC2_TGL_POOL_TRACE">();

// Tier-1 instrumentation canonical frame counter. Defined here (in mclib)
// rather than GameOS/gameos/gameosmain.cpp so data-tool targets (aseconv,
// pak, makefst, makersp) that link mclib but NOT gameosmain still resolve
// the symbol. The per-frame increment still lives in gameosmain.cpp's frame
// loop; data tools never increment it and see a harmless zero.
uint32_t g_mc2FrameCounter = 0;

// VK-FIELD-VFX-5: fire-event present-capture arm. 0 = unarmed; the
// MC2_FX_FORCE_SPAWN=loop fixture (code/warrior.cpp) sets it to the frame
// ~0.5s after its first volley when MC2_VK_PRESENT_DUMP_ON_FIRE is set, and
// the VK present dump (engine_present_vk.cpp verifyReadbackContent +
// gameosmain verify window) treats it as a dynamic MC2_VK_PRESENT_DUMP_FRAME
// pin. Defined next to the frame counter for the same data-tool-link reason.
uint32_t g_mc2FxFireCaptureFrame = 0;

// VK-FAILSOFT-DISARM-1: session-sticky "VK fail-softed to GL" flag. Set ONLY by
// mc2::forceGLFallback (render_backend_select.cpp); read by
// mc2vkgate::umbrellaOn() so the WHOLE shared-GBuffer content family disarms
// when present/device fail-softs — post-ITEM8 there is no per-layer GL
// fallback, so armed islands + GL present = content VANISHES (the torn grey
// scene: VFX/decals over a wash; the S2b-transient signature). Defined here
// (mclib) for the same data-tool-link reason as g_mc2FrameCounter above.
bool g_mc2VkSessionForcedGL = false;

namespace {
	// Emit one [TGL_POOL] line per pool with nulls this frame.
	template<typename PoolT>
	inline void emitPerFrameLine (PoolT* p, const char* poolLabel)
	{
		if (!p || p->nullCountThisFrame == 0) return;
		char _cbbuf[512];
		mc2::fmt::to_buf(_cbbuf, "[TGL_POOL v1] frame={:d} pool={} nulls={:d} first_caller={} shape={:016X} req={:d} high_water={:d}/{:d} mono_total={:d}", mc2::fmt::as_u32((unsigned)g_mc2FrameCounter), mc2::fmt::safe(poolLabel), mc2::fmt::as_u32((unsigned)p->nullCountThisFrame), mc2::fmt::safe(p->firstNullSnapshot.caller ? p->firstNullSnapshot.caller : "?"), mc2::fmt::as_ptr(p->firstNullSnapshot.shape), mc2::fmt::as_u32((unsigned)p->firstNullSnapshot.numRequested), mc2::fmt::as_u32((unsigned)p->firstNullSnapshot.numUsed_at_failure), mc2::fmt::as_u32((unsigned)p->firstNullSnapshot.totalCapacity), mc2::fmt::as_u64((unsigned long long)p->nullCountMonotonic));
		puts(_cbbuf);
		crashbundle_append(_cbbuf);
	}
	template<typename PoolT>
	inline unsigned long long monoOf (PoolT* p) { return p ? (unsigned long long)p->nullCountMonotonic : 0ULL; }
	template<typename PoolT>
	inline void resetOf (PoolT* p) { if (p) p->resetFrameCounters(); }
}

void drainTglPoolStats (void)
{
	// Frame counter is incremented at the call site in gameosmain.cpp,
	// immediately before drainTglPoolStats(). Do NOT increment here.

	if (s_tglPoolTrace) {
		emitPerFrameLine(vertexPool,   "vertex");
		emitPerFrameLine(colorPool,    "color");
		emitPerFrameLine(facePool,     "face");
		emitPerFrameLine(shadowPool,   "shadow");
		emitPerFrameLine(trianglePool, "triangle");
		fflush(stdout);
	}

	// Monotonic summary every 600 frames. Always emit on shutdown (see
	// drainTglPoolStatsOnShutdown). During the run, skip the periodic summary
	// when every pool is still at zero — an all-zeros line provides no new
	// diagnostic information and just adds noise at ~3s intervals on a clean
	// run. First non-zero count triggers emission; shutdown still gets the
	// final "did this ever happen" line regardless.
	if ((g_mc2FrameCounter > 0) && ((g_mc2FrameCounter % 600) == 0)) {
		const unsigned long long vM = monoOf(vertexPool);
		const unsigned long long cM = monoOf(colorPool);
		const unsigned long long fM = monoOf(facePool);
		const unsigned long long sM = monoOf(shadowPool);
		const unsigned long long tM = monoOf(trianglePool);
		if ((vM | cM | fM | sM | tM) != 0ULL) {
			char _cbbuf[384];
			mc2::fmt::to_buf(_cbbuf, "[TGL_POOL v1] summary mono_total={{vertex:{:d}, color:{:d}, face:{:d}, shadow:{:d}, triangle:{:d}}} since=process_start", mc2::fmt::as_u64(vM), mc2::fmt::as_u64(cM), mc2::fmt::as_u64(fM), mc2::fmt::as_u64(sM), mc2::fmt::as_u64(tM));
			puts(_cbbuf);
			crashbundle_append(_cbbuf);
			fflush(stdout);
		}
	}

	// Reset per-frame counters on all five pools.
	resetOf(vertexPool);
	resetOf(colorPool);
	resetOf(facePool);
	resetOf(shadowPool);
	resetOf(trianglePool);
}

void drainTglPoolStatsOnShutdown (void)
{
	// Emit per-pool peak lines while pools are still live (smoke mode never
	// reaches mission_unload, so this is the only path that fires on process exit).
	logTglPoolPeaks("process_exit");

	// Texture manager peak (same reason: flush() won't fire in smoke mode).
	if (mcTextureManager)
	{
		char _txmbuf[128];
		mc2::fmt::to_buf(_txmbuf, "[TXM v1] event=process_exit peak_textures={:d}/{:d}", mc2::fmt::as_i32(mcTextureManager->getPeakUsedTextures()), mc2::fmt::as_i32(MAX_MC2_GOS_TEXTURES));
		MC2_DIAG_PUTS(_txmbuf);
		crashbundle_append(_txmbuf);
	}

	{
		char _cbbuf[384];
		mc2::fmt::to_buf(_cbbuf, "[TGL_POOL v1] summary mono_total={{vertex:{:d}, color:{:d}, face:{:d}, shadow:{:d}, triangle:{:d}}} since=process_start (shutdown)", mc2::fmt::as_u64(monoOf(vertexPool)), mc2::fmt::as_u64(monoOf(colorPool)), mc2::fmt::as_u64(monoOf(facePool)), mc2::fmt::as_u64(monoOf(shadowPool)), mc2::fmt::as_u64(monoOf(trianglePool)));
		puts(_cbbuf);
		crashbundle_append(_cbbuf);
	}
	fflush(stdout);
}

// Unconditional per-mission peak report. Called just before pool destroy()
// at mission teardown so the counter is still live when we read it.
void logTglPoolPeaks (const char* context)
{
	struct PoolDesc {
		const char* name;
		DWORD peak;
		DWORD cap;
	};

	// Skip emit if all pools are NULL (first-ever init, nothing to report).
	bool anyLive = vertexPool || colorPool || facePool || shadowPool || trianglePool;
	if (!anyLive) return;

	PoolDesc pools[] = {
		{ "vertex",   vertexPool   ? vertexPool->peakUsedThisMission   : 0u,
		               vertexPool   ? vertexPool->capacity()             : 0u },
		{ "color",    colorPool    ? colorPool->peakUsedThisMission    : 0u,
		               colorPool    ? colorPool->capacity()              : 0u },
		{ "face",     facePool     ? facePool->peakUsedThisMission     : 0u,
		               facePool     ? facePool->capacity()               : 0u },
		{ "shadow",   shadowPool   ? shadowPool->peakUsedThisMission   : 0u,
		               shadowPool   ? shadowPool->capacity()             : 0u },
		{ "triangle", trianglePool ? trianglePool->peakUsedThisMission : 0u,
		               trianglePool ? trianglePool->capacity()           : 0u },
	};

	for (const auto& p : pools)
	{
		char _cbbuf[256];
		if (p.cap > 0)
		{
			unsigned pct = (unsigned)(((unsigned long long)p.peak * 100u) / p.cap);
			mc2::fmt::to_buf(_cbbuf, "[TGL_POOL v1] event={} pool={:<8} peak={:d}/{:d} pct={:d}%", mc2::fmt::safe(context), mc2::fmt::safe(p.name), mc2::fmt::as_u32((unsigned)p.peak), mc2::fmt::as_u32((unsigned)p.cap), mc2::fmt::as_u32(pct));
		}
		else
		{
			mc2::fmt::to_buf(_cbbuf, "[TGL_POOL v1] event={} pool={:<8} peak={:d}/? pct=?%", mc2::fmt::safe(context), mc2::fmt::safe(p.name), mc2::fmt::as_u32((unsigned)p.peak));
		}
		puts(_cbbuf);
		crashbundle_append(_cbbuf);
	}
	fflush(stdout);
}

void TG_Shape::Touch() {
	lastTurnTransformed = turn;
}
