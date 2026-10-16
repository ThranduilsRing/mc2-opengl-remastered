# Lane 2 s34 cleanup review (9ddea7e06..3a7322523, claude/lane2-s3)

Read-only. Scope: 843417018 row 28 VK highlight, 36133eeaa + a11b7692f VK pass timer, d066a9efb s33 fixes, ledger/state/callers.

## Verdict

FIX-THEN-MERGE. One must-fix in the pass timer (stale results from a recorded-but-unsubmitted bracket once the ring has wrapped); the a11b7692f fix only covers the first ring cycle. Row 28 shader port is correct.

## Must-fix

M1. Unsubmitted bracket reads a stale result after the first ring cycle (`vk_render_pass_timer.cpp` Begin/harvest + `vulkan_vfx_island.cpp`).
The per-bracket reset is `vkCmdResetQueryPool` recorded into the island's own `cb`. The VFX island records Begin/End and then returns before submit when `groupsDrawn == 0`, so neither the reset nor the stamps execute. The create-time `resetAll` makes this read "unavailable" only while the queries have never been written. Once the ring wraps (4 frames), those query indices still hold the available results of the frame 4 back, so `harvest` returns VK_SUCCESS with availability set and publishes a stale AlphaVfx time as current, silently (not counted as dropped). In the first cycle the opposite happens: the never-written query stays unavailable forever, so the whole slot (every pass) is dropped at k==0 rather than just the missing bracket.
Fix options (pick one): (a) the island calls a `Cancel(p)`/`Discard` (or Begin is moved after the "nothing to draw" early-out) so a bracket exists only on a cb that is submitted; (b) record the reset of the whole slot's range in a command buffer that is always submitted first each frame (or `vkResetQueryPool` host reset, core 1.2 hostQueryReset) at slot reuse, and treat unavailable-after-fence as "pass absent" rather than "slot unfinished". (a) is the minimal one. Audit the other six call sites for the same early-return-between-End-and-submit shape (water/mech/terrain/colour-prop checked: their post-End returns are error paths that also disable the island, acceptable; VFX is the live one, and its `groupsDrawn==0` path is the common case on maps without VFX, matching the state note "AlphaVfx showed no sample at mc2_01").

## Should-fix

S1. Doctest covers only `TicksToMs`; brief item (7) asks for the drop path. The ring/harvest/drop logic (k==0 drop, younger slots wait, `open[]` cleared at FrameEnd) is untested. Factor the ring bookkeeping (slot index, query base `(cur*kBracketsPerSlot+i)*2`, pending/drop decision given an availability callback) into a header-only pure function and test: slot reused while unfinished -> one dropped and cleared; younger unfinished slot -> waits; an M1-shaped bracket (closed but never submitted) -> must not publish.
S2. The drop count only fires for the slot at k==0; a harvest failure caused by a hard `VkResult` error (device lost etc.) is indistinguishable from "not ready" and will be counted as a drop every 4 frames forever. Acceptable, but log once on a non-VK_NOT_READY result.
S3. `s_unsupported` is a file-static bool (not an owner, fine for RAII) but never reset on device recreation; note it in the comment (WHY) or move it into the context slot.

## Nits

N1. `kBracketsPerSlot` overflow silently drops brackets for that frame with no counter; a one-shot log would help diagnose a future cascade count change.
N2. `harvest` comment says "false while any is unfinished", but a closed bracket on an unsubmitted cb is never "unfinished" in the sense the reader assumes (see M1); reword after the fix.
N3. Header comment "the begin stamp resets its own two queries first" describes HOW; the WHY (a reused slot must not carry the previous ring pass's result) is the point and is exactly what M1 breaks.

## Checked-OK

1. Ownership: pool is `mc2vk::OwnedQueryPool` (new `release::QueryPool` deleter in `vk_owned.h`) inside `TimerState : IslandState` held in the context's new `IslandSlot::PassTimer` slot, so it is released with the islands ahead of the device; `kSlotNames` extended with static_assert; ledger regenerated (d0ba006f8), state reports `vk_ownership_ledger OK`. No static-duration owner. `resetAll` temporaries are owned and waited before release.
2. No wait on results (`vkGetQueryPoolResults` without WAIT_BIT, WITH_AVAILABILITY, 64-bit, stride 16 matches the 4-u64 buffer). Ring indexing: `cur` advanced before the scan, so k==0 is the oldest slot = the one Begin will write next frame; no off-by-one. Open brackets cleared at FrameEnd and never read.
3. Brackets sit outside render passes (reset is illegal inside one): VFX/water/mech/terrain/colour-prop Begin after `vkBeginCommandBuffer`, End after `vkCmdEndRenderPass`; shadow spans begin_frame..submit_frame, and an unsubmitted shadow frame leaves an open bracket that is discarded. Exception: M1.
4. `timestampPeriod` applied (`nsPerTick`); `timestampValidBits==0` or `timestampPeriod<=0` -> one log line, `s_unsupported`, null state, every entry returns, no queries recorded. Valid-bit mask handles 64. `timestampComputeAndGraphics==false` is covered by the per-queue-family `timestampValidBits` check (the spec-correct test). Pool-create and reset-submit failures also degrade with one line.
5. Row 28 shader terms match GL: vertex `vArgb = clamp(vArgb + aRGBHighlight.rgb)` after the full light sum (static_prop.vert step 6), window branch `clamp(seed + highlight.rgb)`; fragment `rgb += vHighlight.rgb * vHighlight.a` before the additive pools in both the legacy and material branches. Location 9 flat in/out paired.
6. Comments are WHAT+WHY, no journal/dated comments in the new files; the `VK-ISLAND-TRACY-1` tags are pre-existing.
8. Gaps file row 28 (PORTED, highlight-on capture recipe pending) and lane2-state s34 agree; the state honestly lists the highlight-on proof and the AlphaVfx no-sample as open.
