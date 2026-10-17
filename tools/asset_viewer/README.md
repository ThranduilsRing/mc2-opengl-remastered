<!-- doc-status: current | reviewed:2026-10-11 -->
# mc2_asset_viewer (stage 1: texture/asset shell)

Standalone modder tool. Stage 1 = app shell + folder-path browser + texture preview +
`PreviewSurface` seam + asset-type sidebar (Textures live; rest deferred).

**Not a mission editor.** This tool is for assets/modding only (textures,
materials, meshes, VFX, manifests, validation, cooking, packaging). Mission
layout, triggers, objectives, camera paths, and unit placement stay with MC2's
existing mission editor.

## Build

This worktree's `3rdparty/` has no `lib/` — the vendored SDL2/GLEW/ZLIB binaries
live in the root checkout at `A:/Games/mc2-opengl-src/3rdparty/3rdparty` and are
shared across worktrees. Libs are under `lib/x64`, which stock `FindGLEW`/`FindZLIB`
don't search, so pass them explicitly:

```bash
CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
D="A:/Games/mc2-opengl-src/3rdparty/3rdparty"
"$CMAKE" -G "Visual Studio 17 2022" -A x64 -S . -B build64 \
  -DCMAKE_PREFIX_PATH="$D" -DCMAKE_LIBRARY_PATH="$D/lib/x64" \
  -DGLEW_INCLUDE_DIR="$D/include" \
  -DGLEW_SHARED_LIBRARY_RELEASE="$D/lib/x64/glew32.lib" \
  -DGLEW_STATIC_LIBRARY_RELEASE="$D/lib/x64/glew32s.lib"
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```

Exe: `build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe`

## Running

The exe needs its runtime DLLs (`SDL2.dll`, `glew32.dll`, etc.) on PATH or
co-located. They are produced in `build64/RelWithDebInfo/`, NOT next to the exe's
`out/` path — copy the exe beside those DLLs (or add that dir to PATH) before
running. When deployed to a runtime dir (e.g. `mc2-opengl/mc2-win64-v0.4`) the
DLLs are already present.

Usage: launch the exe, type a folder path into the **Folder** field, click
**Load**, and select a texture from the list. PNG/JPG decode is Windows-only
(WIC); BMP/TGA via hand-rolled parsers.

## Smoke

```bash
mc2_asset_viewer.exe --smoke tests/fixtures/asset_viewer   # prints [smoke] PASS, exit 0
```
Regenerate the fixture with `python tests/fixtures/asset_viewer/make_fixture.py`.

## Deferred (stages 2-3 + later)

RenderCore link, lit PBR material preview, real model preview, FIT material
parsing, validation flags ("explain what failed"), texture cooking, KTX2 display,
mod packaging. The `PreviewSurface` seam (`draw(const ImVec2&)`) is shaped so a
RenderCore-backed `PreviewSurface` slots in without a rewrite. **SimpleCamera is
assumed dead** (mech-bay render regression, tracked separately) — stage 3 uses
RenderCore.

**Stage-1.5 debt:** extract a shared `tools/common/MC2AppShell` (SDL/GL/ImGui
bring-up) consumed by both `ui_editor` and `mc2_asset_viewer` (greybeard meta-fix;
retires the SDL-attribute / ImGui-backend skew bug class).

Reuses @Methuselas's `ui_editor/UiEditorImageCache.cpp` + `GameOS/gameos/utils/Image.cpp`.

## KTX2 textures (RGBA8 + BC7)

The browser now lists and previews `.ktx2` files alongside PNG/JPG/TGA/BMP.
Supported KTX2 formats: uncompressed RGBA8 (unorm/sRGB) and stored BC7
(unorm/sRGB). The preview follows the file's KTX color-space metadata
(sRGB vs linear).

> BC7 preview requires the `GL_ARB_texture_compression_bptc` OpenGL extension.
> On GPUs/drivers without it, BC7 files show a friendly "unsupported" message
> instead of a preview. RGBA8 KTX2 always previews.

> The asset viewer follows KTX color-space metadata for preview only. This does
> not change the in-game static-prop BC7 runtime policy, which is audited
> separately.

