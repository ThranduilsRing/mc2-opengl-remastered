// VK-PARITY-F3: Vulkan port of shaders/hdri_skybox.frag, frameFix==0 (the default
// matrix path the game uses: gamecam.cpp -> renderHdriSkybox(view, proj)).
// Math is verbatim: NDC -> view dir (invProj) -> world dir (invViewRot) -> optional
// sun-sync yaw about +Y -> Y-up equirect lookup. uvDebug / frameFix 1,2 are
// debug-only GL paths and are not ported.
#version 450

layout(location = 0) in  vec2 vNdc;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D u_hdri;

// 116 bytes (<= 128 guaranteed push-constant budget). LOCKSTEP with
// vulkan_sky_island.cpp SkyPC.
layout(push_constant) uniform SkyPC {
    mat4  invProj;     // column-major inverse projection from the native producer
    vec4  rotC0;       // inverse view rotation column 0 (xyz)
    vec4  rotC1;
    vec4  rotC2;
    float skyYaw;
} pc;

const float PI = 3.14159265358979323846;

void main()
{
    vec4 clip = vec4(vNdc, 1.0, 1.0);
    // The native producer copies column-major inverse projection bytes. Its
    // three rotation columns already contain the inverse view rotation.
    // Transposing again magnifies a tiny HDRI patch into rectangular sky texels.
    vec3 viewDir = (pc.invProj * clip).xyz;
    mat3 invViewRot = mat3(pc.rotC0.xyz, pc.rotC1.xyz, pc.rotC2.xyz);
    vec3 worldDir = normalize(invViewRot * viewDir);

    if (pc.skyYaw != 0.0) {
        float s = sin(pc.skyYaw);
        float c = cos(pc.skyYaw);
        worldDir = vec3(worldDir.x * c - worldDir.z * s,
                        worldDir.y,
                        worldDir.x * s + worldDir.z * c);
    }

    vec2 uv = vec2(
        atan(worldDir.z, worldDir.x) / (2.0 * PI) + 0.5,
        1.0 - (asin(clamp(worldDir.y, -1.0, 1.0)) / PI + 0.5)
    );
    // Pure-sky panoramas replace the photographed lower hemisphere with fill.
    // At ground-level cameras that fill/stitching must never become the sky.
    uv.y = min(uv.y, 0.47);
    FragColor = vec4(texture(u_hdri, uv).rgb, 1.0);
}
