#include "vk_scene_renderer.h"
// mc2::VkSceneRenderer::renderLists phases 1-7 (state setup, light and scene-data upload, 3D objects,
// static-shadow build, static-prop registry flush); see vk_scene_renderer.cpp for the order they run in.

#include "vk_scene_renderer_internal.h"
#include "mc2fmt.h"
#include "Gates/render_options.h"

extern void mc2SetSceneFogColorRGB(float, float, float);
extern bool g_useGpuStaticProps;
extern bool g_useGpuObjects;

void mc2::VkSceneRenderer::renderListsPreamble (RenderSubmissions& submissions)
{
	{
	ZoneScopedN("RenderLists.Preamble");
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
}

void mc2::VkSceneRenderer::renderListsSceneDataUpload (RenderSubmissions& submissions)
{
#ifdef MC2_VULKAN_ISLAND
    // Vulkan reads this CPU fog snapshot; the legacy GL SceneData UBO is gone.
    const vec4 fc = uint32_to_vec4(eye->fogColor);

    mc2SetSceneFogColorRGB(fc.z, fc.y, fc.x);
#endif
}

void mc2::VkSceneRenderer::renderListsObjects3D (RenderSubmissions& submissions)
{
    // The typed Vulkan islands consume geometry independently of this old GL queue.
    // Drain it so an extra render without a logic tick cannot replay old shapes.
    for (size_t i = 0; i < submissions.nextAvailableHardwareVertexNode; ++i) {
        auto& node = submissions.masterHardwareVertexNodes[i];
        if ((node.flags & MC2_DRAWSOLID) && node.shapes)
            node.currentShape = node.shapes;
    }
}

void mc2::VkSceneRenderer::renderListsPostObjectsStateRestore (RenderSubmissions& submissions)
{
	{
	ZoneScopedN("RenderLists.PostObjectsStateRestore");
	// [Moved in Phase 4 debug] flush() was originally here (after
	// Render.3DObjects). But Render.TerrainSolid runs AFTER us on line
	// ~1287, so terrain was overwriting our building pixels. Flush is
	// now relocated further down, after Render.TerrainSolid completes.

	// restore state as all old-style geometry is culled on CPU and all vertices are already pretransformed
	gos_SetRenderState(gos_State_Culling, gos_Cull_None);

	// restore viewport
	gos_SetRenderViewport(0, 0, Environment.drawableWidth, Environment.drawableHeight);
	} // end RenderLists.PostObjectsStateRestore
}

void mc2::VkSceneRenderer::renderListsStaticShadowBuild (RenderSubmissions& submissions)
{
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
	// ISSUE-34: keyed on the modern shadow path (shadow_terrain material),
	// not tessellation — static shadow map still builds when tess is off.
	if (gos_IsModernShadowPathActive() && !gos_StaticLightMatrixBuilt() &&
	    Terrain::mapData) {
		ZoneScopedN("Shadow.StaticFullMapBuild");

		gos_BuildStaticLightMatrix();   // world-fixed, camera-independent
		gos_MarkStaticLightMatrixBuilt();

		// The static shadow map itself is the VK shadow island's (it draws the terrain and the buildings from the map data); this is only the matrix
		// and the build generation the island and the terrain feed key on.
	}
}

void mc2::VkSceneRenderer::renderListsStaticPropRegistryFlush (RenderSubmissions& submissions)
{
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


		if (g_useGpuStaticProps || g_useGpuObjects) {
			GpuStaticPropRegistry::flush();
		}
	} // end RenderLists.StaticPropRegistryFlush
}
