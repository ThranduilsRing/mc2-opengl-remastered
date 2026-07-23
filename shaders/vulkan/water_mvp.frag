// WATER-MVP-ORACLE-1 -- Vulkan clip-space oracle for the MC2 water fast-path VS.
//
// PROVES (headless, bit-tight) that a Vulkan std140 UBO pack of the water MVP +
// the reverse-Z depth fudge reproduces the GL water vertex shader's clip-space
// output EXACTLY, using an ASYMMETRIC/sheared test matrix so a transpose is
// detectable (a symmetric/identity matrix cannot distinguish row_major from
// column_major -- the LM1 lesson).
//
// GL producer being mirrored (gos_terrain_water_fast_mdi.vert:283-286):
//     vec4 clip = u_worldToClipGL * vec4(worldPos, 1.0);
//     clip.z   += WATER_DEPTH_FUDGE_FAST * clip.w;   // reverse-Z, pre-divide
// where u_worldToClipGL is the baked terrain/dispatch MVP uploaded to the flat
// `uniform mat4` via glUniformMatrix4fv(loc, 1, GL_FALSE, wMvp)
// (gameos_graphics.cpp:3532). GL_FALSE => the 16 raw floats are read
// COLUMN-MAJOR by GLSL, i.e. clip[r] = sum_c m[c*4+r]*worldPos[c]. This is the
// engine's own authoritative CPU mirror `projZ` in
// gos_terrain_indirect.cpp:3519-3529.
//
// std140 UBO convention (the deliverable): a `mat4` with NO explicit qualifier
// is column_major (std140 default). The CPU packs the SAME 16 raw floats the GL
// path uploads (no transpose). Then `worldToClipGL * worldPos` inside this
// shader computes clip[r] = sum_c m[c*4+r]*worldPos[c] -- byte-identical to the
// GL water VS + the projZ CPU mirror. Qualifying this `row_major` (same raw
// bytes) would instead read the transpose and DIVERGE on the asymmetric matrix
// -- the fixture exercises exactly that wrong pack at C++ level to prove the
// sensitivity. Do NOT qualify row_major. Same convention rule as edge_fog.frag /
// ssao.frag (N0 fix).
#version 450

layout(location = 0) in  vec2 TexCoord;   // unused; frag keys off gl_FragCoord
layout(location = 0) out vec4 outClip;     // (clip.z, clip.w, depth, depthPlusFudge)

// Number of KNOWN test points. Each occupies one pixel COLUMN (x index) so the
// C++ side reads the projected result for point i at pixel (i, 0).
const int WATER_MVP_POINT_COUNT = 8;

layout(set = 0, binding = 0, std140) uniform WaterMvpParams {
    // The water world->clip MVP. NO qualifier => column_major (std140 default).
    // CPU packs the SAME 16 raw floats GL uploads (glUniformMatrix4fv GL_FALSE).
    mat4  worldToClipGL;
    // Test worldPos points (xyz used; w padding). std140 aligns each vec4 to 16B.
    vec4  worldPos[WATER_MVP_POINT_COUNT];
    // WATER_DEPTH_FUDGE_FAST (0.00025). Passed in (not hardcoded) so the fixture
    // single-sources it from mc2depth::WATER_DEPTH_FUDGE_FAST -- the byte-equal
    // C++/GLSL lockstep constant. .yzw padding.
    vec4  fudge;
} P;

void main() {
    int idx = int(gl_FragCoord.x);
    if (idx >= WATER_MVP_POINT_COUNT) {
        outClip = vec4(0.0);
        return;
    }
    vec3 wp = P.worldPos[idx].xyz;

    // The GL water VS clip math, verbatim.
    vec4 clip = P.worldToClipGL * vec4(wp, 1.0);
    clip.z   += P.fudge.x * clip.w;

    float depth = clip.z / clip.w;               // reverse-Z post-divide NDC z
    outClip = vec4(clip.z, clip.w, depth, depth); // ch3 mirrors ch2 (post-fudge)
}
