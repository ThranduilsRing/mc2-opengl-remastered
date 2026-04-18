# HUD / Scene Split Design

**Date:** 2026-04-18
**Branch:** `claude/nifty-mendeleev`
**Status:** Approved — ready for implementation planning

## Problem

Post-processing (bloom, FXAA, tonemapping, screen-shadow pass) currently applies to all
draws that occur between `pp->beginScene()` and `pp->endScene()`. This includes the
entire HUD, text, cursor, mission markers, and nav beacons. The result is user-visible
every frame: UI elements receive bloom halos, tonemapping shifts, and unintended shadow
darkening. Mission markers and nav beacons incorrectly receive scene shadows despite
being UI elements by contract.

This is tracked as a known issue in CLAUDE.md and docs/modding-guide.md.

## Goal

Scene content renders to the scene FBO and goes through the full post-process chain.
HUD, text, cursor, mission markers, and nav beacons render after the post-process
composite, directly to the default framebuffer, and receive no post-processing.

## Render Contract Classification

This design respects the existing render contract in `docs/render-contract.md`.

- **Bucket C1 (screen-space):** HUD, text, cursor — screen-space authoritative. No
  post-processing by contract.
- **Mission markers / nav beacons:** classified as UI by intent. They are projected
  world elements, but they are 2D UI elements that must not receive scene
  post-processing or shadowing. Their shader path is an implementation locator, not
  their classification.
- **Bucket A / B (scene):** terrain, 3D objects, overlays (cement, craters, water),
  shadows — unchanged. These remain in the scene pass with no reclassification.

## Approach: In-Renderer HUD Command Buffer with Explicit Contract Bit

### New render state bit

Add `gos_State_IsHUD` to the `gos_RenderState` enum. When set to `1`,
`gosRenderer::drawQuads()` and `gosRenderer::drawLines()` buffer the draw call instead
of submitting to GL. When `0` (default), draws go through the existing immediate path
unchanged.

Classification is **entirely explicit**. The renderer never infers HUD status from
vertex data, shader name, material type, or any heuristic. If `gos_State_IsHUD` is not
set, the draw is a scene draw.

### Frame loop (draw_screen in gameosmain.cpp)

```
gos_RendererBeginFrame()          — gosRenderer::beginFrame(): clear hudBatch_, bind VAO
pp->beginScene()                  — bind sceneFBO, clear (unchanged)
Environment.UpdateRenderers()     — HUD draws buffer; scene draws submit immediately
pp->endScene()                    — screen-shadow, bloom, composite scene → FB 0 (unchanged)
renderer->flushHUDBatch()         — replay buffered HUD to FB 0
swap_window()
```

`beginScene()` and `endScene()` are unchanged at the FBO/postprocess level.
`hudBatch_` is cleared in `gosRenderer::beginFrame()` — before any scene or HUD draws
— so the batch lifetime is exactly one frame from beginFrame onward. Clearing inside
`flushHUDBatch()` is explicitly disallowed: it would hide early-return bugs by either
replaying stale entries or silently discarding evidence.

### HUD draw kinds

Each buffered entry is one of three draw kinds:

**`QuadBatch`**
- Vertex payload (copied)
- Full `RenderState` array snapshot (all slots, verbatim)
- `projection_` at capture time

**`LineBatch`**
- Same fields as `QuadBatch`

**`TextQuadBatch`**
- Expanded glyph vertices (layout runs at buffer time, not replay time)
- Full `RenderState` array snapshot
- Font texture handle (if outside the RenderState array, captured explicitly)
- `projection_` at capture time
- Text foreground color

`gos_TextDraw()` with `gos_State_IsHUD` active runs glyph layout immediately and emits
one or more `TextQuadBatch` entries. `TextDraw` does not persist as a deferred
pipeline re-entry. There is no replay-time dependency on text attributes, text position,
text region, or font globals.

### State snapshot contract

The full `RenderState` array is copied verbatim at buffer time — all slots, not a
curated subset. This covers `gos_State_Overlay`, `gos_State_TextureMapBlend`,
`gos_State_Culling`, `gos_State_Texture2/3`, and all other slots consulted by
`applyRenderStates()`, without the design needing to track which slots HUD currently
uses.

`projection_` is captured per draw, not assumed invariant. Mission markers are already
projected to screen-space vertices before submission, so `projection_` is invariant in
practice — but capturing it is the stated correctness guarantee, not an assumption.

### Replay path (flushHUDBatch)

`flushHUDBatch()` is owned by `gosRenderer`. It iterates `hudBatch_` in submission
order. For each entry:

