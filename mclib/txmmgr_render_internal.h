// Shared surface of the txmmgr_render*.cpp pieces: the renderLists phase helpers, the
// per-frame state they share and the RENDERLISTS cost spans. Declared names are defined
// in txmmgr_render.cpp.
#ifndef TXMMGR_RENDER_INTERNAL_H
#define TXMMGR_RENDER_INTERNAL_H

#include "txmmgr_internal.h"

// Number of shadow-shape slots reserved this frame (see addShadowShape).
extern std::atomic<int> g_numShadowShapes;


#endif  // TXMMGR_RENDER_INTERNAL_H
