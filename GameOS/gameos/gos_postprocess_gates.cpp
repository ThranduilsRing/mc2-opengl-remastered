// gosPostProcess: shader diagnostics and cloud control.
#include "gos_postprocess_internal.h"

int mc2ShaderPathTint()
{
    static const int value = 0;
    return value;
}

bool mc2CloudShadowEnabled()
{
    static const bool enabled = mc2::gate::get<"MC2_CLOUD_SHADOW">();
    return enabled;
}
