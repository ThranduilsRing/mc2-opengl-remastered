<!-- doc-status: current | reviewed:2026-10-11 -->
# POSTPROCESS-COMPUTE-BLUR-1

> s68/P9: Retired GL sources are cited with Git object syntax (`8064c6930:path`) so the historical implementation remains inspectable.

Greenfield, default-OFF, **INERT** GPU compute downsample + separable-Gaussian-blur
substrate. Vulkan-prep: exercises the typed-sync + ping-pong compute pattern.
**No consumer, no visual change.** Clean-room (standard separable Gaussian — no
copied kernel/code).

This is NOT a bloom/HDR revive — that path was DELETED (`92d3a821`/`9c2187d8`/
`3e1f9e0a`). This builds fresh substrate that a future bloom/glow/DOF consumer can
adopt.

## Files

| File | Role |
|---|---|
| `8064c6930:shaders/postprocess_blur.comp` | One `.comp`, three modes via injected define: `MODE_DOWNSAMPLE` (2x2 box -> half-res), `MODE_BLUR_H`, `MODE_BLUR_V` (5-tap Gaussian). No `#version` in file — C++ prepends `#version 430` + tile/mode defines (mirrors `cluster_depth_pyramid.comp`). |
| `GameOS/gameos/gos_postprocess_blur.{h,cpp}` | Self-contained gated pass. Builds its own 3 compute programs, owns the half-res RGBA16F ping-pong textures + a controlled test pattern, dispatches downsample→blurH→blurV, typed `gpuSyncBarrier` edges between stages, one-shot CPU-vs-GPU parity. Structure mirrors `gos_cluster_depth_pyramid.cpp`. |
| `GameOS/gameos/gos_postprocess.cpp` | Frame hook (1 site in `endScene`, after `lightgrid_build::Run`), include, `Shutdown()` wiring. |
| `GameOS/gameos/CMakeLists.txt` | New TU registration. |

## Blur design

- **Downsample**: 2x2 box average, full-res input → half-res, CLAMP_TO_EDGE.
- **Separable Gaussian**: authored binomial 5-tap, sum 1.0:
  `[1/16, 4/16, 6/16, 4/16, 1/16]` = `[0.0625, 0.25, 0.375, 0.25, 0.0625]`
  (sigma ≈ 1.0). Horizontal then vertical → separable 5×5 Gaussian.
- **Ping-pong**: image A (downsample + final V output), image B (H output).
  A → (down) → A; A → (blurH) → B; B → (blurV) → A. Result in A.
- Weights in `.comp` (`W0/W1/W2`) and `.cpp` (`kW0/kW1/kW2`) are kept identical;
  the CPU reference mirrors the GLSL exactly (same weights, same CLAMP_TO_EDGE,
  H-then-V order).

## Input choice

To stay self-contained and give a deterministic CPU-vs-GPU parity, the pass owns a
**controlled test pattern** (64×64 RGBA16F: gradients + a checker + a bright dot,
i.e. high-frequency content for the blur to attenuate). The VERIFY path always
blurs this pattern — parity is independent of any scene RNG or consumer.

When the master gate is ON **and** a feedback-safe scene-color copy exists
(`MC2_VFX_SCENECOLOR_GRAB` on ⇒ `getSceneColorCopyTexture() != 0`), the live ON
path also blurs that copy as substrate — but **nothing reads the result** (no
consumer), so there is zero visual change either way.

## Sync (typed edges only — NO raw glMemoryBarrier)

| Edge | Producer → Consumer | Purpose |
|---|---|---|
| `down->blurH` | `ComputeImageWrite → TextureSample` | order downsample imageStore before blurH samples ping A |
| `blurH->blurV` | `ComputeImageWrite → TextureSample` | order H output before V samples ping B |
| `blurV->consumer` | `ComputeImageWrite → TextureSample` | registers the contract for a FUTURE consumer (no live consumer yet) |
| `image->readback` | `ComputeImageWrite → TextureReadback` | (VERIFY only) order final imageStore before CPU `glGetTexImage` |

All via `gpuSyncBarrier()` (`gos_gpu_sync.h`). `ComputeImageWrite→TextureSample`
maps to `GL_TEXTURE_FETCH_BARRIER_BIT`; `→TextureReadback` to
`GL_TEXTURE_UPDATE_BARRIER_BIT`. No `gos_gpu_sync` edits needed — both edges
already mapped.

## Gates

| Gate | Default | Effect |
|---|---|---|
| `MC2_POSTPROCESS_COMPUTE_BLUR` | OFF | master. OFF ⇒ nothing allocated/dispatched, byte-identical no-op. |
| `MC2_POSTPROCESS_COMPUTE_BLUR_VERIFY` | OFF | one-shot CPU-vs-GPU parity on the test pattern (requires master). |
| `MC2_POSTPROCESS_COMPUTE_BLUR_PLANT` | OFF | planted-error self-test of the verifier (requires VERIFY) — corrupts one CPU texel so the compare SHOULD FAIL. |

## Parity tolerance

**1e-3 absolute.** Justification: the ping-pong images are RGBA16F (half-float,
~11-bit mantissa ⇒ ~5e-4 ulp at unit magnitude), and the Gaussian accumulates in a
different float order on GPU (fma/SIMD) vs the CPU's strict fp32 left-to-right.
Both error sources are well under 1e-3 for values in [0,4]. A delta beyond 1e-3 is
a real failure, not numerical noise.

## Validation (AMD RX 7900 XTX, mc2_24, isolated install mc2-win64-0.4c)

- Build: RelWithDebInfo, `mc2` target linked clean (new TU compiled, no errors).
- Shader compiles clean (no compile/link error lines; pass dispatched).
- gate-ON + `MC2_GL_DEBUG_FATAL=1`: mc2_24 **PASS**, zero GL errors (fatal would
  abort on any error).
- gate-ON + VERIFY: **`PARITY PASS half=32x32 texels=1024 mismatches=0
  worst_delta=5.3e-4 tol=1.0e-03`**.
- PLANT self-test: **`PARITY FAIL ... mismatches=1 worst_delta=123.0`** (checker
  can fail).
- gate-OFF: mc2_24 **PASS**, pass logs `enabled=0`, nothing allocated → byte-
  identical no-op.
- No raw `glMemoryBarrier` (grep clean); typed `gpuSyncBarrier` edges only.

## Unlocks

A future bloom / glow / DOF consumer that samples the blurred half-res output
(the `blurV->consumer` typed edge is already registered for it).
