// Display-domain seam. All shipping pipelines use mode 0 until the HDR present chain exists.
layout(constant_id = 0) const int OUTPUT_MODE = 0;
vec3 outputTransform(vec3 color, bool reconstructed) {
    if (OUTPUT_MODE == 0) {
        if (reconstructed) return clamp(color, 0.0, 1.0);
        color = applyTonemap(color);
        color = clamp(color + ignTriangle(floor(gl_FragCoord.xy)) / 255.0, 0.0, 1.0);
    }
    return color;
}
