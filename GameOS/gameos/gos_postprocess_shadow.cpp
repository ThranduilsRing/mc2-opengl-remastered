// Shared shadow sun clamp and the one-map debug overlay.
#include "gos_postprocess_internal.h"
float mc2ShadowMinSunAltDeg()
{
    static const float s_minAltDeg = mc2::gate::get<"MC2_SHADOW_MIN_SUN_ALT">();
    return s_minAltDeg;
}

void mc2ClampShadowSunAltitude(float& fx, float& fy, float& fz)
{
    const float s_minAltDeg = mc2ShadowMinSunAltDeg();
    if (s_minAltDeg <= 0.0f) return;
    const float minDown = sinf(s_minAltDeg * 0.01745329252f);
    const float down = -fz;              // >0 when pointing downward
    if (down >= minDown) return;         // healthy day sun: untouched
    const float horiz = sqrtf(fx * fx + fy * fy);
    const float ch = sqrtf(1.0f - minDown * minDown);
    if (horiz > 1e-6f) {
        fx = fx / horiz * ch;
        fy = fy / horiz * ch;
    } else {
        fx = ch; fy = 0.0f;              // degenerate straight-up/down input
    }
    fz = -minDown;
}
void gosPostProcess::drawShadowDebugOverlay() {
    if (!showShadowDebug_ || !initialized_) return;
#if defined(MC2_VULKAN) && defined(MC2_VULKAN_ISLAND)
    if (mc2_vk_shadow_island_movers_ready())
        if (void* view=mc2_vk_shadow_island_get_mover_debug_view())
            mc2vk::getOrCreateEnginePresentVk().renderNativeCsmShadowDebugOverlay(view,0);
#endif
}
