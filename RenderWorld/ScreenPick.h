// RenderWorld/ScreenPick.h
//
// Pure screen-to-FBO-pixel coordinate math, extracted from
// code/gameplay_pick.cpp. Consumed by:
//   - code/gameplay_pick.cpp    (gameplay pick spine; owns its own gos_GetViewport wrapper)
//   - EditorBridge/EditorRenderBridge.cpp  (editor pick; owns its own gos_GetViewport wrapper)
//
// Firewall: includes only <cstdint>. No game, GL, or GameOS headers.
// No screenToFboPixel declaration here -- that function lives in each consumer
// as a private static, because it calls gos_GetViewport (GameOS runtime).
// Keeping GameOS out of RenderWorld is a load-bearing firewall invariant.

#pragma once
#include <cstdint>

namespace RenderWorld {

// Keep output-pixel clicks aligned with a spatially upscaled scene. Reject
// outside clicks before integer division can truncate a negative pixel to zero.
inline bool screenPickScenePixel(int& x, int& y, int outputW, int outputH, int sceneW, int sceneH) {
    if (outputW <= 0 || outputH <= 0 || sceneW <= 0 || sceneH <= 0 ||
        x < 0 || y < 0 || x >= outputW || y >= outputH) return false;
    x = static_cast<int>(static_cast<int64_t>(x) * sceneW / outputW);
    y = static_cast<int>(static_cast<int64_t>(y) * sceneH / outputH);
    return true;
}

// All fields. Populated by the caller's local screenToFboPixel wrapper,
// or set manually for unit testing (no GL context required).
struct ScreenPickContext {
    int   mouseX = 0, mouseY = 0;    // Win32 origin top-left (input)
    int   glX = 0,    glY = 0;       // GL origin bottom-left (output)
    int   fboX = 0,   fboY = 0;      // FBO pixel, top-left origin (output)
    int   drawableWidth  = 0;
    int   drawableHeight = 0;
    float vMulX = 0.f, vMulY = 0.f;  // gos_GetViewport scale (FBO-pixel units)
    float vAddX = 0.f, vAddY = 0.f;  // gos_GetViewport offset
};

// Pure coord computation. Caller populates mouseX/Y and all viewport fields
// before calling. Fills fboX/Y and glX/Y. No-op if vMulX/Y <= 0.
//
// Derivation: fboX = vAddX + mouseX * (drawableWidth / vMulX)
//             glY  = drawableHeight - 1 - fboY  (GL y-flip)
// Identical math to the former gameplay_pick.cpp screenToFboPixel.
inline void screenPickCompute(ScreenPickContext* ctx) {
    if (!ctx || ctx->vMulX <= 0.0f || ctx->vMulY <= 0.0f) return;
    const float scaleX = static_cast<float>(ctx->drawableWidth)  / ctx->vMulX;
    const float scaleY = static_cast<float>(ctx->drawableHeight) / ctx->vMulY;
    ctx->fboX = static_cast<int>(ctx->vAddX + static_cast<float>(ctx->mouseX) * scaleX);
    ctx->fboY = static_cast<int>(ctx->vAddY + static_cast<float>(ctx->mouseY) * scaleY);
    ctx->glX  = ctx->fboX;
    ctx->glY  = ctx->drawableHeight - 1 - ctx->fboY;
}

} // namespace RenderWorld
