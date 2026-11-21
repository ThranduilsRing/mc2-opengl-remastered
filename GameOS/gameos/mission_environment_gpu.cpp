#include "mission_environment_gpu.h"
#include "../../RenderCore/IblShCoeffs.h"
#include <algorithm>
#include <cmath>

namespace mc2::environment {
namespace {
float luma(float r, float g, float b) { return 0.2126f*r + 0.7152f*g + 0.0722f*b; }
}

void missionL0Chroma(const std::array<float,3>& ambient, float out[3]) {
    out[0] = out[1] = out[2] = 1.0f;
    const float amb = luma(ambient[0], ambient[1], ambient[2]);
    const float* ref = RenderCore::kIblShCoeffs[0];
    const float refL = luma(ref[0], ref[1], ref[2]);
    if (!(amb > 1e-4f) || !(refL > 1e-4f)) return;   // no mission colour: keep the sky's own hue
    for (int c = 0; c < 3; ++c)
        out[c] = std::clamp((ambient[c]/amb) / (ref[c]/refL), 0.6f, 1.6f);
}

void fillGpu(const RenderCore::MissionEnvironment& env, float sceneLightScale, EnvironmentUBO& out) {
    out = EnvironmentUBO{};
    const float night = env.lighting.night ? 1.0f : std::clamp(env.lighting.nightFactor, 0.0f, 1.0f);
    float chroma[3];
    missionL0Chroma(env.lighting.ambientColor, chroma);
    // The ambient key moves the legacy bytes (encoded space); the prop SH and its light floors are linear light, so the same move is ratio^2.2.
    const float ambientLin = env.lighting.ambientRatio == 1.0f ? 1.0f : std::pow(env.lighting.ambientRatio, 2.2f);
    for (int i = 0; i < 9; ++i)
        for (int c = 0; c < 3; ++c) {
            float v = env.sky.diffuseSh[i][c];
            if (i == 0) v *= 1.0f + (chroma[c] - 1.0f) * (1.0f - night);
            v *= ambientLin;   // the sun keys' ambient scale reaches the sky fill (1 without keys)
            out.shDiffuse[i][c] = v;
        }
    for (int c = 0; c < 3; ++c) out.hemiSky[c] = env.sky.hemisphereSky[c] * env.unitAmbientScale;
    out.hemiSky[3] = ambientLin;
    out.sceneLight[0] = sceneLightScale;
    for (int c = 0; c < 3; ++c) out.sceneLight[1 + c] = env.lighting.flatRatio[c];   // the sun keys' move of the flat light, applied to prelit art
}

}
