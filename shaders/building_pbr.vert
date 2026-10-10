//#version 420

#include <include/lighting.hglsl>
#include <include/terrain_depth_bias.hglsl>  // TERRAIN-DEPTH-BIAS-OWNERSHIP-1: OBJECT_DEPTH_BIAS

layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 aRGBLight;
layout(location = 3) in vec2 texcoord;

layout (binding = 1, std140) uniform mesh_data
{
  vec4 ambient;
  vec4 diffuse;
};

uniform vec4 light_offset_;
uniform mat4 world_;
uniform mat4 wvp_;
uniform mat4 projection_;
uniform vec4 vp;
uniform int gpuProjection;
uniform mat4 u_worldToClipGL;

out vec3 Normal;
out vec2 Texcoord;
out vec4 VertexColor;
out vec3 VertexLight;
out vec3 WorldPos;
out vec3 CameraPos;
out vec3 MC2WorldPos;

void main(void)
{
    WorldPos = (world_ * vec4(pos.xyz, 1.0)).xyz;
    MC2WorldPos = mc2ToRender(WorldPos);

    if (gpuProjection != 0) {
        gl_Position = u_worldToClipGL * vec4(MC2WorldPos, 1.0);
    } else {
        vec4 p = wvp_ * vec4(pos.xyz, 1);
        float rhw = 1 / p.w;
        p.x = (p.x * rhw) * vp.z + vp.x + 100.0;
        p.y = (p.y * rhw) * vp.w + vp.y;
        p.z = p.z * rhw;
        p.w = abs(rhw);
        vec4 p2 = projection_ * vec4(p.xyz,1);
        gl_Position = p2 / p.w;
    }
    // TERRAIN-DEPTH-BIAS-OWNERSHIP-1: buildings/turrets/generators take
    // OBJECT_DEPTH_BIAS (BOTH projection paths) so they sit on top of cement/road
    // transition tiles (no sinking) over true-depth terrain; tiny -> no distance
    // show-through.
    gl_Position.z += OBJECT_DEPTH_BIAS * gl_Position.w;

    Normal = (world_ * vec4(normal, 0)).xyz;
    Texcoord = texcoord;

    vec3 base_light = get_base_light(aRGBLight.bgra, false, 0.0, false, false,
            vec3(0.0), vec3(0.0), vec3(0.0));

#if ENABLE_VERTEX_LIGHTING
    const int lights_index = int(light_offset_.x);
    VertexLight = calc_light(lights_index, Normal, WorldPos, base_light);
#else
    VertexLight = base_light;
#endif

    VertexColor = aRGBLight.bgra;
}
