// Vendor-neutral upscaler seam. Consumers that need to know what the active temporal upscaler wants (today: the
// material sampler mip bias) ask this interface and never name a vendor. A vendor (FSR, later DLSS / XeSS) is one
// implementation file plus one registerUpscaler() call.
//
// Deliberately minimal for the sampler-bias lane. Not yet here (follow-up provider-abstraction lane): the per-frame
// input/dispatch contract (color, depth, motion vectors, reactive mask, jitter, exposure, render vs display size,
// history reset), supported quality modes, and the required jitter sequence.
#pragma once

#include <cmath>
#include <string_view>

namespace RenderCore {

class IUpscalerProvider {
public:
    virtual ~IUpscalerProvider() = default;
    virtual std::string_view name() const = 0;
    // True when this provider can resolve on this device/driver right now (SDK present, context creatable, no
    // earlier real failure). A false answer makes the effective provider fall back to native.
    virtual bool available() const = 0;
    // Texture LOD bias for a render-to-display ratio (renderScale = render / display, 1 = native-size input).
    virtual float mipLodBias(float renderScale) const = 0;
};

// Registry: providers register once in preference order. The effective provider is the first available one for the
// current settings; null means native (no temporal resolve).
inline constexpr int kMaxUpscalers = 4;
struct UpscalerRegistry {
    const IUpscalerProvider* providers[kMaxUpscalers] = {};
    int count = 0;
};
inline UpscalerRegistry& upscalerRegistry() { static UpscalerRegistry r; return r; }
inline void registerUpscaler(const IUpscalerProvider* p) {
    UpscalerRegistry& r = upscalerRegistry();
    if (p && r.count < kMaxUpscalers) r.providers[r.count++] = p;
}
inline const IUpscalerProvider* effectiveUpscaler() {
    const UpscalerRegistry& r = upscalerRegistry();
    for (int i = 0; i < r.count; ++i)
        if (r.providers[i]->available()) return r.providers[i];
    return nullptr;
}

}  // namespace RenderCore
