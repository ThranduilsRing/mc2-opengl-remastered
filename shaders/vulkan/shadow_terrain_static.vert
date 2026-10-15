// The terrain casters of the VK static sun shadow map (vulkan_shadow_static_map.cpp).
// GL twin: shaders/shadow_terrain.{vert,tesc,tese} in the static pre-pass, which projects the world position with the static light matrix (the dirt
// displacement there samples the colormap at uv 0, so this flat port leaves it out). The vertex is already an MC2 world position, so no axis swap: the
// prop casters swap because their model matrices produce Stuff-space positions.
#version 450

layout(location = 0) in vec3 a_position;

// LOCKSTEP with C++ StaticFrameUBO and shadow_prop.vert ShadowFrameUBO (std140; instBase unused here).
layout(set = 0, binding = 0, std140) uniform ShadowFrameUBO {
    mat4 lightSpaceMatrix;
    int  instBase;
    int  _pad0;
    int  _pad1;
    int  _pad2;
} uFrame;

void main() {
    gl_Position = uFrame.lightSpaceMatrix * vec4(a_position, 1.0);
}
