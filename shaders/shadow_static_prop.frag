// shaders/shadow_static_prop.frag
// SHADOW-PROP-ALPHA-1: alpha-tested DEPTH-ONLY shadow caster for static props.
//
// WHY: the shared shadow_instanced.frag is `void main(){}` (pure depth, no
// alpha test). When a foliage/tree card (a textured quad with a binary-alpha
// leaf cutout) casts a shadow through that empty frag, the WHOLE quad writes
// depth -> rectangular ("square") tree shadows. This shader performs the SAME
// alpha-test discard the color pass does (static_prop.frag:215) so the cast
// silhouette matches the visible foliage.
//
// PAIRING: linked ONLY with shadow_static_prop.vert (which now forwards a_uv ->
// v_uv) as program "shadow_static_prop_alpha". The MECH shadow path keeps the
// empty shadow_instanced.frag (shadow_mech.vert forwards no UV) — DO NOT route
// mechs here.
//
// PATH (two compile variants, one source):
//  - default:               replicates the LEGACY (non-coalesce) branch of
//    static_prop.frag / static_prop_depth.frag — `texture(u_tex, v_uv)` with a
//    per-type sampler2D + u_materialFlags, matching the legacy color flush loop
//    (gos_static_prop_batcher.cpp ~7042-7106).
//  - MC2_SHADOW_PROP_ATLAS: SHADOW-PROP-ALPHA-ATLAS-1 — samples the SAME
//    coalesce group/bucket GL_TEXTURE_2D_ARRAY the color pass draws from
//    (u_texArr, unit 0) so the cast silhouette matches the visible foliage on
//    the coalesce/BC7-atlas path too (closes the known_issues "atlas trees
//    cast solid square shadows" TODO). The shadow draws are per-packet
//    (non-MDI, no gl_DrawIDARB here), so the per-draw indirection collapses to
//    plain per-packet uniforms: u_texLayer (bucket-/group-relative layer from
//    the s_packetTexArrayLayer sidecar) + u_uvScale (sub-region scale; (1,1)
//    for BC7 buckets). UV derivation `fract(v_uv) * u_uvScale` is copied from
//    static_prop_depth.frag's coalesce branch so the alpha decision matches
//    the color/depth-prepass byte-for-byte.
//
// NOTE: no #version directive — makeProgram() prepends "#version 430\n" (+
// "#define MC2_SHADOW_PROP_ATLAS 1\n" for the atlas program — GLSL #ifdefs do
// NOT inherit C++ build flags; the define rides the makeProgram prefix).
// All formerly-uint uniforms declared `int` (this project's shader_builder
// crashes on `uniform uint`; see memory/uniform_uint_crash.md).

#define PREC highp

// Forwarded from shadow_static_prop.vert (a_uv, attribute location 2).
in vec2 v_uv;

#ifdef MC2_SHADOW_PROP_ATLAS
// SHADOW-PROP-ALPHA-ATLAS-1: coalesce group/bucket albedo array (unit 0) +
// per-packet layer / sub-region scale, bound per-packet by the shadow flush.
uniform sampler2DArray u_texArr;
uniform int            u_texLayer;   // bucket-/group-relative array layer
uniform vec2           u_uvScale;    // sub-region scale (group arrays; (1,1) buckets)
#else
// Legacy non-coalesce sampling: per-type foliage texture + material flags,
// bound per-packet by drawDynamicPropShadows (mirror of the color flush).
uniform sampler2D u_tex;
#endif
uniform int       u_materialFlags;   // bit 0: ALPHA_TEST
uniform int       u_debugAddrMode;   // mode 8 = LODBUG bypass (discard skipped)

// No color/GBuffer/object-id outputs by design — depth only.

const int ALPHA_TEST_BIT = 1;

void main() {
    // OPAQUE props/buildings: ALPHA_TEST_BIT clear -> skip the texture sample
    // entirely (no unbound-sampler read) and just write depth. Only foliage /
    // tree cards (ALPHA_TEST_BIT set) pay for the sample + discard.
    if (u_debugAddrMode != 8 && (u_materialFlags & ALPHA_TEST_BIT) != 0) {
#ifdef MC2_SHADOW_PROP_ATLAS
        // BYTE-IDENTICAL alpha derivation to static_prop_depth.frag's coalesce
        // branch (fract(v_uv) * uvScale into the group/bucket array layer);
        // same 0.5 threshold as the color pass.
        vec4 tex_color = texture(u_texArr, vec3(fract(v_uv) * u_uvScale, float(u_texLayer)));
#else
        // BYTE-IDENTICAL to static_prop.frag:205+215 legacy branch:
        //   vec4 tex_color = texture(u_tex, v_uv);
        //   ... && tex_color.a < 0.5) discard;
        vec4 tex_color = texture(u_tex, v_uv);
#endif
        if (tex_color.a < 0.5) {
            discard;
        }
    }
    // Surviving fragments write depth only (no color attachments declared).
}
