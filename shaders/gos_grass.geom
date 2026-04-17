//#version 420 (version provided by material prefix)

layout(triangles) in;

// Inputs from TES — arrays of 3
in vec4 Color[];
in float FogValue[];
in vec2 Texcoord[];
in float TerrainType[];
in vec3 WorldNorm[];
in vec3 WorldPos[];
in float UndisplacedDepth[];

// Outputs to grass fragment shader
layout(triangle_strip, max_vertices = 256) out;
out vec2 GrassUV;
out vec3 GrassWorldPos;
out vec3 GrassBaseColor;
out float GrassAlpha;

uniform sampler2D tex1;         // colormap (unit 0)
uniform mat4 terrainMVP;        // axisSwap * worldToClip
uniform vec4 terrainViewport;   // (vmx, vmy, vax, vay) for perspective projection
uniform mat4 mvp;               // screen pixels -> NDC
uniform vec4 cameraPos;         // Stuff/MLR space: x=left, y=elev, z=forward
uniform float time;             // elapsed time in seconds

// --- Helpers ---

vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

// Simple hash for randomization from a 2D world position
float hash21(vec2 p) {
    p = fract(p * vec2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}

// Project a world position using the same two-step projection as TES
vec4 projectWorldPos(vec3 worldPos) {
    vec4 clip = terrainMVP * vec4(worldPos, 1.0);
    float rhw = 1.0 / clip.w;
    vec3 screen;
    screen.x = clip.x * rhw * terrainViewport.x + terrainViewport.z;
    screen.y = clip.y * rhw * terrainViewport.y + terrainViewport.w;
    screen.z = clip.z * rhw;
    vec4 ndc = mvp * vec4(screen, 1.0);
    float absW = abs(clip.w);
    return vec4(ndc.xyz * absW, absW);
}

// Emit one grass quad vertex
void emitGrassVertex(vec3 pos, vec2 uv, vec3 baseColor, float alpha) {
    GrassUV = uv;
    GrassWorldPos = pos;
    GrassBaseColor = baseColor;
    GrassAlpha = alpha;
    gl_Position = projectWorldPos(pos);
    EmitVertex();
}

void main() {
    // Compute triangle centroid in world space
    vec3 triCenter = (WorldPos[0] + WorldPos[1] + WorldPos[2]) / 3.0;

    // Sample colormap at each vertex and average
    vec3 col0 = texture(tex1, Texcoord[0]).rgb;
    vec3 col1 = texture(tex1, Texcoord[1]).rgb;
    vec3 col2 = texture(tex1, Texcoord[2]).rgb;
    vec3 colAvg = (col0 + col1 + col2) / 3.0;

    // Classify grass via HSV
    vec3 hsv = rgb2hsv(colAvg);
    float hue = hsv.x;
    float sat = hsv.y;

    float grassWeight = smoothstep(0.14, 0.17, hue) * smoothstep(0.15, 0.28, sat);

    // Skip water
    float isWater = smoothstep(0.35, 0.45, hue);
    grassWeight *= (1.0 - isWater);

    if (grassWeight < 0.3) return;

    // Distance fade from camera
    vec2 camGround = vec2(-cameraPos.x, cameraPos.z);
    float hDist = distance(triCenter.xy, camGround);
    float altBoost = max(cameraPos.y - triCenter.z, 0.0) * 0.7;
    float camDist = hDist + altBoost;

    if (camDist > 5000.0) return;

    // Density fade: reduce blade count at distance
    float densityFade = 1.0 - smoothstep(3000.0, 5000.0, camDist);

    // Camera direction for billboard facing
    vec2 toCam2D = camGround - triCenter.xy;
    float toCamLen = length(toCam2D);
    if (toCamLen < 0.001) toCam2D = vec2(1.0, 0.0);
    else toCam2D /= toCamLen;

    vec2 right2D = vec2(-toCam2D.y, toCam2D.x);
    vec3 right = vec3(right2D.x, right2D.y, 0.0);
    vec3 up    = vec3(0.0, 0.0, 1.0);

    // Triangle edge vectors for barycentric placement
    vec3 edge1 = WorldPos[1] - WorldPos[0];
    vec3 edge2 = WorldPos[2] - WorldPos[0];

    // Emit up to 20 blades scattered across the triangle
    int maxBlades = int(64.0 * densityFade);
    maxBlades = clamp(maxBlades, 1, 64);

    for (int b = 0; b < maxBlades; b++) {
        // Deterministic random placement within triangle using barycentric coords
        float seed = float(b);
        float u = fract(seed * 0.618034 + hash21(triCenter.xy));
        float v = fract(seed * 0.324719 + hash21(triCenter.xy + vec2(7.13, 3.77)));
        // Fold into triangle (reflect if u+v > 1)
        if (u + v > 1.0) { u = 1.0 - u; v = 1.0 - v; }

        vec3 bladePos = WorldPos[0] + u * edge1 + v * edge2;

        // Per-blade randomization
        float rh = hash21(bladePos.xy);
        float rw = hash21(bladePos.xy + vec2(17.3, 5.7));

        // Thin out at distance using per-blade hash
        float rng = hash21(bladePos.xy * 0.01 + vec2(seed));
        if (rng > densityFade) continue;

        float bladeHeight = mix(1.0, 1.7, rh);
        float bladeWidth  = mix(0.5, 0.9, rw);

        // Wind — varies per blade position
        float windPhase = time * 2.0 + bladePos.x * 0.3 + bladePos.y * 0.2;
        float windOffset = sin(windPhase) * 0.2;
        vec3 windVec = right * windOffset;

        float alpha = grassWeight * densityFade;

        vec3 BL = bladePos - (bladeWidth * 0.5) * right;
        vec3 BR = bladePos + (bladeWidth * 0.5) * right;
        vec3 TL = bladePos - (bladeWidth * 0.5) * right + bladeHeight * up + windVec;
        vec3 TR = bladePos + (bladeWidth * 0.5) * right + bladeHeight * up + windVec;

        emitGrassVertex(BL, vec2(0.0, 0.0), colAvg, alpha);
        emitGrassVertex(BR, vec2(1.0, 0.0), colAvg, alpha);
        emitGrassVertex(TL, vec2(0.0, 1.0), colAvg, alpha);
        emitGrassVertex(TR, vec2(1.0, 1.0), colAvg, alpha);
        EndPrimitive();
    }
}
