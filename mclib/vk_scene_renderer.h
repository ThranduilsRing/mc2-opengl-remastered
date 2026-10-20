// Native Vulkan scene pass executor. Geometry staging belongs to the submission queue.
#pragma once
#include <cstdint>
class MC_TextureManager;
namespace mc2 {
class VkSceneRenderer {
public:
		//Sends down the triangle lists
		static void renderLists (MC_TextureManager& submissions);

	private:
		// Passes run in the order defined by vk_scene_renderer.cpp.
		static void renderListsPreamble (MC_TextureManager& submissions);
		static void renderListsSceneDataUpload (MC_TextureManager& submissions);
		static void renderListsObjects3D (MC_TextureManager& submissions);
		static void renderListsPostObjectsStateRestore (MC_TextureManager& submissions);
		static void renderListsStaticShadowBuild (MC_TextureManager& submissions);
		static void renderListsStaticPropRegistryFlush (MC_TextureManager& submissions);
		static void renderListsDynamicShadowPass (MC_TextureManager& submissions);
		static void renderListsTerrainSolid (MC_TextureManager& submissions);
		static void renderListsGpuStaticProps (MC_TextureManager& submissions);
		static void renderListsOverlayBatches (MC_TextureManager& submissions);
		static void renderListsOverlaysWaterLoops (MC_TextureManager& submissions);
		static void renderListsOverlaysNoUnderlayer (MC_TextureManager& submissions);
		static void renderListsOverlaysShadowBlobs (MC_TextureManager& submissions);
		static void renderListsOverlaysNonTerrainAlpha (MC_TextureManager& submissions);
		static void renderListsOverlaysVfxHudSubmit (MC_TextureManager& submissions);
		static void renderListsOverlaysOld (MC_TextureManager& submissions);
		// VK legacy-shape island queue / run (defined only under MC2_VULKAN_ISLAND).
		static void vkLegacySubmitNode (MC_TextureManager& submissions, long i, uint32_t pass, uint32_t alphaTest);
		static void vkLegacyRun (MC_TextureManager& submissions);
};
}
