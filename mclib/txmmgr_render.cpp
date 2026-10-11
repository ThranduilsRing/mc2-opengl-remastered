// MC_TextureManager::renderLists orchestrator, the RENDERLISTS cost spans and the shadow-shape list.
// The phases it runs live in txmmgr_render_{phases,passes,overlays}.cpp; shared names in txmmgr_render_internal.h.
#include "txmmgr_render_internal.h"
#include "mc2fmt.h"

// [RENDERLISTS_COST v1] — TXMMGR-PERF-EASYWINS-1 coarse per-phase CPU cost
// split of MC_TextureManager::renderLists(). Smoke-visible complement of the
// Tracy zones (smoke runs cannot take user-driven Tracy captures). Pattern =
// SPFLUSH_COST_SPLIT: env-gated, window-averaged, default OFF (zero overhead
// beyond one cached-bool test per phase when unset; ~2 steady_clock reads per
// phase per frame when set — coarse spans only, per the 100ns-floor rule).
// Enable: MC2_RENDERLISTS_COST_SPLIT=1 -> one [RENDERLISTS_COST v1] stderr
// summary every 60 frames: per-frame mean µs per phase + node-table sizes.
// self_us = total minus the sum of instrumented phases (unattributed gaps).
namespace rlcost {
const bool s_enabled = mc2::gate::get<"MC2_RENDERLISTS_COST_SPLIT">();
const char* kName[kPhaseCount] = {
    "preamble", "light_upload", "scene_data", "obj3d", "state_restore",
    "static_shadow_build", "sp_registry_flush", "dyn_shadow", "terrain_solid",
    "gpusp_prep_cull", "sp_batcher_flush", "mech_flush", "overlays",
    "water_loops", "no_underlayer", "shadow_blobs", "alpha_loops",
    "vfx_hud", "total"
};
unsigned long long s_ns[kPhaseCount] = {};
int s_frames = 0;

TotalSpan::~TotalSpan() {
    if (!s_enabled) return;
    s_ns[kTotal] += nowNs() - t0;
    if (++s_frames < 60) return;
    const double wf = static_cast<double>(s_frames);
    double phaseSumUs = 0.0;
    char line[1400]; int off = 0;
    off += mc2::fmt::to_buf_n(line + off, sizeof(line) - off, "[RENDERLISTS_COST v1] event=summary frames={:d} nodes={:d} hw_nodes={:d}", mc2::fmt::as_i32(s_frames), mc2::fmt::as_i32(nodes_), mc2::fmt::as_i32(hwNodes_));
    for (int p = 0; p < kPhaseCount; ++p) {
        const double us = static_cast<double>(s_ns[p]) / 1000.0 / wf;
        if (p != kTotal) phaseSumUs += us;
        if (off < static_cast<int>(sizeof(line)))
            off += mc2::fmt::to_buf_n(line + off, sizeof(line) - off, " {}_us={:.1f}", mc2::fmt::safe(kName[p]), us);
    }
    const double selfUs =
        static_cast<double>(s_ns[kTotal]) / 1000.0 / wf - phaseSumUs;
    if (off < static_cast<int>(sizeof(line)))
        mc2::fmt::to_buf_n(line + off, sizeof(line) - off, " self_us={:.1f}", selfUs);
    mc2::fmt::print_err("{}\n", mc2::fmt::safe(line));
    fflush(stderr);
    memset(s_ns, 0, sizeof(s_ns));
    s_frames = 0;
}
}

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
std::atomic<int> g_numShadowShapes{0};

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

// Draws all textures with isTerrain set that are solid first,
// then draws all alpha with isTerrain set.
#ifdef MC2_VULKAN_ISLAND
void MC_TextureManager::vkLegacySubmitNode (long i, uint32_t pass, uint32_t alphaTest)
{
		DWORD totalVertices = masterVertexNodes[i].numVertices;
		if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			totalVertices = (DWORD)(masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices);
		if (totalVertices)
		{
			Mc2LegacyMlrDraw d{};
			d.vertices = masterVertexNodes[i].vertices;
			d.vertexCount = totalVertices;
			d.texNode = (uint32_t)masterVertexNodes[i].textureIndex;
			const DWORD gosTex = tex_resolve(masterVertexNodes[i].textureIndex);
			d.gosTexHandle = (gosTex == 0xffffffffu) ? 0u : (uint32_t)gosTex;
			d.pass = pass;
			d.alphaTest = alphaTest;
			mc2_vk_legacy_shape_submit(&d);
		}
		masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
}

void MC_TextureManager::vkLegacyRun (void)
{
		// fog_color_ is what gosRenderer would have latched from gos_State_Fog (set in the
		// renderLists preamble: eye->fogColor when fog is on, else 0). The additive loops
		// clear fog themselves (the island ADD pass ignores this).
		Mc2LegacyMlrFrame ff{};
		const vec4 fcv = uint32_to_vec4(useFog ? (uint32_t)eye->fogColor : 0u);
		ff.fogColor[0] = fcv.x; ff.fogColor[1] = fcv.y; ff.fogColor[2] = fcv.z; ff.fogColor[3] = fcv.w;
		if (gosPostProcess* ppVkLeg = getGosPostProcess())
			(void)mc2_vk_legacy_shape_island_run(ppVkLeg->getWidth(), ppVkLeg->getHeight(), &ff);
}
#endif

void MC_TextureManager::renderLists (void)
{
	ZoneScopedN("textureManagerRenderLists");
	// [RENDERLISTS_COST v1] whole-function span; constructed first so its dtor
	// (accumulate + 60-frame emit) runs after every phase span has closed.
	rlcost::TotalSpan _rlTotal(nextAvailableVertexNode,
	                           static_cast<long>(nextAvailableHardwareVertexNode));

	// LIGHT-CONE-MODELS-1 (B): fill the deferred cone-glow vertices into the
	// MC2_ISSPOTLGT batch. Must run before the ISSPOTLGT consumer loop below
	// and AFTER every legacy addVertices for the frame (all of which happen in
	// the object render pass, before renderLists). See mclib/cone_glow.cpp.
	mc2_cone_glow::FlushFrame();
	RenderListsFrame st;
#ifdef MC2_VULKAN_ISLAND
	// VK-PARITY-F1: ground vehicles (and every other TG_Shape on the CPU-transformed
	// gos_VERTEX path) are flushed by the legacy masterVertexNodes loops below (solid,
	// blob-shadow, alpha, additive effects / spotlight cones). Under the VK umbrella the
	// native VK composite is the sole present producer, so those GL draws would land in a
	// DISCARDED GL backbuffer and the objects would vanish. When the VK legacy-shape island
	// is armed, each loop queues its nodes for the island (run right after the loop group)
	// instead of drawing them here. Gate off (default GL) -> vkLegacyOn == false -> every
	// loop is byte-identical.
	st.vkLegacyOn = (mc2_vk_legacy_shape_island_enabled() != 0);
#endif
	renderListsPreamble();
	renderListsLightDataUpload();
	renderListsSceneDataUpload();
	renderListsObjects3D();
	renderListsPostObjectsStateRestore();
	renderListsStaticShadowBuild();
	renderListsStaticPropRegistryFlush();
	renderListsDynamicShadowPass();
	renderListsTerrainSolid(st);
	renderListsGpuStaticProps();
	renderListsOverlayBatches();
	renderListsOverlaysOld(st);
}
