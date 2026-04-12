//#version 420 (provided by material prefix)

layout(location = 0) in vec3 position;

uniform mat4 lightSpaceMatrix;  // col-major, MC2 world → light clip
uniform mat4 worldMatrix;       // row-major Stuff::Matrix4D (uploaded with GL_TRUE)
uniform vec3 lightOffset;       // MC2-space offset toward sun

void main() {
    // Transform model-space position through world matrix (Stuff space)
    vec4 stuffPos = worldMatrix * vec4(position, 1.0);

    // Swizzle Stuff → MC2: MC2.x = -Stuff.x, MC2.y = Stuff.z, MC2.z = Stuff.y
    vec3 mc2Pos = vec3(-stuffPos.x, stuffPos.z, stuffPos.y) + lightOffset;

    gl_Position = lightSpaceMatrix * vec4(mc2Pos, 1.0);
}
