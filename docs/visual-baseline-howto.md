# Visual Baseline How-To (V-BASELINE-0)

This doc explains how to capture and compare visual baselines for the
StaticPropOpaque rendering lane. The goal is **objective** before/after
judgement — no eyeballing across days.

## Concepts

- **Camera preset** — a named tuple `(mission, warmup_s, smoke_seed)` that
  yields a deterministic frame when launched via `mc2.exe --profile stock
  --mission <m> --duration <d>` with `MC2_SMOKE_MODE=1` +
  `MC2_SMOKE_SEED=0xC0FFEE`. No in-engine camera-preset hook exists yet; the
  current preset identity *is* the deterministic state mc2 lands in. The
  registry lives in `tests/visual/baselines/presets.json`.
- **Baseline** — a `(PNG, JSON)` pair captured at a specific commit. PNG is
  the rendered (well, OS-screenshot of the) frame. JSON is the sidecar
  metadata: commit SHA, mission, preset name, resolution, env flags,
  PNG sha256.
- **Reference set** — the committed baselines under
  `tests/visual/baselines/` representing the visual state at the commit
  named in each sidecar's `commit` field.

## Capturing baselines

```
cd A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev

# Capture every preset
py -3 scripts/capture_baseline.py

# Capture a single preset
py -3 scripts/capture_baseline.py --preset staticprop_baseline_01

# Capture-and-self-verify: re-runs each preset twice and reports whether
# the two PNGs are byte-identical (see "Reproducibility" below).
py -3 scripts/capture_baseline.py --verify

# TERRAIN-BASELINE-0: capture a terrain-heavy preset with a fragment debug
# mode. Filename gets a _tdmN suffix so multiple modes coexist on disk.
py -3 scripts/capture_baseline.py --preset terrain_grass_01
py -3 scripts/capture_baseline.py --preset terrain_grass_01 --terrain-debug-mode 2
```

Outputs land in `tests/visual/baselines/<preset>_<commit-short>.{png,json,log}`.
Captures with `--terrain-debug-mode N` land at
`<preset>_<sha>_<strengthtag>_tdmN.png`.

### Terrain-heavy presets

- `terrain_grass_01` (mc2_01) — grass + dirt + rock material classification + POM.
- `terrain_salvage_03` (mc2_03) — heavier rock + cement-overlay coverage.
- `terrain_combined_17` (mc2_17) — mixed biomes + slope + shadow PCF.

Available terrain debug modes (mirror of
`GuiRuntime/GraphicsOptionsWindow.cpp` `kTerrainModes`): `-1` tess-alive
probe, `0` OFF (default — byte-identical final frame), `1` depth-comparison,
`2` raw colormap, `3` blurred colormap, `4` material weights (R=rock G=grass
B=dirt), `5` normal lighting, `6` shadow factor, `7` cloud shadow, `8`
cement diag, `9` thin-record diag. Diagnostic-only; never alters the
default mode-0 final rendering.

### VFX-heavy presets (VFX-BASELINE-0)

```
# Default (mode 0 — byte-identical) capture of a combat scene
py -3 scripts/capture_baseline.py --preset vfx_combat_10

# Particle debug-view capture (filename gets a _vdmN suffix)
py -3 scripts/capture_baseline.py --preset vfx_combat_10 --vfx-debug-mode 4
```

- `vfx_combat_10` (mc2_10) — primary VFX/particle preset; active weapon fire.
- `vfx_combat_24` (mc2_24) — dense urban combat; complementary effect roster.

Available particle debug modes (mirror of `particle_billboard.frag`
`u_debugMode` / `MC2_VFX_DEBUG_MODE`): `0` Final (default — byte-identical
final frame), `1` Albedo (raw atlas texel), `2` Alpha (final alpha
grayscale), `3` ParticleKind (hashed color per kind), `4` Overdraw (additive
proxy). Diagnostic-only; never alters the default mode-0 final rendering.
`MC2_GPU_PARTICLES` defaults OFF, so particles only draw when it is set to 1.

