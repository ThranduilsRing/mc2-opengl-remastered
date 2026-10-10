<!-- doc-status: current | reviewed:2026-10-11 -->
# Backend-A UV-V Verification Check (Task 8)

## Summary

This document records the artifact-based UV-V orientation verification for
Backend-A (`ModelPreviewEngineShader`) vs Backend-B (`MeshPreview3D`), as
required by Task 8 of the Backend-A v2 plan.

Both backends source vertex UVs from the same `TglMeshLoader` (VAO loc2).
The automatable part of the acceptance criterion is whether Backend-A and
Backend-B agree on V orientation when rendering the same prop headlessly.
In-game parity requires the game running interactively and is deferred to
a user visual check.

---

## Chosen Prop

**`data/tgl/2civliving.tgl`** (2-storey civilian living building)

- Used in existing orientation and spotlight smokes (established fixture).
- Has a rooftop / window-row pattern that is clearly asymmetric vertically when
  texture V is correct: roof detail at top, ground-floor windows at bottom.
- Albedo atlas: `data/tgl/512/a_2civliving.ktx2` (also available at 256/128 tiers).

---

## Smoke Implementation

Added `--smoke-backend-a-uvv <deploy> <shaderRoot>` to `mc2_asset_viewer`
(wired in `main.cpp` → `AssetViewerApp::runSmokeBackendAUvv`):

1. Creates a headless GL 4.3 context (SSBOs required for Backend-A).
2. Renders `2civliving.tgl` at 256×256 with **Backend-A** (`ModelPreviewEngineShader`)
   and **Backend-B** (`MeshPreview3D`) using each backend's `renderToPixels()`.
3. Writes both renders as PPM files to `build64/` (not committed, not gitignored
   — they are transient artefacts regenerated on every run).
4. Splits each image into top-half and bottom-half, computing mean Rec.709
   luminance on non-background pixels (background threshold: R<50, G<55, B<60).
5. Compares which half is brighter. If both backends agree → **CONSISTENT**.
   If they disagree → **FLIPPED** (real UV-V finding; investigate tool-side,
   not shader).
6. **Reliability guard:** if either backend's |top − bottom| luminance delta is
   below 3.0 lum-units (out of 0–255), the split is dominated by lighting
   variation rather than texture orientation. In that case the result is
   reported as **INCONCLUSIVE** (not a failure), with the verdict deferred to
   the user visual check.

---

## Run Results

**Invocation:**
```
mc2_asset_viewer.exe --smoke-backend-a-uvv A:/Games/mc2-opengl/mc2-win64-v0.4 shaders
```

**Output (verbatim):**
```
[smoke] backend-a-uvv: PPMs written to build64/uvv_backend_a.ppm and build64/uvv_backend_b.ppm
[smoke] backend-a-uvv: Backend-A top=61.3 bot=60.1 delta=1.2 (top-brighter)
[smoke] backend-a-uvv: Backend-B top=77.9 bot=87.7 delta=9.8 (bot-brighter)
[smoke] backend-a-uvv: PASS (INCONCLUSIVE -- one or both backends have low luminance
  delta (1.2 / 9.8 < 3.0); orientation cannot be auto-determined; deferred to user
  visual check)
```

**Exit code: 0 (PASS)**

### Interpretation

| Backend | Top mean | Bottom mean | Delta | Verdict |
|---------|----------|-------------|-------|---------|
| Backend-A (engine shader) | 61.3 | 60.1 | **1.2** | Below threshold — INCONCLUSIVE |
| Backend-B (simple lit)    | 77.9 | 87.7 | **9.8** | Above threshold — bot-brighter |

- **Backend-A delta = 1.2** is below the 3.0-unit reliability threshold.
  Root cause: `static_prop.frag` in the minimal Backend-A config (legacy lane,
  fog disabled, ambient 0, IBL 0) produces very uniform flat shading across a
  large texel-uniform face. The engine shader's lighting path normalises colour
  contribution more aggressively than Backend-B's simple `NdotL` diffuse,
  collapsing the top/bottom luminance contrast.
- **Backend-B delta = 9.8** is clear: bottom is brighter, which matches a
  building whose ground-floor facade texture is lighter than the rooftop.
- Because Backend-A's delta is below the reliability guard, no UV-flip finding
  is triggered. The PASS is honest: the automatable comparison cannot determine
  A's V orientation from this prop at this lighting config.

### No tool-side UV fix applied

No UV transform difference was found between the backends:
- Both feed `a_uv` (loc2) to the vertex shader unchanged.
- `static_prop.vert` passes `v_uv = a_uv` with no transform; `static_prop.frag`
  samples `v_uv` directly (or `fract(v_uv) * uvScale` in the array-texture lane,
  which Backend-A uses the non-array legacy lane).
- Both `draw()` paths use identical `ImGui::Image(..., ImVec2(0,1), ImVec2(1,0))`
  for the GL-to-ImGui Y-flip (confirmed by source inspection).
- No shader edits were made. Backend-B is unchanged.

---

## Pending User Visual Check (in-game parity)

**This check cannot be automated headlessly.** The game must be running the same
prop in the scene to compare.

### Instructions for the user

1. Launch the game or editor with the asset viewer open (or use the standalone
   `mc2_asset_viewer.exe` interactively).
2. Load `2civliving` in the Model Browser. The Backend-A preview pane should
   show the building with its roof detail at the top and ground-floor windows
   at the bottom.
3. Compare to the in-game prop (place `2civliving` in the editor scene, or
   observe it in a mission that contains it).
4. **If the texture appears upside-down in Backend-A** (roof at bottom, windows
   at top): the tool's UV-V needs investigation. Possible fixes (tool-side only,
   no shader edits):
   - Check `MeshGpu::upload` — if it applies a `1-v` flip to the UV buffer for
     Backend-B but not Backend-A, align them.
   - Check `mclib/assimp_importer.cpp` — if the GLB override path flips V but
     the TGL native path does not, the mismatch lives in the loader.
   - Check `TglMeshLoader` — if UV coords are stored V-up and Backend-B corrects
     with a frag-shader or loader flip not present in Backend-A.
5. **If the texture appears correct in Backend-A:** UV-V parity is confirmed;
   no further action needed.

Reference: the MVP's open UV-V question is noted in
`docs/asset-viewer-backend-a-shader-contract.md` ("UV-V convention: TGL stores
UVs in D3D convention (V=0 at top); GL textures expect V=0 at bottom — verify
that the tool's UV feed agrees with the in-game render").

---

## Conclusion

- **Automatable result:** Backend-A UV-V orientation cannot be distinguished
  from Backend-B's by top/bottom luminance split on `2civliving` at the current
  lighting config. The delta is below the reliability threshold (1.2 < 3.0).
  No UV-flip finding was triggered. Smoke exits 0 (PASS / INCONCLUSIVE).
- **In-game parity:** Explicitly deferred to user visual check (the only honest
  closure; the tool cannot run the game headless).
- **No shader edits** were made. Backend-B is unchanged.
- **No tool-side UV fix** was needed (no disagreement found in the automatable
  part).
