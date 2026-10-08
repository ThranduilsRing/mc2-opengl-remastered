// VK-PARITY-F3: Vulkan port of shaders/hdri_skybox.vert. Fullscreen triangle,
// NDC passed through (the VK shared GBuffer uses the same NDC->memory mapping as
// the GL scene FBO, so no flip is needed -- see fog_oob.vert).
#version 450

layout(location = 0) out vec2 vNdc;

void main()
{
    vec2 ndc = vec2((gl_VertexIndex == 1) ? 3.0 : -1.0,
                    (gl_VertexIndex == 2) ? 3.0 : -1.0);
    vNdc = ndc;
    // z = 0.0 = reverse-Z far plane: the pipeline's GEQUAL depth test passes only
    // where nothing nearer was written (the sky), exactly the pixels GL's
    // draw-sky-first-then-terrain order leaves visible.
    gl_Position = vec4(ndc, 0.0, 1.0);
}
