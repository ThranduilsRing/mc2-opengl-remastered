// Motion-vector debug inset (Graphics Options "Motion Vectors"): red = +x, green = +y,
// mid-grey = no motion, full saturation at u.params.z pixels of motion.
#version 450

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D motionTex;

layout(push_constant) uniform Params {
    vec4 params;   // xy = render size in pixels, z = full-scale motion in pixels
} u;

void main() {
    vec2 px = texture(motionTex, TexCoord).rg * u.params.xy;
    vec2 c = clamp(px / max(u.params.z, 1e-3), vec2(-1.0), vec2(1.0));
    FragColor = vec4(0.5 + 0.5 * c, 0.5, 1.0);
}
