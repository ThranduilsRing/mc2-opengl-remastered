<!-- doc-status: current | reviewed:2026-10-29 -->
# Renderer Feature Flags

A selection of `MC2_*` renderer controls. Set before launching `mc2.exe`.
The renderer is Vulkan-only. Defaults below follow [the gate table](../../Gates/gate_table.json);
[retired names](../../Gates/retired_names.json) are not supported switches.

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
| `MC2_SHADOW_ENABLE` | **on** | Vulkan shadow maps on terrain and objects |
| `MC2_HDRI_SKY` | **on** | HDRI environment skybox |
| `MC2_RENDER_WATER_FASTPATH` | off | GPU-accelerated water path; more accurate depth integration |
| `MC2_GPU_PARTICLES` | **on** | Native Card/CardCloud effects; `=0` suppresses these draws; CPU routes are retired |
| `MC2_DISABLE_GOSFX` | off | Suppress gosFX effects for diagnosis |
| `MC2_GPU_TRAIL_DISABLE` | off | Suppress missile trail ring buffers |

---

## GPU-driven rendering gates

These control the GPU-direct fast paths added in the 0.4 arc.

| Flag | Default | What it does |
|------|---------|-------------|
| `MC2_GPU_CULL_SUBSTRATE` | **on** | Per-frame GPU-side frustum cull substrate |
| `MC2_GPU_CULL` | **on** | GPU-driven culling |
| `MC2_GPU_DRIVEN` | **on** | GPU-driven terrain/water paths |
| `MC2_MATERIAL_GPU` | **on** | GPU material table for albedo lookup (PBR) |
| `MC2_MATERIAL_GPU_SAMPLE` | **on** | Shader-side sampling from GPU material table |
| `MC2_OBJECT_ID_BUFFER` | **on** | Per-pixel object-ID readback buffer (required for Object Inspector) |

---

## Debug overlay and UI

| Flag | Default | What it does |
|------|---------|-------------|
| `MC2_IMGUI` | **on** | Dear ImGui overlay (Ctrl+Shift+G to open) |
| `MC2_IMGUI_INSPECTOR` | **on** | Object Inspector support (click-to-pick pixels) |

---

## Shadow debugging

The former `MC2_DEBUG_SHADOW_FRUSTUM`, `MC2_DEBUG_SHADOW_ZRANGE` and
`MC2_DEBUG_SHADOW_STATIC` switches are retired. Use current shadow diagnostics
from the gate table rather than historical GL commands.

---

## Terrain debugging

| Flag | Default | What it does |
|------|---------|-------------|
| `MC2_TERRAIN_INDIRECT` | **on** | Terrain indirect rendering path |
| `MC2_TERRAIN_LIGHTING_GPU` | **on** | GPU terrain lighting path |
| `MC2_TERRAIN_CULL_WIDE` | off | Wider frustum for terrain cull (debug: force more quads visible) |

---

## Performance instrumentation

These write counters to stderr or Tracy. Useful when profiling a specific subsystem.

| Flag | Default | What it does |
|------|---------|-------------|
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
| `MC2_TEX_LIFECYCLE_TRACE` | Texture load, upload, eviction |
| `MC2_WATER_DEBUG` | Water fast-path state |
| `MC2_FX_TRACE` | Particle FX system events |
| `MC2_ABL_TRACE` | ABL mission script interpreter |

---

## RenderDoc capture

| Flag | What it does |
|------|-------------|
| `MC2_RDC_CAPTURE_FRAME` | Frame number to capture (integer) |
| `MC2_RDC_CAPTURE_PATH` | Output `.rdc` file path |
| `MC2_RDC_EXIT_AFTER` | Exit after capture completes |

Example — capture frame 60 to `E:\mc2-archive-2026-10\proofs\scene.rdc`:

```powershell
$env:MC2_RDC_CAPTURE_FRAME = "60"
$env:MC2_RDC_CAPTURE_PATH  = "E:\mc2-archive-2026-10\proofs\scene.rdc"
$env:MC2_RDC_EXIT_AFTER    = "1"
.\mc2.exe
```

---

## Editor mode

| Flag | Default | Notes |
|------|---------|-------|
| `MC2_EDITOR_MODE` | off | Enables EditorBridge API surface. **Must be set before init.** Cannot be toggled at runtime. |
| `MC2_VSYNC` | off | V-sync control (0 = off, 1 = on) |
| `MC2_FPS_CAP` | 90 | Integer frame-rate cap |

---

## Renderer compatibility

Historical GL shader files and `MC2_SHADER_HOT_RELOAD` are not a supported
mod shader-override interface. Preserve the deployed Vulkan shader payload;
shader replacement needs the engine's native build/package validation.
Native GLSL/SPIR-V assets and ABI fixtures remain required dependencies.

Cliff-wall mesh decal fill/colour blend was accepted for deletion (parity row 18);
custom MarbleCliff/decal content needs explicit compatibility review. Row 29's
blockier beam-head shape is an accepted residual after the missing-head/halo fixes.
See the [retirement audit](../plans/2026-10-29-gl-removal-audit.md) for evidence
and outstanding per-mod visual/save-load checks.

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
