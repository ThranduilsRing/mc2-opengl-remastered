# X1: platform module, then Linux readiness
Split GameOS by responsibility; isolate native OS access; prove the same product with MSVC and clang-cl before Linux.
Design only, 2026-10-13; no engine changes or builds; Windows remains supported through the single final release.

2026-10-29: the census below is historical. Packet 1 removes 72 uncalled API
names and makes three live internal declarations private; see
`docs/plans/2026-10-29-gameos-api-trim.md` for the verified retirement list.

## Scope and measured baseline

Research used only this worktree at HEAD `254751f589429aaf9f33e2c46249fec3c97159be`; other agents' dirty editor files were left untouched.
Implement target-architecture 2.1/2.9/X1 (`.claude/cleanup/target-architecture-2026-10-12.md:53`, `:148`, `:251`).
Owner ruling overrides that document's arm deletion: preserve portability arms, rename their purpose, and never call Linux proven before a native build/run.
X1 follows S2 heap removal and R1 Vulkan ownership/parity; E1 editor conversion is also required before claiming complete product portability.
Canonical sim firewall: `.claude/cleanup/sim-kernel-design.md:208`; wall clock belongs to Platform, never Sim or the VM host.

Census command actually run: `py -3 C:/Users/Joe/AppData/Local/Temp/mc2_platform_inventory.py` (temporary research script, not a delivery dependency).
Its method: `git ls-files`; inspect tracked C/C++ headers/sources and CMake files, strip line/block comments while retaining newlines;
match case-insensitive direct include directives for windows.h, and preprocessor conditionals containing LINUX_BUILD; count distinct files, including inactive arms.
Header census: `py -3 -c "from pathlib import Path; import re; s=Path('GameOS/include/gameos.hpp').read_text(encoding='utf-8'); c=re.sub(r'//[^\n]*|/\*[\s\S]*?\*/','',s); a=re.findall(r'\b(gos_\w+)\s*\(',c); b=re.findall(r'\b(gosAudio_\w+)\s*\(',c); print(len(s.splitlines()),len(a),len(set(a)),len(b),len(set(b)))"`.
Result: 3,753 lines; 227 gos_* declarations / 222 names; 14 gosAudio_* declarations / 13 names. Neither count proves implementation or caller liveness.
The table below assigns all 241 declarations / 235 names; both CreateResource overloads and all six RenderIndexedArray overloads are cited.

| Direct windows.h includers, directory | Files | Representative code evidence |
|---|---:|---|
| GameOS/gameos; GameOS/gameos/utils; GameOS/include | 13; 5; 3 | `GameOS/gameos/gameosmain.cpp:43`; `GameOS/gameos/utils/timing.cpp:5`; `GameOS/include/gameos.hpp:56` |
| code | 4 | `code/mc2video.cpp:23` |
| editor | 8 | `editor/EditorGosRender.cpp:14` |
| GuiRuntime; gui; mclib/gosfx | 1; 1; 1 | `GuiRuntime/GuiRuntime_internal.h:45`; `gui/UiDefs_internal.h:65`; `mclib/gosfx/effect.cpp:18` |
| tests/unit | 4 | `tests/unit/doctest.h:3979`; `tests/unit/test_gate.cpp:33` |
| tools/asset_viewer; tools/mc2_launcher; tools/mc2fx; tools/mc2weapon_gui | 1 each | `tools/mc2_launcher/mc2_launcher.cpp:55` |
| 3rdparty | 28 | `3rdparty/vk_bootstrap/VkBootstrap.cpp:26`; `3rdparty/tracy/client/TracyProfiler.cpp:8` |
| Total | 72 | 44 outside 3rdparty, including the vendored doctest header; 43 owned files, 29 dependency files. |

No direct includers in other tracked source directories under that scope. This is an include census, not a count of compiled Windows dependencies.
Manifest/preset existence checked by the same command: no root vcpkg.json, vcpkg-configuration.json, CMakePresets.json or CMakeUserPresets.json.
Current dependencies use find_package and a vendored Assimp (`CMakeLists.txt:217`, `:227`, `:313`); bootstrap pins local binary paths (`scripts/bootstrap_worktree_build.py:131`).

## Boundary and ownership

Proposed public headers: `Platform/WindowInput.hpp`, `File.hpp`, `Clock.hpp`, `Memory.hpp`, `Sound.hpp`, `Debug.hpp`; graphics lives in `RenderCore/Graphics.hpp`.
Split declarations/types together; callers include only their domain. A temporary gameos.hpp aggregate has a named removal batch; no new caller uses it.
Final APIs are C++23 values, spans, expected errors and move-only owners, not a permanent wrapper around HGOS handles or global Environment.
Application owns PlatformContext; it owns Window/Input, IO jobs/mappings, Clock, AudioDevice and CrashReporter. Headless creates only needed services.
Shutdown: sessions/loaders stop, render drains/destroys swapchain and UI resources, audio/jobs join, windows close, SDL quits, logging/crash services close last.
No static-duration owners/caches; callbacks and mappings cannot outlive their context. Memory is an allocation-policy seam only; custom heaps die in S2.
SDL2 privately owns window/events/text input/controllers/cursor and monotonic performance-counter pacing; calendar timestamps are separate diagnostic values.
Existing SDL event/audio/counter seams: `GameOS/gameos/gameosmain.cpp:410`, `:820`; `GameOS/gameos/gameos_sound.cpp:120`, `:136`.
Replace wall-clock APIs (`GameOS/gameos/gameos.cpp:170`, `GameOS/gameos/utils/timing.cpp:48`) in presentation/scheduler code only.
Sim cannot include/link Platform, SDL, GameOS, render or clocks, transitively or via PCH; Core used by Sim must also be platform-free.
This refines the broad architecture dependency arrow: loaders receive bytes from Platform outside ticks; Sim receives validated values and integer tick context.
Physical audio/video completion never controls script completion; preserve S1 logical media timelines, campaign/won-mission cinematics and in-mission videos.
File returns owned byte buffers/mappings and bounded errors; resource IDs become content catalogs, registry settings become config files with an optional Windows import.
Native DLL probes, RenderDoc loading, shell integration, crash/SEH and any surviving WIC decoder belong in private Platform/Win32 implementation files.
WIC currently enters image decoding (`GameOS/gameos/utils/Image.cpp:208`); replace with portable codecs or isolate it without COM leaking through render headers.
Platform creates an opaque Vulkan surface using SDL; render owns device/swapchain/VMA and never receives HWND/HANDLE. SDL/Vulkan glue remains private.
Only owned Platform implementation files may include windows.h or other Win32 headers; public headers expose fixed-width types, never DWORD/RECT/COM/SEH.
Upstream dependencies may contain native code privately: 3rdparty and doctest are explicit vendor exceptions, never exemptions for owned callers or Sim include leakage.
Extend the existing firewall (`scripts/check-include-firewall.sh:24`) with resolved dependency/PCH and link-graph checks; direct/transitive negative fixtures must fail.

