# VK-P14b PRESENT TAIL — recon verdict (the present/flip tail)

Verified @ `4f853e0d`. The last-unknown endgame piece; scopes VK-owns-present.

## ★KEY: the final-composite seam is ONE clean bind — and copy-1-elim can land NOW, independent of geometry
- **Single seam:** `gosPostProcess::endScene()` binds the backbuffer ONCE at `gos_postprocess.cpp:4038`
  (`glBindFramebuffer(GL_FRAMEBUFFER,0)`, or `getGLBackend().bindBackbuffer()` under MC2_RENDER_BACKEND_IFACE
  at :4036). Everything after inherits it: pillarbox/FORCE-43 (:4040-4095), composite/tonemap/FXAA/viewmode
  draw (:4109-4217), then HUD (`gos_RendererFlushHUDBatch` gameosmain.cpp:639), ImGui (`GuiRuntime::Render`
  :643), projectz overlay (:636) — NONE rebind. Other `glBindFramebuffer(...,0)` in the file (1390/1959/5105/
  5436) are unrelated shadow/setup teardowns. NOT smeared → the prior VK-P14-RECON MEDIUM risk resolves GREEN.
- **Nothing in the post/HUD tail must move to VK** for VK to own present. Even with terrain+props+mechs
  VK-native in the shared GBuffer, the whole back half of endScene (HZB/SSAO/screen-shadow/cloud/shoreline/
  box-decals/fog → composite/tonemap → pillarbox → HUD/ImGui) STAYS GL, sampling the shared GBuffer and
  writing the final frame into the shared exportable present image. VK-owns-present ≠ VK-runs-post. VK owns
  present the moment the shared image holds the finished frame + VK does the swapchain blit + present (copy-2,
  already true today). Native VK post (P6/P7) = optimization, NOT a present-ownership blocker.

## SLICE: `MC2_VK_PRESENT_DIRECT_FBO` (copy-1 elimination) — Size S–M, default-OFF, LANDABLE NOW
When MC2_RENDER_BACKEND=vulkan + gate on: redirect the `:4038` bind to `EnginePresentVk`'s `s.copyFbo` (the
FBO already wrapping the shared exportable present image, COLOR_ATTACHMENT, FRAMEBUFFER_COMPLETE) instead of
FBO-0. Then the shared image IS the finished frame → `presentGLFrame` SKIPS copy-1 (engine_present_vk.cpp:478-497)
→ only interop-signal (:503-504) + copy-2 (:554-564) remain. Move the Y-flip from copy-1 into copy-2
(swap Y in region.srcOffsets/dstOffsets :556-560). Retires the REMOVABLE half of the present tax; converts
the shared image from a staging copy into the frame's true render target — a real standalone step to
VK-owns-present. Copy-2 stays (irreducible until VK produces every pixel).
- Proof: existing `verifyReadbackContent()` (engine_present_vk.cpp:658) + present_frames/readback_changed +
  backend-compare bookmarks. No new fixtures.
- ★INDEPENDENT of terrain/props/mechs — operates on the final-composite→present handoff which exists today
  regardless of scene source. CAN LAND IN PARALLEL with the geometry regions, anytime.

## TOP-3 RISKS
1. Y-flip must move to copy-2 in the SAME commit or the frame presents upside-down.
2. Format/sRGB: FBO-0 default encoding vs the RGBA8 shared image must match or colors shift — verify.
3. FBO-0-assumption scan: the OOB-letterbox `glClear` (:4069-4071) + any `glReadBuffer(GL_BACK)` readbacks
   must tolerate a non-default FBO target (mostly automatic via inherited bind, but scan).

## Evidence: gos_postprocess.cpp:4025-4217 (endScene composite tail), gameosmain.cpp:636-643 (HUD/ImGui/overlay),
engine_present_vk.cpp:478-497 (copy-1) / :503-504 (signal) / :554-564 (copy-2) / :658 (verify).
