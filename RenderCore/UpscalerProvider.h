// Vendor-neutral temporal upscaler seam (modelled on UE5's ITemporalUpscaler and Streamline's per-frame constants).
// Present and the material samplers talk only to IUpscalerProvider; a vendor (FSR today, DLSS / XeSS later) is one
// provider file plus one registration line in GameOS/gameos/vk_upscalers.cpp. See docs/plans/upscaler-providers.md.
//
// Backend-neutral on purpose: images and the command list are opaque handles (VkImage / VkImageView / VkFormat /
// VkImageUsageFlags / VkCommandBuffer on the Vulkan backend), so this header compiles in the offline unit tests.
#pragma once

#include <cmath>
#include <cstdint>
#include <string_view>

namespace RenderCore {

// ---- Capabilities ---------------------------------------------------------------------------------------------------

// Named quality modes. Each provider publishes the subset it supports with its own render scale (render / display per
// axis; vendors differ, e.g. FSR Quality = 1/1.5, DLSS Quality = 0.667, XeSS 1.3 Quality = 1/1.7).
enum class UpscalerQuality : uint8_t { NativeAA, UltraQuality, Quality, Balanced, Performance, UltraPerformance };

struct UpscalerQualityMode {
    UpscalerQuality quality;
    float renderScale;   // render / display per axis, (0, 1]
};

// Units the provider expects its motion vectors in, after the engine applies UpscalerFrame::motionVectorScale.
// Engine vectors (docs/plans/2026-11-06-temporal-aa.md): render-target UV, previousUV - currentUV, +y down.
enum class MotionVectorUnits : uint8_t { RenderPixels, Uv };

struct UpscalerCaps {
    const UpscalerQualityMode* modes = nullptr;   // static table, best quality first
    int modeCount = 0;
    MotionVectorUnits mvUnits = MotionVectorUnits::RenderPixels;
    float mvSign[2] = {1.0f, 1.0f};   // -1 on an axis when the provider wants current - previous or +y up
    bool usesReactiveMask = true;
    bool usesTransparencyMask = false;
};

// Render scale for a quality mode; false when the provider does not offer it.
inline bool renderScaleFor(const UpscalerCaps& caps, UpscalerQuality q, float& scale) {
    for (int i = 0; i < caps.modeCount; ++i)
        if (caps.modes[i].quality == q) { scale = caps.modes[i].renderScale; return true; }
    return false;
}

// Render extent for a display extent at a render scale: rounded to the nearest pixel, never 0, never above display.
inline void renderExtentFor(uint32_t displayW, uint32_t displayH, float scale, uint32_t& renderW, uint32_t& renderH) {
    auto one = [scale](uint32_t d) {
        const uint32_t r = static_cast<uint32_t>(std::lround(static_cast<double>(d) * static_cast<double>(scale)));
        return r < 1u ? 1u : (r > d ? d : r);
    };
    renderW = one(displayW);
    renderH = one(displayH);
}

// Scale that converts engine motion vectors (UV) into the provider's convention for this render extent.
inline void motionVectorScaleFor(const UpscalerCaps& caps, uint32_t renderW, uint32_t renderH, float out[2]) {
    const bool px = caps.mvUnits == MotionVectorUnits::RenderPixels;
    out[0] = caps.mvSign[0] * (px ? static_cast<float>(renderW) : 1.0f);
    out[1] = caps.mvSign[1] * (px ? static_cast<float>(renderH) : 1.0f);
}

// Jitter phase count the current vendors recommend: 8 * (display / render)^2, at least 8.
inline uint32_t defaultJitterPhaseCount(uint32_t renderW, uint32_t displayW) {
    const float s = renderW ? static_cast<float>(displayW) / static_cast<float>(renderW) : 1.0f;
    const uint32_t n = static_cast<uint32_t>(std::ceil(8.0f * s * s));
    return n < 8u ? 8u : n;
}

// ---- Per-frame contract ---------------------------------------------------------------------------------------------

struct UpscalerImage {
    uint64_t image = 0;    // VkImage
    uint64_t view = 0;     // VkImageView (output only; inputs may leave it 0)
    uint32_t format = 0;   // VkFormat
    uint32_t usage = 0;    // VkImageUsageFlags the image was created with
    uint32_t width = 0, height = 0;
    bool valid() const { return image != 0 && width != 0 && height != 0; }
};

// Everything a provider reads for one dispatch. Layouts on entry and on return (the provider restores them): color,
// motionVectors, reactive, transparency SHADER_READ_ONLY_OPTIMAL; depth DEPTH_STENCIL_ATTACHMENT_OPTIMAL. The output
// is left in GENERAL and made visible to fragment-shader reads before dispatch() returns.
struct UpscalerFrame {
    void* commandList = nullptr;          // VkCommandBuffer, recording, outside a render pass
    UpscalerImage color;                  // linear HDR scene colour at render size
    UpscalerImage depth;                  // reverse-Z [0,1], not infinite
    UpscalerImage motionVectors;          // RG16F, engine convention (see MotionVectorUnits)
    UpscalerImage reactive;               // R8, 1 = do not trust history (VFX, mechs until per-object vectors)
    UpscalerImage transparency;           // R8 transparency-and-composition mask; may be invalid
    uint32_t renderWidth = 0, renderHeight = 0;
    uint32_t displayWidth = 0, displayHeight = 0;
    float jitterPx[2] = {0.0f, 0.0f};     // offset the scene feeds applied this frame, render pixels (+x right, +y down)
    float motionVectorScale[2] = {1.0f, 1.0f};   // from motionVectorScaleFor(caps(), ...)
    float preExposure = 1.0f;             // colour was multiplied by this before the provider
    bool autoExposure = true;             // no exposure texture: the provider computes its own
    float frameDeltaMs = 16.6f;
    uint32_t frameIndex = 0;
    bool reset = false;                   // camera cut / mission load / target recreation: discard history
    // Real scene projection (RenderCore::temporal::projection()): positive world-unit distances, vertical FOV in
    // radians, depth convention. Render vs display size is the renderWidth/displayWidth pair above.
    float cameraNear = 1.0f, cameraFar = 20000.0f, cameraFovY = 0.6f;
    bool depthReversed = true, depthInfinite = false, perspective = true;
};

// ---- Provider -------------------------------------------------------------------------------------------------------

// Lifecycle, all on the thread that records the composite: prepare() each frame the upscaler is wanted (allocates or
// resizes; false = run native this frame), dispatch() records into frame.commandList, deactivate() on frames it is not
// wanted, resetHistory() on a camera cut or target recreation, releaseResources() before the device goes away. A real
// create or dispatch failure makes available() false for good, so the registry falls back to the next provider or native.
class IUpscalerProvider {
public:
    virtual ~IUpscalerProvider() = default;
    virtual std::string_view name() const = 0;
    // SDK present, device features present, context creatable, no earlier real failure. Cheap after the first call.
    virtual bool available() const = 0;
    virtual const UpscalerCaps& caps() const = 0;
    virtual uint32_t jitterPhaseCount(uint32_t renderW, uint32_t displayW) const {
        return defaultJitterPhaseCount(renderW, displayW);
    }
    // Texture LOD bias for render / display (1 = native-size input). Vendor guidance: log2(render / display) - 1.
    virtual float mipLodBias(float renderScale) const { return std::log2(renderScale) - 1.0f; }
    virtual bool prepare(const UpscalerFrame& frame) = 0;
    virtual bool dispatch(const UpscalerFrame& frame) = 0;
    virtual UpscalerImage output() const = 0;   // display size, valid after a successful dispatch
    virtual void deactivate() {}
    virtual void resetHistory() = 0;
    virtual void releaseResources() = 0;
};

// ---- Registry -------------------------------------------------------------------------------------------------------

// Providers register once, in fallback order. The effective provider is the preferred one when it is available, else
// the first available one in registration order; null means native (no temporal resolve).
inline constexpr int kMaxUpscalers = 4;
struct UpscalerRegistry {
    IUpscalerProvider* providers[kMaxUpscalers] = {};
    int count = 0;
};
inline UpscalerRegistry& upscalerRegistry() { static UpscalerRegistry r; return r; }

inline bool registerUpscaler(UpscalerRegistry& r, IUpscalerProvider* p) {
    if (!p || r.count >= kMaxUpscalers) return false;
    for (int i = 0; i < r.count; ++i)
        if (r.providers[i] == p || r.providers[i]->name() == p->name()) return false;
    r.providers[r.count++] = p;
    return true;
}
inline bool registerUpscaler(IUpscalerProvider* p) { return registerUpscaler(upscalerRegistry(), p); }

inline IUpscalerProvider* effectiveUpscaler(const UpscalerRegistry& r, std::string_view preferred = {}) {
    if (!preferred.empty())
        for (int i = 0; i < r.count; ++i)
            if (r.providers[i]->name() == preferred && r.providers[i]->available()) return r.providers[i];
    for (int i = 0; i < r.count; ++i)
        if (r.providers[i]->available()) return r.providers[i];
    return nullptr;
}
inline IUpscalerProvider* effectiveUpscaler() { return effectiveUpscaler(upscalerRegistry()); }

inline void releaseUpscalers(UpscalerRegistry& r = upscalerRegistry()) {
    for (int i = 0; i < r.count; ++i) r.providers[i]->releaseResources();
}

}  // namespace RenderCore
