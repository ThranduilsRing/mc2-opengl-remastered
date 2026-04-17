# Overlay Shadow Implementation — The Post-Process Route

## Goal
Get building/mech shadows to draw on cement pads, roads, and burn marks (overlays).

## Executive Summary
Abandon `gos_tex_vertex` modifications entirely. The AMD driver bug makes runtime uniforms impossible, and compile-time variants are silently failing.

**The Real Problem (Corrected):** The dynamic shadow post-process pass explicitly restricts shadow rendering to terrain pixels. Shadows render on terrain, but the darkening does not trigger when the shadow falls on an overlay.

This means the dynamic pass has a gatekeeper: "Is this pixel terrain? If no, skip." Overlays fail this check, so they don't receive shadows. We don't need new shadow math — we just need to trick the dynamic pass into treating overlay pixels as terrain pixels.

## Step-by-Step Implementation Plan

### Step 1: Find the "Is Terrain?" Gatekeeper

Open the dynamic shadow post-process shader (the one darkening the mechs/terrain). Find exactly how it identifies a terrain pixel so it knows to apply the shadow. It will be one of these:

- **A G-Buffer Mask Texture:** `float isTerrain = texture(terrainMaskTex, uv).r; if (isTerrain < 0.5) discard;`
- **The Stencil Buffer:** Check the C++ side for `glStencilFunc` or `glStencilOp` right before the shadow draw call.
- **Pure Depth Reconstruction Failure:** No explicit mask, but reconstructing world position from overlay depth fails the shadow bias test because overlays are coplanar with terrain.

### Step 2: Trick the Gatekeeper (C++ Side)

Once you know how terrain is identified, force the overlay geometry to use the same identity.

**If it's a Stencil Buffer (Most likely for performance):**
When the terrain is drawn, it likely writes a 1 to the stencil buffer. The dynamic shadow pass only draws where stencil == 1.

Fix: In `txmmgr.cpp`, inside the `gos_State_Overlay == 1` block, force the overlay draw calls to also write a 1 to the stencil buffer.

```cpp
// Pseudo-code inside the overlay draw loop in txmmgr.cpp
glEnable(GL_STENCIL_TEST);
glStencilMask(0xFF); // Enable stencil writing
glStencilFunc(GL_ALWAYS, 1, 0xFF); // Always pass, write ref value of 1
glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE); // Replace stencil with 1
// ... draw overlays via gos_RenderIndexedArray ...
glStencilMask(0x00); // Disable stencil writing for subsequent passes
```

**If it's a G-Buffer Mask Texture:**
We can't easily change `gos_tex_vertex` outputs. Instead, add a micro-pass after overlays are drawn:
- Bind a tiny fullscreen shader that reads Depth and the existing terrainMaskTex.
- If depth matches an overlay (however you distinguish them, e.g., via a cheap separate 1-bit overlay mask FBO), overwrite terrainMaskTex to 1.0 for those pixels.

**If it's Depth/Bias related (No explicit mask):**
Overlays are coplanar with terrain. When reconstructing world pos from the depth buffer, slight floating-point differences might cause the shadow comparison to fail (shadow acne on a macro scale).

Fix: In `txmmgr.cpp`, before drawing overlays, add `glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-1.0f, -1.0f);` to nudge the overlay depth slightly closer to the camera than the terrain, ensuring the reconstructed world position passes the shadow bias check correctly. Disable it after the loop.

### Step 3: Pipeline Order Verification

The overlay draw calls (and their new stencil/depth tagging) MUST happen before the dynamic shadow post-process pass executes. Given the coverage debug map works, this is likely already correct, but verify it.

## Why This Bypasses the AMD Bug

- **Zero changes to `gos_tex_vertex.frag/.vert`.** No uniforms, no `#defines`, no variant compilation.
- **Zero new shadow math.** We are simply changing a render state (stencil/depth offset) so an existing, working shader path executes on overlay pixels.
- **One C++ change.** Just a few state management lines wrapped around the existing overlay draw loop in `txmmgr.cpp`.

## What to Delete / Revert

- DELETE the `IS_OVERLAY` variant infrastructure from `gameos_graphics.cpp` (enum, combinations array, material selection).
- DELETE the `#ifdef IS_OVERLAY` blocks from `gos_tex_vertex.frag` and `.vert`.
- DELETE the `gpuProjection` uniform entirely.

## Key Files to Modify

- `shaders/shadow_screen.frag` (or equivalent post-process shadow shader) — READ ONLY first. Find the exact line that restricts drawing to terrain.
- `mclib/txmmgr.cpp` — Add the stencil/depth-offset tagging around the overlay render loop.
- `GameOS/gameos/gos_postprocess.cpp` — The post-process shadow pass setup (FBO binds, draw call).

## Debug Hotkeys (Unchanged)

- RAlt+F2 = shadow debug overlay (cycles static → dynamic → off)
- RAlt+F3 = shadows on/off
- RAlt+F5 = terrain draw killswitch

## Reference: Known Gatekeeper Candidates

From memory: "Post-process shadow pass (Apr 2026): Fullscreen pass reconstructs world pos from depth via inverse VP, samples static+dynamic shadow maps, darkens non-terrain pixels via multiplicative blending. **Terrain skipped via normal alpha flag.**"

So the gatekeeper is the **normal alpha flag** in the G-buffer normal buffer (GBuffer1). Terrain writes `alpha=1` to GBuffer1, non-terrain writes `alpha=0`. The shadow screen pass checks this alpha to decide what to darken. Overlays drawn via `gos_tex_vertex` write the DEFAULT `alpha=0` to GBuffer1, so the shadow pass skips them.

**The fix may be as simple as:** making overlays write `alpha=1` to GBuffer1 — but since `gos_tex_vertex` doesn't write to GBuffer1 at all (no MRT output), the alpha stays at whatever the terrain wrote. If the overlay depth is slightly different from terrain depth, the shadow pass might reconstruct a different world position and fail the shadow test. The polygon offset approach (Step 2, depth/bias case) would fix this.
