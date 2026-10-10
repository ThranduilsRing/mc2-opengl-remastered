<!-- doc-status: current | reviewed:2026-10-11 -->
# Golden-Scene Capture Manifest (GOLDEN-SCENE-MANIFEST-1)

Reproducible-visual-proof metadata for ONE capture of a golden scene. A manifest
lets a later **OFF-vs-ON** (same backend, gate toggled) or **GL-vs-future-backend**
comparison be *rigorous* instead of eyeballed: two captures are equivalent iff
their manifests agree field-by-field.

Written by `scripts/golden_scene.py`. It is a thin LAYER over existing infra —
it adds no engine code:

| Reused infra | Role |
|---|---|
| `scripts/run_smoke.py` | launches the exe, drives one mission fly-through, passes MC2_* env to the child |
| `MC2_DEBUG_STATE_DUMP=1` | engine writes `<exe_dir>/debug_state/latest_render_state.json` (frame / mission / build / features / renderPasses / renderSnapshot / **renderResources[]** registry / framePassStats) |
| `MC2_FRAME_PASS_STATS=1` | adds the `framePassStats` section (per-pass counters) to the dump |
| `MC2_SCREENSHOT_AT_FRAME=N` + `MC2_SCREENSHOT_PATH=out.tga` | engine's **existing** gated one-shot backbuffer screenshot (`gameosmain.cpp` `[SCREENSHOT v1]`): reads the offscreen scene FBO at a FIXED frame, writes a TGA. Default-OFF, zero cost when unset. **No new engine hook was needed** — the pixel hash is computed by hashing this TGA in Python. |

Both env pairs are already in `run_smoke.py`'s Popen env allowlist.

## Schema (`GOLDEN_SCENE_MANIFEST_V1`)

```jsonc
{
  "schema": "GOLDEN_SCENE_MANIFEST_V1",
  "scene": "mc2_01",                 // scene label (default = mission)
  "mission": "...",                  // mission name from the dump (falls back to --mission)
  "frame": 1234,                     // frame index the dump was written at
  "captured_frame_request": 1,       // the --frame the pixel-hash was requested at
  "backend": "GL",                   // rendering backend (future: "VK", ...)
  "build_config": "RelWithDebInfo",  // dump.build.config
  "gate_set": {                      // effective value of each recorded MC2_* gate
    "MC2_FRAMEGRAPH_EXECUTOR": null, // null = unset (engine default)
    "MC2_MATERIAL_GPU": "1",
    ...
  },
  "exe": "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe",
  "exe_md5": "…",                    // md5 of the captured mc2.exe
  "registry_hash": "…",              // 64-bit FNV-1a, see below
  "registry_resource_count": 42,
  "render_health": {                 // health/pass counters from the dump
    "renderSnapshot": { "ok": true, "spBuildFallback": 0, ... },
    "renderPasses":   { "shadow": true, "screenShadow": false },
    "frame_graph":    { ... }        // present when MC2_FRAME_PASS_STATS / executor gates active
  },
  "pass_counters": { ... },          // dump.framePassStats (per-pass draw counts)
  "pixel_hash": "…",                 // 64-bit FNV-1a of the TGA pixel bytes, or null
  "pixel_wh": [W, H],                // screenshot dimensions, or null
  "dump_source": ".../latest_render_state.json",
  "tga_source":  ".../golden_<scene>.tga",
  "generated_at_epoch": 1700000000
}
```

### `registry_hash`

64-bit FNV-1a over the **sorted** `id|kind|format|lifetime|debugName` tuples of
`dump.renderResources[]` — i.e. the registered `RenderResourceId` set plus each
owner's name and lifetime. This is the same ownership data the static gates
`check-gpu-buffer-owners.py` / `check-render-resource-ids.py` parse. Two captures
whose GPU-resource ownership graph is identical hash identically; any resource
add/drop or lifetime drift changes the hash. **Deterministic.**

### `pixel_hash`

64-bit FNV-1a over the raw uncompressed-TGA pixel bytes.

**Determinism caveat.** The smoke is a fly-through and is **not** frame-deterministic,
so the pixel_hash is captured at a **fixed early frame** (`--frame`, default 1) to
minimize camera drift. It is a *best-effort* visual fingerprint. **Pixel-exact**
stability across runs requires a fixed camera (a follow-up slice). Every *other*
field is deterministic. If two runs of the same scene differ only in `pixel_hash`,
that delta IS the fly-through noise floor — the measured input the fixed-camera
follow-up must beat.

## Usage

```bash
# capture mc2_01 on v0.4 -> <exe_dir>/debug_state/golden_manifest.json
py -3 scripts/golden_scene.py --mission mc2_01

# re-hash an existing capture without relaunching
py -3 scripts/golden_scene.py --mission mc2_01 --no-run

# gate-ON capture: set the gate in the environment first; it is recorded in gate_set
MC2_FRAMEGRAPH_EXECUTOR=1 py -3 scripts/golden_scene.py --scene mc2_01_exec_on
```

Always capture on **v0.4** (not 0.4c / 0.5.0). Diff two manifests to prove an
OFF-vs-ON or GL-vs-backend change is (or is not) visually + structurally equal.
