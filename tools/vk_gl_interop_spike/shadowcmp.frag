#version 450
// VK side of the shadow-compare parity spike.
//
// Per output pixel we derive a (uv, refDepth) grid point and sample a
// sampler2DShadow (VkSampler compareEnable=TRUE, compareOp=LESS_OR_EQUAL).
// The hardware returns the (optionally 2x2-PCF-filtered) 0..1 lit fraction.
// We write that fraction into R (x255) so the host can read it back as UNORM8
// and compare against the GL sampler2DShadow result bit-for-bit.
//
// mode (pc.z): 0 = hardware sampler2DShadow compare (LINEAR or NEAREST per the
//   VkSampler filter set on the host side). 1 = NEAREST sampler + MANUAL 4-tap
//   PCF (the recon's fallback path): 4 explicit textureGather-style taps via
//   textureOffset on a sampler2DShadow with the compare done per-tap by hardware
//   but averaged in-shader, so filter-mode differences cannot leak in.
layout(set = 0, binding = 0) uniform sampler2DShadow srcShadow;
layout(push_constant) uniform PC {
    float gridN;   // grid is gridN x gridN
    float mapW;    // depth map width  (texels)
    float mapH;    // depth map height (texels)
    float mode;    // 0 = hw compare, 1 = manual 4-tap PCF
} pc;
layout(location = 0) out vec4 o;

// Map an integer grid coordinate in [0,gridN-1] to a spatial UV that spans the
// depth map INCLUDING inter-texel transitions and out-of-range (>1) points.
// uv range chosen to hit texel centers, edges and one out-of-border sample.
float gridToUV(float i) {
    // spread from -0.1 .. 1.1 so the extremes land in CLAMP_TO_BORDER territory.
    return (i / (pc.gridN - 1.0)) * 1.2 - 0.1;
}
// Reference depth sweep: from below the min depth (fully lit) to above max
// (fully shadowed), including exact texel values for LEQUAL tie behavior.
float gridToRef(float j) {
    return (j / (pc.gridN - 1.0)); // 0 .. 1 inclusive
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float u = gridToUV(float(p.x));
    float v = gridToUV(float(p.y));
    // Reference depth varies with a diagonal blend of x and y so every
    // (uv, ref) combination is exercised across the grid.
    float ref = gridToRef(mod(float(p.x + p.y), pc.gridN));
    float lit;
    if (pc.mode < 0.5) {
        lit = texture(srcShadow, vec3(u, v, ref));
    } else {
        // manual 4-tap PCF: gather 4 neighbour compare results and average.
        vec2 texel = vec2(1.0 / pc.mapW, 1.0 / pc.mapH);
        float s = 0.0;
        s += texture(srcShadow, vec3(u + texel.x * -0.5, v + texel.y * -0.5, ref));
        s += texture(srcShadow, vec3(u + texel.x *  0.5, v + texel.y * -0.5, ref));
        s += texture(srcShadow, vec3(u + texel.x * -0.5, v + texel.y *  0.5, ref));
        s += texture(srcShadow, vec3(u + texel.x *  0.5, v + texel.y *  0.5, ref));
        lit = s * 0.25;
    }
    o = vec4(lit, lit, lit, 1.0);
}
