//#version 420 (version provided by prefix)

layout(location = 0) in vec4 pos;

uniform mat4 lightSpaceMatrix;

void main()
{
    gl_Position = lightSpaceMatrix * vec4(pos.xyz, 1.0);
}
