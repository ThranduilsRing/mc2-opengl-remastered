# Changelog

All rendering features added to the MechCommander 2 OpenGL port, grouped by category.

## Terrain Rendering

- **PBR splatting** -- multi-material terrain with per-type albedo, normal maps, roughness, and tint colors
- **Parallax occlusion mapping** -- depth displacement on terrain materials (strongest on dirt)
- **Hardware tessellation** -- TCS/TES pipeline for smooth terrain geometry with distance-based LOD
- **Triplanar cliff mapping** -- automatic rock texture on steep slopes based on surface normal
- **Cloud shadows** -- animated FBM noise pattern moving across terrain
- **Height-based exponential fog** -- thicker in valleys, fades with altitude
- **Anti-tiling** -- noise-based UV perturbation to break texture repetition at distance

## Lighting and Shadows

- **Static terrain shadow map** -- 4096x4096 world-fixed orthographic projection with multi-frame accumulation (fills as camera pans, re-renders on large camera moves)
- **Dynamic mech shadows** -- 2048x2048 with ray-ground intersection frustum centering and camera bias
- **Poisson disk PCF** -- 16-sample stratified sampling with per-pixel rotation to break banding, adjustable softness via `[`/`]` keys
- **Post-process shadow pass** -- fullscreen depth-reconstruction pass that shadows all geometry (terrain, overlays, buildings, mechs) via multiplicative blending
- **Object shadow casting** -- mechs and buildings cast into shadow map via direct GPU draw (bypasses material system)

## G-Buffer and Deferred Infrastructure

- **MRT normal buffer** -- RGBA16F on GL_COLOR_ATTACHMENT1 with terrain/non-terrain alpha flag
- **Sampleable depth texture** -- converted from renderbuffer for world-position reconstruction
- **Inverse view-projection uniform** -- enables depth-to-world-position reconstruction in post-process

## Post-Processing

- **Bloom** -- threshold extraction + two-pass Gaussian blur + additive composite
- **FXAA** -- post-process anti-aliasing
- **ACES Filmic tonemapping** -- with configurable exposure and gamma correction
- **Procedural skybox** -- gradient sky with sun disc, context-aware (blue-grey in gameplay, black in menus)

## Effects (Infrastructure)

- **GPU grass** -- geometry shader emitting axis-aligned billboard quads on grass-classified terrain with wind animation and distance fadeout (toggle RAlt+5)
- **God rays** -- radial light scattering infrastructure, disabled by default (toggle RAlt+6)
- **Shoreline foam** -- water edge detection infrastructure (toggle RAlt+7)
- **SSAO** -- half-resolution 16-sample hemisphere ambient occlusion, disabled by default (toggle RAlt+9)

## Tools and Infrastructure

- **Validation mode** -- `--validate` flag for autonomous build-test iteration: auto-loads mission, renders N frames, writes telemetry JSON + screenshot, exits with status code
- **AI texture upscaling** -- Python pipeline using realesrgan-ncnn-vulkan for 4x upscaling of art and TGL textures
- **Loose file overrides** -- `data/art/`, `data/tgl/`, `data/objects/` override FST archive contents
- **Tracy profiler** -- always-on with 18 CPU+GPU zones for real-time performance analysis
- **Debug hotkeys** -- RAlt+F1-F5 and RAlt+4-9 for toggling every visual feature live
- **Shader hot-reload** -- modified shaders take effect on next frame (bad compiles silently keep old shader)
- **Wolfman mode** -- extended zoom (altitude 6000), removed LOD culling, removed fog, scaled vertex buffers

## Performance Optimizations

- **Shadow caching** -- skip re-render when camera moves <100 units (19 FPS -> 49 FPS)
- **Cached uniform locations** -- eliminated ~21 string lookups per draw call (+3-5 FPS)
- **Direct GPU shadow draw** -- eliminated CPU-side vertex readback for mech shadows (26.71% -> 3.05% of frame)
- **CPU terrain displacement** -- `terrainElevation()` matches GPU tessellation for unit placement without GPU readback

## Bug Fixes

- **CPU displacement for units** -- mechs/vehicles no longer float above tessellated terrain
- **Skybox color context** -- blue-grey sky in gameplay, black in menus/loading/mech bay
- **Decompression buffer** -- increased MAX_LZ_BUFFER_SIZE from 263KB to 8MB for upscaled textures
