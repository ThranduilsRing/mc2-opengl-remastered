# Present sync note (lane present-sync; design note only, no behaviour change)

State: steps 1-3 done at 847f5dcf6 (zones, bench-sink pres.*, C6 stress, audit passthrough). callers.json refresh 337a76ece.

## Measured: mc2_24, 20 s, Tracy + MC2_VK_GPU_TIMESTAMPS=1, 4022 steady frames (frame>300)
Source: frames.jsonl cpu rows ("pres") and [PERF_COUNTERS v1]. Frame mean 4.46 ms. Worker = present thread.
| item | thread | mean ms/frame | p95 |
|---|---|---:|---:|
| Frame.WaitSealed (blocked) | main | 0.817 | >0.5 ms on 98.8% of frames |
| Acquire | worker | 0.054 | 0.066 |
| TailRecord (parent) | worker | 0.840 | 1.137 |
|  Finish.Collect (gpuprof::frameEnd) | worker | 0.564 | 0.833 |
|  Hud | worker | 0.148 | 0.183 |
|  Submit | worker | 0.017 | 0.022 |
|  Blit | worker | 0.008 | 0.011 |
|  ImGui | worker | 0.003 | 0.004 |
|  Finish.Boundary (frameBoundary) | worker | 0.005 | 0.004 |
| CompositeWait (fence) | worker | 0.001 | 0.002 |
| QueuePresent (holds submitMutex_) | worker | 0.087 | 0.116 |
Tail minus Collect = 0.28 ms. Collect only runs with GPU timestamps on (the profile run forces them), so it is mostly a
measurement artifact: without it the worker total is about 0.44 ms. Acquire + CompositeWait (the GPU/WSI waits) are about 0.06 ms,
so on mc2_24 the main wait is the worker CPU tail, not the GPU. Per-frame intervals (acquire_t0/t1, cwait_t0/t1) are in each cpu row.
Caveat: first_vk_wait_site records only the function name (SDL_main on 99% of frames); it needs a real site tag before it can drive D3c.

## Stress test (C6): tests/unit/test_present_handoff.cpp
100k jittered cycles on a model of present_thread.cpp; 3 consecutive green doctest runs, stress under 10 s.
Checks: main never owns Render while unsealed (judged under the mailbox lock); seal before WSI step; waitSealed returns only
when sealed; a simulated main VK access in [enqueue, seal) is caught by the isOwner(Render) predicate; a sequence log replays
through an FSM that rejects seal-less WSI and early return. Worker-side ownership checks must be taken under the lock with the seal,
because main legitimately enqueues the next frame during the WSI step.

## VK-context access between frame top and DrawScreen (static read; no runtime log hook was run)
- gameosmain.cpp:1030 beginFrame (once, before the loop).
- gameosmain.cpp:1048 present_thread::waitSealed, the only wait; main owns Render after it.
- gameosmain.cpp:1051 retirePresentHandles -> gos_host_vk.cpp:187-195: getOrCreateEngineVulkanContext(), tracker().frameNo/isComplete/frameTicket,
  frameContext(), textureService().collectSourceRetirements(), RenderWorld::advanceFrame. No submit, no lock.
- ExtractRenderSnapshot / freezeDrawFrame / frameEmitDrawPackets: no direct engine VK context use found by grep. Texture uploads queue
  into uploads_ and flush inside submit() (vulkan_context.cpp:582-586) or flushUploads() (:614), both under submitMutex_.
- Hazard: VulkanContext::present (vulkan_context.cpp:589-595) takes submitMutex_, calls seal() (waking main), then vkQueuePresentKHR.
  A main submit()/flushUploads() in that window blocks for the whole QueuePresent (0.087 ms mean; worse under vsync or WSI stalls).

## Proposal
D3b (tail cuts, no wait reordering): (1) run Collect after seal or off the critical path when timestamps are on, skip when off (0.56 ms);
(2) Hud 0.15 ms: reuse the previous record when the HUD list is unchanged; (3) leave ImGui/Blit/Submit/Boundary (under 0.03 ms).
D3c-lite: (a) engineVulkanContextValid() as a lock-free atomic read; (b) retirePresentHandles reads atomics only, so it can run before
waitSealed returns; (c) one waitSealed, placed immediately before the first main call that can submit (DrawScreen) instead of frame top;
the prefix (input, logic, ExtractRenderSnapshot) is VK-free per the list above.
Avoiding submits during QueuePresent: keep seal() inside present() before the call only for the VK-free prefix release; main's first submit
must additionally wait on a "queue present returned" flag set after vkQueuePresentKHR. Update the C6 model's Seal/Wsi order in the same commit
and run audit levels 2..52 on mc2_10 and mc2_01.
Next: re-measure with GPU timestamps OFF; the Collect cut alone is probably most of the gain on mc2_24.
