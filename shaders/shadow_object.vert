//#version 420 (provided by material prefix)

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 aRGBLight;
layout(location = 3) in vec2 texcoord;

uniform mat4 shadowMVP;

void main() {
    gl_Position = shadowMVP * vec4(position, 1.0);
}
