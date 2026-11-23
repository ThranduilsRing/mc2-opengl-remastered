// Display-domain seam. Mode 0 is the frozen SDR path.
#include "hdr_transfer.glsl"
layout(constant_id = 0) const int OUTPUT_MODE = 0;
layout(constant_id = 1) const bool OUTPUT_DITHER = true;
vec3 outputTransform(vec3 color, bool reconstructed) {
    if (OUTPUT_MODE == 0) {
        if (reconstructed) return clamp(color, 0.0, 1.0);
        color = applyTonemap(color);
        if (OUTPUT_DITHER) color = clamp(color + ignTriangle(floor(gl_FragCoord.xy)) / 255.0, 0.0, 1.0);
        else color = clamp(color, 0.0, 1.0);
    }
    if (OUTPUT_MODE == 1 && !reconstructed) {
        color = vec3(hdrDecode(max(color.r, 0.0)), hdrDecode(max(color.g, 0.0)), hdrDecode(max(color.b, 0.0)));
        float y = dot(color, vec3(0.2126, 0.7152, 0.0722));
        if (y > 0.0) color *= hdrEetf(y, u.p1.y, u.p1.z) / y;
    }
    return color;
}
