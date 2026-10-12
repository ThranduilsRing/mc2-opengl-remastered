// Shared surface of the txmmgr_render*.cpp pieces: the renderLists phase helpers, the
// per-frame state they share and the RENDERLISTS cost spans. Declared names are defined
// in txmmgr_render.cpp.
#ifndef TXMMGR_RENDER_INTERNAL_H
#define TXMMGR_RENDER_INTERNAL_H

#include "txmmgr_internal.h"

// Maximum vertices sent down per draw call by the legacy masterVertexNodes loops.
inline constexpr int MAX_SENDDOWN = 10002;

// Number of shadow-shape slots reserved this frame (see addShadowShape).
extern std::atomic<int> g_numShadowShapes;

mat4 gos2my(Stuff::Matrix4D& m);
mat4 gos2my(Stuff::LinearMatrix4D& m);

// Per-frame state the renderLists phases share.
struct MC_TextureManager::RenderListsFrame
{
#ifdef MC2_VULKAN_ISLAND
	bool vkLegacyOn = false;   // VK legacy-shape island armed for this frame
#endif
};

#endif  // TXMMGR_RENDER_INTERNAL_H
