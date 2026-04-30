# MC2R Editor Changelog

## 0.14.0-editor — EditorCamera terrain shader hookup

- Added the per-frame terrain shader uniform composition block to
  `EditorCamera::render()` in `Editor/EditorCamera.h`.
- Calls `gos_SetTerrainMVP`, `gos_SetTerrainViewport`,
  `gos_SetTerrainCameraPos`, and `gos_SetTerrainLightDir` before
  `land->render()`, mirroring the equivalent block in
  `GameCamera::render()` (`code/gamecam.cpp`).
- Preserved the existing `GL_FALSE` upload convention; left the
  misleading transpose comment in `code/gamecam.cpp` untouched.
- Editor-only change. No engine, shader, or mclib files modified.
- Bumped Editor SemVer to `0.14.0-editor` in `Editor/EditorVersion.h`
  and `Editor/EditorVersion.cpp`.

## 0.13.0-editor — SemVer compile fix

- Fixed `EditorVersion.cpp` compile error C1017 by removing invalid preprocessor string indexing.
- Moved the Editor SemVer string literals into explicit macros in `EditorVersion.h`.
- Kept the Editor version independent from the main engine version.

## 0.12.0-editor — Legacy Editor version header retired

- Removed the Editor-side legacy `Editor/version.h` stamp from the active Editor source package.
- Removed the legacy version-header include from the Editor runtime path.
- Routed `Environment.version` through `EditorVersion_GetSemVer()`.
- Routed Editor `CreateProviderEngine("MC2Editor", ...)` calls through `EditorVersion_GetSemVer()`.
- Left the main engine `code/version.h` untouched so game/runtime versioning remains separate.

This changelog records Editor Remaster improvements only. It intentionally excludes failed attempts, speculative fixes, and engine-wide changes.

## 0.11.0-editor — Editor-owned SemVer

- Added `Editor/EditorVersion.h` and `Editor/EditorVersion.cpp`.
- Added Editor-local SemVer constants independent of the MC2R engine version.
- Wired the Editor version string into:
  - MFC startup trace output.
  - GameOS environment startup trace output.
  - Editor title bar text.
- Added README notes for SemVer ownership, changelog discipline, and `by Methuselas` source-header expectations.

## 0.10.0-editor — 2x Editor tacmap display

- Doubled the floating Editor tactical map display from 128x128 to 256x256.
- Preserved the legacy 128x128 tacmap backing bitmap for mission packet compatibility.
- Updated tacmap click/world mapping to use the enlarged display size.
- Updated viewport overlay drawing to match the enlarged display.

## 0.9.0-editor — Click and double-click selection restored

- Restored left-click object selection in the Editor viewport.
- Restored double-click unit settings access for team, pilot, and brain setup.
- Kept drag-select behavior intact.
- Kept selected-object `DRAW_TEXT` disabled to avoid legacy `cLoadString()` selected-name rendering crashes.

## 0.8.0-editor — Mech selection crash repair

- Repaired the selection path that crashed when selecting mechs.
- Disabled the selected-name text render flag for Editor object selection.
- Preserved visible selection through bars.

## 0.7.0-editor — Claude repair cleanup

- Removed the accidentally appended duplicate `EditorObjectMgr.cpp` translation unit.
- Preserved the first valid implementation copy and the selection/null-guard work.

## 0.6.0-editor — Selection safety pass

- Guarded object-selection hit-test paths against missing `ObjectAppearance`.
- Guarded selected-object distance display after clicks.
- Made `EditorObject::appearance()` and `EditorObject::isSelected()` null-safe.
- Rechecked terrain vertex bounds after edge-click rounding in selection brushes.

## 0.5.0-editor — Camera rotation restored

- Forwarded mouse messages from the embedded GL child window back to the MFC `EditorInterface` window.
- Restored right-drag camera rotation while preserving arrow movement and mouse-wheel zoom.
- Kept MFC as the Editor shell/input owner and SDL/OpenGL as the embedded render surface owner.

## 0.4.0-editor — Editor source comment/header standard

- Added block-style Editor source headers to touched Editor files.
- Standardized touched Editor source comments around `by Methuselas`.
- Added maintenance comments for resource ownership, MFC/GL ownership, frame-loop caution, MOVE readiness, object fallback naming, and save safety.

## 0.3.0-editor — Editor resource catalog and FIT migration

- Added Editor-local FIT-backed resource catalog behavior.
- Kept `EditorResourceFallback.h` as a local compatibility shim.
- Preserved the rule that new Editor resource names belong in FIT data, not new `mc2res.dll` entries.

## 0.2.0-editor — Input bridge validation baseline

- Preserved the merged input bridge state.
- Confirmed arrow movement and mouse-wheel zoom work through the Editor path.

## 0.1.0-editor — MC2 Editor Remaster working baseline

- Established the current Editor Remaster work line.
- Set the immediate active scope to Editor-only fixes.
