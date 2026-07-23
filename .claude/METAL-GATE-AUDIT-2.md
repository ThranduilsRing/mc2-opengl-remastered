# METAL-GATE-AUDIT-2 (2026-07-09, HEAD 8eeae587) — supersedes the pre-strict audit

Read-only audit of the CURRENT strict VK stack for MoltenVK/macOS. Full evidence tables live in the
session transcript; this file records the verdicts + the three schedule risks. UNTRACKED — pending
adversarial+meta review per standing rule.

## Verdict summary

MoltenVK-CLEAN as code stands: zero optional device features enabled (`vulkan_context.cpp:310-317`
never sets pEnabledFeatures), no geometry/tessellation/compute stages (HZB is frag-based reduce),
VK 1.1 core, binary semaphores only (no timeline), classic render passes (no dynamic rendering),
push constants ≤64B, readonly SSBOs only, SPIR-V 1.0 via glslangValidator -V, strict-path formats
all Metal-supported (RGBA16F / R32_UINT / D32_SFLOAT_S8_UINT / D32 / BC7 / BC6H — BCn = Mac2 family,
Apple-silicon macOS OK, min-version NEEDS-VERIFY).

BLOCKERS (all in the deletable interop family):
- VK_KHR_external_memory_win32 + VK_KHR_external_semaphore_win32 (+ their capabilities instance exts)
  enabled unconditionally via wantInterop at vulkan_context.cpp:159-163 / 295-304. Does not exist in
  MoltenVK → vkCreateDevice fails outright.
- VK_KHR_win32_surface + raw vkCreateWin32SurfaceKHR on the SDL window's HWND
  (engine_present_vk.cpp:1577-1608) → needs SDL_Vulkan_GetInstanceExtensions/CreateSurface
  (VK_EXT_metal_surface) platform branch.

NEEDS-VERIFY: copy-2 vkCmdBlitImage RGBA16F→BGRA8 on MoltenVK (engine_present_vk.cpp:2196); BC6H/BC7
min macOS; non-FIFO present modes (ladder flags non-FIFO UNVERIFIED, VK-RUNG8-LADDER-1.md:40-42);
gpu-assisted/debug-printf validation presets (dev-only).

## 8.7 deletion inventory — correction + boundary

vulkan_postprocess_subgraph.cpp and the 4 old islands are ALREADY DELETED (8.7a-8.7h per
VK-RUNG8-LADDER-1.md:275-374). Remaining deletables (see transcript for full 11-row table with LOC +
covering proofs): gl_interop_view.cpp whole TU (~484 LOC), exportable half of vk_image_resource
(~120 of 269 — non-exportable core SURVIVES, strict stack allocates through it), wantInterop
plumbing (~35), per-island GL↔VK semaphore handoff branches (~600-900 across 9 TUs), GL-import
halves of shared scene resources (~150), static-shadow import substrate (~130 — DECISION: native
non-CSM screen-shadow configs need it), terrain CSM copy-mode (~200-350), present interop tail
(~150-250; copy-2 blit + Win32 surface SURVIVE on Windows), rung-7 GL-readback mech fallbacks
(~120), ~20 vestigial gates. Pure-GL backend untouched — only the GL↔VK bridge dies.

## Top-3 Metal-gate risks (schedule-shaping)

1. **Fail-soft semantics assume a GL rescue.** wantInterop=true hardwired at gameosmain.cpp:1206 +
   vulkan_context.cpp:406; every island decline path falls back to GL. On macOS there is no GL —
   8.7-final must redefine decline/fail-soft (strict canaries become product behavior), not just
   delete extensions.
2. **Asset-feed layer is GL-dependent and survives 8.7.** All texture/geometry feeds are copy-once
   readbacks out of LIVE GL objects (glGetTexImage/glGetCompressedTexImage/glGetBufferSubData:
   terrain 1269/1331/1511/1591/1714/1756, prop 667, water 467, mech 502/812-855). Only the mech
   vertex/light feed has a native source (MC2_VK_MECH_FEED_NATIVE). A GL-free asset-sourcing layer
   (load KTX2/BC direct from disk → VK) is NEW work, larger than the 8.7 deletion, on no ladder yet.
   THIS is the long pole for macOS.
3. **Win32 present-surface plumbing** — small SDL_Vulkan fix, but inside the resize/direct-FBO/
   strict-present interlock that 8.7q already bug-fixed once; non-FIFO present matrix unverified.
