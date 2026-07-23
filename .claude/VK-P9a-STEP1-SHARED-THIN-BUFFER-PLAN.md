# VK-P9a WATER-PERF STEP 1 — shared thin buffer + fence pipeline (implementation map)

Worktree `A:/Games/mc2-vk-interop-bridge`, branch `claude/vk-interop-bridge-1`.
STEP 0 **PASSED** (`1425d896`): VK-exportable buffer → GL-import (`glNamedBufferStorageMemEXT`)
→ GL compute write → VK compute GPU-side read is validation-clean on the AMD 7900 XTX,
byte-exact, LUID-matched, zero CPU readback of the shared buffer. **No pipeline-N-1 latency
tradeoff needed.** Re-run the proof any time: `tools/vk_gl_buffer_interop_probe/build64/RelWithDebInfo/vk_gl_buffer_interop_probe.exe` (exit 0 = PASS).

## The target (measured [VK_WATER_PERF], gate `MC2_VK_WATER_PERF=1`)
- `thin_readback` 1817µs (63%) — 2× `glGetBufferSubData` in `WaterStream::ReadbackGpuThinForVk`
  (`gos_terrain_water_stream.cpp:2287` bucket count, `:2297` thin records). HARD GL sync stall.
- `fence_wait` 412µs (14%) — inline `vkWaitForFences` at `vulkan_water_island.cpp:1052`.
- `depth_readback` 383µs (13%) — separate later win; leave.

## Seam A — make the 3 water feed buffers GL-imported-from-VK-exportable

Current creation (plain `glGenBuffers`+`MC2_GL_BufferData`):
- Thin **ring** SSBO `g_thinBuffer`: `gos_terrain_water_stream.cpp:769-792` (ring-3, per-slot
  fenced `WATER-THINRING-FENCE-1`) and the fallback create at `:1511-1514`.
- Bucket header `g_waterBucketHeaderSsbo` (16 B): `:1448-1452`.
- Indirect cmd `g_waterIndirectCmdBuffer` (2×`DrawArraysIndirectCommand`, 32 B): `:1456-1460`,
  filled by `gpu_driven_cmd_patch.comp` (dispatch 2, `:2009-2022`).

Replace each with: island creates a VK-exportable `VkBuffer` (usage `STORAGE_BUFFER` +
`INDIRECT_BUFFER` for the cmd buf; + `TRANSFER_SRC` if a VK copy is ever needed) using the
EXACT pattern proven in `tools/vk_gl_buffer_interop_probe/main.cpp` stage 3
(`VkExternalMemoryBufferCreateInfo{OPAQUE_WIN32}` + dedicated `VkExportMemoryAllocateInfo` +
`vkGetMemoryWin32HandleKHR`), export the Win32 handle, and GL-import it
(`glCreateMemoryObjectsEXT` + `DEDICATED` param + `glImportMemoryWin32HandleEXT` +
`glNamedBufferStorageMemEXT(glBuf, size, memObj, 0)`, probe stage 4). The GL cull/pack compute
(`ComputeDispatchAndBindThinRecords`) then writes DIRECTLY into the shared GL buffer handle —
no code change to the compute dispatch itself, just the buffer id it binds.

Factor the create/import into a reusable `VkGlSharedBuffer` type next to `VkImageResource`
(same TU discipline, `#ifdef MC2_VULKAN_ISLAND`). It is the buffer analog the plan's
"Strategic reshape" anticipated; keep it GL-free at its core + a `GlInteropBufferView` adapter,
mirroring `VkImageResource`/`GlInteropView`.

**Ring caveat:** the thin buffer is a 3-slot ring, per-slot GL-fenced. Either (a) make one
exportable buffer covering all 3 slots (simplest — GL still sub-ranges by
`g_vkLastThinSlotOffset`, VK reads the same offset), or (b) 3 exportable buffers. (a) matches
the current single-`g_thinBuffer` layout; prefer it.

## Seam B — VK reads the shared buffers GPU-side; delete the readback

