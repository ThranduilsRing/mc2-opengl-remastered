//#version 420 (provided by material prefix)

// TG_HWTypeVertex layout (36 bytes):
// position (vec3), normal (vec3), aRGBLight (uint packed as vec4), texcoord (vec2)
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 aRGBLight;
layout(location = 3) in vec2 texcoord;

uniform mat4 shadowMVP;  // lightSpaceMatrix * worldMatrix

void main() {
    // Dummy reads to keep AMD happy (attrib 0 must be active, others declared)
    vec3 p = position;
    gl_Position = shadowMVP * vec4(p, 1.0);
}
