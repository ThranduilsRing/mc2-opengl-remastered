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

// VEHICLE-AMBIENT-FLOOR-1: give this path mech.vert's ambient FLOOR and its
// output CLAMP, so a vehicle and a mech standing side by side compute the SAME
// multiplier instead of the vehicle landing below.
//
// WHAT WAS STILL MISSING AFTER VEHICLE-LIGHTING-PATH-1. That slice gave
// vehicles a populated ObjectLights row, which is necessary and was the whole
// bug for "unlit". But mech.vert does two more things this shader never did
// (shaders/mech.vert:204 and :223, read at 1c4e2b94):
//     const float kAmbientFloor = 0.35;
//     vec3 base   = vec3(kAmbientFloor);
//     vec3 litRGB = calc_light(idx, worldNormal, worldMC2, base);
//     baseLight   = clamp(litRGB + highlight, 0.0, 1.0);
// The 0.35 seed is what keeps a shadow-side mech from going near-black; the
// clamp is what stops a lit-side mech from running past 1.0. Vehicles had
// neither, so post-VEHICLE-LIGHTING-PATH-1 their multiplier was
//     M = base_light + A + S*ndl
// against a mech's
//     M = clamp(0.35 + A + S*ndl, 0, 1)
// where A = AmbientRed/255 and S = getLightRed(terrainLight)/255
// (camera.cpp:4392-4408; slot 1 is the AMBIENT light at camera.cpp:516, slot 0
// the INFINITE sun whose colour gvactor.cpp:2611-2620 sets per vehicle).
// On mc2_10 (AmbientRed 192) that is 0.753 for the truck against 1.0 for the
// Werewolf beside it -- the user's original report, surviving the fix that was
// supposed to cure it. On mc2_03 (AmbientRed 31) the vehicle PEAK is 0.796,
// i.e. below 1.0 at every normal, so every vehicle pixel got darker.
//
// SELF-LIMITING BY CONSTRUCTION. This program is shared with other legacy lit
// shapes, but a shape that never gathers still has numLights == 0, and
// calc_light returns vec3(1,1,1) for it before it ever reads base_light
// (lighting.hglsl:232-236 -- the MC2_STATIC_PROP_LIGHTING branch is NOT taken
// here). clamp(vec3(1),0,1) == vec3(1). So non-gathering shapes are
// bit-identical with this on; only shapes carrying a real light row move.
//
// max() NOT plain assignment. mech.vert can hand calc_light a bare vec3(0.35)
// because it has no per-vertex seed to lose. This shader does: get_base_light
// still implements the hot-colour window/lamp conventions. max(base_light,
// 0.35) is a true FLOOR -- it can only ever raise the seed, never discard it.
//
// WHAT base_light ACTUALLY IS ON STOCK VEHICLES -- measured, not assumed.
// tgl.cpp:1093 seeds aRGBLight to 0xff000000, but the ASE loader OVERWRITES it
// from *MESH_VERTCOL whenever *MESH_NUMCVFACES is present (tgl.cpp:1319-1448),
// and every stock vehicle ASE has that block. The binary .tgl loader then reads
// the whole TG_TypeVertex array verbatim (tgl.cpp:641), so whatever was baked
// ships. Counted over the 8 stock ground-vehicle .tgl binaries:
//     0xff000000 (0.0000)   56-110 per file   -- the overwhelming majority
//     0xff010101 (0.0039)    3-6   per file
//     0xff646257 (0.3922)    1-2   per file   -- the ONLY value above the floor
//     0xffece7cc (0.9255)    0     per file   -- in the ASE palette, but no
//                                               CFACE ever references it
// So the floor raises ~97%+ of vehicle vertices and is inert on at most two,
// and the largest above-floor seed is 0.392 -- 12% over the floor, not a
// multiple of it. Per-component max() also nudges that vertex's G/B (0.384 /
// 0.341) up to 0.35, a <= 0.009 hue shift on those two vertices; mech.vert
// flattens all three channels to 0.35 outright, so this is the milder form.
//
// 0 = pre-slice behaviour, byte-identical. 1 = floor + clamp.
// Driven by MC2_VEHICLE_AMBIENT_FLOOR, DEFAULT-ON (mclib/txmmgr.cpp).
uniform int u_vehicleAmbientFloor;

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

    // VEHICLE-AMBIENT-FLOOR-1: same constant as shaders/mech.vert:204, kept as
    // a literal in both places deliberately -- lighting.hglsl is included by
    // shaders that must NOT get a floor (terrain, particles), so hoisting it
    // there would widen the blast radius far past the object paths.
    const float kAmbientFloor = 0.35;
    vec3 seed_light = (u_vehicleAmbientFloor != 0)
                    ? max(base_light, vec3(kAmbientFloor))
                    : base_light;

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
    vec3 lit;
    if (u_legacyLitMc2Frame != 0) {
        // MC2-frame feed, matching mech.vert and static_prop.vert.
        // MC2WorldPos is already the Stuff->MC2 swap of WorldPos (line above);
        // apply the identical swap to the normal.
        vec3 normalMC2 = normalize(vec3(-Normal.x, Normal.z, Normal.y));
        lit = calc_light(lights_index, normalMC2, MC2WorldPos, seed_light);
    } else {
        lit = calc_light(lights_index, Normal, WorldPos, seed_light);
    }
    // VEHICLE-AMBIENT-FLOOR-1: mech.vert:223 clamps its calc_light result to
    // [0,1] before it leaves the vertex stage. Without the same clamp here a
    // vehicle on a bright-ambient map overshoots the mech it is parked beside
    // (mc2_10 lit faces reach 1.506 against the mech's 1.0), so shipping the
    // floor without the clamp would swap one mismatch for its mirror image.
    // Applied to BOTH frame branches: MC2_LEGACY_LIT_MC2_FRAME is an orthogonal
    // gate and its =0 revert must not silently drag this one with it.
    VertexLight = (u_vehicleAmbientFloor != 0) ? clamp(lit, 0.0, 1.0) : lit;
#else
    // Unreachable: lighting.hglsl:8 defines ENABLE_VERTEX_LIGHTING 1 globally.
    // Left on base_light so this dead branch carries no claim of its own.
    VertexLight = base_light;
#endif

    VertexColor = aRGBLight.bgra;
}