## Exhaustive declaration assignment

R = preserve the capability with a typed domain redesign; D = delete this GL-specific operation when GL dies; V = retire its GL signature and replace the live capability.
X = delete independently in S2/S1; N = frozen multiplayer transport, assigned to Window/input's separate network quarantine behind MC2_MULTIPLAYER, not an SDL input API.
Debug is the initial triage home for mixed settings/machine queries and RNG: replace query switches with typed domain queries; RNG goes to owned Core/Sim streams, never Platform.
Memory X has no replacement custom-heap surface; ordinary RAII/containers replace it. gos_TestSetRawMouse remains only in test support; IME becomes SDL text composition.

| Domain | Entry points, fate, exact declaration source |
|---|---|
| Window/input | `gos_TerminateApplication` [R] (`GameOS/include/gameos.hpp:396`); `gos_UserExit` [R] (`GameOS/include/gameos.hpp:400`); `gos_AbortTermination` [R] (`GameOS/include/gameos.hpp:404`); `gos_RunMainLoop` [R] (`GameOS/include/gameos.hpp:414`) |
| Window/input | `gos_EnableIME` [R] (`GameOS/include/gameos.hpp:839`); `gos_ToggleIME` [R] (`GameOS/include/gameos.hpp:846`); `gos_PositionIME` [R] (`GameOS/include/gameos.hpp:852`); `gos_FinalizeStringIME` [R] (`GameOS/include/gameos.hpp:857`) |
| Window/input | `gos_SetIMELevel` [R] (`GameOS/include/gameos.hpp:864`); `gos_SetIMEAppearance` [R] (`GameOS/include/gameos.hpp:869`); `gos_GetIMEAppearance` [R] (`GameOS/include/gameos.hpp:874`); `gos_SetIMEInsertMode` [R] (`GameOS/include/gameos.hpp:880`) |
| Window/input | `gos_GetKeyStatus` [R] (`GameOS/include/gameos.hpp:1391`); `gos_GetKey` [R] (`GameOS/include/gameos.hpp:1416`); `gos_DescribeKey` [R] (`GameOS/include/gameos.hpp:1422`); `gos_KeyboardFlush` [R] (`GameOS/include/gameos.hpp:1426`) |
| Window/input | `gos_GetMouseInfo` [R] (`GameOS/include/gameos.hpp:1642`); `gos_SetMousePosition` [R] (`GameOS/include/gameos.hpp:1648`); `gos_TestSetRawMouse` [R] (`GameOS/include/gameos.hpp:1652`); `gos_NetCheckLobby` [N] (`GameOS/include/gameos.hpp:1772`) |
| Window/input | `gos_NetStartGame` [N] (`GameOS/include/gameos.hpp:1782`); `gos_NetEndGame` [N] (`GameOS/include/gameos.hpp:1787`); `gos_NetInformation` [N] (`GameOS/include/gameos.hpp:1844`); `gos_NetGetMessage` [N] (`GameOS/include/gameos.hpp:1897`) |
| Window/input | `gos_NetSendMessage` [N] (`GameOS/include/gameos.hpp:1902`); `gos_NetServerCommands` [N] (`GameOS/include/gameos.hpp:1923`); `gos_SetScreenMode` [R] (`GameOS/include/gameos.hpp:3028`); `gos_ProbeDrawableSize` [R] (`GameOS/include/gameos.hpp:3048`) |
| Window/input | `gos_GetWindowDisplayIndex` [R] (`GameOS/include/gameos.hpp:3570`); `gos_GetNumDisplayModes` [R] (`GameOS/include/gameos.hpp:3571`); `gos_GetDisplayModeByIndex` [R] (`GameOS/include/gameos.hpp:3572`) |
| File | `gos_OpenResourceDLL` [R] (`GameOS/include/gameos.hpp:806`); `gos_CloseResourceDLL` [R] (`GameOS/include/gameos.hpp:811`); `gos_GetResourceString` [R] (`GameOS/include/gameos.hpp:818`); `gos_GetResourceData` [R] (`GameOS/include/gameos.hpp:832`) |
| File | `gos_GetFile` [R] (`GameOS/include/gameos.hpp:1092`); `gos_OpenMemoryMappedFile` [R] (`GameOS/include/gameos.hpp:1098`); `gos_CloseMemoryMappedFile` [R] (`GameOS/include/gameos.hpp:1103`); `gos_ReadFileInBackground` [R] (`GameOS/include/gameos.hpp:1110`) |
| File | `gos_CloseBackgroundFile` [R] (`GameOS/include/gameos.hpp:1115`); `gos_LoadDataFromRegistry` [R] (`GameOS/include/gameos.hpp:1716`); `gos_SaveDataToRegistry` [R] (`GameOS/include/gameos.hpp:1723`); `gos_SaveStringToRegistry` [R] (`GameOS/include/gameos.hpp:1730`) |
| File | `gos_GetUserDataDirectory` [R] (`GameOS/include/gameos.hpp:1734`); `gos_CheckValidTGA` [R] (`GameOS/include/gameos.hpp:3306`) |
| Time | `gos_GetFormattedDate` [R] (`GameOS/include/gameos.hpp:887`); `gos_GetUnformattedDate` [R] (`GameOS/include/gameos.hpp:893`); `gos_GetFormattedTime` [R] (`GameOS/include/gameos.hpp:901`); `gos_GetElapsedTime` [R] (`GameOS/include/gameos.hpp:1681`) |
| Memory | `gos_Malloc` [X] (`GameOS/include/gameos.hpp:1162`); `gos_Free` [X] (`GameOS/include/gameos.hpp:1163`); `gos_CreateMemoryHeap` [X] (`GameOS/include/gameos.hpp:1181`); `gos_DestroyMemoryHeap` [X] (`GameOS/include/gameos.hpp:1192`) |
| Memory | `gos_PushCurrentHeap` [X] (`GameOS/include/gameos.hpp:1199`); `gos_GetCurrentHeap` [X] (`GameOS/include/gameos.hpp:1201`); `gos_PopCurrentHeap` [X] (`GameOS/include/gameos.hpp:1203`); `gos_WalkMemoryHeap` [X] (`GameOS/include/gameos.hpp:1216`) |
| Sound | `gosAudio_CreateResource` [R] (`GameOS/include/gameos.hpp:663`, `GameOS/include/gameos.hpp:664`); `gosAudio_DestroyResource` [R] (`GameOS/include/gameos.hpp:671`); `gosAudio_AllocateChannelSliders` [R] (`GameOS/include/gameos.hpp:678`); `gosAudio_AssignResourceToChannel` [R] (`GameOS/include/gameos.hpp:683`) |
| Sound | `gosAudio_SetChannelSlider` [R] (`GameOS/include/gameos.hpp:689`); `gosAudio_GetChannelSlider` [R] (`GameOS/include/gameos.hpp:690`); `gosAudio_SetSpeakerConfig` [R] (`GameOS/include/gameos.hpp:695`); `gosAudio_SetChannelPlayMode` [R] (`GameOS/include/gameos.hpp:700`) |
| Sound | `gosAudio_GetChannelPlayMode` [R] (`GameOS/include/gameos.hpp:704`); `gosAudio_GetResourceInfo` [R] (`GameOS/include/gameos.hpp:709`); `gosAudio_GetChannelInfo` [R] (`GameOS/include/gameos.hpp:710`); `gosAudio_Reset` [R] (`GameOS/include/gameos.hpp:716`) |
| Sound | `gosAudio_DeviceOptions` [R] (`GameOS/include/gameos.hpp:721`) |
| Graphics | `gos_LoadFont` [R] (`GameOS/include/gameos.hpp:948`); `gos_DeleteFont` [R] (`GameOS/include/gameos.hpp:953`); `gos_TextSetAttributes` [R] (`GameOS/include/gameos.hpp:967`); `gos_TextSetPosition` [R] (`GameOS/include/gameos.hpp:974`) |
| Graphics | `gos_TextGetPrintPosition` [R] (`GameOS/include/gameos.hpp:978`); `gos_TextSetRegion` [R] (`GameOS/include/gameos.hpp:985`); `gos_TextDrawBackground` [R] (`GameOS/include/gameos.hpp:992`); `gos_TextStringLength` [R] (`GameOS/include/gameos.hpp:998`) |
| Graphics | `gos_TextVisualBounds` [R] (`GameOS/include/gameos.hpp:1013`); `gos_FontGetAtlasDesc` [R] (`GameOS/include/gameos.hpp:1046`); `gos_FontGetGlyphDesc` [R] (`GameOS/include/gameos.hpp:1047`); `gos_TextDraw` [R] (`GameOS/include/gameos.hpp:1061`) |
| Graphics | `gos_TextDrawV` [R] (`GameOS/include/gameos.hpp:1065`); `gos_SetupViewport` [R] (`GameOS/include/gameos.hpp:2248`); `gos_GetViewport` [R] (`GameOS/include/gameos.hpp:2261`); `gos_Compute43Box` [R] (`GameOS/include/gameos.hpp:2270`) |
| Graphics | `gos_DrawPoints` [R] (`GameOS/include/gameos.hpp:2276`); `gos_DrawLines` [R] (`GameOS/include/gameos.hpp:2282`); `gos_DrawTriangles` [R] (`GameOS/include/gameos.hpp:2287`); `gos_DrawStrips` [R] (`GameOS/include/gameos.hpp:2292`) |
| Graphics | `gos_DrawFans` [R] (`GameOS/include/gameos.hpp:2297`); `gos_DrawQuads` [R] (`GameOS/include/gameos.hpp:2302`); `gos_RenderIndexedArray` [R] (`GameOS/include/gameos.hpp:2313`, `GameOS/include/gameos.hpp:2314`, `GameOS/include/gameos.hpp:2315`, `GameOS/include/gameos.hpp:2318`, `GameOS/include/gameos.hpp:2319`, `GameOS/include/gameos.hpp:2870`); `gos_SetRenderViewport` [R] (`GameOS/include/gameos.hpp:2322`) |
| Graphics | `gos_GetRenderViewport` [R] (`GameOS/include/gameos.hpp:2323`); `gos_ForceApplyRenderStates` [D] (`GameOS/include/gameos.hpp:2325`); `gos_InvalidateRenderStateCache` [D] (`GameOS/include/gameos.hpp:2326`); `gos_SetTerrainTessParams` [R] (`GameOS/include/gameos.hpp:2329`) |
| Graphics | `gos_SetTerrainPhongAlpha` [R] (`GameOS/include/gameos.hpp:2330`); `gos_SetTerrainDisplaceScale` [R] (`GameOS/include/gameos.hpp:2331`); `gos_SetTerrainWireframe` [R] (`GameOS/include/gameos.hpp:2332`); `gos_TerrainExtraReset` [R] (`GameOS/include/gameos.hpp:2333`) |
| Graphics | `gos_TerrainExtraAdd` [R] (`GameOS/include/gameos.hpp:2334`); `gos_SetTerrainBatchExtras` [R] (`GameOS/include/gameos.hpp:2335`); `gos_SetTerrainDebugMode` [R] (`GameOS/include/gameos.hpp:2336`); `gos_GetTerrainDebugMode` [R] (`GameOS/include/gameos.hpp:2337`) |
| Graphics | `gos_IsTerrainTessellationActive` [R] (`GameOS/include/gameos.hpp:2338`); `gos_IsModernShadowPathActive` [R] (`GameOS/include/gameos.hpp:2342`); `gos_GetTerrainLightDir` [R] (`GameOS/include/gameos.hpp:2345`); `gos_GetTerrainCameraPos` [R] (`GameOS/include/gameos.hpp:2349`) |
| Graphics | `gos_GetSkyNumber` [R] (`GameOS/include/gameos.hpp:2352`); `gos_SetMapHalfExtent` [R] (`GameOS/include/gameos.hpp:2355`); `gos_SetWaterElevation` [R] (`GameOS/include/gameos.hpp:2356`); `gos_StaticLightMatrixBuilt` [R] (`GameOS/include/gameos.hpp:2357`) |
| Graphics | `gos_BuildStaticLightMatrix` [R] (`GameOS/include/gameos.hpp:2358`); `gos_MarkStaticLightMatrixBuilt` [R] (`GameOS/include/gameos.hpp:2359`); `gos_GetStaticShadowGeneration` [R] (`GameOS/include/gameos.hpp:2362`); `gos_ResetStaticLightMatrix` [R] (`GameOS/include/gameos.hpp:2365`) |
| Graphics | `gos_ResetStaticShadowPriming` [R] (`GameOS/include/gameos.hpp:2369`); `gos_BeginShadowPrePass` [R] (`GameOS/include/gameos.hpp:2375`); `gos_DrawShadowBatchTessellated` [R] (`GameOS/include/gameos.hpp:2376`); `gos_EndShadowPrePass` [R] (`GameOS/include/gameos.hpp:2379`) |
| Graphics | `gos_DrawShadowObjectBatch` [R] (`GameOS/include/gameos.hpp:2383`); `gos_DrawShadowObjectBatchStatic` [R] (`GameOS/include/gameos.hpp:2392`); `gos_BeginDynamicShadowPass` [R] (`GameOS/include/gameos.hpp:2396`); `gos_EndDynamicShadowPass` [R] (`GameOS/include/gameos.hpp:2397`) |
| Graphics | `gos_BuildDynamicLightMatrix` [R] (`GameOS/include/gameos.hpp:2398`); `gos_SetWorldToClipGL` [D] (`GameOS/include/gameos.hpp:2453`); `gos_SetTerrainMVP` [R] (`GameOS/include/gameos.hpp:2458`); `gos_SetTerrainViewport` [R] (`GameOS/include/gameos.hpp:2459`) |
| Graphics | `gos_SetTerrainCameraPos` [R] (`GameOS/include/gameos.hpp:2460`); `gos_PushTerrainOverlay` [R] (`GameOS/include/gameos.hpp:2481`); `gos_PushDecal` [R] (`GameOS/include/gameos.hpp:2485`); `gos_DrawTerrainOverlays` [R] (`GameOS/include/gameos.hpp:2489`) |
| Graphics | `gos_DrawDecals` [R] (`GameOS/include/gameos.hpp:2490`); `gos_SetTerrainLightDir` [R] (`GameOS/include/gameos.hpp:2494`); `gos_SetTerrainMissionFlatLight` [R] (`GameOS/include/gameos.hpp:2499`); `gos_GetTerrainMissionFlatLight` [R] (`GameOS/include/gameos.hpp:2500`) |
| Graphics | `gos_SetTerrainDetailParams` [R] (`GameOS/include/gameos.hpp:2501`); `gos_SetTerrainMaterialNormal` [V] (`GameOS/include/gameos.hpp:2502`); `gos_SetTerrainWorldScale` [R] (`GameOS/include/gameos.hpp:2503`); `gos_SetTerrainCellBombParams` [R] (`GameOS/include/gameos.hpp:2504`) |
| Graphics | `gos_SetTerrainPOMParams` [R] (`GameOS/include/gameos.hpp:2505`); `gos_CreateTerrainNormalTexture` [V] (`GameOS/include/gameos.hpp:2506`); `gos_SetTerrainDetailNormalTexture` [V] (`GameOS/include/gameos.hpp:2507`); `gos_SetTerrainDisplacementTexture` [V] (`GameOS/include/gameos.hpp:2508`) |
| Graphics | `gos_SetTerrainViewDir` [R] (`GameOS/include/gameos.hpp:2509`); `gos_GetTerrainPhongAlpha` [R] (`GameOS/include/gameos.hpp:2512`); `gos_GetTerrainDisplaceScale` [R] (`GameOS/include/gameos.hpp:2513`); `gos_GetTerrainDetailTiling` [R] (`GameOS/include/gameos.hpp:2514`) |
| Graphics | `gos_SetTerrainShadowSoftness` [R] (`GameOS/include/gameos.hpp:2517`); `gos_GetTerrainShadowSoftness` [R] (`GameOS/include/gameos.hpp:2518`); `gos_SetTerrainDrawEnabled` [R] (`GameOS/include/gameos.hpp:2521`); `gos_GetTerrainDrawEnabled` [R] (`GameOS/include/gameos.hpp:2522`) |
| Graphics | `gos_SetHudScale` [R] (`GameOS/include/gameos.hpp:2528`); `gos_GetHudScale` [R] (`GameOS/include/gameos.hpp:2529`); `gos_SetHudScaleActive` [R] (`GameOS/include/gameos.hpp:2530`); `gos_GetHudScaleActive` [R] (`GameOS/include/gameos.hpp:2531`) |
| Graphics | `gos_SetHudScaleExempt` [R] (`GameOS/include/gameos.hpp:2538`); `gos_GetHudScaleExempt` [R] (`GameOS/include/gameos.hpp:2539`); `gos_HudInverseMousePoint` [R] (`GameOS/include/gameos.hpp:2544`); `gos_SetUiCanvasActive` [R] (`GameOS/include/gameos.hpp:2551`) |
| Graphics | `gos_ComputeUiCanvasBox` [R] (`GameOS/include/gameos.hpp:2552`); `gos_SetHudCanvasActive` [R] (`GameOS/include/gameos.hpp:2571`); `gos_SetHudCanvasExemptMode` [R] (`GameOS/include/gameos.hpp:2576`); `gos_GetHudCanvasExemptMode` [R] (`GameOS/include/gameos.hpp:2577`) |
| Graphics | `gos_ComputeHudCanvasBox` [R] (`GameOS/include/gameos.hpp:2578`); `gos_SetShadowMode` [R] (`GameOS/include/gameos.hpp:2581`); `gos_GetGLTextureName` [D] (`GameOS/include/gameos.hpp:2588`); `gos_SetRenderState` [V] (`GameOS/include/gameos.hpp:2595`) |
| Graphics | `gos_PushRenderStates` [D] (`GameOS/include/gameos.hpp:2602`); `gos_PopRenderStates` [D] (`GameOS/include/gameos.hpp:2603`); `gos_SetViewport` [R] (`GameOS/include/gameos.hpp:2838`); `gos_SetTransform` [R] (`GameOS/include/gameos.hpp:2843`) |
| Graphics | `gos_MultiplyTransform` [R] (`GameOS/include/gameos.hpp:2848`); `gos_SetLight` [R] (`GameOS/include/gameos.hpp:2853`); `gos_LightEnable` [R] (`GameOS/include/gameos.hpp:2858`); `gos_SetMaterial` [R] (`GameOS/include/gameos.hpp:2863`) |
| Graphics | `gos_getRenderMaterial` [R] (`GameOS/include/gameos.hpp:2873`); `gos_ApplyRenderMaterial` [R] (`GameOS/include/gameos.hpp:2874`); `gos_SetRenderMaterialParameterFloat4` [R] (`GameOS/include/gameos.hpp:2875`); `gos_SetRenderMaterialParameterMat4` [R] (`GameOS/include/gameos.hpp:2876`) |
| Graphics | `gos_SetRenderMaterialParameterInt` [R] (`GameOS/include/gameos.hpp:2877`); `gos_SetRenderMaterialSamplerUnit` [D] (`GameOS/include/gameos.hpp:2878`); `gos_SetRenderMaterialUniformBlockBindingPoint` [D] (`GameOS/include/gameos.hpp:2879`); `gos_SetCommonMaterialParameters` [R] (`GameOS/include/gameos.hpp:2880`) |
| Graphics | `gos_SetupObjectShadows` [R] (`GameOS/include/gameos.hpp:2882`); `gos_LightDataSsbo_Upload` [R] (`GameOS/include/gameos.hpp:2886`); `gos_LightDataSsbo_UploadSplit` [R] (`GameOS/include/gameos.hpp:2891`); `gos_LightDataSsbo_Destroy` [R] (`GameOS/include/gameos.hpp:2893`) |
| Graphics | `gos_BindLightDataStorageBlock` [D] (`GameOS/include/gameos.hpp:2894`); `gos_UploadObjectLightPools` [R] (`GameOS/include/gameos.hpp:2897`); `gos_CreateVertexBuffer` [R] (`GameOS/include/gameos.hpp:2907`); `gos_DeleteVertexBuffer` [R] (`GameOS/include/gameos.hpp:2914`) |
| Graphics | `gos_LockVertexBuffer` [R] (`GameOS/include/gameos.hpp:2923`); `gos_UnLockVertexBuffer` [R] (`GameOS/include/gameos.hpp:2930`); `gos_VertexBuffersLost` [R] (`GameOS/include/gameos.hpp:2939`); `gos_RenderVertexBuffer` [R] (`GameOS/include/gameos.hpp:2946`) |
| Graphics | `gos_CreateBuffer` [R] (`GameOS/include/gameos.hpp:2982`); `gos_DestroyBuffer` [R] (`GameOS/include/gameos.hpp:2983`); `gos_UpdateBuffer` [R] (`GameOS/include/gameos.hpp:2984`); `gos_BindBufferBase` [D] (`GameOS/include/gameos.hpp:2985`) |
| Graphics | `gos_GetBufferSizeBytes` [R] (`GameOS/include/gameos.hpp:2987`); `gos_CreateVertexDeclaration` [R] (`GameOS/include/gameos.hpp:2989`); `gos_DestroyVertexDeclaration` [R] (`GameOS/include/gameos.hpp:2990`); `gos_SetHudResClampEnabled` [R] (`GameOS/include/gameos.hpp:3036`) |
| Graphics | `gos_GetHudResClampEnabled` [R] (`GameOS/include/gameos.hpp:3042`); `gos_BeginCameraPreviewRender` [R] (`GameOS/include/gameos.hpp:3059`); `gos_EndCameraPreviewRender` [R] (`GameOS/include/gameos.hpp:3060`); `gos_GetCameraPreviewTexture` [V] (`GameOS/include/gameos.hpp:3061`) |
| Graphics | `gos_SetGammaValue` [R] (`GameOS/include/gameos.hpp:3066`); `gos_SetBrightnessValue` [R] (`GameOS/include/gameos.hpp:3067`); `gos_SetContrastValue` [R] (`GameOS/include/gameos.hpp:3068`); `gos_NewTextureFromFile` [R] (`GameOS/include/gameos.hpp:3180`) |
| Graphics | `gos_NewTextureFromMemory` [R] (`GameOS/include/gameos.hpp:3196`); `gos_NewCompressedTexture2D` [V] (`GameOS/include/gameos.hpp:3206`); `gos_NewEmptyTexture` [R] (`GameOS/include/gameos.hpp:3230`); `gos_DestroyTexture` [R] (`GameOS/include/gameos.hpp:3235`) |
| Graphics | `gos_LockTexture` [R] (`GameOS/include/gameos.hpp:3246`); `gos_UnLockTexture` [R] (`GameOS/include/gameos.hpp:3251`); `gos_GetTextureGLId` [D] (`GameOS/include/gameos.hpp:3258`); `gos_ConvertTextureRect` [R] (`GameOS/include/gameos.hpp:3263`) |
| Graphics | `gos_RecreateTextureHeaps` [R] (`GameOS/include/gameos.hpp:3271`); `gos_PreloadTexture` [R] (`GameOS/include/gameos.hpp:3276`); `gos_SetTextureName` [R] (`GameOS/include/gameos.hpp:3278`); `gos_GetTextureName` [R] (`GameOS/include/gameos.hpp:3279`) |
| Graphics | `gos_StartRenderToTexture` [R] (`GameOS/include/gameos.hpp:3295`); `gos_EndRenderToTexture` [R] (`GameOS/include/gameos.hpp:3301`) |
| Debug | `gos_srand` [X] (`GameOS/include/gameos.hpp:1952`); `gos_rand` [X] (`GameOS/include/gameos.hpp:1956`); `gos_ReturnStatistic` [R] (`GameOS/include/gameos.hpp:3373`); `gos_MathExceptions` [R] (`GameOS/include/gameos.hpp:3444`) |
| Debug | `gos_GetMathExceptions` [R] (`GameOS/include/gameos.hpp:3445`); `gos_EnableSetting` [R] (`GameOS/include/gameos.hpp:3485`); `gos_GetMachineInformation` [R] (`GameOS/include/gameos.hpp:3568`) |

