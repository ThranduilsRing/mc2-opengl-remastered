# Vast box bring-up, 2026-11-16

Assigned PC worktree: `A:/Games/mc2-slot08`, branch
`claude/box-bringup-1116`, base `a9069cc06aff5552e464f4f14c71f5fd4f5ac55f`.
Ops configuration: `A:/Games/mc2-ops/vast/SLOTS.md` and `cores.env`.
Connection: `ssh4.vast.ai:33053` (IP `3.89.223.125`); credentials remain on the PC.

Hardware verified on the instance: 28 logical CPUs, cgroup quota 26.87999,
32756228 kB RAM, NVIDIA GeForce RTX 2060 with 12288 MiB VRAM, 100 GB disk.
The old core map is preserved as `cores.env.prev-2026-11-16` beside the ops copy.

## Core map

One FIFO build queue at `/workspace/locks/build.queue`, `BUILD_CPUS=0-13`,
`BUILD_JOBS=14`, `MC2_BOX_QUOTA_CPUS=$BUILD_JOBS`.
RAM sizing: min(14 build cores, floor((31 GB - 3 GB link reserve) / 1.3 GB/TU))
= 14 jobs. Slots 0-5 use cores 14-15, 16-17, 18-19, 20-21, 22-23, 24-25.
`GAME_CPUS=26-27`, `GPU0_CPUS=26-27`, `GPU_SLOTS=1`, `GATE_GPU=-1`,
`GPU_NAME="RTX 2060"`. Game jobs serialize through `game.lock` and use Xvfb `:90`.

## Asset provenance

Stock source: WSL `/home/joe/mc2-installs/linux-port`, streamed by Windows SSH
with `mods/` excluded. The PC source is unchanged.
Parity source: read-only `A:/Games/mc2-opengl/mc2-win64-v0.4`.
The exact set is the 423 keys of `intentional-asset-changes.json`, as specified
by ops `vast/SLOTS.md:17` and the previous box's 11-15 parity refresh note in
`pending-launches-1115.md:47`. The earlier refresh script retained in
`G:/mc2-tmp/slot09/refresh_assets.py` uses sorted paths, SHA256 and tarfile.
All 423 source hashes matched the committed declarations before transfer;
the resulting uncompressed archive is 3000637440 bytes. No mods are included.

## Services

Ops slot scripts are installed in `/workspace/mc2-slot-tools`.
The ops diskwatch script runs under supervisor with thresholds 70/80/90%.
Existing repository health/supervisor modules are installed in
`/workspace/mc2-health`, linked from `/workspace/bin`.
Filler has an explicit STOP marker: six assigned slots are the requested service;
legacy pueue filler jobs are not enabled.

The PC copies of `lean_install.sh` and `dedupe_install.sh` lagged the previous
box's live 11-15 change documented in `pending-launches-1115.md:47`.
Restored that behavior in ops and on the new box: lean_install privatizes manifest
payload and files smaller than 1 MiB, leaving other parity assets shared;
dedupe excludes both deploy manifests but permits identical non-payload parity
files to be relinked. The priv loop explicitly skips absent files, matching the
11-15 fix noted at line 45. No deploy link-guard override is used.

## Receipts

R1 stock=7.3G files=29346 seed-install=ok

R2 v04-assets files=423 sha=ok

R3 cores BUILD_CPUS=0-13 BUILD_JOBS=14 GAME_CPUS=26-27

R4 slot tooling ok diskwatch running

R5 slots=6 disk=34%

R6 smoke-lite BLOCKED-ON-BASE mc2_01 NOT RUN mc2_10 NOT RUN health=OK

Final health: 2026-11-16 01:31:54 UTC, 66.6 GB free, load 0.5 / quota 26.9,
one build slot, zero holders/waiters/orphans, diskwatch and box-supervisor RUNNING.
Slot creation finished at 32% disk; repairing payload isolation brought it to 34%.
The existing deploy link guard passed all 3,226 payload paths in each of six slots.
All six installs have no `mods/` directory. No guard overrides were used.

Slot0 source sync and RelWithDebInfo production build passed at
`a9069cc06aff5552e464f4f14c71f5fd4f5ac55f` (1,300 tasks, `BUILD_EXIT=0`).
No second build or game launch was made.

The prescribed deploy command is unsafe under this assignment's no-mod rule:
`scripts/deploy_payload.py:1524` unconditionally appends example mods to the
payload. Its enumeration reported 17 files under four `mods/` packages.
`--classic-assets` only skips the remastered kit; the documented `--skip-kit`
option at line 2368 also does not suppress example mods. No supported no-mod
option was found. Deployment was therefore not executed; smoke/fingerprint/FPS
remain NOT RUN. Per lane-rules section 1, base tooling defects are handed to the
orchestrator rather than patched in an ops lane or worked around.

An offline payload audit found 87 present current payload paths still hardlinked
after cloning with the October stock manifest. The seed manifest was refreshed
with the existing `--write-manifest-only` command, and these payload paths are
made private in all slots. This is bootstrap inventory maintenance, not a claim
that the old seed executable is deployed play. Each lane must build and complete
a guarded deploy before running a game.

Detailed working state: `G:/mc2-tmp/slot08/notes/box-bringup-state.md`.

Evidence archive: `E:/mc2-archive-2026-10/proofs/box-bringup-1116/`.
It includes stock transfer, exact asset-set provenance/checksums, slot0 sync/build,
install creation, payload isolation and health logs, plus the updated ops files.
The original core map is retained there and in ops.

Compared with the previous box: RAM increases from 15 to 31 GB, GPU changes from
RTX 3080 to RTX 2060 12 GB, build parallelism increases from 10 to 14 jobs, and the
SSH endpoint changes. The core split and single-build/single-game limits stay the
same. Old RTX 3080 FPS numbers are historical and do not establish RTX 2060 speed.
Tracy/A-B tooling and performance were not validated by this bring-up.

## Cleanup limitation

PC scratch deletion was rejected by automatic approval review:
`rm -f style commands are not permitted. Use a safer approach`.
Lane-rules section 3 makes a refusal final; no alternate deletion method was
attempted. Retained scratch: `E:/mc2-tmp/slot08/box-bringup/v04-assets.tar`
(3000637440 bytes), its SHA256 list, and `G:/mc2-tmp/slot08/notes/`
(logs, three helper scripts, and the requested state note). The worktree remains
on its assigned branch; slot release was not run because it would recursively
delete the same refused scratch. All one-shot jobs have completed. The two
temporary remote bring-up helper scripts were removed before this refusal.
Persistent box assets, scripts, installs, sources, queue, and services are retained.
