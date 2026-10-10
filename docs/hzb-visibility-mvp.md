<!-- doc-status: current | reviewed:2026-10-11 -->
# HZB Visibility Substrate (MVP)

`TRACKRV-HZB-VISIBILITY-OPUS-1`. A debuggable reverse-Z Hi-Z (HZB) depth pyramid
plus a diagnostic occlusion probe. **No culling is performed.** Nothing in this
arc suppresses, filters, or reorders any draw. It exists to prove the depth
pyramid and the conservative cull math are correct *before* any real culling is
attempted.

Companion: `docs/hzb-depth-convention.md` (the locked depth/reduction contract)
and `tests/unit/test_depth_hzb.cpp` (the GL-free reference math).

## Gates (all default OFF)

| Env var          | Effect                                                              | Requires        |
|------------------|--------------------------------------------------------------------|-----------------|
| `MC2_HZB_BUILD`  | Allocate + build the HZB pyramid each frame (`runHzbReduce`).       | —               |
| `MC2_HZB_PROBE`  | Run the diagnostic occlusion probe (`runHzbProbe`).                 | `MC2_HZB_BUILD` |
| debug preview    | HZB section in the ImGui GBuffer-preview panel.                     | `MC2_HZB_BUILD` + `MC2_IMGUI` |

Default-OFF means byte-identical: with `MC2_HZB_BUILD` unset the textures/FBO are
never created and both passes are no-ops. Registered in
`RenderCore/RendererFeatureRegistry.h` (`env_registry` contract).

## Depth convention (summary)

Reverse-Z, `GL_ZERO_TO_ONE`: near = 1.0, far/sky = 0.0, **larger depth = closer**.
The HZB parent stores the **MIN** of its children = the farthest occluder =
conservative (never cull a visible object). Mip ladder rounds **up** (ceil) so no
odd-extent texel is dropped. See `docs/hzb-depth-convention.md` for the full
derivation and why MAX (the forward-Z choice) would cause invisible-object bugs.

## Implementation notes (load-bearing)

- **Source:** the `MainDepth` registry resource (full-res `GL_DEPTH24_STENCIL8`
  scene depth), registered by `gos_postprocess` after the scene FBO is built.
- **Reduction:** a custom fragment MIN pass (`shaders/hzb_reduce.frag`), **not**
  `glGenerateMipmap` (which averages *and* floor-sizes — both wrong).
- **Storage = one ceil-sized `GL_R32F` texture PER level, NOT a mip chain.**
  A single mipped texture cannot be used: AMD rejects attaching mip level > 0 of
  a *mipmap-incomplete* texture, and the ceil ladder is deliberately
  mipmap-incomplete (`glTexStorage2D` also caps levels at `floor(log2)+1` and
  forces floor sizes). Separate per-level textures sidestep this and remove any
  read/write feedback (source and destination are distinct objects). This is the
  single biggest integration surprise of the arc.
- **Built in `endScene` before any post pass**, so it captures the resolved
  scene depth.

## Debug preview

Run with `MC2_HZB_BUILD=1 MC2_IMGUI=1`. Open the Graphics Options window →
GBuffer preview → **HZB Pyramid** section: shows dimensions, mip count, build
count, a mip selector, and an `ImGui::Image` of the selected level. R32F shows in
the red channel — reverse-Z near = bright, far/sky = black; coarser levels store
the MIN (farthest occluder), so they get progressively darker where far geometry
dominates a tile.

## Occlusion probe interpretation

Run with `MC2_HZB_BUILD=1 MC2_HZB_PROBE=1`. Each frame (logged for the first 3
frames, then every 600, plus *any* frame with a nonzero anomaly) emits:

```
[HZB_PROBE v1] parentL=4(50x38) childL=3(100x75) tested=7600 wouldKeep=7600 \
    wouldCull=0 integrityMismatch=0 invalidDepth=0 neverAppliedToDraws=1
```

- `integrityMismatch` — parent texels where GPU value != CPU MIN of the exact
  children the shader sampled. **Must be 0.** Nonzero = the GPU reduction is not a
  faithful MIN (a real conservativeness bug).
- `wouldCull` — self-points (a child depth) that the conservative cull comparison
  `childDepth < parentMin` would reject. **Must be 0**: a texel inside a tile can
  never be farther than that tile's MIN. Nonzero = non-conservative HZB.
- `wouldKeep` — self-points kept (expected = `tested`).
- `invalidDepth` — NaN/Inf depths (should be 0).
- `neverAppliedToDraws` — always 1; the probe is read-only.

The probe replicates the shader footprint exactly:
`floor(((px+0.5)/pw)*cw ± 0.5)` (NEAREST + clamp). On **odd** levels the 2×2
windows overlap at the boundary — a naive `px*2`/`cx/2` mapping double-assigns
boundary texels and reports false anomalies; do not "simplify" it back.

## Validation (suggested missions)

GL-free math + contracts (fast):

```bash
cmake --build build64-tests --config RelWithDebInfo --target mc2_tests
build64-tests/RelWithDebInfo/mc2_tests.exe "--tc=*HZB*,*DepthConvention*"
sh scripts/check-contracts.sh
```

Runtime (deploy fresh exe + shaders first; never `--kill-existing`):

```bash
# gate OFF (parity), then BUILD, then BUILD+PROBE
py -3 scripts/run_smoke.py --mission mc2_03            # default: no HZB lines
MC2_HZB_BUILD=1               ... --mission mc2_03      # builds, 0 FBO-incomplete
MC2_HZB_BUILD=1 MC2_HZB_PROBE=1 ... --mission mc2_03    # probe all-zero anomalies
```

Suggested coverage: `mc2_03` (terrain/salvage), `mc2_17` (combined-arms),
`mc2_24` (mech-heavy). Confirm: gate-OFF unchanged; gate-ON no GL errors, no
missing objects, `+0` destroys; probe `wouldCull=0 integrityMismatch=0`.

## STOP conditions before real culling

Real culling is a **separate, later** arc (`HZB-STATICPROP-CULL-RECON-1` →
cull). Do **not** wire any draw suppression until **all** of the following hold:

1. The probe runs across `mc2_03/17/24` with `wouldCull=0` and
   `integrityMismatch=0` for the **self-consistency** test (done here) **and**
   for **real object bounds** (next slice) over a sustained soak.
2. Object screen-rect projection uses the GL-NDC reverse-Z matrix consistent with
   the depth buffer (the same transform that produced `MainDepth`), validated
   against known on-screen/off-screen objects.
3. A documented false-occlusion budget is measured and shown to be ~0 (a single
   wrongly-culled visible object is a worse regression than no culling).
4. There is a kill-switch and the cull consumer is itself default-OFF.

Until then: **build + observe only. No object is ever hidden by this code.**
