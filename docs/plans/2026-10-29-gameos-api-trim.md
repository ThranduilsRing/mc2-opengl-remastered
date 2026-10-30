# GameOS public API trim

Base: `c7ef9a666`. Rechecked names with `rg -n -w` over every repository
C/C++ source/header plus tool scripts, CMake, resource and export files; this
includes code, mclib, gui, editor, tools, GameOS, viewer and test targets.
The historical survey counts changed: 62 names have no implementation or caller;
10 more have definitions or declarations only, with no executable caller.
Deleted all 72 public declarations and the ten remaining function bodies.

Three internally used functions retain their implementations and private declarations:
`gos_GetHudResClampEnabled`, `gos_ProbeDrawableSize`, and
`gos_GetTerrainMissionFlatLight`. They are no longer public GameOS API.
The flat-light getter already has a private declaration in the terrain chunk header.

Removed names (including declaration-only APIs):

- `gosAudio_DeviceOptions`
- `gosAudio_GetChannelInfo`
- `gosAudio_GetResourceInfo`
- `gosAudio_Reset`
- `gosAudio_SetSpeakerConfig`
- `gos_AbortTermination`
- `gos_ApplyRenderMaterial`
- `gos_CheckValidTGA`
- `gos_CloseBackgroundFile`
- `gos_ConvertTextureRect`
- `gos_CreateVertexBuffer`
- `gos_DeleteVertexBuffer`
- `gos_DrawFans`
- `gos_DrawStrips`
- `gos_EndRenderToTexture`
- `gos_ForceApplyRenderStates`
- `gos_GetBufferSizeBytes`
- `gos_GetMathExceptions`
- `gos_GetRenderViewport`
- `gos_GetResourceData`
- `gos_GetTextureName`
- `gos_GetUnformattedDate`
- `gos_LightDataSsbo_Destroy`
- `gos_LightDataSsbo_Upload`
- `gos_LightDataSsbo_UploadSplit`
- `gos_LightEnable`
- `gos_LockVertexBuffer`
- `gos_MathExceptions`
- `gos_MultiplyTransform`
- `gos_NetEndGame`
- `gos_NetGetMessage`
- `gos_NetInformation`
- `gos_NetSendMessage`
- `gos_NetServerCommands`
- `gos_PreloadTexture`
- `gos_ReadFileInBackground`
- `gos_RecreateTextureHeaps`
- `gos_RenderVertexBuffer`
- `gos_ReturnStatistic`
- `gos_RunMainLoop`
- `gos_SaveStringToRegistry`
- `gos_SetBrightnessValue`
- `gos_SetCommonMaterialParameters`
- `gos_SetContrastValue`
- `gos_SetGammaValue`
- `gos_SetIMEInsertMode`
- `gos_SetLight`
- `gos_SetMaterial`
- `gos_SetRenderMaterialParameterFloat4`
- `gos_SetRenderMaterialParameterInt`
- `gos_SetRenderMaterialParameterMat4`
- `gos_SetRenderMaterialSamplerUnit`
- `gos_SetRenderMaterialUniformBlockBindingPoint`
- `gos_SetShadowMode`
- `gos_SetTerrainBatchExtras`
- `gos_SetTerrainMVP`
- `gos_SetTerrainViewport`
- `gos_SetTerrainWorldScale`
- `gos_SetTextureName`
- `gos_SetTransform`
- `gos_SetViewport`
- `gos_StartRenderToTexture`
- `gos_TerrainExtraAdd`
- `gos_TerrainExtraReset`
- `gos_TextDrawV`
- `gos_TextGetPrintPosition`
- `gos_UnLockVertexBuffer`
- `gos_UpdateBuffer`
- `gos_UploadObjectLightPools`
- `gos_UserExit`
- `gos_VertexBuffersLost`
- `gos_getRenderMaterial`


## Packet 2: bounded convenience and thread ownership

`cLoadString(id)` returns the existing owning resource value. The capacity
overload delegates to the original bounded loader, preserving empty results
and diagnostics for missing or oversized mod strings. Four fixed-buffer
display sites use that overload; mutating and caller-owned buffers stay bounded.
The resource-string corpus checks both ownership and the old rejection boundary.

Both editor background routines use `std::jthread` and stop-aware shutdown.
The smoke driver joins after editor teardown; its normal delays and exit
deadlines are unchanged. The simulation worker retains its explicit 8 MB stack.

The GL-removal audit branch still declared both Vulkan options. They are now
fixed normal CMake variables, so old cache entries cannot disable the required
backend or its native regions. Existing compile definitions and target wiring
remain intact for standalone tools.
