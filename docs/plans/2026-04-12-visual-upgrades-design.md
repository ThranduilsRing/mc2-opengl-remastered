# Visual Upgrades Design — GPU Grass, God Rays, Shorelines, Texture Upscaling

**Date:** 2026-04-12
**Branch:** claude/nifty-mendeleev
**Status:** Approved

## Overview

Four visual upgrades to the MC2 OpenGL renderer, ordered by priority:

1. **GPU Grass** — Geometry shader billboard grass on terrain
2. **God Rays** — Post-process radial light scattering
3. **Depth-Aware Shorelines** — Post-process foam at water/land edges
4. **Texture Upscaling** — Offline AI 4x upscale of menu/UI textures

All GPU effects leverage the existing G-buffer (RGBA16F normals + depth texture), tessellation pipeline, and post-process fullscreen quad infrastructure.

---

## Feature 1: GPU Grass via Geometry Shader

### Approach
A second terrain draw pass that reuses the tessellation pipeline (same VAO, TCS, TES) but adds a geometry shader that conditionally emits axis-aligned billboard grass quads where the terrain colormap classifies as grass.

### Pipeline
1. Bind terrain VAO with `GL_PATCHES`
2. TCS/TES produce world-position vertices (identical to main terrain pass)
3. **New geometry shader** (`gos_grass.geom`):
   - Input: `triangles` from TES
   - For each vertex: sample colormap via HSV, check grassWeight > 0.3
   - Emit axis-aligned quad (locked world Y-up, rotates around Y to face camera)
   - Output: `triangle_strip, max_vertices = 12`
4. **New fragment shader** (`gos_grass.frag`):
   - Alpha-tested grass blade texture
   - Tinted by underlying terrain color (sampled from colormap)
   - Wind animation: sinusoidal top-vertex displacement using `time + worldPos.x * hash`
   - Shadow sampling (static + dynamic shadow maps)
   - Distance alpha fadeout

### Grass Quad Parameters
- Height: 15-25 world units (randomized via position hash)
- Width: 10-15 world units
- Orientation: axis-aligned (vertical quad, rotates around Y toward camera)
- Density: 1 quad per tessellated vertex where grass detected

### Distance LOD
- Full density: < 3000 world units from camera
- Linear fade: 3000-5000 units (reduce alpha + skip quad emission)
- Culled: > 5000 units (geometry shader emits nothing)

### New Files
- `shaders/gos_grass.geom` — geometry shader
- `shaders/gos_grass.frag` — fragment shader

### Modified Files
- `GameOS/gameos/gameos_graphics.cpp` — add grass draw call after terrain draw
- `GameOS/gameos/gos_postprocess.cpp` — compile grass shader program

### Performance Notes
- Engine is CPU-bound; GPU has headroom for geometry shader work
- RDNA3 (7900 XTX) geometry shaders are efficient
- Distance culling in geometry shader prevents GPU overwork at Wolfman zoom

---

## Feature 2: God Rays (Radial Light Scattering)

### Approach
Screen-space volumetric light scattering via radial blur in a post-process pass. Projects sun to screen space, creates an occlusion map, then blurs bright pixels radially toward the sun position.

### Pipeline (new post-process pass, after screen shadows, before bloom)
1. **Occlusion pass** (half-res FBO):
   - Sky pixels (depth == 1.0) = sun color brightness
   - Occluded pixels = black
   - Modulated by cloud shadow FBM noise (from `noise.hglsl`) to create gaps between clouds
2. **Radial blur** (same half-res FBO, in-place or ping-pong):
   - 32 samples marching from each pixel toward sun screen position
   - Accumulates with exponential decay
3. **Composite**: Additive blend onto scene (before bloom, so bloom picks up rays)

### Sun Screen Position
- Project `cameraWorldPos + normalize(terrainLightDir) * farPlane` through VP matrix to NDC
- Convert to screen UV for radial blur center

