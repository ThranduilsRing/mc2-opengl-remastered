# Present tail (lane7, claude/present-tail) -- running note

Outcome: NO CODE CHANGE SHIPPED. Premise of D3b/D3c-lite does not hold; evidence below.

## Measured (nifty exe vs identical exe, mc2_24 low_horizon_0, 1080p, 20 s, bench harness, NOISY host ext_cpu_p95 16%)
- A/A pair (same code): frame_ms 4.585 vs 3.912 (delta -0.67 ms), wait_sealed 0.551 vs 0.528. Run-to-run noise on MAIN_BUSY is ~0.7 ms,
  an order of magnitude above any gain a tail change could deliver (<=0.15 ms). 7 pairs cannot give a CI excluding 0 for such a change.
- Worker tail medians (ms): acquire 0.055, hud 0.14, imgui 0.003, blit 0.008, submit 0.016, collect 0.28-0.30, boundary 0.002, seal at ~0.52, queue_present 0.08 (after seal, off critical path).
- collect 0.29 ms appears ONLY because the battle-bench harness sets MC2_VK_GPU_TIMESTAMPS=1 (engine.log CONFIG line). With the default (timers off, no Tracy)
  gpuprof::frameEnd returns at its first check, so D3b item 1 is already satisfied in shipping config; the harness-visible 0.29 ms is instrumentation cost.
- Moving Collect after seal is unsafe: it reads/advances gpuprof ring state and submits an end command buffer while main (now Render owner) records timestamps for the next frame.

## D3c-lite premise check
- first_vk_wait_ms_into_frame = 0.0002 ms: the first VK touch is the waitSealed itself. The frame prefix before Mission::update is GuiRuntime::NewFrame (prepareImguiVulkanFrame, VK) then
  overlapJoinHook/commitTickQueues (commitTickTextures, needs Render ownership and the retired present handles). No VK-free main work exists to overlap with the worker tail without
  splitting NewFrame and the join/commit chain, which changes determinism-sensitive ordering. Not done.
- Pre-acquiring the swapchain image after QueuePresent would save at most ~0.05 ms (acquire) and interacts with rebuildSwapchain/fail-soft retry paths. Not worth the risk.
- Hud record reuse (0.14 ms) needs a draw-list equality proof across ImGui + pendingHudDraws; potential <0.14 ms, below the noise floor.

## Recommendation
Do not pursue. If the top-of-frame wait matters, the lever is the whole worker body (about 0.52 ms) vs main work, i.e. a deeper pipeline (depth-2 mailbox), a separate design item.
Proof artifacts: E:/mc2-archive-2026-10/proofs/present-tail/ab/. Driver: G:/mc2-tmp/ab7.py (interleaved A/B over tools/perf/main_busy.py).