- Delete the `ReadbackGpuThinForVk` call + its `[VK_WATER_PERF] thin_readback_us` timing at
  `gameos_graphics.cpp:3743-3751` and the `vkThinPtr`/`vkThinCount` CPU feed. The island's
  `run()` signature (`gameos_graphics.cpp:3781`, impl `vulkan_water_island.cpp` step 2
  `:898-908`) changes from a CPU `thinRecords` ptr to binding the SHARED thin buffer as an
  SSBO descriptor (drop the `vmaMapMemory`+`memcpy` at `:901-907`). `ReadbackGpuThinForVk`
  itself (`gos_terrain_water_stream.cpp:2277`) can stay as dead code or be removed.
- **Draw count GPU-side:** the VK water pipeline currently does `vkCmdDraw(thinCount*6,...)`
  (`vulkan_water_island.cpp:1020`) with a CPU count. Replace with `vkCmdDrawIndirect` reading
  the SHARED `g_waterIndirectCmdBuffer` (already patched to `visibleCount*6` verts by
  `gpu_driven_cmd_patch.comp`). No CPU count crosses. (The VK pipeline's procedural VS already
  expands 6 verts/record — indirect `vertexCount` = visibleCount*6 is exactly right.)

## Seam C — GL→VK sync (acquire is CORRECT here, unlike shadow depth)

This is a GL-produces / VK-consumes buffer, so the GL→VK acquire signal is right. After the
compute's final barrier (`:2022`), GL signals the existing GL→VK semaphore
(`glInteropSignalToVk` pattern; probe uses `glSignalSemaphoreEXT(sem,1,&glBuf,...)`), the VK
water submit WAITS it (`vulkan_water_island.cpp:1031` already wires a wait-sem — point it at the
buffer-hand-off sem). Keep validation clean (probe proved this exact edge clean).

## Seam D — pipeline the fence (recover 412µs)

`vulkan_water_island.cpp:1052` `vkWaitForFences(fs.fence)` is inline (waits THIS frame's submit).
The double-buffered fence machinery already exists (`VkImageResource::frameFence[2]`,
`vkImageResourceBeginFrame` waits+resets the parity slot at `:953`). Move the wait so at
BeginFrame we wait frame **N-2**'s fence (the [parity] slot's fence, already the design) and do
NOT inline-wait N's fence after submit. The inline wait at `:1052` was added as a sync-val WAW
fix for the SHARED COLOR image across the two parity command buffers — verify that removing the
inline wait doesn't reopen that hazard; if it does, the fix is a proper VK render-pass external
dependency / timeline, not a CPU stall. (The color self-barrier at `:985` already exists.)

## PROVE (byte-exact is the STOP-gate — this is an OPT)
1. Build `build64_island` clean (`-DMC2_VULKAN=ON -DMC2_VULKAN_ISLAND=ON`, RelWithDebInfo, mc2).
2. Deploy 0.4c ONLY. FOREGROUND, window VISIBLE. `MC2_VK_WATER_PERF=1` +
   `MC2_RENDER_BACKEND=vulkan` + `MC2_NO_LAUNCHER=1`, overview_high bookmark,
   `MC2_SMOKE_FIXED_TIMESTEP=1`.
3. PERF: `thin_readback`→~0/gone, `fence_wait`→~0 steady state; combined VK-water CPU sync
   ~2.86ms → target <0.5ms. Report before→after breakdown.
4. PARITY: water SCENE-PIXEL byte-exact vs pre-fix water (GPU-side read of the SAME current-frame
   thin data = no latency). Non-water byte-identical, `validation_errors=0`, gate-OFF
   byte-identical. ANY pixel divergence → STOP (numbers).

## Watch items
- Shoreline depth-test is LOAD-BEARING (STEP-0 note addendum "THE HARD MUST-HAVE"): the shared
  thin buffer does not touch depth-in, so this is unchanged — but re-confirm the shoreline reads
  correct after the feed swap.
- The GL water cull/pack compute must still run (it's the producer). Do NOT disarm it.
