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
layout(triangle_strip, max_vertices = 12) out;
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
    // Compute triangle triCenter in world space
    vec3 triCenter = (WorldPos[0] + WorldPos[1] + WorldPos[2]) / 3.0;
    vec2 triCenterTexcoord = (Texcoord[0] + Texcoord[1] + Texcoord[2]) / 3.0;

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
    // cameraPos: Stuff/MLR space x=left, y=elev, z=forward
    // WorldPos: MC2 x=east, y=north, z=elevation
    vec2 camGround = vec2(-cameraPos.x, cameraPos.z);
    float hDist = distance(triCenter.xy, camGround);
    float altBoost = max(cameraPos.y - triCenter.z, 0.0) * 0.7;
    float camDist = hDist + altBoost;

    // Nothing beyond 5000
    if (camDist > 5000.0) return;

    // Density fade 3000→5000
    float densityFade = 1.0 - smoothstep(3000.0, 5000.0, camDist);
    // Use hash to thin out at distance
    float rng = hash21(triCenter.xy * 0.01);
    if (rng > densityFade) return;

    // Randomize dimensions via position hash
    float h1 = hash21(triCenter.xy);
    float h2 = hash21(triCenter.xy + vec2(17.3, 5.7));
    float h3 = hash21(triCenter.xy + vec2(3.1, 22.9));

    float bladeHeight = mix(15.0, 25.0, h1);
    float bladeWidth  = mix(8.0, 14.0, h2);

    // Camera direction vector (MC2 world space)
    // camera is at (-cameraPos.x, cameraPos.z) in XY, so direction toward camera
    vec2 toCam2D = camGround - triCenter.xy;
    float toCamLen = length(toCam2D);
    if (toCamLen < 0.001) toCam2D = vec2(1.0, 0.0);
    else toCam2D /= toCamLen;

    // Grass blade right vector: perpendicular to camera direction, in XY plane
    // In MC2 coords, "up" is +Z; the blade stands along Z
    // right = rotate toCam2D by 90 degrees in XY
    vec2 right2D = vec2(-toCam2D.y, toCam2D.x);
    vec3 right = vec3(right2D.x, right2D.y, 0.0);
    vec3 up    = vec3(0.0, 0.0, 1.0);

    // Wind displacement on top vertices (sinusoidal along X-axis)
    float windFreq = 0.05;
    float windAmp  = 3.0;
    float windPhase = time + triCenter.x * windFreq;
    float windOffset = sin(windPhase) * windAmp;
    vec3 windVec = right * windOffset;

    // Base terrain color from colormap average
    vec3 baseColor = colAvg;

    // 4 vertices of a quad:
    //   BL = base - halfWidth*right
    //   BR = base + halfWidth*right
    //   TL = base - halfWidth*right + height*up + wind
    //   TR = base + halfWidth*right + height*up + wind
    vec3 basePos = triCenter;
    vec3 BL = basePos - (bladeWidth * 0.5) * right;
    vec3 BR = basePos + (bladeWidth * 0.5) * right;
    vec3 TL = basePos - (bladeWidth * 0.5) * right + bladeHeight * up + windVec;
    vec3 TR = basePos + (bladeWidth * 0.5) * right + bladeHeight * up + windVec;

    // Alpha: full at base, fade at distance
    float alpha = grassWeight * densityFade;

    // Emit triangle strip: BL, BR, TL, TR
    emitGrassVertex(BL, vec2(0.0, 0.0), baseColor, alpha);
    emitGrassVertex(BR, vec2(1.0, 0.0), baseColor, alpha);
    emitGrassVertex(TL, vec2(0.0, 1.0), baseColor, alpha);
    emitGrassVertex(TR, vec2(1.0, 1.0), baseColor, alpha);
    EndPrimitive();
}
