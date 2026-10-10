<!-- doc-status: current | reviewed:2026-10-11 -->
# VFX-DISTORTION-1 — heat-haze refraction (procedural, clean-room)

Status: **BUILT** (gate default OFF).

## What it does

A dedicated ALPHA billboard particle group whose fragment shader **refracts** the
pre-VFX scene-color grab (`sceneColorCopyTex_`, RGBA16F, from VFX-SCENECOLOR-GRAB-1,
commit `642a72ff`) at a procedurally-wobbled screen UV, soft-clipped by scene depth
so the haze does not bleed over near geometry. Reads as a shimmering heat haze
behind the effect (mech exhaust / fire / impact thermal bloom feel).

Clean-room: the offset is a hand-authored sine base + in-house 2-octave value-noise
fbm (`vd_hash`/`vd_valnoise`/`vd_fbm` in `particle_billboard.frag`). **NO imported
noise/normal/LUT art, no copied shader code.** BattleTech `VFX_Distortion` / MW5 heat
are technique reference only.

## Design (frozen, settled by VFX-DISTORTION-RECON-1)

- **Refraction REPLACES dst** — it does NOT compose additively. Reuses the existing
  ALPHA pipeline `VfxBillboardAlpha` (SRC_ALPHA / ONE_MINUS_SRC_ALPHA, depth-write
  off). **No new blend mode, no new pipeline row.** The distortion fragment outputs
  the warped scene color as `rgb` with a moderate alpha (= card coverage × soft-fade),
  so the existing alpha blend cross-fades dst toward the refracted sample.
- Distortion is an **alpha group** (`blendMode==0`) carrying a per-flush distortion
  flag (`u_vfxDistort`), set by the bridge — not a GroupInfo schema field. No content
  effect→distortion mapping (that is a follow-up; OUT of scope).
- Screen-UV reuses the existing `vec2 suv = gl_FragCoord.xy / u_screenSize;`;
  `u_screenSize` is already uploaded for soft particles.
- Soft-clip reuses the alpha-only `u_sceneDepth` / `u_softDistance` machinery to scale
  replace strength (no-bleed over near geometry).
- `u_time` (new) is uploaded **once per flush** from a monotonic seconds accumulator
  (`gos_GetElapsedTime(1)` summed into a file-static), not per group.
- The scene-color grab is bound on **tex unit 2** (depth grab is on unit 1), with
  save/restore mirroring the unit-1 restore (avoids the tex-unit-leak class).

## Gates

| Gate | Default | Effect |
|---|---|---|
| `MC2_VFX_DISTORTION` | OFF | Master enable. OFF → FS `u_vfxDistort==0` branch dead → byte-identical. |
| `MC2_VFX_SCENECOLOR_GRAB` | OFF | **Required.** Produces `sceneColorCopyTex_`. If unset while distortion is ON, the bridge forces `u_vfxDistort=0` (no GL-0 sampler bind) → inert. |
| `MC2_VFX_DISTORT_FIXTURE` | OFF | FIXTURE: tags **every alpha group** (`blendMode==0`) as distortion so the effect is observable on stock smoke/dust. With it OFF, no content tags a distortion group yet → byte-identical even gate-ON. |
| `MC2_VFX_DISTORT_AMP` | 0.04 | Optional screen-UV wobble amplitude override (clamped 0..0.5). |

## Files changed

- `shaders/particle_billboard.frag` — `u_sceneColor` / `u_vfxDistort` / `u_time` /
  `u_distortAmp` uniforms; in-house value-noise fbm; the distortion branch (early
  return, guarded `u_vfxDistort==1 && u_debugMode==0`).
- `GameOS/gameos/gos_particle_bridge.cpp` — gate + fixture init helper; uniform
  locations; unit-2 grab bind (save/restore); `u_time` per-flush upload;
  per-group `u_vfxDistort` set for the distortion alpha group only.

## Ledger

```
VFX_DISTORTION:
  status: BUILT
  gate_default: OFF
  requires: MC2_VFX_SCENECOLOR_GRAB
  gate_off: BYTE_IDENTICAL
  blend: ALPHA_reuse_VfxBillboardAlpha
  offset: PROCEDURAL_NO_ART
  visual_status: documented-untriggered-by-deterministic-oracle
```

## Validation results (2026-06-23, AMD RX 7900 XTX, deploy mc2-win64-0.4c)

| Gate | Result |
|---|---|
| Build RelWithDebInfo + relink | OK (mc2.exe produced; only benign LNK4199 delay-load warnings) |
| Shader compile | CLEAN (no `[SHADER WARN]`, no link fail in log) |
| Gate-OFF mc2_24 | PASS (byte-identical; `u_vfxDistort==0` branch dead) |
| Grab-absent safety (`MC2_VFX_DISTORTION=1`, grab UNSET, GL-fatal) | PASS — inert, no GL-0 bind, no crash |
| Gate-ON + grab + `MC2_GL_DEBUG_FATAL=1` mc2_24 | PASS — zero GL errors (unit-2 bind + sampler + feedback-safe) |
| Distortion + `MC2_VFX_SOFT_PARTICLES` + GL-fatal | PASS — soft-clip scales replace strength, GL-clean |
| `scripts/check-vfx-blend-distinction.py` | PASS |

**Visual confirm: documented-untriggered (deterministic oracle).** The fixture path
(`MC2_VFX_DISTORT_FIXTURE` + grab + distortion, and `MC2_FX_FORCE_SPAWN` for weapon
VFX) fires and runs GL-clean under debug-fatal — the distortion alpha group executes.
But the headless smoke harness runs minimized with no pixel oracle for the
NON-DETERMINISTIC fixture, so the visible "background wobble behind the effect" cannot
be byte-confirmed here (expected; the deterministic oracle cannot consume a
non-deterministic fixture — blackbody precedent). NOT claimed VISUAL_PROVEN. An
interactive windowed capture is required to record the wobble; deferred.
