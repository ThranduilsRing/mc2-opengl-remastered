#pragma once

#include "effect.hpp"
#include <mlr/mlrclipper.hpp>

namespace gosFX {

// The submission owns its transform until MLR consumes it; particle geometry stays class-specific.
class MlrCloudDraw {
public:
    glm::mat4 localToWorld;

    MlrCloudDraw(Effect::DrawInfo* info, const MidLevelRenderer::MLRState& state,
                 MidLevelRenderer::MLREffect* effect, const glm::mat4& localToParent)
    {
        draw.effect = effect;
        draw.state.Combine(info->m_state, state);
        draw.clippingFlags = info->m_clippingFlags;
        localToWorld = localToParent * *info->m_parentToWorld;
        draw.effectToWorld = &localToWorld;
    }
    MlrCloudDraw(const MlrCloudDraw&) = delete;
    MlrCloudDraw& operator=(const MlrCloudDraw&) = delete;

    void submit(Effect::DrawInfo* info) { info->m_clipper->DrawEffect(&draw); }

private:
    MidLevelRenderer::DrawEffectInformation draw;
};

} // namespace gosFX
