//#version 420 (version provided by prefix)

layout(location = 4) in vec3 worldPos;

uniform mat4 lightSpaceMatrix;

void main()
{
    gl_Position = lightSpaceMatrix * vec4(worldPos, 1.0);
}
