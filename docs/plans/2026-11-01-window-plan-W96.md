# Window plan W96 (issued 2026-11-01 by the Opus meta review)

Base: claude/nifty-mendeleev e5cf6ba9c (W95 published). Lanes run this plan; Flash and adversarial reviews check
against it. Any deviation needs a written one-paragraph justification in the window STATE.md, filed before the
deviating step. Source review: G:/mc2-tmp/opus-vulkan-path-review.md.

## 1. Tips

| Tip | Decision | Why |
|---|---|---|
| save-capacity, A:/Games/mc2-lane4, claude/save-capacity 8373afa33 (fix 3c36d6734) | IN, first | Fixes real save memory corruption (terrobj.cpp:1732: 40 pairs held, up to 161 accepted). Built on e5cf6ba9c. OBJS v2; the reader accepts versions 1..max (code/save_sections.cpp:99), so W95 saves still load. |
| Flash W95 P2 follow-ups (one small tip, new lane or lane4) | IN, second, only if ready by the precheck | vk_object_lights.cpp:535 null re-check after firstActiveLightSourceIndex; camera.h:1427-1439 removeWorldLight logs a mismatch and still calls updateLights(); merge_gate.py --clean-output/preflight identity (verify_published_identity kept when a deploy stage is planned); a comment in object_slot_codec.h:38 recording the slot-0 invariant. No behaviour change beyond these. |
| static-p1, A:/Games/mc2-lane9 846cd4aae | OUT | Pixel diffs remain. The light-generation hash covers moving local lights, so re-arms can churn every frame in combat or at night (review P0-1). The oracle cannot see light-row contents (P1-1). Re-enter only after the rescope in section 5. |
| record-thread, A:/Games/mc2-lane8 (cont22 live, tip after 0c0fbec26, base a06e91928) | OUT by default | The run is live and the tip is moving. It is not rebased on e5cf6ba9c. S3 main-thread cost is unmeasured. IN only if ALL of: cont22 ends at a green checkpoint commit; that checkpoint is rebased on e5cf6ba9c; the census (section 6) shows S3 overhead ≤0.10 ms main-thread median on mc2_24 vs its base. |

Order: save-capacity, then the Flash follow-ups, then (conditional) record-thread. One `lane_merge.py --batch` with the
included tips. Ratchets tighten once per batch.

## 2. Required proofs per tip

save-capacity:
- precheck;
- save unit tests (0/40/41/81/161/162/255 cells, truncated and malformed);
- first saves 01/03/10/16/24 at current policy;
- save-compat: W95 published saves for 01/03/10/24 + CVE-G load in W96 and continue ≥60 ticks;
- the W95 exe refuses a W96 save cleanly with section_version, without crashing;
- Metropolitan quicksave/load/resave.

Known open item: late stock 10 (+23, RNG counter) and 24 (+39, mover 725) post-load divergence. Before the window, run
the same late probe on the W95 exe (one baseline pair on the farm). If W95 also diverges: record it as pre-existing and
merge. If W95 is clean: the tip is OUT.

Flash follow-ups: their unit/contract rows, plus merge_gate pytest for the preflight identity change, plus tier1 in
the gate. The gate change must ship with a test that fails on the old behaviour.

record-thread (if IN):
- lane8's battery: pixel parity day/night, sync validation, SIMID with ownership audit, zero VIOLATIONs;
- the census overhead number;
- tier1 in the gate.

## 3. Stop conditions (the window stops; it does not retry blindly)

- Any tip's precheck fails twice: drop that tip and merge the rest.
- A gate stage red whose class is fixture, stale receipt or known mod content: no code change. Fix the harness input
  once, then resume. A second red of the same class: stop and report.
- Any determinism or save-compat red that the baseline pair does not reproduce: drop the tip that touches it.
- Vulkan validation errors > 0, or a new mod error signature beyond the known budgets (CVE 7, Exodus 0, Carver5 100):
  stop.
