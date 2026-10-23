<!-- doc-status: current | reviewed:2026-10-11 -->
# Debugging Render Issues

How to use the built-in ImGui panels to diagnose rendering problems at runtime.

## Opening the overlay

Press **Ctrl+Shift+G** (or set `MC2_IMGUI=1` in the environment before launch — it defaults on in dev builds).

Two main windows appear: **Graphics Options** and **Renderer Features**. A third, **Object Inspector**, is opened by clicking any pixel when `MC2_IMGUI_INSPECTOR=1` is set.

## Graphics Options panel (`Ctrl+Shift+G`)

Source: `GuiRuntime/GraphicsOptionsWindow.cpp`

### Post-Process section

Toggle bloom, FXAA, and tonemapping on/off. Useful for isolating whether a visual artifact is in the scene pass or the composite pass.

### Terrain section

Toggle terrain tessellation, POM (parallax occlusion), and distance LOD. If terrain looks wrong, disable POM first to separate displacement artifacts from UV/splat issues.

### Draw Packets section

Shows per-frame static-prop dispatch counters: slot count, instance count, coalesce hits. If static props vanish, check `slot_count` drops to zero — that indicates a batcher flush issue, not a shader issue.

### Render Passes section

Per-pass timing (CPU-side). Shadow pre-pass time, terrain time, object time, post time. Use this before opening Tracy for a quick check on which pass is slow.

### GBuffer Preview section

Cycle through intermediate buffers: depth, normals, albedo, shadow map. Indispensable for diagnosing:
- Wrong normals → terrain flickering or incorrect PCF
- Depth artifacts → z-fighting or reverse-Z misconfiguration
- Shadow map content → shadow frustum coverage or bias issues

### Env Gates section

Read-only table of all active `MC2_*` env vars for this session. Confirms which feature gates actually fired on startup — avoids "I set the var but it's not working" confusion from shell inheritance issues.

### Debug Overlays section

Cycle the ProjectZ heatmap overlay. Shows which objects are using CPU-side depth projection vs GPU-derived depth. Anything in the hot zone that should not be there indicates a bridge path that escaped retirement.

### Terrain Tuning section (collapsed by default)

15 sliders for live terrain parameter adjustment: splat blend sharpness, POM depth, foam intensity, water foam width, shadow bias offset, etc. Changes take effect immediately; no restart needed. Write tuned values back to `shaders/` constants or INI when satisfied.

## Renderer Features panel

Source: `GuiRuntime/EditorInspector.cpp:125`

Checkboxes for every `MC2_*` feature gate that can be toggled at runtime without restart. Note: some gates (like `MC2_EDITOR_MODE`) are init-time only and are greyed out here.

Use this panel to:
- Enable `MC2_SHADOW_ENABLE` for a single session without relaunching
- Toggle `MC2_GPU_OBJECTS` off to confirm a static-prop bug is in the GPU path, not the legacy path
- Arm `MC2_RENDER_CONTRACT_ASSERT` to catch GL state violations immediately

## Object Inspector panel

Requires `MC2_IMGUI_INSPECTOR=1` (set in environment before launch, or toggle from Renderer Features panel if available).

Source: `GuiRuntime/EditorInspector.cpp:208`

### Object-ID section

Displays the raw object-ID pixel under the cursor. Click any pixel to freeze the readback. The ID maps to a slot in the RenderWorld object table.

### Render Explain section

Given the frozen object ID:
- Object type (static prop, mech, terrain tile, FX node)
- Mesh handle, material index, pipeline ID
- Draw-call slot, packet index, instance offset

If an object renders black or wrong, this section identifies which pipeline descriptor and material GPU table slot it used — narrows the fix to one shader variant or one material row.

### StaticProp section

For static-prop hits: type index, type name, SSBO instance offset, texture array layer, LOD level. Confirms the correct albedo is bound and the instance is in the expected SSBO slot.

### Render Spine section

Shows the full v6 DrawPacket chain for the selected object: sorted slot, global packet index, pipeline ID, material index, base instance. Compare against expected values when debugging dispatch ordering or multi-draw instancing issues.

### Mech section

For mech hits: mech index, node count, LOD, batcher slot. Shows whether the mech is going through the GPU batcher path or the legacy per-draw path.

### Terrain section

For terrain hits: tile X/Z, quad index, splat weights (all four channels), height, normal vector. Indispensable for splat-blend edge artifacts.

### Material section

Shows the GPU material table row for the selected object: albedo texture handle, normal/roughness/metallic packed handles, and the KTX sidecar path when one is loaded.

### Lookup section

Arbitrary mesh handle → material handle lookup. Paste any numeric handle to resolve it without having to click in-world.

### Env Gates section (in inspector)

Same as the Graphics Options Env Gates — repeated here for convenience when the Graphics Options window is not open.

## Common diagnostic workflows

### Object is invisible

1. Open Object Inspector, click where the object should be.
2. If Object-ID returns 0: object not rendered at all. Check `MC2_GPU_OBJECTS=1` and draw packet slot count.
3. If Object-ID returns a valid ID but object is black: shader issue. Check Render Explain → pipeline ID → find that shader variant.
4. If Object-ID returns valid ID and Render Explain shows wrong material index: material GPU table mismatch.

### Terrain flickering or seams

1. GBuffer Preview → normals. Seams in normals = tessellation adjacency issue.
2. GBuffer Preview → depth. Striping = depth bias misconfigured. Tune in Terrain Tuning sliders.
3. Terrain section in Inspector: check splat weights at the seam vertex.

### Shadow artifact (acne / peter-panning)

1. GBuffer Preview → shadow map. Check coverage and depth distribution.
2. Enable `MC2_DEBUG_SHADOW_FRUSTUM=1` (Renderer Features) to overlay frustum bounds.
3. Terrain Tuning → shadow bias offset slider. Adjust while watching the scene.

### Static props flicker or pop

1. Draw Packets section → watch `slot_count` over several frames. Should be stable.
2. Enable `MC2_STATIC_PROP_TRACE=1` from Renderer Features. Check log for admission/eviction churn.
3. Object Inspector → StaticProp → LOD level. If LOD thrashes, check camera distance threshold.

### Performance regression

1. Render Passes timing in Graphics Options: identify the expensive pass.
2. Toggle passes off one by one (GBuffer Preview → each channel) to confirm which sub-pass changed.
3. Open Tracy for GPU zone detail (always compiled in; connect Tracy profiler to running process).

## Environment variable quick-set

Set before launching the exe (PowerShell):

```powershell
$env:MC2_IMGUI = "1"
$env:MC2_IMGUI_INSPECTOR = "1"
$env:MC2_RENDER_CONTRACT_ASSERT = "1"   # catch GL state violations
.\mc2.exe
```

See `docs/modding/renderer-feature-flags.md` for the full flag list.