Not yet supported (deferred): DDS, Basis/supercompressed KTX2, CPU BC7
transcoding, asset cooking.

## Resolution tiers + display sizing

When the current folder sits in a set of numeric sibling folders (e.g.
`data/tgl/{128,256,512}` or `data/textures/{64,128,256}`), a **Resolution** row
appears with a button per available tier. Switching tiers keeps the selected
texture (same filename) and reloads it at the new resolution. Only tiers that
exist on disk are shown.

The preview now fits each texture to the view area (1.00× = fit); the zoom slider
multiplies that. Switching resolution tiers — or opening a higher-resolution
texture — no longer changes the on-screen size, only the detail.

## Static Props (model preview)

Select **Static Props** in the sidebar to browse and preview the 2,030 props
registered in `tgl.fst`. The left panel shows a filterable list of `.tgl` archive
names; clicking one loads it and renders it on a lit turntable in the inspector
panel.

The prop is rendered with its real albedo texture (looked up from the deploy root
via TglMeshLoader). Use the orbit and zoom controls to inspect the model from any
angle.

> **Default preview mode: approximate (Backend B).** The viewer renders with a
> self-contained phong/albedo shader. Backend A (engine shader preview, v2) is
> available via the Backend toggle in the panel header — see
> "Backend-A: Engine Shader preview (v2)" below. Mechs, vehicles, trees, and other
> asset types are deferred.

## Backend-A: Engine Shader preview (v2)

Backend-A is an opt-in rendering mode for the Static Props panel that compiles and
runs the real engine shaders (`shaders/static_prop.vert` and
`shaders/static_prop.frag`) directly inside the viewer. The shaders are referenced
from the repo or deploy `shaders/` directory — not copied or modified. Backend-B
(the approximate phong/albedo shader) remains the default; Backend-A is activated
via the **Backend** toggle in the Static Props panel header.

### What it is

Backend-A (class `ModelPreviewEngineShader`) compiles the actual engine GLSL at
startup, binds the same uniform blocks and SSBOs that the MC2 static-prop batcher
uses, and renders the prop through that pipeline. The shader contract — every
uniform, SSBO block, and sampler binding — is captured in
`docs/asset-viewer-backend-a-shader-contract.md` (regenerate with
`--smoke-backend-a-compile shaders`).

UV-V orientation parity between Backend-A and Backend-B is documented in
`docs/asset-viewer-backend-a-uvv-check.md`. Structural consistency is confirmed;
in-game pixel parity is a pending user visual check.

### What it is not

Backend-A is not pixel-exact to in-game rendering. The following scene inputs are
absent or stubbed in `StandaloneSceneStubs` (the v3 swap point):

