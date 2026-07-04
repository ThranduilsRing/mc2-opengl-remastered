#version 450
// VK "prop" fragment: solid known blue. Written IN-PLACE into the shared color
// image that GL already authored. Only survives where the depth test passes.
layout(location = 0) out vec4 o;
void main() { o = vec4(0.0, 0.0, 1.0, 1.0); } // known VK prop color = blue
