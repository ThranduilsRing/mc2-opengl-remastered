#include "vk_scene_renderer.h"
// mc2::VkSceneRenderer::renderLists orchestrator, the RENDERLISTS cost spans and the shadow-shape list.
// The phases it runs live in txmmgr_render_{phases,passes,overlays}.cpp; shared names in vk_scene_renderer_internal.h.
#include "vk_scene_renderer_internal.h"
#include "mc2fmt.h"
#include "effect_resources.h"
#ifdef MC2_VULKAN_ISLAND
#include "../GameOS/gameos/vulkan_vfx_island.h"  // mission-unload sprite descriptor pool chain reset
#endif

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
void mc2::VkSceneRenderer::vkLegacySubmitNode (RenderSubmissions& submissions, long i, uint32_t pass, uint32_t alphaTest)
{
		DWORD totalVertices = submissions.masterVertexNodes[i].numVertices;
		if (submissions.masterVertexNodes[i].currentVertex != (submissions.masterVertexNodes[i].vertices + submissions.masterVertexNodes[i].numVertices))
			totalVertices = static_cast<DWORD>((submissions.masterVertexNodes[i].currentVertex - submissions.masterVertexNodes[i].vertices));
		if (totalVertices)
		{
			Mc2LegacyMlrDraw d{};
			d.vertices = submissions.masterVertexNodes[i].vertices;
			d.vertexCount = totalVertices;
			d.texNode = static_cast<uint32_t>(submissions.masterVertexNodes[i].textureIndex);
			const DWORD gosTex = tex_resolve(submissions.masterVertexNodes[i].textureIndex);
			d.gosTexHandle = (gosTex == 0xffffffffu) ? 0u : static_cast<uint32_t>(gosTex);
			d.pass = pass;
			d.alphaTest = alphaTest;
			mc2_vk_legacy_shape_submit(&d);
		}
		submissions.masterVertexNodes[i].currentVertex = submissions.masterVertexNodes[i].vertices;
}

void mc2::VkSceneRenderer::vkLegacyRun (RenderSubmissions& submissions)
{
		// fog_color_ is what gosRenderer would have latched from gos_State_Fog (set in the
		// renderLists preamble: eye->fogColor when fog is on, else 0). The additive loops
		// clear fog themselves (the island ADD pass ignores this).
		Mc2LegacyMlrFrame ff{};
		const vec4 fcv = uint32_to_vec4(useFog ? static_cast<uint32_t>(eye->fogColor) : 0u);
		ff.fogColor[0] = fcv.x; ff.fogColor[1] = fcv.y; ff.fogColor[2] = fcv.z; ff.fogColor[3] = fcv.w;
		if (gosPostProcess* ppVkLeg = getGosPostProcess())
			(void)mc2_vk_legacy_shape_island_run(ppVkLeg->getWidth(), ppVkLeg->getHeight(), &ff);
}
#endif

void mc2::VkSceneRenderer::renderLists (RenderSubmissions& submissions)
{
	ZoneScopedN("textureManagerRenderLists");

	// LIGHT-CONE-MODELS-1 (B): fill the deferred cone-glow vertices into the
	// MC2_ISSPOTLGT batch. Must run before the ISSPOTLGT consumer loop below
	// and AFTER every legacy addVertices for the frame (all of which happen in
	// the object render pass, before renderLists). See mclib/cone_glow.cpp.
	{ ZoneScopedN("RL.ConeGlow"); mc2_cone_glow::FlushFrame(); }
	{ ZoneScopedN("RL.renderListsPreamble"); renderListsPreamble(submissions); }
	{ ZoneScopedN("RL.renderListsSceneDataUpload"); renderListsSceneDataUpload(submissions); }
	{ ZoneScopedN("RL.renderListsPostObjectsStateRestore"); renderListsPostObjectsStateRestore(submissions); }
	renderListsStaticPropRegistryFlush(submissions);
	{ ZoneScopedN("RL.renderListsDynamicShadowPass"); renderListsDynamicShadowPass(submissions); }
	// Terrain samples the current mover map, after its producer has submitted.
	{ ZoneScopedN("RL.TerrainFlush"); Terrain::flushDrawCommands(); }
	{ ZoneScopedN("RL.renderListsTerrainSolid"); renderListsTerrainSolid(submissions); }
	{ ZoneScopedN("RL.renderListsGpuStaticProps"); renderListsGpuStaticProps(submissions); }
	{ ZoneScopedN("RL.renderListsOverlayBatches"); renderListsOverlayBatches(submissions); }
	{ ZoneScopedN("RL.renderListsOverlaysOld"); renderListsOverlaysOld(submissions); }
}

void mc2::VkSceneRenderer::beginFrame()
{
    renderSubmissions().clearArrays();
    vkLights::resetLightData();
}

bool mc2::VkSceneRenderer::active() { return renderSubmissions().active(); }
void mc2::VkSceneRenderer::start()
{
    mc2::textures::startSources();
    renderSubmissions().start();
    vkLights::start();
    initTexResolveTable();
}
void mc2::VkSceneRenderer::shutdown()
{
    mc2::textures::flushSources(true);
    renderSubmissions().clearTextureBins();
    mc2::effects::shutdown();
    renderSubmissions().destroy();
    vkLights::shutdown();
}
void mc2::VkSceneRenderer::reset()
{
    renderSubmissions().clearTextureBins();
    invalidateTexResolveTable();
#ifdef MC2_VULKAN_ISLAND
    mc2_vk_legacy_shape_island_reset();
    mc2_vk_overlay_reset();
    mc2_vk_vfx_island_reset();
#endif
}
