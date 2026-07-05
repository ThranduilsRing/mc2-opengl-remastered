# Renderer Feature Flags

All `MC2_*` environment variables that control renderer behavior. Set before launching `mc2.exe`.

```powershell
$env:MC2_SHADOW_ENABLE = "1"
.\mc2.exe
```

The **Renderer Features** ImGui panel (Ctrl+Shift+G) toggles most of these at runtime without restart. The **Env Gates** section in both the Graphics Options and Object Inspector panels shows which flags fired on startup.

The full reference with implementation notes is in `docs/tier1_env_vars.md`. This doc is the modder-facing quick-reference: grouped by use case, defaults noted, internals omitted.

---

## Visual features

| Flag | Default | What it does |
|------|---------|-------------|
| `MC2_SHADOW_ENABLE` | off | Shadow map pre-pass + PCF soft shadows on terrain and objects |
| `MC2_HDRI_SKY` | off | HDRI environment skybox; falls back to solid black |
| `MC2_RENDER_WATER_FASTPATH` | off | GPU-accelerated water path; more accurate depth integration |
| `MC2_GPU_PARTICLES` | off | GPU billboard particles (explosions, smoke, FX) |
| `MC2_DISABLE_GOSFX` | off | Kill all CPU-side gosFX particles (useful to isolate GPU particles) |
| `MC2_GPU_TRAIL_DISABLE` | off | Suppress missile trail ring buffers |

---

## GPU-driven rendering gates

These control the GPU-direct fast paths added in the 0.4 arc.

| Flag | Default | What it does |
|------|---------|-------------|
| `MC2_GPU_OBJECTS` | **on** | GPU instance batcher for static props (trees, buildings) |
| `MC2_GPU_MECHS` | **on** | GPU instance batcher for mechs and vehicles |
| `MC2_GPU_CULL_SUBSTRATE` | **on** | Per-frame GPU-side frustum cull substrate |
| `MC2_GPU_CULL` | off | Full GPU-driven cull (extends substrate; experimental) |
| `MC2_GPU_DRIVEN` | off | Master gate for GPU-driven terrain/water fast paths |
| `MC2_MATERIAL_GPU` | **on** | GPU material table for albedo lookup (PBR) |
| `MC2_MATERIAL_GPU_SAMPLE` | **on** | Shader-side sampling from GPU material table |
| `MC2_MATERIAL_KTX` | off | KTX2 sidecar texture loader (higher-quality asset path) |
| `MC2_STATIC_PROP_LEGACY_DISPATCH` | off | Kill-switch: revert static props to legacy `glMultiDrawElementsIndirect` |
| `MC2_OBJECT_ID_BUFFER` | off | Per-pixel object-ID readback buffer (required for Object Inspector) |

---

## Debug overlay and UI

| Flag | Default | What it does |
|------|---------|-------------|
| `MC2_IMGUI` | **on** | Dear ImGui overlay (Ctrl+Shift+G to open) |
| `MC2_IMGUI_INSPECTOR` | off | Object Inspector panel (click-to-pick pixels) |
| `MC2_DEBUG_RENDERER` | off | World-space debug primitive renderer (lines, boxes) |
| `MC2_GL_DEBUG` | off | GL debug message callback → stderr |
| `MC2_GL_DEBUG_FATAL` | off | Abort on `GL_DEBUG_SEVERITY_HIGH` (use in CI) |
| `MC2_RENDER_CONTRACT_ASSERT` | off | Runtime GL state assertions from `render_contract.hglsl` |

---

## Shadow debugging

| Flag | Default | What it does |
|------|---------|-------------|
| `MC2_DEBUG_SHADOW_FRUSTUM` | off | Overlay shadow frustum bounds in scene |
| `MC2_DEBUG_SHADOW_ZRANGE` | off | Log shadow Z-range per frame |
| `MC2_DEBUG_SHADOW_STATIC` | off | Freeze shadow map update (useful to inspect static shadow content) |
| `MC2_SHADOW_DIAG` | off | Shadow pass diagnostics to stderr |

---

## Terrain debugging

