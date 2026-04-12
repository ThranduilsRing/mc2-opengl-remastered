//#version 420 (version provided by prefix)

uniform sampler2D shadowDebugMap;

in vec2 TexCoord;
out vec4 FragColor;

void main()
{
    float d = texture(shadowDebugMap, TexCoord).r;

    // Magenta = depth 1.0 (cleared, nothing written)
    if (d >= 0.999) {
        FragColor = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }

    // Red = depth 0.0 (near plane clipping)
    if (d <= 0.001) {
        FragColor = vec4(1.0, 0.0, 0.0, 1.0);
        return;
    }

    // Grayscale ramp for normal depth values
    float v = pow(d, 0.5);
    FragColor = vec4(v, v, v, 1.0);
}
