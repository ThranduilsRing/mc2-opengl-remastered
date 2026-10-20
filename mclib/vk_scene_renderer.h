// Native Vulkan scene pass executor. Geometry staging belongs to the submission queue.
#pragma once
#include <cstdint>
namespace mc2 { class RenderSubmissions; }
namespace mc2 {
class VkSceneRenderer {
public:
    static void beginFrame();
		//Sends down the triangle lists
		static void renderLists (RenderSubmissions& submissions);

	private:
		// Passes run in the order defined by vk_scene_renderer.cpp.
		static void renderListsPreamble (RenderSubmissions& submissions);
		static void renderListsSceneDataUpload (RenderSubmissions& submissions);
		static void renderListsObjects3D (RenderSubmissions& submissions);
		static void renderListsPostObjectsStateRestore (RenderSubmissions& submissions);
		static void renderListsStaticShadowBuild (RenderSubmissions& submissions);
		static void renderListsStaticPropRegistryFlush (RenderSubmissions& submissions);
		static void renderListsDynamicShadowPass (RenderSubmissions& submissions);
		static void renderListsTerrainSolid (RenderSubmissions& submissions);
		static void renderListsGpuStaticProps (RenderSubmissions& submissions);
		static void renderListsOverlayBatches (RenderSubmissions& submissions);
		static void renderListsOverlaysWaterLoops (RenderSubmissions& submissions);
		static void renderListsOverlaysNoUnderlayer (RenderSubmissions& submissions);
		static void renderListsOverlaysShadowBlobs (RenderSubmissions& submissions);
		static void renderListsOverlaysNonTerrainAlpha (RenderSubmissions& submissions);
		static void renderListsOverlaysVfxHudSubmit (RenderSubmissions& submissions);
		static void renderListsOverlaysOld (RenderSubmissions& submissions);
		// VK legacy-shape island queue / run (defined only under MC2_VULKAN_ISLAND).
		static void vkLegacySubmitNode (RenderSubmissions& submissions, long i, uint32_t pass, uint32_t alphaTest);
		static void vkLegacyRun (RenderSubmissions& submissions);
};
}
