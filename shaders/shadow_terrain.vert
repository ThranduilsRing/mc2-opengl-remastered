//#version 420 (version provided by prefix)

layout(location = 0) in vec4 dummyPos;  // AMD requires attrib 0 to be active
layout(location = 4) in vec3 worldPos;

uniform mat4 lightSpaceMatrix;

void main()
{
    gl_Position = lightSpaceMatrix * vec4(worldPos, 1.0);
    gl_Position.w += dummyPos.x * 0.0;  // prevent strip
}
