# Lane 2 session 38 cleanup review (05f081b35..21d247eed, 85cdc3bc3 skipped)

## Verdict
MERGE-READY with two doc fixes. Code is sound; the gaps are the row 29 recipe (not yet implementable as written), a stale OPEN count, and one untested rejection path.

## Must-fix
1. `.claude/VK-PARITY-GAPS.md` header "PARITY STATE" (line 1) still says "OPEN: none ... rows 1-17 and 19-28" and the Total line still says "Open: 18". Row 29 is OPEN, so both are wrong (header should list row 29 open; Total should say 19, or name 29). A reader of the header concludes parity is fully closed, which is the exact claim the MLR-retirement decision rests on.

## Should-fix
1. Row 29 recipe is not concrete enough for a Sonnet lane. Cause and numbers are good (8,800 / 7,471 px, 26 particles / 26 groups / 26 draws, exe 85cdc3bc3). Missing: the atlas import source is only `uAtlas` plus "like the legacy shape island" (no file, no function, no struct that holds the atlas texels on the CPU side); `particle_billboard.vert/.frag` have no path (GL shader dir vs `shaders/vulkan/`); no binding plan (set/binding number for the sampler, whether the island's descriptor layout has a free slot, push-constant vs UBO for atlas grid dims); "colour-key and head-brighten" are not defined. Add: file:line of the GL atlas upload, the island's current descriptor layout, the proposed binding, and the grid-dimension source.
2. Screenshot Slot doctests cover refuse-while-pending and expiry, but the GL-run rejection ("the Vulkan backend is not drawing", gos_screenshot.cpp request()) and the empty-path rejection are engine-side and have no test. Each has one log line; the doctest requirement is met for 2 of 3 paths. Either note it as engine-bound or extract the predicate.
3. Oracle `--install` rejects a bad value via argparse `choices`, which prints a multi-line usage block, not one line. Acceptable but not the "one line" asked; `parser.error` text is fine to leave. No test pins that `--install lane2` reaches the `--exe` path (check `tools/test_capture_vfx_oracle.py` assertion covers the path string, not only parse).

## Nits
- `debug_renderer.cpp` keeps three file/function-static items: `static Queue* s_active` (non-owning pointer, cleared in ~Queue, so no leak), and function-local `s_firstDrawLogged` / `s_cullLogged` flags (log-once state, process lifetime). Not buffers, but they outlive a renderer re-create so the "first_draw" log fires once per process, not per Queue. Move the two flags into Queue if a re-created renderer should log again.
- lane2-state "check-env-drive-guard commit" has no sha; add it.
- Row 29 cell is one very long line; the "earlier montage showed white squares" sentence is journal-like and could move to lane2-state.

## Checked-OK
1. Queue ownership: `DebugRenderer::Queue debugQueue_` is a member of gosRenderer (gameos_graphics_renderer_internal.h:737); ctor sets s_active, dtor clears it only if it is still this; free functions no-op without an instance. Single owner.
2. Slot: header-only, 3 doctests (refuse second begin, expiry at kMaxFramesPending and slot frees, take restarts count / tick no-op idle). Expiry and pending-rejection each log one line. `tick()` now runs before the `#ifdef MC2_VULKAN` block so it runs on declined/GL presents.
3. Fence-wait failure: sample recorded before `return false`, correct.
4. Oracle sidecar: `capture_backend.check(VULKAN, ...)` rejects backend gl, a missing backend/source, and `vulkan + gl_fbo`; tests include all three plus non-deterministic and wrong seed. An old gl_fbo sidecar cannot satisfy it. `--install` is `choices=("lane2","lane3")`, forwarded through the `--plan` recursion and into `command()`.
5. check-env-drive-guard: empty table accepted; Control C now injects `MC2_SELFTEST_POPPED` and requires `run_checks()` to refuse it (the pop-path refusal stays proven). Loop at check 4 still iterates members.
6. Comments are WHAT+WHY (Slot header, Queue, fence wait, tick placement). No journal comments in code.
7. Inventory section 19 and lane2-state agree (GL arms deleted, `capture_backend.py` kept for rejecting old sidecars, Queue owner, POPPED_ENV empty, fence sample, Slot with doctest, row 29 open). The inventory VFX-oracle row (DONE, row 29 open) matches.
8. test_build_compiler_presets red: `git log -3 -- scripts/tests/test_build_compiler_presets.py tools/build_contract_harnesses.py` gives 54e8e7cb3, 3c60e0d6a, 6a54a4583 (sim lane / merge sol). 6a54a4583 is an ancestor of 05f081b35, and neither file is in this range's diff stat: the red PREDATES this range. It is the extra `order_seam` cmake pair from that merge; lane 2 correctly did not claim it.
