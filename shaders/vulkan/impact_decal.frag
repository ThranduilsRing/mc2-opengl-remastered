// Impact box decal fragment: reconstructs the surface under the pixel from the shared scene depth, keeps it when it lies in
// the decal box, belongs to a surface class the decal affects and faces the shot, then
//   mode 0 (multiply): scene *= decal albedo ratio x char tint x relight ratio of the perturbed normal (the deformation)
//   mode 1 (additive): scene += cooling emissive glow (hot decals only)
// The multiply edits the material response before the later frame composition (screen shadow, fog, tone mapping) and needs
// no knowledge of how the surface was lit: a darker albedo scales diffuse light linearly.
#version 450

struct ImpactDecal { vec4 v[8]; };
layout(std430, set = 0, binding = 5) readonly buffer Decals { ImpactDecal d[]; } decals;
layout(std140, set = 0, binding = 4) uniform ImpactFrame {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 eye;
    vec4 lightDir;
    vec4 screen;
} F;
layout(set = 0, binding = 0) uniform sampler2D sceneDepth;
layout(set = 0, binding = 1) uniform sampler2D sceneNormal;       // .a = surface class
layout(set = 0, binding = 2) uniform sampler2DArray surfaceAtlas; // r albedo*0.5, g emissive, b char, a coverage
layout(set = 0, binding = 3) uniform sampler2DArray normalAtlas;  // rg tangent normal, b height, a roughness
layout(push_constant) uniform ImpactPC { uint mode; } pc;

layout(location = 0) flat in uint vInstance;
layout(location = 0) out vec4 oColor;

vec3 surfaceAt(ivec2 p) {
    p = clamp(p, ivec2(0), ivec2(F.screen.xy) - 1);
    float depth = texelFetch(sceneDepth, p, 0).r;
    vec2 ndc = (vec2(p) + 0.5) * F.screen.zw * 2.0 - 1.0;
    vec4 w = F.invViewProj * vec4(ndc, depth, 1.0);
    return w.xyz / w.w;
}

void main() {
    ImpactDecal D = decals.d[vInstance];
    ivec2 ip = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(sceneDepth, ip, 0).r;
    vec3 P = surfaceAt(ip);

    // Decal-local coordinates, [-1, 1]^3 inside the box.
    vec3 rel = P - D.v[0].xyz;
    vec3 l = vec3(dot(rel, D.v[4].xyz), dot(rel, D.v[5].xyz), dot(rel, D.v[6].xyz));

    // Surface class from the scene normal target's alpha: terrain 1.0, props 0.1875..0.25, units/shapes 0.
    float classA = texelFetch(sceneNormal, ip, 0).a;
    uint classBit = classA > 0.75 ? 1u : (classA > 0.1 ? 2u : 4u);
    uint affect = uint(D.v[6].w + 0.5);

    // Geometric normal from the depth neighbours (the side with the smaller step avoids silhouettes).
    vec3 px1 = surfaceAt(ip + ivec2(1, 0));
    vec3 px0 = surfaceAt(ip - ivec2(1, 0));
    vec3 py1 = surfaceAt(ip + ivec2(0, 1));
    vec3 py0 = surfaceAt(ip - ivec2(0, 1));
    vec3 ddx = dot(px1 - P, px1 - P) < dot(P - px0, P - px0) ? px1 - P : P - px0;
    vec3 ddy = dot(py1 - P, py1 - P) < dot(P - py0, P - py0) ? py1 - P : P - py0;
    vec3 N = normalize(cross(ddx, ddy));
    if (dot(N, F.eye.xyz - P * F.eye.w) < 0.0) N = -N;

    // Response layer: terrain pixels take the ground tile, everything else the tile of what was hit.
    float layer = classBit == 1u ? D.v[5].w : D.v[4].w;
    vec2 uv = l.xy * 0.5 + 0.5;
    vec4 S = texture(surfaceAtlas, vec3(uv, layer));
    vec4 T = texture(normalAtlas, vec3(uv, layer));

    vec3 zAxis = normalize(D.v[3].xyz);
    float facing = dot(N, -zAxis);
    bool inside = all(lessThan(abs(l), vec3(1.0)));
    float cov = S.a * D.v[0].w * smoothstep(0.05, 0.35, facing) * (1.0 - smoothstep(0.65, 1.0, abs(l.z)));
    if (!inside || (affect & classBit) == 0u || depth <= 0.0001 || cov < 0.004) discard;

    if (pc.mode == 1u) {
        float heat = D.v[1].w;
        vec3 glowCol = mix(vec3(1.0, 0.12, 0.02), D.v[7].rgb, heat);   // full-heat colour -> dull red as it cools
        oColor = vec4(glowCol * (D.v[7].w * pow(heat, 1.3) * S.g * cov), 0.0);
        return;
    }

    // Deformation: tilt the geometric normal by the atlas tangent normal in the decal's own frame, and relight the
    // sun term by the ratio of the tilted to the untilted response (ambient share in lightDir.w).
    vec3 Tt = D.v[1].xyz - N * dot(N, D.v[1].xyz);
    Tt = dot(Tt, Tt) > 1e-8 ? normalize(Tt) : normalize(cross(N, vec3(0.0, 1.0, 0.0)) + vec3(1e-4));
    vec3 Bt = cross(N, Tt);
    if (dot(Bt, D.v[2].xyz) < 0.0) Bt = -Bt;
    vec2 nt = (T.rg * 2.0 - 1.0) * D.v[2].w;
    vec3 Np = normalize(N * sqrt(max(0.05, 1.0 - min(dot(nt, nt), 0.95))) + Tt * nt.x + Bt * nt.y);
    vec3 L = normalize(F.lightDir.xyz);
    float amb = F.lightDir.w;
    float lit0 = amb + (1.0 - amb) * max(dot(N, L), 0.0);
    float lit1 = amb + (1.0 - amb) * max(dot(Np, L), 0.0);
    float relight = clamp(lit1 / max(lit0, 0.05), 0.35, 1.9);

    vec3 charTint = mix(vec3(1.0), vec3(0.92, 0.84, 0.76), S.b);
    vec3 response = (S.r * 2.0) * charTint * relight;
    oColor = vec4(mix(vec3(1.0), response, clamp(cov, 0.0, 1.0)), 1.0);
}