1. Restore the captured `RenderState` snapshot via `applyRenderStates`.
2. Re-upload the captured `projection_`.
3. For `TextQuadBatch`: restore the captured font texture and foreground color.
4. Submit through an **internal immediate path** that reuses existing
   material-selection and state-application logic but does not re-enter HUD buffering.
   `flushHUDBatch()` never calls `drawQuads()`/`drawLines()` through the public
   entrypoints while `gos_State_IsHUD` is set.

After the last entry, the renderer restores the pre-flush `RenderState` snapshot (the
state active before flush began), not a hardcoded GL baseline.

### Late submission

Any draw with `gos_State_IsHUD=1` received after `flushHUDBatch()` returns is buffered
with a diagnostic warning and replayed next frame. This is defined behavior, not a
crash or silent drop.

## Game-Side Callsites (First-Pass Scope)

### What is in scope

- **Screen-space HUD, text, cursor:** debug windows (`DEBUGWINS_render()`), user input
  overlay / cursor (`userInput->render()`), status bar and text draws in
  `mechcmd2.cpp`.
- **Mission markers and nav beacons:** classified by intent (UI, no post-processing),
  not by shader name. `gos_tex_vertex_lighted.frag` is the current implementation
  path, but the classification is the contract.
- **Logistics and options screens:** `logistics->render()` and
  `optionsScreenWrapper->render()` execute only when mission is not active (no scene
  content in the same frame). They are HUD by definition.

### What stays in the scene pass

Terrain, 3D objects, overlays (cement, craters, water), and shadows. No changes.
Projected world overlays must not be reclassified into HUD.

### Callsite scoping rules

Function-level `IsHUD` wrapping is allowed only for functions verified to emit no
scene content. For all other callsites, `IsHUD` must be scoped to the specific draw
block, not the enclosing function.

State hygiene is a first-class constraint:

- Every callsite uses the narrowest viable scope.
- Any early-return path must restore `IsHUD=0` before exiting.
- The design does not rely on callsites remembering a trailing clear.

The remaining implementation task for mission markers is callsite discovery: locating
the exact marker/nav-beacon draw submissions inside the mission render tree so `IsHUD`
can be scoped at those explicit calls. This is callsite discovery, not a phase-boundary
design dependency.

## Validation

**Primary check (visual):** after deployment with bloom enabled:

- Screen-space HUD, text, and cursor should be visually identical before and after,
  except for removal of unintended postprocess artifacts (bloom halos, tonemapping
  shifts).
- Mission markers and nav beacons **are expected to change** — they should no longer
  receive scene postprocessing or shadow darkening. This is the intended behavior.

**Scene-pass regression check:** projected world overlays (cement, craters, water) and
other non-HUD content must still receive the same postprocess and shadow treatment as
before. Accidental reclassification of scene content into the HUD pass is the most
likely first-pass regression.

**State hygiene assertion:** `gosRenderer` asserts in debug builds that
`gos_State_IsHUD` is `0` at frame end. This is a frame-level state-hygiene check, not
tied narrowly to the flush function body. A latched `IsHUD=1` at frame end means a
callsite failed to restore.

**Instrumentation:** `Render.HUDFlush` Tracy zone is optional. The existing screenshot
framework in `gos_validate.cpp` is the correct tool for all correctness checks.

## Non-Goals

- Automatic HUD detection from vertex data, `rhw` values, or material type.
- Splitting `UpdateRenderers()` into scene and HUD phases at the call-graph level.
- Moving projected world overlays (cement, water) to the HUD pass.
- Any changes to `beginScene()` / `endScene()` / the FBO lifecycle.

## Files Expected to Change

| File | Change |
|---|---|
| `GameOS/gameos/gameos_graphics.cpp` | `gos_State_IsHUD` enum value; `gosRenderer` HUD batch fields, `drawQuads`/`drawLines` buffering, `flushHUDBatch`, internal submit path |
| `GameOS/gameos/gos_postprocess.h/.cpp` | No changes — `beginScene()`/`endScene()` are unchanged |
| `GameOS/gameos/gameosmain.cpp` | One `renderer->flushHUDBatch()` call after `pp->endScene()` |
| `code/mechcmd2.cpp` | `IsHUD` wrapping for debug windows, status bar, text draws |
| Mission render tree (TBD) | `IsHUD` scoped around marker/nav-beacon draw callsites (callsite discovery task) |
| Logistics/options render (TBD) | `IsHUD` wrapping verified after function audit |