D removes GL object names, binding/reflection operations and mutable state-cache plumbing, not textures, lighting, editor previews or camera transforms.
Evidence: raw buffer binding `GameOS/gameos/gameos_graphics_capi.cpp:605`, program reflection `:704`, matrix conversion `GameOS/gameos/gameos_graphics_tparams.cpp:555`.
V replacements use TextureRef/PixelFormat/material records, UI image descriptors and one canonical camera transform; no GLuint or GL enum crosses the new API.
Other graphics R capabilities move to explicit Vulkan frame/pass records and owners, including fonts/HUD, light pools, render-to-texture and shadows.
Existing GL bodies of R/V die too; feature deletion needs a caller/content audit, never inference from a header or a zero-reference grep.
Move enums/records/packing with their domain; retain fixed-width on-disk layouts in Core readers, not public Win32 aliases or header-wide pack pragmas (`GameOS/include/gameos.hpp:75`).

## LINUX_BUILD: preserve, then name the actual policy

Census above finds 25 tracked files with conditional arms (ARM 1, GameOS 4, code 8, mclib 12). Comments/definitions are excluded from this conditional count.
Root defines LINUX_BUILD unconditionally (`CMakeLists.txt:204`); editor also defines it as 0 (`editor/CMakeLists.txt:70`). Defined-for-every-target does not mean value 1 everywhere.
First rename to MC2_PORTABLE_RUNTIME with identical per-target presence/value; preserve #ifdef/#ifndef versus #if semantics and every arm, including inactive ones.
Preprocess before/after on every affected game/editor/tool TU under both compiler presets; compare token streams excluding paths/line markers, and record effective definitions.
Do not define the portable arm only on Linux, change editor's 0 to 1, or convert #ifdef to #if as part of the rename.

