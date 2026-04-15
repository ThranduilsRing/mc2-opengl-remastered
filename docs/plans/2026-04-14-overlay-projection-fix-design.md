# Overlay Projection Fix — Design Doc
**Date:** 2026-04-14  
**Status:** Approved (Approach A)

## Problem

Partial cement tiles and road overlays are invisible in-game. These tiles are submitted via `setOverlayWorldCoords()` → `MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS` → Overlay.SplitDraw → `gos_tex_vertex.vert` (IS_OVERLAY variant).

## Root Cause

In `shaders/gos_tex_vertex.vert`, the IS_OVERLAY world-position branch applies a manual axis swap before multiplying by `terrainMVP`:

```glsl
vec3 projectedWorldPos = vec3(-MC2WorldPos.x, MC2WorldPos.z, MC2WorldPos.y);
gl_Position = terrainMVP * vec4(projectedWorldPos, 1.0);
```

`terrainMVP = axisSwap * worldToClip` already bakes in the `(-x, z, y)` conversion from MC2 coords to the rendering coordinate space. The manual swap applies that same conversion a second time, producing vertices in the wrong position — outside the frustum → invisible.

The trigger: `setOverlayWorldCoords()` sets `v.rhw = 1.0f`, which signals the shader to take the world-position GPU-projection branch. Only partial cement and road overlay tiles call this function (solid cement tiles were rerouted to the tessellated terrain path and already work).

## Why the Mission Marker Is Visible

The green mission marker crosshair uses CPU-projected vertices (`rhw = 1/w ≠ 1.0`) and takes the standard `mvp * pos / pos.w` branch, bypassing the broken world-pos path entirely.

## Approved Fix: Approach A (one-line shader change)

Remove the manual axis swap. Pass MC2 world coordinates directly to `terrainMVP`:

```glsl
// Before (broken — double-applies the axis swap):
vec3 projectedWorldPos = vec3(-MC2WorldPos.x, MC2WorldPos.z, MC2WorldPos.y);
gl_Position = terrainMVP * vec4(projectedWorldPos, 1.0);

// After (correct — terrainMVP owns the swap):
gl_Position = terrainMVP * vec4(MC2WorldPos, 1.0);
```

**File:** `shaders/gos_tex_vertex.vert` only. No C++ changes needed.

**Precedent:** `shaders/gos_tex_vertex_lighted.vert` gpuProjection path already does exactly this:
```glsl
MC2WorldPos = vec3(-WorldPos.x, WorldPos.z, WorldPos.y);  // Stuff → MC2
gl_Position = terrainMVP * vec4(MC2WorldPos, 1.0);        // terrainMVP adds swap internally
```
And mission markers appear at the correct world position.

**Depth precision:** Overlay.SplitDraw uses `glDisable(GL_DEPTH_TEST)`, so Z precision is irrelevant. The full TES projection chain (Approach B) is unnecessary.

## Scope

- Fixes: partial cement boundary tiles, roads (all `MC2_ISCRATERS` overlays with `rhw=1.0`)
- Does not change: solid cement tiles (already on tessellated path), mission marker shadow (deferred)
- Deploy: shader-only, no exe rebuild needed
