# gpu-route-second-xtx-2
- Started 2026-11-14 20:03 CST; budget <=150 tool calls. Worktree/branch verified clean at a616de493.
- Merged newest local claude/play-1113 cleanly before edits.
- RCA: host_lease.acquire blocks on every foreign engine (line 835); device_hint/pin_device force all jobs to non-display before leasing (lines 155,212); prevents 2-card capacity.
- Owner v0.4 PID 49128 active; read-only process check. Runtime GPU probe: display UUID 00000000-2a00-0000-0000-000000000000, non-display 00000000-2700-0000-0000-000000000000.
- Plan: reserve display slot for unmanaged games, choose non-display first then display without owner, pin child to acquired slot UUID. Remove PC NVIDIA defaults; preserve explicit routing on other hosts/box. Targeted lease/routing pytest, lint, commit, one build/deploy and smoke-lite on E:/mc2-installs/gpu-route-second-xtx-2.
- Implemented: runtime UUID slots; unmanaged games reserve display only, secondary preferred, both available without owner; exclusive holds still wait for owner; PC orphan-kill option cannot touch owner. Shared nofocus routing follows acquired slot; merge gate defers; AMD/AMD fallback replaces NVIDIA default. Appended correction to external lane rules.
- Offline: initial pytest 273 pass / 1 stale NVIDIA-default expectation; corrected expectation. Final run_scripts_tests: 8 modules green; cxx17_ratchet PASS. Receipts E:/mc2-archive-2026-10/proofs/gpu-route-second-xtx-2/{pytest-final,ratchet}.log.
- Next: remaining launch/lock-order tests and lint; commit then build/deploy E:/mc2-installs/gpu-route-second-xtx-2 and smoke-lite with owner PID active. Archive evidence and release slot.
