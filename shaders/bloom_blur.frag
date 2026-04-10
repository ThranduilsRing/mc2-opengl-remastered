//#version 420 (version provided by prefix)

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D image;
uniform int horizontal;
uniform vec2 texelSize;

const float weight[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

void main()
{
    vec3 result = texture(image, TexCoord).rgb * weight[0];

    if (horizontal == 1) {
        for (int i = 1; i < 5; ++i) {
            result += texture(image, TexCoord + vec2(texelSize.x * i, 0.0)).rgb * weight[i];
            result += texture(image, TexCoord - vec2(texelSize.x * i, 0.0)).rgb * weight[i];
        }
    } else {
        for (int i = 1; i < 5; ++i) {
            result += texture(image, TexCoord + vec2(0.0, texelSize.y * i)).rgb * weight[i];
            result += texture(image, TexCoord - vec2(0.0, texelSize.y * i)).rgb * weight[i];
        }
    }

    FragColor = vec4(result, 1.0);
}