| Purpose to extract after equivalent rename | Existing anchors and disposition |
|---|---|
| Canonical content paths | `mclib/paths.h:49`, `mclib/ablscan.cpp:679`, `GameOS/gameos/utils/file_utils.cpp:10`: preserve forward-slash content paths on every host; native path conversion is Platform. |
| Portable declarations/includes | `GameOS/include/platform_winnt.h:4`, `ARM/Microsoft.Xna.Arm.h:11`, `mclib/msl.h:29`, `mclib/msl_internal.h:69`, `mclib/estring.cpp:19`, `mclib/mlr/mlr.hpp:14`, `mclib/stuff/iterator.hpp:19`: migrate types/includes, not fake Windows SDK APIs. |
| Scalar versus legacy asm | `mclib/stuff/mstring.hpp:526`, `mclib/routines.cpp:26`, `mclib/vfx_ellipse.cpp:10`: keep the portable arm; no x86 resurrection. |
| SDL presentation versus DirectDraw/timer | `mclib/mouse.cpp:49`, `code/mainmenu.cpp:9`, `code/loadscreen.cpp:9`, `:675`, `code/saveload_internal.h:125`: SDL input and Vulkan snapshots replace legacy surfaces. |
| Diagnostics/native remnants | `GameOS/gameos/gameos_debugging.cpp:3`, `:25`, `GameOS/gameos/gameos_graphics_material_internal.h:35`, `code/dw.cpp:14`, `mclib/msodw.h:122`, `code/mechcmd2.cpp:251`, `:304`, `code/mechcmd2_cmdline.cpp:356`: native services move to Platform; the &&0 arm stays inactive during rename. |
| Frozen MP path | `code/logisticsdata.cpp:128`, `code/multplyr_internal.h:13`, `:103`: remain frozen; redesign transport separately, not as a portability shortcut. |

