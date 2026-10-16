#pragma once
#include <cstdint>

namespace DebugRenderer {

struct Vec3 { float x, y, z; };

// Enqueue a world-space line segment. No-op when MC2_DEBUG_RENDERER is unset.
void drawLineWorld(Vec3 a, Vec3 b, uint32_t rgba);

// Enqueue a world-space AABB as 12 edges (24 verts).
// No-op when MC2_DEBUG_RENDERER is unset.
void drawAabbWorld(Vec3 mn, Vec3 mx, uint32_t rgba);

// Enqueue a world-space horizontal ring (XZ plane at center.y).
// segments clamped to [3, 256]. No-op when MC2_DEBUG_RENDERER is unset.
void drawRingWorld(Vec3 center, float radius, int segments, uint32_t rgba);

// Oriented ring in the plane perpendicular to 'normal'.
// Falls back to XZ-plane ring when normal has near-zero length (< 1e-6).
// segments clamped to [3, 256]. No-op when MC2_DEBUG_RENDERER is unset.
void drawRingWorld(Vec3 center, Vec3 normal, float radius, int segments, uint32_t rgba);

// World-space 3-axis cross: lines along X, Y, Z through center, halfSize each side.
// No-op when MC2_DEBUG_RENDERER is unset.
void drawCrossWorld(Vec3 center, float halfSize, uint32_t rgba);

// Approximate sphere: 3 great-circle rings (XY, YZ, XZ planes) plus (rings-1)
// latitude rings. rings clamped to [1, 16]; segments passed through ([3, 256]).
// No-op when MC2_DEBUG_RENDERER is unset.
void drawSphereApproxWorld(Vec3 center, float radius, int rings, int segments, uint32_t rgba);

// Reserved: world-space text label. No-op in M1/M2.
// Future spec: billboard projected from 'pos' to screen, rendered in flushScreenPrims.
void drawTextWorld(Vec3 pos, const char* text, uint32_t rgba);

// Freeze the primitives collected this frame together with the scene view-projection. Call once per frame after the
// scene has set its matrices. No-op when MC2_DEBUG_RENDERER is unset.
void flushWorldPrims();

// Draw the frozen primitives on the UI overlay (the layer the Vulkan present draws over the scene). Call once per
// frame after the UI frame has begun. Lines are not depth-tested. No-op when nothing is frozen. Under Vulkan the overlay
// is visible only while the Vulkan ImGui renderer is on (MC2_VK_IMGUI).
void drawFrozenPrimsToOverlay();

// Reserved: future crisp HUD/debug-text pass (post post-process). No-op in M1.
void flushScreenPrims();

} // namespace DebugRenderer
