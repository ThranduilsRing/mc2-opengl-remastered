#version 450
layout(constant_id = 0) const bool HDR_OPAQUE = false;

layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vColor;
    if (HDR_OPAQUE) outColor.a = 1.0;
}