Eventually replace shared policy tests with unconditional portable code or private OS selection (_WIN32/__linux__/__APPLE__) after capability proofs.
Rename alone proves no Linux runtime support; surviving branches need their own compile/corpus evidence. No blanket deletion of the old portability arm.

## Manifest and presets (proposed files; not installed here)

Add root vcpkg.json, a pinned registry baseline, CMakePresets.json and ignored CMakeUserPresets.json; user file supplies VCPKG_ROOT/SDK/compiler locations.
The schematic manifest below needs a real audited 40-hex baseline before configuring; do not publish an unpinned or placeholder manifest.

```json
{"name":"mc2","version-string":"final-migration","builtin-baseline":"<audited-40-hex-vcpkg-commit>",
 "dependencies":["sdl2",{"name":"sdl2-mixer","default-features":false},"sdl2-ttf","zlib","glm","assimp",
 {"name":"ffmpeg","default-features":false,"features":["avcodec","avformat","swresample","swscale"]}]}
```

Use find_package imported targets and target-scoped includes/options; remove bootstrap hardcoded library/cache paths in the same batch, not duplicate dependency discovery.
Vulkan headers/loader/shader tools come from the pinned SDK; source-pin imgui, Tracy, volk, VMA, vk_bootstrap, stb, tinyexr and meshoptimizer with revisions/licenses.
During pre-R1 migration add glew as a temporary manifest feature dependency; remove it and OpenGL discovery/linkage at GL deletion (`CMakeLists.txt:222`, `:697`).
Replace vendored Assimp atomically, preserving importer formats/options and corpus results; one zlib provider, no old dependency cache mixed with vcpkg.
FFmpeg must stay shared with the required BIK/WAV decoding enabled; no gpl/nonfree feature bundle. Configure/link/runtime codec probes and all video scenarios are gates.
Current FFmpeg discovery is explicitly win64 DLL/.lib-only (`cmake/FindFFmpegLGPL.cmake:9`, `:20`); replace it and DLL delay-load/copy policy (`CMakeLists.txt:724`) for each OS.
Port feature names were checked against the [official FFmpeg port](https://github.com/microsoft/vcpkg/blob/master/ports/ffmpeg/vcpkg.json); pin the audited revision, not master.
Use separate installed/build roots and cache keys including baseline/features/triplet/compiler/CRT. Windows uses x64-windows with /MD; Linux adds an overlay x64-linux-mc2 shared-library triplet.

Minimal CMake preset gate, schema 6 / CMake 3.25+, in an initialized x64 VS environment with LLVM/Ninja available:

```json
{"version":6,"cmakeMinimumRequired":{"major":3,"minor":25,"patch":0},
 "configurePresets":[
  {"name":"base","hidden":true,"generator":"Ninja","binaryDir":"${sourceDir}/out/${presetName}",
   "cacheVariables":{"CMAKE_BUILD_TYPE":"RelWithDebInfo","CMAKE_EXPORT_COMPILE_COMMANDS":"ON","MC2_CXX_STANDARD":"23",
    "CMAKE_TOOLCHAIN_FILE":"$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake","MC2_IMGUI":"ON","MC2_EDITOR":"ON",
    "MC2_EDITOR_IN_ALL":"ON","ENABLE_MC2FX":"ON","ENABLE_MC2FX_PREVIEW":"ON","ENABLE_MC2WEAPON_GUI":"ON"}},
  {"name":"win","hidden":true,"inherits":"base","condition":{"type":"equals","lhs":"${hostSystemName}","rhs":"Windows"},
   "cacheVariables":{"VCPKG_TARGET_TRIPLET":"x64-windows","CMAKE_MSVC_RUNTIME_LIBRARY":"MultiThreadedDLL"}},
  {"name":"msvc-ninja-dev","inherits":"win","cacheVariables":{"CMAKE_C_COMPILER":"cl","CMAKE_CXX_COMPILER":"cl","MC2_PCH":"ON"}},
  {"name":"clang-cl-dev","inherits":"win","cacheVariables":{"CMAKE_C_COMPILER":"clang-cl","CMAKE_CXX_COMPILER":"clang-cl","MC2_PCH":"ON"}},
  {"name":"msvc-asan","inherits":"msvc-ninja-dev","cacheVariables":{"MC2_ASAN":"ON"}},
  {"name":"msvc-release","inherits":"msvc-ninja-dev"}],
 "buildPresets":[{"name":"msvc-ninja-dev","configurePreset":"msvc-ninja-dev","targets":["all","ui_editor"]},
  {"name":"clang-cl-dev","configurePreset":"clang-cl-dev","targets":["all","ui_editor"]},
  {"name":"msvc-asan","configurePreset":"msvc-asan","targets":["all","ui_editor"]},
  {"name":"msvc-release","configurePreset":"msvc-release","targets":["all","ui_editor"]}]}
```

All presets use RelWithDebInfo, including the release packaging preset; sanitizer disables Tracy via existing policy (`CMakeLists.txt:206`).
Compiler-private PCHs are built per preset, never reused across compilers. [CMake preset reference](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html) defines inheritance/build selection.
Add linux-gcc inheriting base with gcc/g++, Linux condition, x64-linux-mc2 triplet and its overlay directory; no Windows runtime cache settings.
Enable it only once E1 and the dependency/platform batches expose all product targets on Linux; initial game-only probes are explicitly partial.

## clang-cl first-build gate and expected failures

Host inventory reports clang-cl present, not execution-validated (`docs/host-tools.md:12`, `:31`). X1 is the approved second-compiler/Linux arc, not an earlier ad hoc compiler rollout.
Use `cmake --preset clang-cl-dev` then `cmake --build --preset clang-cl-dev`; save configure/build logs and actual compiler, linker, STL and SDK versions.
OS predicates use WIN32; compiler switches use compiler ID plus frontend variant. CMake's [MSVC variable](https://cmake.org/cmake/help/latest/variable/MSVC.html) includes simulating compilers.
Root and standalone currently force /std:c++23preview for MSVC-like tools (`CMakeLists.txt:18`, `cmake/Mc2Standalone.cmake:25`); restrict that override to compiler ID MSVC.
Let CMake select Clang's supported C++23 flag and verify __cplusplus/required std::format/expected with compile/link probes; never lower the project standard.
Expected classes (hypotheses until that first build): unsupported /MP or MSVC-only /we flags; clang flag translation; pragmas/packing/calling conventions/intrinsics and inline asm;
ill-formed narrowing, format/lifetime errors, two-phase lookup/missing includes; MFC/resource entry/link issues; incorrect import/runtime DLL discovery; compiler FP/digest differences.
Fix owned code or compiler-specific flags; no global -fpermissive, -Wno-c++11-narrowing, disabling editor, or broad warning suppression to make the gate pass.
Current GCC branch already uses -fpermissive (`CMakeLists.txt:64`); remove it with fixes. Keep vendor warnings scoped to vendor targets.
Do not assume VEH replaces every __try safely: video probes (`code/mc2video.cpp:94`) become explicit loader errors; crash-time guards (`GameOS/gameos/gos_crashbundle.cpp:211`) stay private or get reviewed recovery semantics.
Extend build_all_targets with a proposed --preset argument and separate compiler trees for root, every standalone project and harness/unit stage.
Today it hardcodes vcvars/root tree and VS standalone generation (`tools/build_all_targets.py:45`, `:72`); root-only clang-cl builds are insufficient.
The harness builder must propagate the same compiler/toolchain too, execute unit/harness tests, and fail on skipped requested targets; no silent MSVC fallback.

## Batches and proof obligations (implementation recipes, not claims of runs)

1. Freeze Windows baselines after S1/S2/R1 prerequisites; record target/caller manifest, options, content identity, compiler/FP policy, tier1 logs, editor workflows and video probes.
2. Land manifest/presets/compiler dispatch and all-target builder propagation; bring clang-cl to green before API moves so future diagnostics have a trustworthy baseline.
3. Rename LINUX_BUILD equivalently across root/editor/standalones; preprocessing equality plus all-targets on both compilers; keep inactive arms and numeric values.
4. Split header/types without behavior changes, migrate includers domain by domain, and add firewall fixtures; compare rebuild/parse cost with the same touched header and target set.
5. Extract Platform ownership and SDL window/input/time/file/audio/debug services; remove native dependencies from callers and link interfaces; prove headless has no SDL/GPU/clock path into Sim.
6. Complete E1/editor and R1 semantic replacements before removing D/V signatures, GL twins, GLEW and the aggregate header; audit callers, link symbols and content before deletion.
7. Native Linux GCC configure/all-targets, doctest/harness, headless/windowed replay, saves, tier1, editor workflows, movies, mods, case-sensitive VFS and install packaging; then macOS.
Every batch: offline contracts first (orchestrator's `bash scripts/check-contracts.sh`), proposed `py -3 tools/build_contract_harnesses.py --preset <compiler-preset> --run --unit` for each compiler,
then proposed `py -3 tools/build_all_targets.py --preset msvc-ninja-dev --reconfigure` and `py -3 tools/build_all_targets.py --preset clang-cl-dev --reconfigure` (all three stages).
Those --preset options do not exist yet; batch 2 must implement them in both proof tools and prove propagation. ASan unit tier remains a separate required ownership proof.
Orchestrator commits before deploy; harness-only, background, host-lease launches of separate compiler installs, never the golden install or direct exe launches.
Run unchanged tier1: `py -3 scripts/run_smoke.py --tier tier1 --duration 30 --keep-logs --exe <compiler-install>/mc2.exe` with MC2_DEBUG_STATE_DUMP=1 and MC2_DIAG_TAGS=CONFIG,BUILD,DEVICE.
Require the same missions, grading thresholds, pinned 1080p/options/content, fingerprint, backend readback and 5/5 PASS on both; no reduced tier or relaxed oracle.
Also `py -3 scripts/run_editor_smoke.py --exe <compiler-install>/EditRel.exe`; every editor workflow stays must-have, plus campaign/won/in-mission video play evidence and mod install coverage.
Native compiler parity needs identical authoritative S1 digests for the same tick/order/seed/content corpus and save/resume; rendering A/B is viewed, not accepted from MAD alone.
FP differences are failures to investigate; cross-compiler identity is an explicit gate, not something clang-cl compilation proves. No silent SIMID rebaseline.

## macOS later, risks, and owner questions

macOS adds AppleClang presets (arm64, x64 if supported), shared dependency triplets, SDK/deployment target and MoltenVK/Metal Vulkan surface/device portability support.
Negotiate the required Vulkan profile against MoltenVK; log and reject unsupported capabilities rather than inventing a second renderer. Prove all targets, editor, movies and replay on native hardware.
Add app bundle/resource/runtime-library lookup, DPI/text composition/audio, filesystem case/Unicode behavior and packaging/signing; native crash reporting substitutes for Win32 crash bundles.
Risks: fake Win32 types may hide serialized width assumptions; codecs/importers may alter mod pixels/meshes; MFC/SDL callbacks and asynchronous IO may outlive their owners;
dependency DLL/CRT mixing can fail only at runtime; removing GL state may expose live editor/font/VFX contracts; cross-platform FP and path case are unproven.
Mitigation is per-domain corpus/teardown tests plus unchanged product floors, not retaining static owners or manufacturing compatibility wrappers.
No design-changing owner question is open: Linux/macOS, preserved arms, editor/video scope and final Vulkan are already ruled. Revision pins and diagnostic fixes are implementation choices.
No builds, contracts, doctest, deployments or smokes were run for this design; builds/mutations were outside this read-only task and Bash/index writes are forbidden here.
