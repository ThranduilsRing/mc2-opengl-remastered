//#version 420 (version provided by prefix)

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D ssaoTex;  // unit 0: blurred AO (R channel)

void main()
{
    float ao = texture(ssaoTex, TexCoord).r;
    FragColor = vec4(ao, ao, ao, 1.0);
}
