// VK-REGION-SHADOW-1 (sub-slice A) -- depth-only shadow-caster fragment shader.
// A depth-only pass has NO color attachment and writes nothing; the rasterizer
// records gl_FragDepth (== interpolated gl_Position.z / w) into the D32 target.
// This frag exists only because the pipeline needs a fragment stage; it emits no
// color. Keeping it empty (no outputs) matches a color-attachmentless render pass.
#version 450

void main() {
    // No color output. Depth is written implicitly by the fixed-function stage.
}
