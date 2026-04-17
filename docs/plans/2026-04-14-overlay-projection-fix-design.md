# Overlay Projection Fix — Design Doc
**Date:** 2026-04-14  
**Status:** Implemented (Fragment Fog Fix)

## Problem

Partial cement tiles and road overlays are invisible in-game. These tiles are submitted via `setOverlayWorldCoords()` → `MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS` → Overlay.SplitDraw → `gos_tex_vertex.vert` (IS_OVERLAY variant).

## Root Cause (confirmed by deep analysis)

**Fragment shader:** `shaders/gos_tex_vertex.frag` IS_OVERLAY world-pos path used fog blend with reversed parameters:

```glsl
// Wrong: FogValue=1.0 → fog_color → invisible when fog is disabled
FragColor = mix(c, fog_color, FogValue);
```

`FogValue = fog.w = fogResult/255`. For nearby terrain (hazeFactor≈0, no distance fog): `fogResult=255 → FogValue≈1.0`. With fog disabled, `fog_color=(0,0,0,0)` → `mix(c,(0,0,0,0),1.0)=(0,0,0,0)` → transparent → **invisible**.

The non-overlay path correctly uses `mix(fog_color.rgb, c.rgb, FogValue)` where FogValue=1 means clear (no fog).

## Previous Analysis (Approaches A and B)

**Approach A** (`gl_Position = terrainMVP * MC2WorldPos`) was initially "approved" based on wrong assumptions:
- The lighted shader "precedent" for GPU projection is dead code — AMD driver bug makes `gpuProjection` uniform always read 0.
- Approach A puts D3D clip space values directly in `gl_Position`. After OpenGL perspective divide: x/w, y/w ∈ [0,1] → renders to upper-right screen quadrant only. **Wrong.**

**Approach B** (full TES viewport chain in overlay vertex shader) is mathematically correct:
```
terrainMVP * MC2WorldPos = AW^T * (vx,vy,elev,1) = projectZ(vx,vy,elev)
```
Because: C++ `mat4` row-major, uploaded `GL_FALSE` → GLSL column c = C++ row c → `terrainMVP*v = AW^T*v`. And `AW^T * (vx,vy,elev,1)` is exactly `projectZ(vx,vy,elev)` (Stuff row-vector convention). The full TES chain → correct OpenGL NDC.

The current vertex shader (`gos_tex_vertex.vert` IS_OVERLAY) already has the correct Approach B implementation.

## Implemented Fix

In `shaders/gos_tex_vertex.frag`, fixed fog blend in the IS_OVERLAY world-pos path:

```glsl
// Before (wrong — double-reversed, invisible with fog disabled):
if (OverlayUsesWorldPos > 0.5) {
    FragColor = mix(c, fog_color, FogValue);
    return;
}

// After (correct — matches non-overlay convention: FogValue=1 → clear):
if (OverlayUsesWorldPos > 0.5) {
    if(fog_color.x>0.0 || fog_color.y>0.0 || fog_color.z>0.0 || fog_color.w>0.0)
        c.rgb = mix(fog_color.rgb, c.rgb, FogValue);
    FragColor = c;
    return;
}
```

**File changed:** `shaders/gos_tex_vertex.frag` only. No C++ changes needed.

## Key Invariants (do not change)

- `terrainMVP` uploaded with `GL_FALSE` in `gameos_graphics.cpp` — this is intentional and correct.
- `gos_tex_vertex.vert` IS_OVERLAY world-pos branch uses full TES viewport chain — this is correct.
- Comment in `code/gamecam.cpp` line ~165 previously said "uploaded with GL_TRUE" — **corrected** to GL_FALSE.

## Scope

- Fixes: partial cement boundary tiles (isAlpha=true), roads (all `MC2_ISCRATERS` overlays with `rhw=1.0`)
- Does not change: solid cement tiles (already on tessellated path), vertex shader projection

## Also Explains

- "Pink stripes at top" previously observed: with fog enabled and non-zero fog_color, `mix(c, fog_color, 1.0) = fog_color` → tiles rendered in fog color at correct positions.
- "No change" with Approach A: tiles projected to upper-right quadrant (out of typical camera view), and also invisible due to the fragment fog bug.
