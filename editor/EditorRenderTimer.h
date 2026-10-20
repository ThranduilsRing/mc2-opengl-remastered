#pragma once
// CPU render-phase timing ([EDCPU]), plus native GPU pass collection.
// The existing MC2_EDITOR_GPU_TIMERS diagnostic switch enables collection.
// GPU values are published by gos_render_pass_timer from Vulkan timestamps;
// this module never labels CPU elapsed time as GPU time.
void EditorRenderTimer_Begin();
void EditorRenderTimer_Mark(const char* name);
void EditorRenderTimer_End();

//----------------------------------------------------------------------------
// CPU-only phase timer (no GL) for the PRE-render work in Editor::update
// (land->update / land->geometry / etc.). Emits "[EDPRE f=N] ..." lines.
// Same env gate (MC2_EDITOR_GPU_TIMERS=1). Use independently of the GPU timer.
//----------------------------------------------------------------------------
void EditorCpuPhase_Begin();
void EditorCpuPhase_Mark(const char* name);
void EditorCpuPhase_End();

//----------------------------------------------------------------------------
// Whole-frame phase timer (CPU wall-clock) for RunGameOSLogic: partitions the
// full frame period (BeginFrame/DoGameLogic/UpdateRenderers/EndFrame/endScene/
// gui/swap) so we can find time spent OUTSIDE the render passes (e.g. a
// SwapBuffers GPU-wait stall). Emits "[EDFRM f=N dt=..] ..." lines.
//----------------------------------------------------------------------------
void EditorFramePhase_Begin();
void EditorFramePhase_Mark(const char* name);
void EditorFramePhase_End();