**Transience caveat (important).** Unlike terrain/static-prop/mech, VFX is
*not* present in every frame — particles are combat-only and appear
opportunistically as the AI engages under the passive smoke seed. The
captured frame at `warmup_s=28` **may or may not** contain on-screen
particles. The capture sidecar records `vfxDebugMode` + a `vfxNote`, but the
**authoritative proof of particle activity is the capture `.log`**: grep for
`GOSFX_GPU` `enabled=1 sprites=N` and `TRAIL_PROBE`. Treat a VFX baseline PNG
as a *sample*, not a guaranteed particle-populated reference. Staging a
deterministic particle-dense frame would need a future scripted-fire / camera
hook — and **gameplay, emission, and lifetime must not be altered** to force
captures (hard constraint of the VFX lane).

## Comparing before/after

Workflow for evaluating a candidate change:

1. **Capture reference** at the baseline commit (or use the already-committed
   reference set).
2. Apply the candidate change. Build + deploy as usual (see
   `.claude/skills/mc2-build.md` + `.claude/skills/mc2-deploy.md`).
3. **Capture candidate** at the new HEAD:
   ```
   py -3 scripts/capture_baseline.py
   ```
4. **Diff**. For each preset, you now have:
   - `<preset>_<ref-sha>.png`  (reference)
   - `<preset>_<new-sha>.png`  (candidate)

   Three diff techniques in order of rigor:

   - **Side-by-side eyeball** (fast, low rigor). Open both PNGs.
   - **ImageMagick pixel diff** (medium rigor):
     ```
     magick compare -metric AE \
       tests/visual/baselines/staticprop_baseline_01_<ref>.png \
       tests/visual/baselines/staticprop_baseline_01_<new>.png \
       diff.png
     ```
     Prints the count of differing pixels and writes a red-on-grey diff
     image. Threshold to taste; "zero" is achievable only in the
     deterministic-engine case below.
   - **Sidecar JSON diff** to see which env flags changed between the two
     captures:
     ```
     diff <(jq .flags tests/visual/baselines/<preset>_<ref>.json) \
          <(jq .flags tests/visual/baselines/<preset>_<new>.json)
     ```

## Reproducibility

Same commit + same preset + same env should produce a frame that, *as
rendered by the engine*, is byte-identical thanks to `MC2_SMOKE_SEED` (see
`GameOS/gameos/gos_smoke.cpp`).

But the captured PNG goes through an **OS-level screenshot path** (pyautogui
→ Win32 desktop capture), which introduces non-determinism that is
*outside* the engine:

- Cursor position (we park to screen center, but the cursor pixel is still
  in the image)
- Taskbar clock and any other desktop chrome behind the mc2 window
- Window-manager async paint timing on the first frame after foreground
- Multi-monitor / DPI scaling deltas across machines

Therefore: **engine-output equivalence is provable; PNG byte-identical
equivalence is not, on this capture path.** This is acceptable for the
V-BASELINE-0 charter: humans compare via PNG diff, and the sidecar's
`png_sha256` + flags catalog are the audit trail.

If true byte-identical capture is needed later (e.g. for CI), a future slice
should add an in-engine `glReadPixels` capture hook gated by an env var like
`MC2_FRAMEBUFFER_CAPTURE_PATH`. That is **out of scope for V-BASELINE-0**
per the brief's hard constraints (no GL state changes, no draw-path edits).

The `--verify` flag on `capture_baseline.py` exists to make this
non-determinism *visible* — if you see "identical=NO" with the two sha256s
differing, that is the OS-screenshot delta, not an engine regression.

## What the harness deliberately does NOT do

These are out of scope for V-BASELINE-0 and would belong to a follow-up
slice if needed:

- **In-engine framebuffer capture.** Would require a new env-gated
  `glReadPixels` path in the renderer.
- **In-engine camera preset positioning.** Would require a new env-gated
  camera init hook (`MC2_CAMERA_PRESET=...`). The current "preset" is
  defined entirely by mission + seed + warmup; if those produce a useful
  framing, that becomes the preset.
- **Automated pass/fail thresholding.** We emit data; humans (or a future
  reviewer slice) judge.
- **Cross-machine reproducibility.** GPU driver/version, monitor DPI, and
  desktop chrome vary; baselines are valid on the capture machine.

## Adding a new preset

Edit `tests/visual/baselines/presets.json` and add a new entry under
`presets`. Then capture and commit the resulting PNG+JSON. See
`tests/visual/baselines/README.md` for the registry schema.

## Files

- `scripts/capture_baseline.py` — capture harness
- `tests/visual/baselines/presets.json` — preset registry
- `tests/visual/baselines/README.md` — corpus naming + schema reference
- `tests/visual/baselines/*.png` + `*.json` — the reference set
