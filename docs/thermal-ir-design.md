<!-- doc-status: current | reviewed:2026-10-11 -->
# Thermal / IR view — design (THERMAL-IR-DESIGN-1)

> s68/P9: Retired GL sources are cited with Git object syntax (`8064c6930:path`) so the historical implementation remains inspectable.

Status: **Phase 1 shipped as a labeled placeholder; Phase 2 deferred.**

The shipped Thermal mode (`MC2_VIEW_MODE=3`) is a **luminance placeholder**, NOT
real thermal/IR. This doc records why, and the honest path to a real heat view.

## Why the renderer cannot do real thermal today

Recon (Subagent B, 2026-05-29) established three hard gaps:

1. **No per-mech heat reaches the GPU.** Heat is a local `float heat` in
   `code/mech.cpp` (update loop ~3616-3739). It is never written into
   `GpuMechSubmitDesc` (`gos_mech_batcher.h`) or `GpuMechInstance` (64B; the
   only render-visible per-instance state is `renderFlags` bits like
   `lightsOut`, plus highlight/fog color and `objectIdRaw`/`materialIdx`).
2. **The object-ID buffer carries no kind/heat.** `sceneObjectIdTex_` is
   `GL_R32UI` holding a `RenderObjectHandle` (`[19:0]=index, [31:20]=generation`).
   `RenderObjectKind` (StaticProp/Mech/Terrain/Vfx) lives only in the CPU-side
   `RenderObjectRecord`. A composite shader reading the raw uint cannot recover
   mech-vs-terrain without a parallel kind/heat texture.
3. **The object-ID buffer is not bound to the composite** in the baseline
   pipeline (this opus binds it only for ObjectIdDebug).

So any "mechs read hot" claim today would be a stub producing wrong output.

## Phase 1 (SHIPPED): luminance placeholder

`8064c6930:shaders/postprocess.frag`, `u_viewMode == 3`: map scene luminance to an iron
palette (black→indigo→red→orange→yellow→white). Honest behavior: emissive /
bright regions (fire, muzzle flash, exhaust glow, specular) read hot; dark
terrain reads cool. Labeled "Thermal (luminance placeholder)" in the UI and
capture matrix. No new data, ~20 lines, zero risk to the Visual path.

This is useful as a stylized sensor look but must never be presented as a
calibrated heat signature.

## Phase 2 (DEFERRED): real heat channel

Minimum work to make mechs/units carry real heat into the view:

1. **Producer (gameplay-adjacent, GameAdapters lane):** add a normalized heat
   `[0,1]` to the mech submit path — either a new `uint8_t heat` in
   `GpuMechInstance` (reuse `_pad2`/`_pad3` at byte 56/60) written from
   `mech.cpp`'s `heat` at submit (`mech3d.cpp` ~2567-2619), or a per-frame
   `RenderWorld::updateMechHeat(handle, heat)` adapter call.
2. **GPU output:** `mech.frag` writes heat to a dedicated attachment (e.g.
   `R8`/`R16F` heat buffer, or pack into an unused channel) so the composite can
   read per-pixel heat.
3. **Static/terrain ambient heat:** small constant per kind (terrain ≈ 0.1,
   buildings ≈ 0.05, exhaust stacks ≈ 0.6) so non-mech geometry reads cool.
4. **Composite:** bind the heat buffer; map heat (not luminance) to the palette.
5. **Richer signal (optional):** route weapon-firing, jump-jet, and shutdown
   state into the heat channel for dynamic hotspots.

### Stop conditions (when NOT to start Phase 2 in a renderer-only lane)
- If the work requires reaching into `code/mech.cpp` heat from the renderer
  directly: STOP — that crosses the RenderWorld firewall. The producer step
  belongs in a GameAdapters / gameplay-collaborator lane.
- If only Phase 1 (luminance) is in scope: it is already shipped; do not
  relabel it as real thermal.

## Cross-references
- ViewMode framework + capture: `docs/viewmode-capture-matrix.md`
- Sensor/contact bridge (same firewall constraint): docs/sensor-contact-presentation-recon.md (archived 2026-10-11, E:/mc2-archive-2026-10/docs/cull-wave1-docs-2026-10-11.tar.gz)
