//#version 430 (provided by makeProgram prefix)
#include <include/axis.hglsl>
//
// VFX mesh substrate VS (MC2_VFX_ORACLE_SHAPE slice — gosFX::Shape only).
//
// Persistent-mesh model: real index/vertex buffers are bound (NOT a dummy
// gl_VertexID-driven VAO like the billboard path). Per-instance transform +
// scale + color arrive as uniforms set by the bridge before each indexed draw.
//
// Coordinate convention matches particle_billboard.vert: a model-space vertex
// is first placed in MC2/Stuff world space by (vert*scale) * modelToWorld, then
// axis-swapped to GL world (GLx=-Stuffx, GLy=Stuffz, GLz=Stuffy), then projected
// by u_worldToClipGL (= terrainMVP, the same matrix the GPU rasterizes terrain
// with) so it is pixel-consistent with every other bridge pass.

layout(location = 0) in vec3 a_modelPos;   // model-space position
layout(location = 1) in vec2 a_uv;         // model UV

uniform mat4  u_worldToClipGL;   // world(GL) -> GL clip (same as billboard)
uniform mat4  u_modelToWorld;    // Stuff/MC2 model -> world (row-major affine)
uniform float u_scale;           // uniform model scale
uniform vec4  u_instanceColor;   // per-instance paint color (rgba)

out vec2 v_uv;
out vec4 v_color;

void main() {
    // (model * scale) -> Stuff world. u_modelToWorld is uploaded transposed
    // (GL_TRUE) by the bridge so column-vector multiply here matches the
    // legacy LinearMatrix4D row-major affine.
    vec3 scaled    = a_modelPos * u_scale;
    vec3 stuffWorld = (u_modelToWorld * vec4(scaled, 1.0)).xyz;

    // Same axis swap the billboard path uses.
    vec3 glWorld = mc2ToRender(stuffWorld);

    gl_Position = u_worldToClipGL * vec4(glWorld, 1.0);

    v_uv    = a_uv;
    v_color = u_instanceColor;
}
