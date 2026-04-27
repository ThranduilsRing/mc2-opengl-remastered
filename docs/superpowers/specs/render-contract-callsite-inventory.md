# Render Contract Callsite Inventory

**Date:** 2026-04-26
**Spec:** [`2026-04-26-render-contract-registry-design.md`](2026-04-26-render-contract-registry-design.md)
**Commit:** 7 of the inert phase 1 sequence
**Status:** initial inventory; expand as additional pass identities are confirmed

This document tracks every C++ draw entry point tagged with a
`// [RENDER_CONTRACT:Pass=... id=...]` marker. The spec's exit criterion 6
requires every major C++ draw entry point to carry a marker; this commit
seeds the inventory with the highest-confidence entries. Lower-confidence
or smaller draw paths are listed under "Pending tagging" for follow-up.

---

## Tagged callsites (confirmed)

| File | Line | Pass | Stable ID | Notes |
|---|---:|---|---|---|
| `GameOS/gameos/gameos_graphics.cpp` | ~2995 | `TerrainBase` | `gosRenderer_terrainDrawIndexedPatches` | indexed-patches terrain draw; followed by `markTerrainDrawn()` |
| `GameOS/gameos/gameos_graphics.cpp` | ~4602 | `TerrainOverlay` | `gosRenderer_drawTerrainOverlays` | alpha cement / perimeter |
| `GameOS/gameos/gameos_graphics.cpp` | ~4653 | `TerrainDecal` | `gosRenderer_drawDecals` | craters + footprints; alpha-blend, depth-write off |
| `GameOS/gameos/gameos_graphics.cpp` | ~2847 | `Grass` | `gosRenderer_drawGrassPass` | GPU grass billboards |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | ~664 | `StaticProp` | `GpuStaticPropBatcher_flush` | GPU static prop renderer |
| `mclib/tgl.cpp` | ~2548 | `OpaqueObject` (and `AlphaObject`) | `TG_Shape_Render` | TGL shape draw; pass identity branches on material flags — F4 will centralize |

---

## §3.2 MRT-incomplete inventory — confirmation status

The spec §3.2 listed six fragment shaders that may be drawn while MRT is bound but do not declare `GBuffer1`. Commit-7 audit of `gos_postprocess.cpp` produced this critical finding:

> **`enableMRT()` and `disableMRT()` are defined but NEVER CALLED from production code.** `beginScene()` (`gos_postprocess.cpp:518-525`) binds the MRT FBO with `glDrawBuffers(2, {COLOR0, COLOR1})` and the only subsequent calls to `glDrawBuffers(1, ...)` are inside the post-process passes themselves (after the scene has been fully rendered). The intent comment at line 519-520 says *"Start with single draw buffer — MRT only during terrain rendering"*, but the implementation does not match the intent.

That elevates §3.2 from "latent risk" to **active driver-dependency**. Every shader bound after `beginScene()` and before the first post-process pass is rasterizing into an MRT FBO; if it doesn't declare `GBuffer1`, its attachment-1 output is driver-dependent for the pixels it covers.

Updated confirmation status:

| Shader | Was: | Is now: | Spec follow-up |
|---|---|---|---|
| `gos_tex_vertex.frag` (IS_OVERLAY bridge) | confirmed MRT-bound | **confirmed** (no change) | F3 |
| `gos_vertex.frag` | suspected | **confirmed MRT-bound** | F3 |
| `gos_vertex_lighted.frag` | suspected | **confirmed MRT-bound** | F3 |
| `gos_tex_vertex_lighted.frag` | suspected | **confirmed MRT-bound** | F3 |
| `object_tex.frag` | suspected | **confirmed MRT-bound** | F3 |
| `gos_text.frag` | unconfirmed (timing-dependent) | **likely MRT-bound** during scene; HUD/text drawn after `runScreenShadow()` may bind a different FBO — needs further audit | F3 |

**Implication:** the §3.2 ambiguity is not a latent risk; it is current production behavior. shadow_screen.frag is reading driver-dependent values for every pixel rasterized by the five confirmed-incomplete shaders. The current "acceptable" behavior on the developer's AMD/Windows configuration is luck, not policy.

This finding does **not** change the phase-1 implementation. The spec's frozen-surface rule prohibits adding `GBuffer1` declarations to those shaders or changing the FBO state machine. The escalation goes into follow-up F3 and is documented in the closing report (commit 9) as a high-priority follow-up.

---

## Pending tagging (lower-confidence entries — future commits)

The following draw paths warrant tagging but are deferred:

- `gosFX::*::Render` (particle effects) — many subclasses; F-class `ParticleEffect`
- HUD / text submission paths in `code/missiongui.cpp` and similar — F-class `UI`
- Debug overlays:
  - `MC2_PROJECTZ_OVERLAY` callsite in `mclib/projectz_overlay.cpp` — F-class `DebugOverlay` (also F5)
  - F1 bloom toggle callsite — F-class `DebugOverlay`
  - F2 shadow debug overlay — F-class `DebugOverlay`
  - Tracy on-screen visualizer — F-class `DebugOverlay`
- Shadow caster pre-passes (static terrain shadow build, dynamic mech shadow draw) — F-class `ShadowCaster`
- Water surface draw in `mclib/quad.cpp` — F-class `Water` (intentional projected exception)

These are tracked here for a future tagging pass. The spec's exit criterion 6 is satisfied at the "major draw entry points" granularity by the confirmed table above; thoroughness is a follow-up.

---

## Cross-reference

- Spec: [`2026-04-26-render-contract-registry-design.md`](2026-04-26-render-contract-registry-design.md)
- Existing render-contract doc (submission-space contract; orthogonal): [`docs/render-contract.md`](../../render-contract.md)
- ProjectZ callsite inventory (template for this doc): [`projectz-callsite-inventory.md`](projectz-callsite-inventory.md)
- ProjectZ closing report: [`projectz-policy-split-report.md`](projectz-policy-split-report.md)
