// Native crossed-card geometry. Instance culling uses the same CPU block/distance inputs.
#include "vegetation_card_trim.glsl"
void mc2VegetationVertex(vec3 i_worldPos, float i_yaw, float i_scale, float i_seed, uint i_atlasFrame, float lodFade, float windFade, float lightZ, float time, float widthScale) {
    uint vi = uint(gl_VertexIndex);
    uint row = (vi%8u)/2u;
    // Bit 3 marks a meadow-field blade (vegetation_clumps.h kFieldBladeBit); its low bits keep the grass layer.
    bool fieldBlade = (i_atlasFrame & 8u) != 0u;
    uint atlasIdx = fieldBlade ? (i_atlasFrame & 3u) : min(i_atlasFrame & 0x0fu, 7u);
    vec2 a_uv = D.flags.z != 0u ? kVegCardTrim[atlasIdx * 8u + (3u - row) * 2u + (vi & 1u)]
                                : vec2(float(vi & 1u), 1.0 - float(row) / 3.0);
    float height = 1.0 - a_uv.y;
    vec3 a_card = vec3(a_uv.x - 0.5, height, vi >= 8u ? 1.5707963 : 0.0);
    v_uv = a_uv;
    v_atlasLayer = atlasIdx;
    v_rootPos = i_worldPos;
    v_vegData.w=lightZ;
    v_flags=i_atlasFrame;
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
    // Placement marks boundary survivors; interior field cards retain role 0.
    if (fieldBlade && cardRole == 1u) tiltAngle = 0.08 + 0.15 * i_seed;
    float vertFactor = cos(tiltAngle);
    float flatFactor = sin(tiltAngle);

    // Scale multipliers: top/cap smaller; ground cover very short (1.25-2.5 WU).
    float effectiveScale = i_scale * (cardRole == 3u ? 0.25
                                     : cardRole == 2u ? 0.6
                                     : 1.0);
    if (fieldBlade && cardRole == 1u) effectiveScale *= 0.90 + 0.10 * i_seed;

    float totalYaw = i_yaw + a_card.z;
    float cy = cos(totalYaw), sy = sin(totalYaw);

    // Horizontal spread: a_card.x rotated by totalYaw.
    float eastFromX  = a_card.x * cy * widthScale;
    float northFromX = a_card.x * sy * widthScale;
    // Vertical component: a_card.y rises if vertFactor>0, spreads outward if flatFactor>0.
    // Outward direction = perpendicular to card face = (−sy, cy).
    float eastFromY  = -a_card.y * sy * flatFactor;
    float northFromY =  a_card.y * cy * flatFactor;
    float upFromY    =  a_card.y      * vertFactor;

    // Wind: two slow overlapping harmonics for flowing, continuous motion.
    // Spatial gradient is gentle so the wave across the field is wide.
    // Ground cover (role 3) is static — too short to visibly sway.
    float windPhase1 = time * 0.35 + i_worldPos.x * 0.02 + i_worldPos.y * 0.03 + i_seed * 6.28;
    float windPhase2 = time * 0.55 + i_worldPos.x * 0.03 + i_worldPos.y * 0.05 + i_seed * 3.14;
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

    // Grass (layers 0-3, not ground cover): every blade leans downwind and gust
    // bands travel across the field. Bushes and trees keep the sway above.
    float gust = 0.0;
    if (atlasIdx < 4u && cardRole != 3u) {
        const vec2 W = vec2(0.8192, 0.5736);                       // one prevailing wind, 35 degrees
        float along = dot(i_worldPos.xy, W), across = dot(i_worldPos.xy, vec2(-W.y, W.x));
        // Bands ~180 WU apart roll downwind at ~45 WU/s; a slow cross-wind warp keeps them from reading as stripes.
        float g1 = sin(along * 0.035 - time * 1.6 + 1.8 * sin(across * 0.011 + time * 0.23));
        float g2 = sin(along * 0.012 - time * 0.62 + across * 0.007);
        gust = clamp(0.5 + 0.38 * g1 + 0.22 * g2, 0.0, 1.0);
        gust = gust * gust * (3.0 - 2.0 * gust);
        float flutter = 0.07 * sin(time * 3.3 + i_seed * 41.0 + along * 0.25);
        float jitter = (i_seed - 0.5) * 0.7;                       // per-blade lean direction spread
        vec2 lean = vec2(W.x * cos(jitter) - W.y * sin(jitter), W.x * sin(jitter) + W.y * cos(jitter));
        float bend = (fieldBlade ? 0.32 : 0.18) + (0.50 + flutter) * gust;  // tip offset per unit height
        bend *= windFade * vertFactor;
        float h2 = a_card.y * a_card.y;
        worldPos.xy += lean * (bend * h2 * effectiveScale);
        worldPos.z -= upFromY * effectiveScale * 0.35 * bend * h2;   // a bent blade is shorter, not stretched
    }
    v_vegAux.y = gust;



    v_shadowWorld = worldPos;
    v_vegData.y  = 1.0 - a_card.y;
    v_vegData.x        = i_seed;

    v_vegAux.x     = lodFade;
    v_vegData.z        = (cardRole == 3u) ? 0.25 : float(cardRole) * 0.5;  // 0=vert, 0.5=tilt, 1=top, 0.25=ground

    v_cardNormal = vec3(sy * vertFactor, -cy * vertFactor, flatFactor);
    v_worldPos = worldPos;
    gl_Position = U.worldToClipGL * vec4(worldPos, 1.0);
}
