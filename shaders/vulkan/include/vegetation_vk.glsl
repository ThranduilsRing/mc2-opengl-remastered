// Native crossed-card geometry. Instance culling uses the same CPU block/distance inputs.
#include "../../include/terrain_depth_bias.hglsl"
void mc2VegetationVertex() {
    const uint indices[36] = uint[](0,1,2,1,3,2,2,3,4,3,5,4,4,5,6,5,7,6,
                                   8,9,10,9,11,10,10,11,12,11,13,12,12,13,14,13,15,14);
    Particle p = particles[uint(gl_VertexIndex)/36u];
    uint vi = indices[uint(gl_VertexIndex)%36u];
    uint row = (vi%8u)/2u;
    const float heights[4] = float[](0.0,0.333333,0.666667,1.0);
    vec3 a_card = vec3((vi&1u)==0u ? -0.5 : 0.5,heights[row],vi>=8u ? 1.5707963 : 0.0);
    vec2 a_uv = vec2((vi&1u)==0u ? 0.0 : 1.0,heights[row]);
    vec3 i_worldPos=p.position;
    float i_yaw=p._pad0,i_scale=p.color.x,i_seed=p.color.y;
    uint i_atlasFrame=p.kind_flags;
    float lodFade=p.size;
    uint atlasIdx=i_atlasFrame&0x0fu;
    v_uv=vec2(a_uv.x*0.25+float(atlasIdx%4u)*0.25,float(atlasIdx/4u)*0.5+(1.0-a_uv.y)*0.5);
    v_vegData.w=p.color.z;
    v_flags=16u;
    // Card role from bits 4-5 of i_atlasFrame:
    //   0 = vertical    (~40% of instances, 0° tilt)
    //   1 = tilted      (~25% of instances, 45° from vertical)
    //   2 = top/cap     (~15% of instances, 80° from vertical — nearly horizontal)
    //   3 = ground cover (~20% of instances, 20° splay, 0.25× scale, no wind)
    uint  cardRole  = (i_atlasFrame >> 4u) & 0x3u;
    float tiltAngle = (cardRole == 3u) ? 0.3491          // ground cover: 20° splay
                    : (cardRole == 2u) ? 1.3963           // top/cap: 80°
                    : (cardRole == 1u) ? 0.7854           // tilted: 45°
                    : 0.0;                                // vertical: 0°
    float vertFactor = cos(tiltAngle);
    float flatFactor = sin(tiltAngle);

    // Scale multipliers: top/cap smaller; ground cover very short (1.25-2.5 WU).
    float effectiveScale = i_scale * (cardRole == 3u ? 0.25
                                     : cardRole == 2u ? 0.6
                                     : 1.0);

    float totalYaw = i_yaw + a_card.z;
    float cy = cos(totalYaw), sy = sin(totalYaw);

    // Horizontal spread: a_card.x rotated by totalYaw.
    float eastFromX  = a_card.x * cy;
    float northFromX = a_card.x * sy;
    // Vertical component: a_card.y rises if vertFactor>0, spreads outward if flatFactor>0.
    // Outward direction = perpendicular to card face = (−sy, cy).
    float eastFromY  = -a_card.y * sy * flatFactor;
    float northFromY =  a_card.y * cy * flatFactor;
    float upFromY    =  a_card.y      * vertFactor;

    // Wind: two slow overlapping harmonics for flowing, continuous motion.
    // Spatial gradient is gentle so the wave across the field is wide.
    // Ground cover (role 3) is static — too short to visibly sway.
    float trueDist   = p.age;
    float windFade   = uintBitsToFloat(p.atlasIndex);
    float windPhase1 = p.lifetime * 0.35 + i_worldPos.x * 0.02 + i_worldPos.y * 0.03 + i_seed * 6.28;
    float windPhase2 = p.lifetime * 0.55 + i_worldPos.x * 0.03 + i_worldPos.y * 0.05 + i_seed * 3.14;
    float windNoise  = sin(windPhase1) * 0.75 + sin(windPhase2) * 0.25;
    float windStr    = (cardRole == 3u) ? 0.0           // ground cover: no sway
                     : (cardRole == 2u) ? 0.05           // top/cap: gentle
                     : 0.10;                             // vertical/tilted: full
    // Quadratic sway: tip bends most (y²=1), mid-row less (y²=0.11/0.44), base fixed (y²=0).
    float sway = windNoise * a_card.y * a_card.y * windStr * effectiveScale * windFade * vertFactor;

    vec3 worldPos;
    worldPos.x = i_worldPos.x + (eastFromX + eastFromY) * effectiveScale + sway * cy;
    worldPos.y = i_worldPos.y + (northFromX + northFromY) * effectiveScale + sway * sy;
    worldPos.z = i_worldPos.z + upFromY * effectiveScale;



    v_vegData.y  = 1.0 - a_card.y;
    v_vegData.x        = i_seed;

    v_vegAux.x     = lodFade;
    v_vegData.z        = (cardRole == 3u) ? 0.25 : float(cardRole) * 0.5;  // 0=vert, 0.5=tilt, 1=top, 0.25=ground

    // VEGETATION-DEPTH-BIAS (TERRAIN-DEPTH-BIAS-OWNERSHIP-1): terrain now writes
    // TRUE depth (0), so veg can no longer ride on the old -0.002 vs -0.004 gap.
    // Use a small explicit positive VEG_DEPTH_BIAS so veg wins the GEQUAL tie over
    // the coplanar ground (cards visible on terrain) while props (true depth, 0)
    // still draw in front of veg at the ground line. VEG_DEPTH_BIAS < OVERLAY so
    // road/decals stay on top of grass.
    vec4 clip = U.worldToClipGL * vec4(worldPos, 1.0);
    clip.z += VEG_DEPTH_BIAS * clip.w;
    gl_Position = clip;
}
