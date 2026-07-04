// VK-P11 STATIC-PROP COLOR — Tier-0 flat/debug color FS.
// =============================================================================
// Tier-0 does NOT wire the full texture-array / MaterialGpu / lighting path (that
// is a later parity tier). It outputs a flat premultiplied color per prop fragment
// so the seam proof is a clear scene-pixel check: "do static props VISIBLY render,
// with the right silhouettes, at roughly the right positions?".
//
// PREMULTIPLIED-alpha over (matches the water island composite contract): the FS
// emits vec4(rgb*a, a); the pipeline blend + GL composite are premult-over
// (ONE / ONE_MINUS_SRC_ALPHA), so a single opaque prop fragment (a=1) stores
// (rgb, 1) and composites as MainColor = rgb — a clean opaque overlay of the props.
// The color target is cleared to transparent black each frame, so non-prop pixels
// stay a=0 and the composite leaves the scene untouched there.
#version 450

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 o;

void main() {
    // Flat debug albedo (a light teal). UV kept live (varies across the surface)
    // so the silhouette + surface orientation are legible in the probe capture.
    vec3 rgb = vec3(0.35, 0.62, 0.68) * (0.6 + 0.4 * vUv.x);
    float a = 1.0;                 // opaque props
    o = vec4(rgb * a, a);          // premultiplied (premult-over composite)
}