- Mission lights and the per-mission ambient term.
- Shadow cascade (no depth map; NdotL shading only).
- Fog parameters (stubbed to no-fog values).
- Hot-window lights (the engine's SSBO light list is empty).

No pixel-parity claim is made or implied. Backend-A is useful for confirming that
the real engine shaders compile cleanly against the prop's materials and that basic
lighting and UV mapping behave as expected.

### Fail-open behaviour

If the engine shaders fail to compile or link (for example, because the `shaders/`
root is missing or the shader source has a syntax error), Backend-A reports the
error in the **Shader Contract** panel and automatically falls back to rendering
Backend-B. The viewer never crashes on a shader failure; the error is surfaced as
text and the prop continues to render via the approximate backend.

### Dependency

Backend-A requires a readable `shaders/` root. This is satisfied by running the
viewer from the repo worktree root (where `shaders/` is checked in) or from a
deploy directory that contains a `shaders/` subtree. If neither is present the
compile fails and the fail-open fallback activates.

### Smoke tests

The five Backend-A smokes are part of the standard regression sweep:

| Smoke flag | What it checks |
|---|---|
| `--smoke-shader-include <fx>` | GLSL `#include` resolver resolves nested includes |
| `--smoke-backend-a-compile <shaders>` | Engine shaders compile; contract uniforms/SSBOs printed |
| `--smoke-backend-a-render <deploy> <shaders>` | Compiled shaders render a non-empty prop frame |
| `--smoke-backend-a-fallback <deploy>` | Broken shaders fall back to Backend-B (non-empty frame) |
| `--smoke-backend-a-uvv <deploy> <shaders>` | UV-V A/B luminance orientation check; inconclusive = PASS |

## Materials (Stage 2)

Select **Materials** in the sidebar to preview a PBR material on a lit sphere.
Assign up to four slots via **Browse...**: Base Color (sRGB), Normal (linear),
ORM (linear; R=AO, G=Roughness, B=Metallic), Emissive (sRGB). Use the View and
Light controls to orbit, zoom, and rotate the directional light.

> **Preview mode: Local PBR approximation, not exact MC2 shader.** The viewer
> renders with a self-contained Cook-Torrance shader (Backend B). MC2 has no
> standalone ORM material shader to mirror, so this preview is approximate and
> must not be treated as pixel-exact to in-game rendering.

## Mod Workbench S5 panels

S5 extends the Mod Workbench with a stock-roster picker, appearance-key roster,
LOD-chain panel, texture-set panel, a record/health strip, and two export paths.

### Stock roster picker

A filterable list of every `.tgl` entry in `tgl.fst` (sourced via ModelBrowser).
Click any row to bind that stock model as the workbench reference; hover a row
to see its full resolved path in a tooltip. The bound stock model renders in the
left (reference) half of the side-by-side preview.

### Appearance-key roster

Source: unique `st FileName` / `st FileNameN` values scanned from
`<deploy>/data/tgl/*.ini`. The roster is built on first open and rebuilt by the
**Refresh roster** button. A filterable combo-box lets you pick the appearance
key that this record will register under.

The **Verified** indicator is derived, not a manual checkbox. It shows green when
the selected key matches a roster entry (scanned from the ini files) or matches
an explicit stock-picker selection; it is amber otherwise. The intent is to flag
records whose appearance key was typed by hand and does not correspond to any
deployed asset ini, which may indicate a typo or a key that is not yet in the
deploy tree.

### LOD-chain panel

A table with one row per LOD level. Columns: LOD index / Source GLB path /
Transition distance / Status / row-order controls (Up / Down / Remove). The
**Add LOD** button appends a new empty row. Row order drives the LOD index: rows
are automatically re-indexed by their table position, so reordering via Up/Down
is enough to renumber all LODs.

Advisory WARNs (non-blocking) are raised for:

- Transition distances that are not strictly ascending (LOD N switches closer
  than LOD N-1).
- The first LOD's distance not being 0 (LOD0 should activate at distance 0).
- LOD0-only records (no coarser fallback; flagged as informational).

### Texture-set panel

Four named slots: **Base Color** (sRGB), **Normal** (linear), **ORM** (linear;
R=AO, G=Roughness, B=Metallic), **Emissive** (sRGB). Each slot has a Browse
button and shows the file's resolved path. An unresolved or missing path raises
a `texture-missing` WARN in the validation summary. Texture WARNs are advisory
and never block export.

### Record/health strip

A single summary line shown at the top of the workbench panel:

```
key:<key>  stock:<name>  appearance:<verified|unverified>  LODs:<n>  BLOCKs:<n>  WARNs:<n>
```

Tokens use ASCII only. The strip is a quick at-a-glance health indicator;
details (per-field messages) are shown in the full validation list below it.

### Export paths

Two export paths are available simultaneously; neither is gated on the other.

**Export Draft Bundle** writes `<out_dir>/<id>/model.glb` and
`<out_dir>/<id>/models.generated.json`. It never touches the central manifest.
Use this to produce a self-contained draft bundle for inspection, redistribution,
or manual review before committing to the central registry.

**Append/Merge to Central models.json** merges the current record into the
shared `models.json` at the deploy root. The merge logic: existing records for
other IDs are preserved exactly; the record for the current ID is replaced (or
appended if absent). A `.bak` copy of the previous models.json is written before
any mutation. The write is atomic (temp-file + rename). After writing, the file
is round-trip parsed to verify the JSON is valid. The operation is safe and
reversible: restore the `.bak` to undo.

> **Side-by-side preview uses Backend-B (approximate) by default.** Backend-A
> (engine shader preview, v2) is available via the Backend toggle in the Static
> Props panel; see "Backend-A: Engine Shader preview (v2)" for scope and
> limitations.
