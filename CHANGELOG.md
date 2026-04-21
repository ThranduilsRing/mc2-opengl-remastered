# Changelog

All rendering features added to the MechCommander 2 OpenGL port, grouped by category.

## Terrain Rendering

- **PBR splatting** -- multi-material terrain with per-type albedo, normal maps, roughness, and tint colors
- **Parallax occlusion mapping** -- depth displacement on terrain materials (strongest on dirt)
- **Hardware tessellation** -- TCS/TES pipeline for smooth terrain geometry with distance-based LOD
- **Triplanar cliff mapping** -- automatic rock texture on steep slopes based on surface normal
- **Cloud shadows** -- animated FBM noise pattern moving across terrain
- **Height-based exponential fog** -- off by default; may need tuning before re-enabling
- **Anti-tiling** -- noise-based UV perturbation to break texture repetition at distance

## Lighting and Shadows

- **Static terrain shadow map** -- 8192x8192 world-fixed orthographic projection, rendered once on the first frame (not dynamically re-baked)
- **Dynamic mech shadows** -- 4096x4096 with ray-ground intersection frustum centering and camera bias
- **Poisson disk PCF** -- 16-sample stratified sampling with per-pixel rotation to break banding, adjustable softness via `[`/`]` keys
- **Post-process shadow pass** -- fullscreen depth-reconstruction pass that shadows all geometry (terrain, overlays, buildings, mechs) via multiplicative blending
- **Object shadow casting** -- mechs and buildings cast into shadow map via direct GPU draw (bypasses material system)

## G-Buffer and Deferred Infrastructure

- **MRT normal buffer** -- RGBA16F on GL_COLOR_ATTACHMENT1 with terrain/non-terrain alpha flag
- **Sampleable depth texture** -- converted from renderbuffer for world-position reconstruction
- **Inverse view-projection uniform** -- enables depth-to-world-position reconstruction in post-process

## Post-Processing (infrastructure)

Post-process pipeline is built and running; most effects are **off by default**. The goal is to have the plumbing in place so effects can be tuned in later without re-wiring the renderer.

- **Procedural skybox** -- gradient sky with sun disc, context-aware (blue-grey in gameplay, black in menus) -- **on by default**
- **Bloom** (off) -- threshold extraction + two-pass Gaussian blur + additive composite
- **FXAA** (off) -- post-process anti-aliasing
- **ACES Filmic tonemapping** (off) -- configurable exposure and gamma correction

## Effects (Infrastructure)

- **God rays** -- radial light scattering infrastructure, disabled by default (toggle RAlt+6)
- **Shoreline foam** -- water edge detection infrastructure, currently non-functional (toggle RAlt+7)
- **SSAO** -- half-resolution 16-sample hemisphere ambient occlusion, disabled by default (toggle RAlt+9)
- **GPU grass** -- deprecated; geometry-shader grass billboard path removed from default build

## Tools and Infrastructure

- **Validation mode** -- `--validate` flag for autonomous build-test iteration: auto-loads mission, renders N frames, writes telemetry JSON + screenshot, exits with status code
- **AI texture upscaling** -- Python pipeline using realesrgan-ncnn-vulkan for 4x upscaling of art and TGL textures
- **Loose file overrides** -- `data/art/`, `data/tgl/`, `data/objects/` override FST archive contents
- **Tracy profiler** -- always-on with 18 CPU+GPU zones for real-time performance analysis
- **Debug hotkeys** -- RAlt+F1-F5 and RAlt+4-9 for toggling every visual feature live
- **RAlt+0 GPU static-prop killswitch** -- experimental GPU-driven prop rendering (buildings, trees, generics). Partially wired: enabling it currently acts as a "hide all static props" toggle, useful for screenshotting terrain without clutter. CPU path (default, killswitch off) is the supported rendering path. See `docs/gpu-static-prop-cull-lessons.md` for why the GPU path is incomplete.
- **RAlt+8 surface debug mode** -- visualize terrain surface classification / material IDs
- **RAlt+9 GPU frag debug-mode cycle** -- when the static-prop killswitch is on, cycles the fragment shader through 8 isolation modes (normal / addr-gradient / addr-hash / WHITE / ARGB-only / TEX-only / HIGHLIGHT-only / TEX+HIGHLIGHT) for per-component visual bisection. Replaces the old SSAO toggle hotkey; SSAO infrastructure is preserved in code but unbound.
- **Shader hot-reload** -- modified shaders take effect on next frame (bad compiles silently keep old shader)
- **Extra zoom mode** -- pulled-back camera (altitude 6000), removed LOD culling, removed fog, scaled vertex buffers

## Performance Optimizations

- **Shadow caching** -- skip re-render when camera moves <100 units (19 FPS -> 49 FPS)
- **Cached uniform locations** -- eliminated ~21 string lookups per draw call (+3-5 FPS)
- **Direct GPU shadow draw** -- eliminated CPU-side vertex readback for mech shadows (26.71% -> 3.05% of frame)
- **CPU terrain displacement** -- `terrainElevation()` matches GPU tessellation for unit placement without GPU readback

## Bug Fixes

- **Removed per-frame 10ms sleep** -- was a hardcoded `Sleep(10)` in the frame loop (not vsync, not adaptive pacing); removing it uncapped framerate
- **CPU displacement for units** -- mechs/vehicles no longer float above tessellated terrain
- **Skybox color context** -- blue-grey sky in gameplay, black in menus/loading/mech bay
- **Decompression buffer** -- increased MAX_LZ_BUFFER_SIZE from 263KB to 8MB for upscaled textures
- **Color flickering** -- resolved intermittent frame-to-frame color shifts
