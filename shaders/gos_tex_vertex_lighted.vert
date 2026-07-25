//#version 300 es
// using this because it is required if we want to use "binding" qualifier in layout (can be set in cpp code but it is easier to do in shader, so procedd like this and maybe change later)
//#version 420

#include <include/lighting.hglsl>

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

uniform mat4 mvp_; // TODO: remove, use wvp_ instead

uniform mat4 world_;
uniform mat4 view_;
uniform mat4 wvp_;

// common parameters
uniform mat4 projection_;
uniform vec4 vp; //viewport

uniform float forceZ; // baked in a wvp matrix

// GPU projection: when enabled, use u_worldToClipGL with MC2 world coords
uniform int gpuProjection;
uniform mat4 u_worldToClipGL;  // world -> GL clip (kAxisSwapMC2toGL * worldToClip)

// VEHICLE-NIGHT-LIT-1: coordinate-frame killswitch for the calc_light feed.
//
// ROOT CAUSE. ObjectLights rows produced by GatherLightsParameters
// (mclib/txmmgr.cpp) carry light_to_world translations and light_dir in the
// MC2 world frame. The two OTHER consumers of calc_light both feed it MC2-frame
// inputs:
//   - mech.vert         : calc_light(idx, worldNormal /*MC2*/, worldMC2, base)
//   - static_prop.vert  : worldPos is MC2-frame (Stuff->MC2 swap) and
//                         STATIC-PROP-NORMAL-FRAME-FIX converts the normal via
//                         normalize(vec3(-n.x, n.z, n.y)) for the same reason.
// This shader — the legacy mcTextureManager/ShapeRenderer lit path, which is
// what draws GROUND VEHICLES (GVAppearance::render -> gvShape->Render ->
// TG_Shape::Render -> addRenderShape -> ShapeRenderer::render) — was the last
// consumer still feeding STUFF-frame position and normal. Consequences:
//   - POINT / SPOT / INFINITEWITHFALLOFF: length(vertex_world_pos - light_pos)
//     mixes frames, so the distance is garbage and GetFalloff() rejects the
//     light. Vehicles receive NO compound pool light and NO guard-tower cone
//     light even when parked inside the cone.
//   - INFINITE (sun): dot(stuffNormal, -mc2LightDir) is ~90deg off and clamps
//     to 0 on most vertices.
// What is left is base_light (the static per-vertex aRGBLight seed) plus
// AMBIENT — which at night reads as a near-black silhouette. Hence "truck dark,
// mech lit, buildings lit, in the same pool of light".
//
// 0 = legacy Stuff-frame feed (byte-identical to pre-fix). 1 = MC2-frame feed,
// matching mech.vert / static_prop.vert. Driven by MC2_LEGACY_LIT_MC2_FRAME.
uniform int u_legacyLitMc2Frame;

out vec3 Normal;
out float FogValue;
out vec2 Texcoord;
out vec4 VertexColor;
out vec3 VertexLight;
out vec3 WorldPos;
out vec3 CameraPos;
out vec3 MC2WorldPos;

void main(void)
{
    // World-space position in Stuff coordinates (x=left, y=up, z=forward)
    WorldPos = (world_ * vec4(pos.xyz, 1.0)).xyz;

    // MC2 world coordinates (x=east, y=north, z=elev) for shadow sampling
    MC2WorldPos = vec3(-WorldPos.x, WorldPos.z, WorldPos.y);

    if (gpuProjection != 0) {
        // GPU projection: transform to MC2 world space, then to clip via u_worldToClipGL
        gl_Position = u_worldToClipGL * vec4(MC2WorldPos, 1.0);
    } else {
        // Legacy CPU-assist projection: manual perspective divide + viewport mapping
        vec4 p = wvp_ * vec4(pos.xyz, 1);
        float rhw = 1 / p.w;

        p.x = (p.x * rhw) * vp.z + vp.x + 100.0;
        p.y = (p.y * rhw) * vp.w + vp.y;
        p.z = p.z * rhw;
        p.w = abs(rhw);

        vec4 p2 = projection_ * vec4(p.xyz,1);
        gl_Position = p2 / p.w;
    }

    Normal = (world_ * vec4(normal, 0)).xyz;
    Texcoord = texcoord;

    // base light can be only calculated in VS because relies on exact vertex colors
    // TODO: pass correct parameters here
    vec3 base_light = get_base_light(aRGBLight.bgra, false, 0.0, false, false,
            vec3(0.0), vec3(0.0), vec3(0.0));

#if ENABLE_VERTEX_LIGHTING
    const int lights_index = int(light_offset_.x);
    // Slice 2 (object-offload) — Stage 2.C: calc_light() signature changed
    // to 4-param.
    //
    // VEHICLE-NIGHT-LIT-1: the original comment here claimed "distances are
    // rotation-invariant so any consistent world space works". That is true
    // only if BOTH operands share a frame. They do not: the light positions in
    // the ObjectLights row are MC2-frame, so feeding a Stuff-frame vertex
    // position makes the distance meaningless. Same for the normal vs
    // light_dir. See the u_legacyLitMc2Frame block above.
    if (u_legacyLitMc2Frame != 0) {
        // MC2-frame feed, matching mech.vert and static_prop.vert.
        // MC2WorldPos is already the Stuff->MC2 swap of WorldPos (line above);
        // apply the identical swap to the normal.
        vec3 normalMC2 = normalize(vec3(-Normal.x, Normal.z, Normal.y));
        VertexLight = calc_light(lights_index, normalMC2, MC2WorldPos, base_light);
    } else {
        VertexLight = calc_light(lights_index, Normal, WorldPos, base_light);
    }
#else
    VertexLight = base_light;
#endif

    VertexColor = aRGBLight.bgra;
}

