//#version 430 (provided by makeProgram prefix)
//
// GPU particle billboard expansion VS. Per plan v5 §5.4 B1 Stage 1' Commit 3.
//
// Draw call: glDrawArrays(GL_TRIANGLES, 0, 6 * particleCount) — issued by the
// GameOS bridge. No index buffer. Each particle expands to two triangles
// (six vertices); the per-vertex corner index is gl_VertexID % 6.
//
// Per AMD discipline (memory: gpu_direct_renderer_bringup_checklist.md
// trap #10 + amd-driver-rules.md:6):
//   - AMD attribute-0 rule: we reference gl_VertexID directly; no
//     layout(location=0) attribute needed because the engine binds a
//     dummy/empty VAO and the GL spec says gl_VertexID is always defined
//     even when no attribute is enabled.
//   - textureLod (not texture) in the FS — partner shader uses textureLod
//     to dodge AMD auto-LOD black-on-incomplete-mips strict-fail.
//
// In-front discipline post-F1 (kAxisSwapMC2toGL has the R-clipw polarity
// fold per addendum-rclipw-polarity.md, so u_worldToClipGL produces
// clip.w > 0 for in-front MC2 verts). Hardware clip-volume test handles
// rejection of behind-camera vertices; no explicit pz cull in this VS.
// CPU-side particle visibility is gated upstream by the gosFX/MLR system.

#include <include/particles.hglsl>
#include <include/axis.hglsl>

layout(std430, binding = 14) readonly buffer Particles {
    Particle particles[];
};

uniform mat4 u_worldToClipGL;  // world -> GL clip (kAxisSwapMC2toGL * worldToClip)

// B2 P1: view-aligned billboard basis. Set per-flush by gos_particle_bridge
// via gos_SetActiveCamera(). Default identity (right=+X, up=+Y) keeps the
// pre-B2 east-up fixed-axis behaviour if the caller never sets them.
uniform vec3 u_cameraRight;
uniform vec3 u_cameraUp;

// P2-1 UV sub-rect: atlas origin and per-tile size for this draw group.
// Set per draw call by the bridge (gos_particle_bridge.cpp).
// Default full-page: u_uvOffset=(0,0), u_uvSize=(1,1), u_atlasColumns=0.
// For a static atlas sub-rect: u_uvOffset=(tileU0,tileV0), u_uvSize=(tileUs,tileVs),
//   u_atlasColumns=0 — same tile for all particles.
// VFX-FLIPBOOK-ASSET-TABLE-1: for animated atlases (u_atlasColumns>1), the VS
//   reads p.atlasIndex as the per-particle frame index and applies a tile offset:
//   col = atlasIndex % u_atlasColumns; row = atlasIndex / u_atlasColumns.
//   Final UV = u_uvOffset + (vec2(col,row) + kCornerUv[i]) * u_uvSize.
uniform vec2 u_uvOffset;
uniform vec2 u_uvSize;
uniform uint u_atlasColumns;  // VFX-FLIPBOOK-ASSET-TABLE-1: 0/1=static, >1=animated

// VFX-SHADER-AGE-FADE-PARITY-1: per-particle soft death fade from normalized age.
// Oracle particles have p.lifetime > 0.5 (normalized sentinel 1.0f, set by the
// oracle harvest path in VFX-AGE-LIFETIME-UPLOAD-1). Non-oracle: p.lifetime==0.0.
// u_vfxAgeFade=0.0 (default) → mix factor 0 → v_color.a unchanged → byte-identical.
// u_vfxAgeFade=1.0 → full age-driven fade: the final 30% of life smoothly fades to 0.
// Blend control (0..1) lets the user dial between CPU-uploaded alpha and GPU age fade.
uniform float u_vfxAgeFade;

// Atlas UVs for the four billboard corners (normalized [0,1] within the sub-rect).
// The final UV is: u_uvOffset + kCornerUv[i] * u_uvSize.
const vec2 kCornerUv[4] = vec2[](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(0.0, 1.0),
    vec2(1.0, 1.0)
);

// Two triangles per quad, six vertices, with corner index per VS invocation.
// P0-2 winding fix: both triangles CCW under GL_CCW default.
// tri0 = (BL, BR, TL) = CCW; tri1 = (TL, TR, BR) -> reordered to (TL, TR, BR).
// Old: int[](0,1,2, 2,1,3) — tri1 was (TL,BR,TR) = CW, culled when GL_CULL_FACE on.
// New: int[](0,1,2, 2,3,1) — tri1 is (TL,TR,BR) = CCW.
const int kCornerIdx[6] = int[](0, 1, 2, 2, 3, 1);

