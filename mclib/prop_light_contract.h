// mclib/prop_light_contract.h
//
// Colour-prop light contract (W1). A static colour prop's sun term is a pure function of
// the camera's authored sun S, ambient A, night floor F and the actor's terrain light I.
// This header is the single CPU statement of that function; the shader
// (color_prop_tier1.vert) reproduces it from the frame UBO + the per-instance word.
//
// contractChannel() is Camera::getLightRed/Green/Blue verbatim: float2long (truncation),
// floor F, clamp 255, and the I <= 0 branch.
//
// Instance word (GpuStaticPropInstance::actorTint, offset 68):
//   bits  0..15  terrain light I as 16-bit fixed point (I * 65535, clamped 0..1)
//   bit   16     kSunOff   (trees: the bake deactivated the infinite light, no sun term)
//   bit   17     kNeutral  (submitters with no terrain light: contract with I = 1)
#pragma once

#include <cstdint>

namespace mc2::prop_light {

constexpr uint32_t kIMask   = 0xffffu;
constexpr uint32_t kSunOff  = 1u << 16;
constexpr uint32_t kNeutral = 1u << 17;

constexpr uint32_t packWord(float I, uint32_t policy) {
    const float c = I < 0.0f ? 0.0f : (I > 1.0f ? 1.0f : I);
    return static_cast<uint32_t>(c * 65535.0f + 0.5f) | policy;
}

constexpr float unpackI(uint32_t word) { return static_cast<float>(word & kIMask) / 65535.0f; }

// Camera::getLightRed/Green/Blue(intensity) for one channel.
constexpr int contractChannel(int S, int A, int F, float I) {
    if (I > 0.0f) {
        long r = static_cast<long>(static_cast<float>(S) * I + static_cast<float>(A));
        if (r < F) r = F;
        return r < 0xff ? static_cast<int>(r) : 0xff;
    }
    return A < F ? F : A;
}

struct SunRgb {
    int  c[3];
    bool sunOn;
};

// The row colour an actor's bake carried (0..255 per channel) under the policy bits.
constexpr SunRgb contractSun(const int S[3], const int A[3], int F, float I, uint32_t policy) {
    SunRgb out{{0, 0, 0}, true};
    if (policy & kSunOff) { out.sunOn = false; return out; }
    const float i = (policy & kNeutral) ? 1.0f : I;
    for (int k = 0; k < 3; ++k) out.c[k] = contractChannel(S[k], A[k], F, i);
    return out;
}

} // namespace mc2::prop_light
