<!-- doc-status: current | reviewed:2026-10-11 -->
# Lane 3 state (debug lane; worktree A:/Games/mc2-lane3, branch claude/lane3-debug from caf7e8bdb, install mc2-win64-lane3)

Session 2026-10-11 11:36 -> (cap 17:35). Tasks: A = mc2_15 crash_silent, B = tier2 asset_load_fail.

## Commits on the branch (cherry-pick / merge as needed, each self-contained)
- 211d0db9e fix: Camera::updateLights leaves no stale light pointers past the active count (ASan-proven latent use-after-free read; NOT the mc2_15 cause)
- 8e17112f0 fix: a heap-corruption exit (0xC0000374) now writes a crash bundle (vectored handler in gos_crashbundle.cpp)
- bcbfdb3ee smoke: a crash-bundle banner counts as a crash handler hit (mc2_15 now reports crash_no_summary, not crash_silent)
- docs: this file, tier2-data-gap.md, lane3-diag/ (diagnostic scripts)

## Task B: DONE (no repo fix; install content)
`tier2-data-gap.md`. Cause = loose `jaegermech.ini` override in `mc2-win64-v0.4/data/tgl` without its `jaegermech.tgl`. With the archived
`jaegermech.tgl` copied in, mc2_11/12/13/16/22/24 all PASS tier2 on lane 3. Owner picks: copy the .tgl into 0.4, or delete the loose ini.

## Task A: crash pinned, root cause (the heap writer) NOT found
Pinned with first-hand evidence:
1. Exit code is 0xC0000374 STATUS_HEAP_CORRUPTION (crash_evidence.json, all failing runs), not std::terminate / bad_alloc / abort / ExitProcess. The
   heap kills the process without running the unhandled-exception filter (probe: only a vectored handler runs), hence no bundle. FIXED by 8e17112f0.
2. It predates the program: tip caf7e8bdb, green mark ecc094a53 (2 of 3 runs crash) and 07b6f02af (4 of 5, before the logging facade). Bisecting by
   commit is pointless. Failure rate ~70 % per run at 118 fps; non-deterministic (frame 16 .. 5061, ~10-60 s into the mission, after the first
   weapon fx textures load).
3. What the heap reports (3 VEH bundles, `crashes/crash_*`): at an ALLOCATION (objmgr_update.cpp:712 unordered_map ctor, ABL allocLocal, CRT _stat)
   RtlAllocateHeap -> its zero check fails = a FREED block was written to after free (or overflowed into). A free-quarantine build
   (lane3-diag/quarantine_patch.py) caught it three times: a freed 56-byte block (the unordered_map list node of
   GameObjectManager::updateTerrainObjects, allocated at :712, freed at :1187) holds the 16 ASCII bytes `839715553810841)` at offset 0, identical in
   every run, so a deterministic, simulation-derived string is written through a stale pointer / by an overflow of the preceding block.
4. Build-config correlation: MC2_ASAN build (Tracy OFF) runs mc2_15 for 8192 frames clean (twice) after the light fix; a non-ASan build with Tracy
   disabled (CMake option MC2_NO_TRACY, local only) passes 8/8 (a ninth run was a host-load heartbeat freeze). Tracy ON crashes ~70 %. Tracy ON without
   TracyGpuContext/Collect still crashes (3/6); Tracy ON with TRACY_NO_CALLSTACK still crashes (3/8). CAVEAT: the passing runs were at ~70 fps (host
   loaded by other lanes), failing runs mostly at higher fps, so timing is a confounder; do not read the Tracy result as proven.
5. A page-guard allocator for every gos_Malloc block <= 512 bytes (end-aligned, guard page after, decommit on free) cut the crash to ~1 in 4 and the
   residual failure was an AV in `GpuStaticPropBatcher::reinjectPersistentStatic` (vector in `s_persistentStaticStore` with begin pointer 1), i.e. a
   live node of a guarded map was overwritten: corruption is not (only) in the small gos_Malloc blocks. Unguarded users: blocks > 512, direct
   malloc/CRT, driver-owned memory.
Ruled out: operator new throwing (62b326872), std::terminate, SEH bypass by /EHsc (a bundle is written for AVs), exit() from a worker, FRAME_JOBS
(default off), GL readback overruns of the obvious per-frame readbacks (not exhaustively), the light UAF read (fixed, crash persists).

## Next steps for whoever continues (cheapest first)
1. ASan + Tracy together: add `|| MC2_TRACY_WITH_ASAN` to the `if(NOT MC2_ASAN)` Tracy block in CMakeLists.txt and run mc2_15 under
   `lane3-diag/asan_build.py`; if ASan reports, the line is the answer. (Not run: only the Tracy-off ASan build was run.)
2. Application Verifier / PageHeap for mc2.exe (IFEO registry key, needs the owner: system setting) gives the exact writer as an AV with a stack.
3. Same experiment at equal fps: pin both builds with `MC2_FPS_CAP=60` (or run Tracy-off with other lanes idle) to remove the timing confounder.
4. The deterministic string: grep the sim for a decimal-formatted double or id ending in `)`; the content is the same in every run.
Method notes: crash bundles are now written (look in `<install>/crashes/crash_*`; `dumpthreads.py` lists all threads of a minidump with symbols);
`lane3-diag/quarantine_patch.py` is the free-quarantine / page-guard / polling-watch patch for gameos.cpp (apply with python, never commit).
Pitfalls hit: heredoc python in this shell turns `\\n` into a real newline (use the Write tool for edits with backslashes); `ninja_build.py | tail`
hides compile failures (check the exit code); run_smoke leases are host-wide and `mc2.exe` in tasklist may be another lane's.

## Proof status against the task bar
- mc2_15 passes 2 runs: NOT met (flaky, roughly 1 pass in 3-4 on every Tracy-on build). tier1 5/5 at bcbfdb3ee on lane 3: MET (2026-10-11 16:20, report 2026-10-11T16-20-10).
- Task B proof met (6 of 6).

## Hygiene
- Worktree A:/Games/mc2-lane3-old (detached, scripts copied in, builds in build64-ninja) can be removed with `git worktree remove --force`.
- lane3 build trees: build64-ninja (tip), build64-asan, build64-notracy, build64-nocs under A:/Games/mc2-lane3 (untracked, delete when done).
- Install mc2-win64-lane3 is a full copy of 0.4 (8 GB) plus the payload; it also holds crashes/ bundles from the investigation.
- Nothing pushed. My temporary stash was dropped.