- Merge-through-publish exceeds 3 gate attempts: stop, write STATE.md, and hand off.

## 4. One-run publish checklist

1. `merge_gate.py --preflight` green before any game stage: fixtures, seed and exe identity, first-save refs, known-mod
   list.
2. ONE proof path for the whole window: E:/mc2-archive-2026-10/proofs/merge96/gate. Never switch proof dirs mid-window;
   switching invalidates receipt signatures (the W95 lesson).
3. Fresh-copy installs only through make-isolated-deploy.py from the verified lean seed. One acceptance install,
   E:/mc2-installs/merge96-acceptance. No extra copies.
4. Receipts: per-stage seconds and return codes, exe SHA256 matching HEAD, a same-window publish receipt (never a
   reused older one), day+night montages VIEWED (mc2_01 day, mc2_10 night), not MAD.
5. Publish to v0.4 once. Hardlink owner-test without launching it. Reseed the farm. Protected cleanup.
6. In STATE.md, list the installs left and the disk freed.

## 5. Lane9 rescope brief (≤10 lines)

ASSIGNMENT: A:/Games/mc2-lane9, new branch claude/static-p1r from e5cf6ba9c. Reuse commits only by cherry-pick.
(a) Light-independent props: in color_prop_tier1.vert:258-276, take sun colour and direction from the frame UBO, with a
    4-byte per-instance actor tint carried in the 112 B instance record. Ambient is already ignored there (:275); local
    lights already come from pool lights (frag:136).
(b) Audit every other reader of TG_HWLightsData (shadow_prop, legacy shape, trees) and list them, with file:line, in the
    plan note.
(c) P1 = classification + activation counters + reason histogram ONLY. No re-arm, no recipe erase, no walk skip.
Proof:
- 20 s oracle runs on stock 01/03/10/24;
- viewed day/night montages (pixels identical except where (a) intentionally changes the sun source);
- light re-arm count = 0 per frame on battle and on mc2_10 night with movers.
No P2 and no new flags.

## 6. Census brief (Sonnet, ≤10 lines)

ASSIGNMENT: read-mostly measurement lane on e5cf6ba9c plus the lane8 checkpoint exe. No engine edits except
counter-only logging, if a counter is missing.
On mc2_24 low_horizon and on battle at night (20 s each, host quiet lease), report one table:
- light-environment generation bumps per frame (lane9 exe);
- INERT reason histogram;
- draws recorded per island per frame;
- VKIsl record and submit ms per island (375025c2b timers);
- main-thread busy ms (section 7) for the lane8 checkpoint vs its base: 5 A/B pairs, with spread and machine load.
Output: docs/perf/2026-11-0x-census.md. That table gates the record-thread IN decision and the order of the next perf
lanes.

## 7. Definition of the "<1 ms" target

- Metric: MAIN-THREAD BUSY TIME per frame = the main-thread `Frame` scope minus main-thread waits released by the GPU or
  WSI (VKContext.RingWait, VKIsl.*.FenceWait / SlotWait, timeline waits, swapchain acquire and present).
- Waits on CPU workers stay IN, because they are on the critical path: present_thread::waitSealed, sim-overlap join,
  job joins.
- Zones on other threads are never summed into it.
- Conditions:
  - stock 01/03/10/24;
  - fixed bookmark poses (01/10 sun_facing_low, 24 low_horizon) at 1920x1080, so the GPU does not bind;
  - RelWithDebInfo, Tracy not connected (VkCtx Collect excluded);
  - reserved cores, quiet host lease;
  - frames from 3 s to 20 s, 5 captures.
- Pass: median < 1.0 ms AND p95 < 1.5 ms on every stock mission.
- DrawScreen < 0.1 ms uses the same rule, applied to the main-thread DrawScreen scope.
- 4K 200 fps stays a separate GPU throughput target (PERF window fps at 3840x2160, day and night).
