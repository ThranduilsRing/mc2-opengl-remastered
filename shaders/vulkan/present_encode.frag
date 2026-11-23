#version 450
#extension GL_GOOGLE_include_directive : require
#include "hdr_transfer.glsl"
layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 0) uniform sampler2D sceneTex;
layout(set = 0, binding = 1) uniform sampler2D uiTex;
layout(push_constant) uniform EncodeParams { float paperNits; float scRgb; } u;
void main() {
    vec2 uv = vec2(TexCoord.x, 1.0 - TexCoord.y);
    vec3 scene = texture(sceneTex, uv).rgb;
    vec4 ui = texture(uiTex, uv);
    vec3 straight = ui.a > 0.0 ? ui.rgb / ui.a : vec3(0.0);
    vec3 linearUi = vec3(hdrDecode(straight.r), hdrDecode(straight.g), hdrDecode(straight.b));
    vec3 nits = (scene * (1.0 - ui.a) + linearUi * ui.a) * u.paperNits;
    if (u.scRgb != 0.0) {
        FragColor = vec4(nits / 80.0, 1.0);
    } else {
        vec3 bt2020 = mat3(0.627404, 0.069097, 0.016391,
                           0.329283, 0.919540, 0.088013,
                           0.043313, 0.011362, 0.895595) * nits;
        FragColor = vec4(hdrPq(bt2020.r), hdrPq(bt2020.g), hdrPq(bt2020.b), 1.0);
    }
}