out vec2 v_uv;
out vec4 v_color;
flat out uint v_kind;
flat out uint v_is_head;
flat out float v_age;  // VFX-SHADER-AGE-FADE-PARITY-1: normalized [0,1] age for FS debug view

void main() {
    uint particleId = uint(gl_VertexID) / 6u;
    int  cornerSlot = int(uint(gl_VertexID) % 6u);
    int  cornerIdx  = kCornerIdx[cornerSlot];

    Particle p = particles[particleId];

    // B2 P1: view-aligned billboard using camera right/up uniforms.
    // corner_offset.xy is in [-0.5, +0.5]; multiply by 2*effSize to get
    // half-extents in each direction. u_cameraRight and u_cameraUp are
    // already in GL world space (axis-swapped by the C++ caller), so
    // worldPos is directly in the same space as terrainMVP expects.
    //
    // This replaces the B1 fixed-axis approach (Stuff-X east + Stuff-Z up
    // with an axis swap) which produced a thin-strip artifact when the camera
    // rotates to face east or west.
    // VFX-ORIGINAL-RENDER-ANIM-FIELDS-1: aspect-correct billboard + in-plane spin.
    // velocity.xyz = (sizeX, sizeY, spinAngle) from oracle path; (0,0,0) from placeholder.
    // Fallback to scalar size when velocity.x == 0 (placeholder path, byte-identical).
    // VFX-WEAPON-FX-RESTORE-OPUS-1: oracle particles (p.lifetime > 0.5 sentinel)
    // have spec-driven sizes and use a 1.0wu floor; placeholder particles keep
    // 8.0wu floor since placeholder Spawn may set size=0 for unrouted classes.
    float sizeFloor = (p.lifetime > 0.5) ? 1.0 : 8.0;
    float sX = (p.velocity.x > 0.0) ? max(p.velocity.x, 4.0) : max(p.size, sizeFloor);
    float sY = (p.velocity.y > 0.0) ? max(p.velocity.y, 4.0) : max(p.size, sizeFloor);
    vec2 unitCorner = (kCornerUv[cornerIdx] - vec2(0.5, 0.5)) * 2.0;  // range [-1,1]
    float cs = cos(p.velocity.z);   // velocity.z = spinAngle (0.0 for placeholder)
    float sn = sin(p.velocity.z);
    // Rotate the aspect-scaled corner by the spin angle (in billboard plane)
    vec2  corner      = mat2(cs, sn, -sn, cs) * (unitCorner * vec2(sX, sY));
    // Particle center is in Stuff/MC2 world space; apply the same axis swap
    // (GL_x=-Stuff_x, GL_y=Stuff_z, GL_z=Stuff_y) used pre-B2 so the center
    // lands correctly, then add the view-aligned offsets in GL world space.
    vec3 stuffCenter  = p.position.xyz;
    vec3 glCenter     = mc2ToRender(stuffCenter);
    vec3 worldPos     = glCenter
                      + u_cameraRight * corner.x
                      + u_cameraUp    * corner.y;

    // F1 Stage A: direct GL clip emit.
    gl_Position = u_worldToClipGL * vec4(worldPos, 1.0);

    // VFX-FLIPBOOK-ASSET-TABLE-1: apply per-particle atlas frame offset when
    // u_atlasColumns > 1.  frame = p.atlasIndex; col = frame % columns;
    // row = frame / columns.  For non-animated groups (u_atlasColumns <= 1)
    // frameOffset stays (0,0) and the result is identical to the old path.
    vec2 frameOffset = vec2(0.0);
    if (u_atlasColumns > 1u) {
        uint atlasFrame = p.atlasIndex;
        uint col = atlasFrame % u_atlasColumns;
        uint row = atlasFrame / u_atlasColumns;
        frameOffset = vec2(float(col), float(row));
    }
    v_uv      = u_uvOffset + (frameOffset + kCornerUv[cornerIdx]) * u_uvSize;
    v_color   = p.color;
    v_kind    = (p.kind_flags >> 4u) & 0xFu;
    v_is_head = p.kind_flags & 1u;
    v_age     = p.age;
    // VFX-SHADER-AGE-FADE-PARITY-1: soft death fade from normalized age.
    // p.lifetime > 0.5 detects oracle particles (normalized sentinel = 1.0f).
    // Non-oracle particles have p.lifetime == 0.0 → condition false → byte-identical.
    // u_vfxAgeFade=0 (default) → mix resolves to 1.0 → v_color.a unchanged.
    if (u_vfxAgeFade > 0.0 && p.lifetime > 0.5) {
        float ageFade = clamp(1.0 - smoothstep(0.7, 1.0, p.age), 0.0, 1.0);
        v_color.a *= mix(1.0, ageFade, u_vfxAgeFade);
    }
}
