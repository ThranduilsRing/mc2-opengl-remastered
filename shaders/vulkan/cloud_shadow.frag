// VK-POST-SCREENFX-NATIVE-1 (rung 8.3) -- Vulkan port of shaders/cloud.frag
// (fullscreen procedural cloud-shadow pass, multiplicative DST_COLOR/ZERO onto
// the scene color).
//
// MATH is a VERBATIM port of the GL cloud.frag (proven offline by
// tools/glsl_port_diff.py pairs cloudShadowNoise / cloudShadowMain). ONLY the
// uniform/sampler plumbing changed: the GL default-block uniforms became an
// explicit std140 UBO + descriptor bindings, with #define aliases so the ported
// block text stays byte-identical to the GL source:
//   set=0,binding=0  sceneDepthTex -> sampler2D (SHARED full-res scene depth,
//                    depth-aspect view, NEAREST -- fog/ssao-native parity)
//   set=0,binding=1  CloudFxParams -> std140 UBO mirroring the GL uniforms.
//
// Matrix convention: inverseViewProj is uploaded by GL with a SINGLE
// glUniformMatrix4fv(GL_FALSE) of the column-major-stored CPU array (same
// upload as the SSAO path, gos_postprocess.cpp runCloudShadow) -> std140 mat4
// default column_major + raw memcpy on the CPU side reads the SAME bytes the
// GL shader reads. Do NOT qualify row_major (see ssao.frag N0 FIX note).
//
// Convention matches ssao.frag: reverse-Z, far=0 (sky ~0.0), TexCoord is the
// interpolated [0,1] screen UV from the fullscreen triangle (ssao.vert shape),
// no Y-flip needed (shared images are the very texels GL samples).
#version 450

#define PREC highp

layout(location = 0) in  vec2 TexCoord;
layout(location = 0) out PREC vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D sceneDepthTex;

// std140 offsets (cross-checked offline by tools/check_ubo_layout.py label
// screenfx_cloud against the CPU POD in vulkan_post_screenfx_native.cpp):
//   invViewProj    @  0  (mat4, 64B, column-major raw bytes)
//   cloudScroll    @ 64  (vec2)
//   cloudThreshold @ 72  (vec2)
//   timeSec        @ 80
//   cloudScale     @ 84
//   cloudStrength  @ 88
//   cloudOctaves   @ 92  (int -- AMD uniform-uint crash trap, keep int)
//   cloudEnable    @ 96  (int)
//   cloudWaterElev @100  cloudWaterScale @104 (floats)
//   sunDirCloudH   @112  (vec4: xyz unit vector toward sun, Z-up; w cloud-base height wu)
layout(set = 0, binding = 1, std140) uniform CloudFxParams {
    mat4  invViewProj;
    vec2  cloudScroll;
    vec2  cloudThreshold;
    float timeSec;
    float cloudScale;
    float cloudStrength;
    int   cloudOctaves;
    int   cloudEnable;
    float cloudWaterElev;
    float cloudWaterScale;
    vec4  sunDirCloudH;
} u;

// Aliases so the ported GL block below stays textually VERBATIM.
#define inverseViewProj  u.invViewProj
#define u_time           u.timeSec
#define u_cloudScale     u.cloudScale
#define u_cloudScroll    u.cloudScroll
#define u_cloudStrength  u.cloudStrength
#define u_cloudThreshold u.cloudThreshold
#define u_cloudOctaves   u.cloudOctaves
#define u_cloudEnable    u.cloudEnable
#define u_sunDirCloudH   u.sunDirCloudH

//PORT-BEGIN:cloudShadowNoise
PREC vec3 mod289_3(PREC vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
PREC vec2 mod289_2(PREC vec2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
PREC vec3 permute(PREC vec3 x) { return mod289_3(((x * 34.0) + 1.0) * x); }

PREC float snoise(PREC vec2 v) {
    const PREC vec4 C = vec4(0.211324865405187, 0.366025403784439, -0.577350269189626, 0.024390243902439);
    PREC vec2 i  = floor(v + dot(v, C.yy));
    PREC vec2 x0 = v - i + dot(i, C.xx);
    PREC vec2 i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    PREC vec4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;
    i = mod289_2(i);
    PREC vec3 p = permute(permute(i.y + vec3(0.0, i1.y, 1.0)) + i.x + vec3(0.0, i1.x, 1.0));
    PREC vec3 m = max(0.5 - vec3(dot(x0, x0), dot(x12.xy, x12.xy), dot(x12.zw, x12.zw)), 0.0);
    m = m * m; m = m * m;
    PREC vec3 x = 2.0 * fract(p * C.www) - 1.0;
    PREC vec3 h = abs(x) - 0.5;
    PREC vec3 ox = floor(x + 0.5);
    PREC vec3 a0 = x - ox;
    m *= 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);
    PREC vec3 g;
    g.x = a0.x * x0.x + h.x * x0.y;
    g.yz = a0.yz * x12.xz + h.yz * x12.yw;
    return 130.0001 * dot(m, g);
}

PREC float fbm(PREC vec2 p, int octaves) {
    PREC float value = 0.0;
    PREC float amplitude = 0.5;
    PREC float frequency = 1.0;
    for (int i = 0; i < octaves; i++) {
        value += amplitude * snoise(p * frequency);
        frequency *= 2.0;
        amplitude *= 0.5;
    }
    return value;
}
//PORT-END:cloudShadowNoise

//PORT-BEGIN:cloudShadowMain
vec3 reconstructWorldPos(vec2 uv, float depth)
{
    vec2 ndc_xy = uv * 2.0 - 1.0;
    // glClipControl(ZERO_TO_ONE): window depth and NDC z share [0,1]; pass through.
    vec4 worldPos4 = inverseViewProj * vec4(ndc_xy, depth, 1.0);
    return worldPos4.xyz / worldPos4.w;
}

// Part-d contract: shafts call cloudCover(sunProject(p)) at marched points.
// sunProject: ground-plane position of the cloud-layer point on the ray from wp toward the sun.
vec2 sunProject(vec3 wp)
{
    return wp.xy + u_sunDirCloudH.xy * ((u_sunDirCloudH.w - wp.z) / max(u_sunDirCloudH.z, 0.2588));
}

// 0 clear .. 1 covered. Reads only the UBO.
float cloudCover(vec2 groundXY)
{
    int   oct = clamp(u_cloudOctaves, 1, 6);
    vec2  uv  = groundXY * u_cloudScale + u_time * u_cloudScroll;
    float n   = fbm(uv, oct) * 0.5 + 0.5;
    return 1.0 - smoothstep(u_cloudThreshold.x, u_cloudThreshold.y, n);
}

void main()
{
    if (u_cloudEnable == 0) {
        FragColor = vec4(1.0);
        return;
    }

    float depth = texture(sceneDepthTex, TexCoord).r;
    // reverse-Z: far/cleared depth ~0.0 (sky/background). Leave it untouched.
    if (depth <= 0.0001) {
        FragColor = vec4(1.0);
        return;
    }

    vec3 worldPos = reconstructWorldPos(TexCoord, depth);

    // Water pixels (depth-reconstructed z at the water plane) get the darkening scaled by cloudWaterScale.
    float k = worldPos.z < u.cloudWaterElev + 0.5 ? u.cloudWaterScale : 1.0;
    float factor = 1.0 - k * u_cloudStrength * cloudCover(sunProject(worldPos));

    FragColor = vec4(vec3(factor), 1.0);
}
//PORT-END:cloudShadowMain
