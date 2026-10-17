#version 450
layout(set=0, binding=0, std140) uniform U {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;     // travel direction; surface->light L = -lightDir
    vec4 lightColor;
    vec4 ambient;
    ivec4 has;         // x normal, y orm, z emissive
} u;
layout(set=0, binding=1) uniform sampler2D baseColorTex;   // sRGB view -> linear on sample
layout(set=0, binding=2) uniform sampler2D normalTex;      // linear
layout(set=0, binding=3) uniform sampler2D ormTex;         // linear R=AO G=Rough B=Metal
layout(set=0, binding=4) uniform sampler2D emissiveTex;    // sRGB

layout(location=0) in vec3 v_worldPos;
layout(location=1) in vec3 v_normal;
layout(location=2) in vec3 v_tangent;
layout(location=3) in vec3 v_bitangent;
layout(location=4) in vec2 v_uv;
layout(location=0) out vec4 fragColor;

const float PI = 3.14159265359;

float D_GGX(float NdotH, float a) {
    float a2 = a * a;
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d + 1e-7);
}
float G_SchlickGGX(float NdotV, float k) { return NdotV / (NdotV * (1.0 - k) + k); }
float G_Smith(float NdotV, float NdotL, float rough) {
    float k = (rough + 1.0); k = (k * k) / 8.0;
    return G_SchlickGGX(NdotV, k) * G_SchlickGGX(NdotL, k);
}
vec3 F_Schlick(float cosT, vec3 F0) { return F0 + (1.0 - F0) * pow(1.0 - cosT, 5.0); }

void main() {
    vec3 albedo = texture(baseColorTex, v_uv).rgb;

    // MATERIAL-M0: roughness default PINNED to 1.0 (matches the MaterialGpu record default and static_prop.frag).
    float ao = 1.0, rough = 1.0, metal = 0.0;
    if (u.has.y != 0) {
        vec3 orm = texture(ormTex, v_uv).rgb;
        ao = orm.r; rough = clamp(orm.g, 0.04, 1.0); metal = orm.b;
    }

    vec3 N = normalize(v_normal);
    if (u.has.x != 0) {
        vec3 tn = texture(normalTex, v_uv).xyz * 2.0 - 1.0;
        mat3 TBN = mat3(normalize(v_tangent), normalize(v_bitangent), N);
        N = normalize(TBN * tn);
    }

    vec3 V = normalize(u.cameraPos.xyz - v_worldPos);
    vec3 L = normalize(-u.lightDir.xyz);
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    vec3 F0 = mix(vec3(0.04), albedo, metal);
    float D = D_GGX(NdotH, rough * rough);
    float G = G_Smith(NdotV, NdotL, rough);
    vec3 F = F_Schlick(VdotH, F0);
    vec3 spec = (D * G) * F / (4.0 * NdotV * NdotL + 1e-4);
    vec3 kd = (vec3(1.0) - F) * (1.0 - metal);
    vec3 diffuse = kd * albedo / PI;

    vec3 color = (diffuse + spec) * u.lightColor.xyz * NdotL;
    color += u.ambient.xyz * albedo * ao;
    if (u.has.z != 0) color += texture(emissiveTex, v_uv).rgb;

    color = color / (color + vec3(1.0));              // Reinhard tonemap
    color = pow(color, vec3(1.0 / 2.2));              // gamma encode (target is UNORM)
    fragColor = vec4(color, 1.0);
}
