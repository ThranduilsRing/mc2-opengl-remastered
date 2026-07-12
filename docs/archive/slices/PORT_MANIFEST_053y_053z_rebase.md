# MC2R v0.4 Correct + UI Editor/View 0.5.3y + 0.5.3z-r2 Port Manifest

## Applied drop-ins

1. `mc2r_ui_editor_v053y_viewer_button_state_adapter_dropin.zip`
2. `mc2r_ui_editor_v053z_r2_cursor_assets_corrected_dropin.zip`

## Extra build/integration repairs

- Viewer/View.cpp (restored known-good viewerEffectStream version from uploaded old View.cpp)
- Viewer/ViewerHostGlobals.cpp
- CMakeLists.txt (path normalization + guarded ui_editor target)
- 3rdparty/imgui/CMakeLists.txt (SDL.h include path repair)
- ui_editor/CMakeLists.txt (project ImGui target + STB fallback)
- Viewer/CMakeLists.txt (ViewerHostGlobals + v0.4 link seams + project ImGui target)

## Conflict marker scan

No `<<<<<<<` / `>>>>>>>` conflict markers found in text files after patch assembly.

Note: vendored documentation may contain Markdown heading lines made of `=======`; those are not merge conflict markers.

## Build order

```bat
cmake -S D:/Repos/MC2_Remastered -B D:/Repos/MC2_Remastered/build64 ^
  -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_PREFIX_PATH=D:/Repos/MC2_Remastered/3rdparty ^
  -DCMAKE_LIBRARY_ARCHITECTURE=x64

cmake --build D:/Repos/MC2_Remastered/build64 ^
  --config RelWithDebInfo ^
  --target ui_editor ^
  -- /m

cmake --build D:/Repos/MC2_Remastered/build64 ^
  --config RelWithDebInfo ^
  --target viewer ^
  -- /m
```
