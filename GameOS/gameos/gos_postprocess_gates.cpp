// gosPostProcess: shader diagnostics, mech terminator softness and cloud control.
#include "gos_postprocess_internal.h"

int mc2ShaderPathTint()
{
    static const int value = mc2::gate::get<"MC2_SHADER_PATH_TINT">();
    return value;
}

// Wider smoothstep around the mech terminator; prop receivers keep their floor.
float mc2ShadowMechSoft()
{
    static const float value = mc2::gate::get<"MC2_SHADOW_MECH_SOFT">();
    return value;
}

bool mc2CloudShadowEnabled()
{
    static const bool enabled = mc2::gate::get<"MC2_CLOUD_SHADOW">();
    return enabled;
}
