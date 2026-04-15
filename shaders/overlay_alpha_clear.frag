//#version 420 (version provided by prefix)

// Clears the terrain flag (GBuffer1 alpha) on overlay pixels.
// Drawn as a fullscreen quad with stencil test — only touches pixels
// where overlays were rendered (stencil == 1).

#define PREC highp

layout(location = 0) out PREC vec4 FragColor;

void main()
{
    // Flat-up normal (0,0,1) encoded as 0.5,0.5,1.0; alpha=0 = non-terrain
    FragColor = vec4(0.5, 0.5, 1.0, 0.0);
}