| Flag | Default | What it does |
|------|---------|-------------|
| `MC2_TERRAIN_INDIRECT` | off | Terrain indirect rendering path |
| `MC2_TERRAIN_SURFACE` | off | Modern terrain surface producer |
| `MC2_TERRAIN_LIGHTING_GPU` | off | GPU terrain lighting path |
| `MC2_TERRAIN_CULL_WIDE` | off | Wider frustum for terrain cull (debug: force more quads visible) |

---

## Performance instrumentation

These write counters to stderr or Tracy. Useful when profiling a specific subsystem.

| Flag | Default | What it does |
|------|---------|-------------|
| `MC2_BATCHER_FLUSH_TIMING` | off | Per-frame flush timing for static-prop batcher |
| `MC2_MECH_BATCHER_STATS` | off | Per-frame mech batcher instance counts |
| `MC2_LIGHT_COST_SPLIT` | off | Light SSBO cost breakdown |
| `MC2_TERRAIN_COST_SPLIT` | off | Terrain pass cost breakdown |
| `MC2_TOBJ_COST_SPLIT` | off | TerrainObj cost breakdown |

---

## Tracing (verbose logging)

Flags ending in `_TRACE` write per-event or per-frame messages to stderr. Enable only the specific subsystem being debugged; enabling many at once creates too much output to read.

| Flag | Subsystem |
|------|-----------|
| `MC2_STATIC_PROP_TRACE` | Static-prop admission and eviction |
| `MC2_MECH_LOD_TRACE` | Mech LOD transitions |
| `MC2_MECH_NODE_TRACE` | Mech node traversal |
| `MC2_RENDER_WORLD_TRACE` | RenderWorld object lifecycle |
| `MC2_TEX_LIFECYCLE_TRACE` | Texture load, upload, eviction |
| `MC2_LIGHTSSBO_TRACE` | Light SSBO upload |
| `MC2_WATER_DEBUG` | Water fast-path state |
| `MC2_FX_TRACE` | Particle FX system events |
| `MC2_ABL_TRACE` | ABL mission script interpreter |
| `MC2_TGL_POOL_TRACE` | TGL pool NULL slots (warn on exhaustion) |
| `MC2_SHADER_HOT_RELOAD` | Shader reload events |

---

## RenderDoc capture

| Flag | What it does |
|------|-------------|
| `MC2_RDC_CAPTURE_FRAME` | Frame number to capture (integer) |
| `MC2_RDC_CAPTURE_PATH` | Output `.rdc` file path |
| `MC2_RDC_EXIT_AFTER` | Exit after capture completes |

Example — capture frame 60 to `C:\caps\scene.rdc`:

```powershell
$env:MC2_RDC_CAPTURE_FRAME = "60"
$env:MC2_RDC_CAPTURE_PATH  = "C:\caps\scene.rdc"
$env:MC2_RDC_EXIT_AFTER    = "1"
.\mc2.exe
```

---

## Editor mode

| Flag | Default | Notes |
|------|---------|-------|
| `MC2_EDITOR_MODE` | off | Enables EditorBridge API surface. **Must be set before init.** Cannot be toggled at runtime. |
| `MC2_VSYNC` | off | V-sync control (0 = off, 1 = on) |
| `MC2_FPS_CAP` | off | Integer frame-rate cap |

---

## Flags that are always on (informational)

| Flag | What it does |
|------|-------------|
| `MC2_STATIC_PROP_REGISTRY` | GpuStaticPropRegistry (the canonical static-prop store). Cannot be disabled in release builds. |
| `MC2_GPU_CULL_SUBSTRATE` | See above — on by default, disabling it reverts to CPU-only cull. |

---

## Flag naming conventions

- `MC2_*_TRACE` — verbose per-event stderr logging
- `MC2_*_PARITY` or `MC2_*_PARITY_CHECK` — validation that two code paths produce identical results; use in debugging, never in shipping builds
- `MC2_*_SELFTEST` — automated self-test that runs once at startup and prints PASS/FAIL
- `MC2_*_LEGACY` — kill-switch reverting to the old path; intended as a temporary escape hatch

See `docs/tier1_env_vars.md` for the complete list including pre-commit invariant scripts and CI enforcement commands.
