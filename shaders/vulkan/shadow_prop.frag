// VK-P8a SHADOW-INTEROP — depth-only static-prop shadow-caster FS.
// =============================================================================
// Depth-only pass: NO color attachment, emits no color. The fixed-function stage
// writes gl_FragDepth (interpolated gl_Position.z / w) into the D32 target. This
// matches the GL "shadow_static_prop" program (shadow_instanced.frag, empty main).
// P8a is OPAQUE-ONLY; the foliage alpha-test (shadow_static_prop_alpha) path is P8b.
#version 450

void main() {
    // No color output. Depth written implicitly by the fixed-function stage.
}
