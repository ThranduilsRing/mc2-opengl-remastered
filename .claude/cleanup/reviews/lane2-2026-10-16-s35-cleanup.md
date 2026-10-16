# Lane 2 session 35 cleanup review (3a7322523..3582651b6)

## Verdict
MERGE after the Should-fix on the error-path M1 gap is either fixed or recorded as accepted. The happy-path M1 fix is correct and tested; no Must-fix.

## Must-fix
None.

## Should-fix
1. M1 is fixed on success paths only. A closed bracket is left readable when the buffer is not submitted because of an error return:
   - `vulkan_post_screenfx_native.cpp` static-shadow pass (`runVkPostScreenShadowStaticNative`): `vkEndCommandBuffer` / `vkQueueSubmit` / fence failures `return false` WITHOUT `s->disabled`, so the pass retries next frame and the stale-slot hazard recurs. lane2-state says all such returns "disable the island"; that is wrong for this function.
   - VFX island: `vkEndCommandBuffer` failure after `End` (disables the island, but that frame's slot can still publish stale data; no `Cancel`).
   - Fog / SSAO / screenfx FX / mech: same shape, disabled afterwards, one stale publish possible.
   Cheapest cure: call `Cancel(Pass_Post)` (and `Pass_AlphaVfx`) before each post-`End` error return, or a tiny scope guard that cancels unless submit succeeded. At minimum correct the lane2-state audit sentence.
2. Dup-lines baseline reason (check 6): the commit message only says "reason in lane2-state". The reason (GameOS +5 from timer Begin/End inside the three near-copy record functions) is in the state file, not in the commit. Also the baseline `head` is 18a0fa4f0, not the final tip.
3. Hook parse: `strtoul` on Windows is 32-bit, so `argb > 0xFFFFFFFFul` can never fire; "FFFFFFFFF" (overflow, ERANGE unchecked), "-1", " 12" and "0x12" are all accepted. Use `strtoull` and reject sign/space/ERANGE, or the "malformed rejected" claim is not fully true.

## Nits
- A frame whose only brackets were all cancelled still counts as pending and publishes an empty row (`has[]` all false); harmless, but skip when no closed bracket exists.
- Ring tests do not cover cancel of a still-open bracket, or double cancel.
- Inventory section 16 (a) says `gos_render_pass_timer` GL half is "deletable now"; GL is still the runtime default, so say "deletable with the GL exe".

## Checked-OK
- (1) Begin/End/submit are in one function for the VFX island and the fog, SSAO, FX and static-shadow passes on the happy path; the composite lost its bracket (`engine_present_vk.cpp`); VFX nothing-to-draw cancels.
- (2) Doctest "bracket on an unsubmitted command buffer ... stale result after the wrap" runs 5 frames over a 4-slot ring with pre-"available" queries and asserts the cancelled query is never read and no stale value is published. Drop-at-reuse, younger-wait, hard-error and overflow cases are also present.
- (3) Hook: `isSet<MC2_VISUAL_BOOKMARK_CAPTURE>` gates the env read; function-local static, one read; malformed value gives one `print_err` and stays 0; gate row sorted, doc under 160 chars, marked CAPTURE-ONLY. It cannot fire in a normal run (no capture env, value 0).
- (4) `submitOneShot`: pool and fence are locals, no statics, device-wait failure path preserved; vk_first_layout keeps its null checks and its own failure line; only the log tag changes to `[VK_ONE_SHOT]` (census updated). The timer's reset path was not re-run under the pass-time env on the final exe (stated in the state file).
- (5) New comments state WHAT+WHY, no journal text found.
- (7) Inventory section 16 (gl_fbo arm: four consumers; step-2 (a)/(b) lists) matches the state file's Open list (row 18, gl_fbo consumers, step-2 rulings).
