# P3 recon — SSAO island onto the zero-copy seam

**Verdict: GREENFIELD-EASY.** Copy the edge-fog template. Gate `MC2_VK_SSAO_INTEROP` (default OFF).
Build on POST-P1.5 primitives.

## SSAO island I/O (`GameOS/gameos/vulkan_ssao_island.cpp`)
- Inputs (readback today): DEPTH D32_SFLOAT `glGetTexImage` :1047-48; NORMAL RGBA16F :1052-53; SCENE
  COLOR RGBA16F :1057-58. Uploaded via `vkCmdCopyBufferToImage` :1229/:1236/:1243.
- Internal: OCCLUSION R16_SFLOAT half-res — VK-owned between pass1→pass2, no readback (untouched).
- Output: blended COLOR RGBA16F — `vkCmdCopyImageToBuffer` :1285 → CPU → `glTexSubImage2D` :1343.

## P3 scope = COLOR I/O zero-copy only
Depth + normal inputs STAY readback (depth → P5; normal maybe later). Exactly like edge-fog kept depth
on readback. Occlusion is internal (no bridge). So P3 zero-copies: scene-color IN + blended-color OUT.
- Formats all color-like/exportable (RGBA16F ↔ GL_RGBA16F). Depth D32 sampled-only (T0-proven, but not
  P3).

## Device / exts
SSAO builds its OWN VkInstance/VkDevice (NOT shared with edge-fog): instance exts :445-468 = debug-utils
only; device exts :543-547 = empty. **Must add the 6 external-memory/-semaphore exts conditionally**
(gate ON), mirroring edge-fog :449-451 (instance caps) + :571-574 (device). Gate OFF → no probe →
byte-identical (blocker #6). LUID match in glInteropViewCreate (own device) — single-GPU auto-passes,
multi-GPU fail-soft to readback.

## Forbidden-counters (color path → 0)
`glGetTexImage(color)` :1057, `vkCmdCopyBufferToImage(color)` :1243, `vkCmdCopyImageToBuffer(color)`
:1285, `glTexSubImage2D(color)` :1343. Depth/normal counters out of scope (stay readback).

## Oracle
SSAO is parity-proven **5/5** (commit a883801b): 4 byte-exact + `highangle_wide` WITHIN declared
tolerance-floor (±1 LSB, ≤0.05% px — transcendental AMD/glslang divergence, deterministic). **Reproduce
the floor exactly, do NOT try to improve it.** Command:
```
$env:MC2_SSAO="1"; $env:MC2_POSTPROCESS_BACKEND="vk"; $env:MC2_VULKAN_VALIDATION="sync"
py -3 scripts/run_golden_parity.py mc2_01 --backend-compare --region PostprocessSSAO --exe <islandExe>
# expect 5/5 (4 byte-exact + highangle_wide within floor); health [VK_SSAO_ISLAND] used_vulkan>0 val=0
```
Run the offline transport harness (RGBA16F) FIRST (seconds), golden-parity as final confirm.

## Task breakdown (mirror edge-fog T3-T5)
T3a exts+gate on SSAO device → T3b color VkImageResource+GlInteropView, repoint pass2 framebuffer
attachment 0 → T4a GL→VK color input (drop glGetTexImage(color)+copyBufToImg) → T4b VK→GL color output
(drop copyImgToBuf :1285 + texSubImage :1343, use glInteropWaitFromVk) → T5 forbidden-counters + asserts +
health → T6 gate register/doc → offline harness + golden-parity, STOP-gate.

## Top risk
Depth readback may become the frame bottleneck once color is zero-copy (SSAO reads full-res depth) — NOT
a P3 blocker (ships color-only), but makes P5 (depth zero-copy, T0-proven) the natural immediate follow.