### Cloud Shadow Interaction
- Include `noise.hglsl` in god ray shader
- Sample same FBM noise used for terrain cloud shadows at ray sample positions
- Clouds thin = brighter sky pixels = stronger rays through gaps

### Tunable Uniforms
- `density` (0.8-1.0): ray march length
- `weight` (0.4-0.6): ray brightness
- `decay` (0.95-0.99): per-sample falloff
- `exposure` (0.3-0.5): final intensity
- `numSamples` (32): quality/perf tradeoff

### Edge Cases
- Sun behind camera: screen position off-screen, radial blur naturally fades to edges
- Sun below horizon: no bright sky pixels, effect zeroes out
- Night maps: disable via sun elevation check

### New Files
- `shaders/godray.frag` — occlusion + radial blur + composite

### Modified Files
- `GameOS/gameos/gos_postprocess.cpp` — half-res FBO allocation, pass in endScene()

### Performance
- Half-res: 32 texture fetches/pixel at quarter pixel count = trivial on modern GPU
- Single additional FBO (half-res RGBA16F)

---

## Feature 3: Depth-Aware Shorelines

### Approach
Post-process pass that detects water/land boundaries using the G-buffer and applies animated foam along the shoreline.

### G-Buffer Water Flag
Water is currently classified in `gos_terrain.frag` via HSV hue (0.35-0.45). The normal buffer alpha channel currently encodes:
- `alpha = 1.0` → terrain
- `alpha = 0.0` → objects/overlays

**Change**: Write `alpha = 0.25` for water-classified terrain pixels. This creates a 3-value material ID in the existing buffer with no new render targets.

### Shoreline Detection
1. For each pixel, read normal alpha to check if water (alpha ~0.25)
2. Sample neighboring pixels' depth values (4-8 neighbors)
3. If neighbor is non-water AND depth difference < threshold → shoreline zone
4. Foam intensity proportional to proximity (depth gradient)

### Foam Effect
- Animated FBM noise (reuse cloud shadow noise from `noise.hglsl`)
- White/bright foam color blended along detected shoreline
- Width controlled by depth difference threshold (~50-100 world units)
- Time-animated to create shifting foam pattern

### New Files
- `shaders/shoreline.frag` — shoreline detection + foam

### Modified Files
- `shaders/gos_terrain.frag` — write alpha = 0.25 for water pixels
- `GameOS/gameos/gos_postprocess.cpp` — add shoreline pass in endScene()

---

## Feature 4: Texture Upscaling (Offline)

### Approach
Extend existing ESRGAN pipeline (`esrgan_upscale.py`) to process menu, loading screen, HUD, and unit portrait textures at 4x.

### Scope
- Menu backgrounds and UI elements
- Loading screen textures
- HUD elements and icons
- Unit portraits / faction art
- **Exclude**: Normal maps (would corrupt directional encoding), already-processed terrain

### Pipeline
1. Inventory non-terrain TGA files across game data directories
2. Categorize by type (menu, HUD, unit, etc.)
3. Run ESRGAN 4x with `4x_GameAI_2.0.pth` model
4. Preserve alpha channels (process RGB separately, recombine)
5. Output as TGA to match engine loader expectations
6. No engine code changes needed (same filenames, same format)

### Existing Infrastructure
- `esrgan_upscale.py` — RRDB network with tiled inference, model loading, TGA deploy
- `upscale_textures.py` — Pillow LANCZOS for terrain mip chains
- `pack_mat_normal.py` — normal map packing (not relevant here)

---

## Hotkey Plan
- **RAlt+F4**: Toggle grass on/off (debug)
- God rays and shorelines always-on (can add toggles if needed)

## Render Order (updated endScene)
1. Scene draw (terrain, objects, water)
2. **Grass draw** (new — geometry shader pass)
3. Screen shadows
4. **Shoreline pass** (new — post-process)
5. **God ray pass** (new — post-process, before bloom)
6. Bloom (threshold + blur)
7. Composite + FXAA
8. Shadow debug overlay
