<!-- doc-status: current | reviewed:2026-10-11 -->
# VFX-SCENECOLOR-GRAB-1

Feedback-safe **scene-color copy** frame resource. Gated, default-OFF. This is
**FRAME_RESOURCE_SUBSTRATE**, not a VFX effect: it adds the keystone resource the
render tree was missing — a mid-frame snapshot of the resolved scene color that a
future pass can sample without a read-from-bound-attachment feedback loop.

**No consumer ships in this slice.** Nothing samples the copy yet. Even gate-ON
produces no visual change — it only costs one `glCopyImageSubData`.

## Why it exists

The engine already does the analogous thing for **depth**:
`copySceneDepthForParticles()` snapshots `sceneDepthTex_` into `sceneDepthCopyTex_`
so the in-scene particle flush can sample scene depth without a feedback loop
(`sceneDepthTex_` is the bound FBO's depth attachment during the flush).

There was **no color equivalent**. `sceneColorTex_` is the live render target
(`COLOR_ATTACHMENT0` of `sceneFBO_`); sampling it while it is the bound write
target is a GL feedback loop (undefined). Distortion / refraction / soft-color
particles all need to read scene color *mid-frame, pre-VFX*. This slice mirrors
the depth-copy pattern exactly to unlock those.

## What it does

A new texture `sceneColorCopyTex_` (RGBA16F, full-res — matches `sceneColorTex_`
format and dimensions). A new method `copySceneColorForVfx()` copies
`sceneColorTex_ -> sceneColorCopyTex_` via `glCopyImageSubData` at the same frame
point the depth copy uses: in the in-scene particle/VFX flush, **after** the
opaque scene color is resolved but **before** the VFX/transparent draws that
would sample it. So the snapshot captures the scene as it stands pre-VFX.

`glCopyImageSubData(RGBA16F -> RGBA16F)` is a same-internalformat copy — no blit,
no format-match constraint, GL-clean under `GL_DEBUG_FATAL`.

## The gate

`MC2_VFX_SCENECOLOR_GRAB` — **default OFF** (`envFlagDefaultOff` pattern, mirrors
`MC2_VFX_SOFT_PARTICLES` / `MC2_VFX_LIT_PARTICLES` / `MC2_VFX_BLACKBODY`).

- Gate OFF (default): the bridge never calls `copySceneColorForVfx()`.
  `sceneColorCopyTex_` is never allocated, never written -> **byte-identical to
  today**.
- Gate ON: the bridge calls `copySceneColorForVfx()` once per in-scene flush ->
  one `glCopyImageSubData` into `sceneColorCopyTex_`. **NOTHING samples it** (no
  consumer) -> still **no visual change**, cost is the one copy.

## Frame placement

In `gos_particle_bridge.cpp`, in the in-scene particle flush, immediately after
the `MC2_VFX_SOFT_PARTICLES` depth-copy block (same frame window: scene color is
resolved, the VFX flush has not yet drawn). The copy is lazily allocated on first
call and freed + re-created on resize (`destroyFBOs()` zeroes it, `createFBOs()`
re-makes it on demand) — identical lifecycle to `sceneDepthCopyTex_`.

## Files changed

- `GameOS/gameos/gos_postprocess.h` — `sceneColorCopyTex_` member,
  `getSceneColorCopyTexture()` getter, `copySceneColorForVfx()` declaration.
- `GameOS/gameos/gos_postprocess.cpp` — `copySceneColorForVfx()` (mirrors
  `copySceneDepthForParticles()`; RGBA16F lazy alloc + `glCopyImageSubData`) and
  the `destroyFBOs()` teardown of `sceneColorCopyTex_`.
- `GameOS/gameos/gos_particle_bridge.cpp` — gate (`MC2_VFX_SCENECOLOR_GRAB`,
  `s_scenecolor_enabled` + `vfxSceneColorGrabInitIfNeeded()`) and the gated
  `copySceneColorForVfx()` invocation in the flush.
- `docs/VFX-SCENECOLOR-GRAB-1.md` — this doc.
- `.claude/FRAME-RESOURCE-LEDGER-1.md` — `sceneColorCopyTex_` row MISSING -> EXISTS.

## Ledger

```yaml
VFX_SCENECOLOR_GRAB:
  type: FRAME_RESOURCE_SUBSTRATE
  gate_default: OFF
  gate_off: BYTE_IDENTICAL
  visual_change: NONE (no consumer)
  unlocks: distortion/refraction/soft-color-particles
  copy_pattern: mirrors copySceneDepthForParticles
```
