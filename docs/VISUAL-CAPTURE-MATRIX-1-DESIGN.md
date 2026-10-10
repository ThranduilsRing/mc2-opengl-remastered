<!-- doc-status: current | reviewed:2026-10-11 -->
# VISUAL-CAPTURE-MATRIX-1 Design

## Purpose

A canonical capture is a reproducible render sample produced by the tuple:

    (mission, preset, gate-env, warmup-frames, seed) -> pixel output + JSON state

Fixing all five inputs guarantees that two captures taken at different times from
the same binary are byte-comparable at the pixel level (modulo OS compositor
non-determinism -- see Known Limitations).  The capture framework automates this
tuple so that gate changes, shader patches, and material system migrations can be
compared against a stored baseline without manual camera work.

## Three Capture Paths

| Path | Script | Purpose |
|------|--------|---------|
| Smoke gate | `scripts/run_smoke.py` | Per-mission exit/crash/ok gate; fast (<30s/mission); no pixel output |
| Baseline capture | `scripts/run_smoke.py --baseline-update` | Record a new pixel + JSON baseline for a mission+preset combo |
| Matrix runner | `scripts/run_smoke_matrix.py` | Run a named set of env variants against smoke gate in sequence |

Each path invokes the same underlying `RunConfig` / `run_one` machinery in
`scripts/smoke_lib/runner.py`.  The matrix runner is a thin wrapper that
iterates a JSON file and sets env overrides before each `run_smoke.py` call.

## Canonical Tier-1 Missions

Five missions selected to cover distinct biome + asset density combinations:

| Mission | Biome profile | Key coverage |
|---------|--------------|-------------|
| `mc2_01` | Grassland / open terrain | Terrain normals, water shoreline, sparse props |
| `mc2_03` | Salvage / rocky outcrop | Rock splat layers, diffuse-only sky, dry bed water |
| `mc2_10` | Urban / dense props | Static-prop density, VFX combat smoke, gosFX |
| `mc2_17` | Mixed biomes | Splat transition boundaries, terrain lighting variance |
| `mc2_24` | Steep urban + terrain | Terrain height delta, shadow cascade stress, fog |

Run all five with `--tier tier1`.  Inner-loop dev may use a 2-mission subset
(`mc2_01` + `mc2_24`) then promote to tier1 before marking a slice complete.

## Env-Selectable Debug Modes

These `MC2_*` vars are all in the `run_smoke.py` subprocess allowlist and can be
set in the parent shell or in a matrix JSON `env` block:

**Terrain**
- `MC2_TERRAIN_DEBUG_MODE` -- integer debug visualizer (10 = height-derived normals as RGB)
- `MC2_TERRAIN_LIGHTING_GPU` -- GPU terrain lighting compute gate (0/1)

**Static props**
- `MC2_STATIC_PROP_DEBUG_MATERIAL` -- material debug view (1=albedo, 2=materialIdx, 3=normal, 4=texArrayLayer, 5=roughness, 6=metallic)
- `MC2_STATIC_PROP_IBL_SH` -- SH-L2 image-based ambient (0=off, 1=on)
- `MC2_STATIC_PROP_PBR_V1` -- Schlick-Fresnel specular gate (0/1; requires `MC2_VIEW_UNIFORMS=1`)

**Water**
- `MC2_GPU_DRIVEN_WATER` -- GPU-driven water path (0=legacy, 1=GPU-driven)
- `MC2_RENDER_WATER_FASTPATH` -- water fast-path gate
- `MC2_WATER_DEBUG` -- water debug trace (stdout)
- `MC2_WATER_RENDERPROBE` / `MC2_WATER_DEPTHPROBE` -- reverse-Z water parity probes

**VFX**
- `MC2_GPU_PARTICLES` -- GPU particle batcher gate (0=legacy gosFX only, 1=GPU bridge)
- `MC2_DISABLE_GOSFX` -- gosFX CPU sim gate (0=legacy on, 1=legacy off; gosFX on when unset, since A2)
- `MC2_FX_TRACE` -- neutral FX invocation counter (default-OFF)

**Shadows**
- `MC2_SHADOW_ENABLE` -- dynamic sun-shadow enable
- `MC2_SHADOW_DYNAMIC_PROP_CASTERS` -- registry-fed dynamic caster pass

**Diagnostics**
- `MC2_DEBUG_STATE_DUMP` -- JSON render-state snapshots (writes `debug_state/latest_render_state.json`)
- `MC2_RENDER_CONTRACT_ASSERT` -- runtime GL state validation against render_contract expectations

## Matrix Files Index

All matrices live in `tests/smoke/matrices/`.

| File | `matrix_id` | Entries | Description |
|------|------------|---------|-------------|
| `staticprop.json` | `staticprop` | 4 | IBL SH, PBR specular, material GPU kill-switch |
| `terrain.json` | `terrain` | 3 | Normals-from-height, lighting V1+V2, height debug view |
| `water.json` | `water` | 3 | GPU-driven path on/off, legacy path regression |
| `vfx.json` | `vfx` | 2 | GPU particle batcher on/off |

## Running a Single Matrix Entry

Dry-run to verify commands before executing:

```powershell
py -3 scripts/run_smoke_matrix.py water --dry-run
```

Run a specific entry only:

```powershell
py -3 scripts/run_smoke_matrix.py water --entry gpu_driven_off
```

Run all entries in a matrix:

```powershell
py -3 scripts/run_smoke_matrix.py vfx --duration 30
```

The full tier-1 smoke gate (used as the pre-commit regression gate):

```powershell
py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Artifacts land in `tests/smoke/artifacts/<timestamp>/`.

## Known Limitations

**OS-level screenshot non-determinism.**  Captures taken via OS desktop-grab
(e.g. `PIL.ImageGrab`) are subject to compositor state, DWM compositing delay,
and monitor scaling.  Two runs from an identical binary may produce 1-3 LSB
variation in pixels not covered by alpha.  The current baselines tolerate a
small per-channel epsilon; hard pixel-exact comparison is not enforced at tier1.

**`glReadPixels` deferred.**  The planned pixel-exact path reads the framebuffer
via `glReadPixels` inside the engine process (no OS compositor in the path).
This is tracked as a future slice; the current capture baseline relies on
post-process color values sampled from the debug JSON state dump, not raw pixels.
Baseline stability improves once the `glReadPixels` path lands.
