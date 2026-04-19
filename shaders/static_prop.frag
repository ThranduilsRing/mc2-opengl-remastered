// shaders/static_prop.frag
// GPU static prop renderer — main fragment shader (Task 9).
// NOTE: no #version directive here — makeProgram() prepends "#version 420\n".

in vec3  v_normal;
in vec2  v_uv;
flat in uint v_flags;
in vec4  v_highlight;
in vec4  v_fog;
in vec4  v_argb;
flat in uint v_localVertexID;

uniform sampler2D u_tex;
uniform uint  u_materialFlags;   // bit 0: ALPHA_TEST
uniform float u_fogValue;        // 1.0 = clear, 0.0 = fully fogged
uniform int   u_debugAddrMode;   // 0 normal, 1 gradient, 2 hash
uniform uint  u_maxLocalVertexID;// uploaded per type: the current type's
                                 // vertexCount, so debug gradient is
                                 // normalized per shape (not against a
                                 // global max across all types)
uniform uint  u_packetID;        // uploaded per-draw for mode 2

layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 GBuffer1;  // scene normal buffer, alpha=0 for non-terrain

const uint ALPHA_TEST_BIT = 1u;

uint hash_u(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

void main() {
    vec4 tex_color = texture(u_tex, v_uv);

    if ((u_materialFlags & ALPHA_TEST_BIT) != 0u && tex_color.a < 0.5) {
        discard;
    }

    // Debug address modes — validate the gl_VertexID-free indexing math.
    if (u_debugAddrMode == 1) {
        float t = float(v_localVertexID) / max(float(u_maxLocalVertexID), 1.0);
        FragColor = vec4(t, t, t, 1.0);
        GBuffer1  = vec4(0.0, 0.0, 1.0, 0.0);
        return;
    }
    if (u_debugAddrMode == 2) {
        uint h = hash_u(u_packetID * 2654435761u + v_localVertexID);
        FragColor = vec4(
            float((h >>  0) & 0xFFu) / 255.0,
            float((h >>  8) & 0xFFu) / 255.0,
            float((h >> 16) & 0xFFu) / 255.0,
            1.0);
        GBuffer1  = vec4(0.0, 0.0, 1.0, 0.0);
        return;
    }

    vec4 c = tex_color * v_argb;
    c.rgb += v_highlight.rgb * v_highlight.a;

    // Fog: matches the non-overlay path in gos_tex_vertex.frag.
    c.rgb = mix(v_fog.rgb, c.rgb, u_fogValue);

    FragColor = c;
    GBuffer1  = vec4(normalize(v_normal) * 0.5 + 0.5, 0.0);
}
