//#version 420 (version provided by prefix)
//
// Fullscreen procedural cloud-shadow pass. Replaces the four inline cloud
// blocks that used to live in gos_terrain.frag / terrain_overlay.frag /
// decal.frag / shadow_screen.frag. Runs AFTER runScreenShadow as a separate
// multiplicative (GL_DST_COLOR, GL_ZERO) fullscreen pass.
//
// World position reconstructed from scene depth via inverseViewProj (ZERO_TO_ONE
// reversed-Z, same convention as shadow_screen.frag). worldPos.xz is the terrain
// ground plane; scroll animates the shadow across the terrain in world space.
// Sky pixels (reverse-Z depth ~0.0) output 1.0 so the background is untouched.

#define PREC highp

// fbm is inlined here on purpose (same rationale as shadow_screen.frag): the
// shader include machinery uses a backslash path separator on Windows while
// shader references use forward slashes, so pulling in an external fbm helper
// resolves to nothing silently in the post-process program path.
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
    return 130.0 * dot(m, g);
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

in vec2 TexCoord;
layout(location = 0) out PREC vec4 FragColor;

uniform sampler2D sceneDepthTex;
uniform mat4  inverseViewProj;

// Tunable cloud params (int, not uint — AMD uniform-uint crash trap).
uniform int   u_cloudEnable;     // 0 = pass-through 1.0
uniform float u_time;
uniform float u_cloudScale;      // default 0.0006
uniform vec2  u_cloudScroll;     // default (0.012, 0.005)
uniform float u_cloudStrength;   // default 0.15 (max darkening; floor = 1-strength)
uniform vec2  u_cloudThreshold;  // default (0.3, 0.7) smoothstep band
uniform int   u_cloudOctaves;    // default 4

//PORT-BEGIN:cloudShadowMain
vec3 reconstructWorldPos(vec2 uv, float depth)
{
    vec2 ndc_xy = uv * 2.0 - 1.0;
    // glClipControl(ZERO_TO_ONE): window depth and NDC z share [0,1]; pass through.
    vec4 worldPos4 = inverseViewProj * vec4(ndc_xy, depth, 1.0);
    return worldPos4.xyz / worldPos4.w;
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

    int   oct      = clamp(u_cloudOctaves, 1, 6);
    float tWrapped = mod(u_time, 1000.0);

    vec2  uv    = worldPos.xz * u_cloudScale + tWrapped * u_cloudScroll;
    float n     = fbm(uv, oct) * 0.5 + 0.5;
    float clear = smoothstep(u_cloudThreshold.x, u_cloudThreshold.y, n);
    float factor = mix(1.0 - u_cloudStrength, 1.0, clear);

    FragColor = vec4(vec3(factor), 1.0);
}
//PORT-END:cloudShadowMain
