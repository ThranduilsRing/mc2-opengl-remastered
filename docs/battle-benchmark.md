# Integrated battle benchmark

Vegetation has its own GPU scope inside the shared VFX pass. The inclusive VFX timing also contains vegetation; do not add those two timings together. Total GPU time uses the measured span, never a sum of overlapping passes.

Commit, build RelWithDebInfo, and deploy the combined engine to the idle benchmark install. From this checkout:

```powershell
py -3 scripts/run_battle_bench.py
```

The default is two complete repetitions of day/night at native 3840x2160 and 1920x1080, 30 seconds per performance run, plus deterministic captures of all four views. The existing smoke and visual harnesses launch the game offscreen under the exclusive host lease. Lease contention waits. Captures always pass `--no-kill`. Output goes to `G:/mc2-proof/battle-bench/<full-sha>/`: `battle-bench.json`, `battle-bench.md`, `montage.png`, and raw frame evidence/logs. Existing results are preserved; use `--out <other-directory>` for another experiment. View the montage before accepting the evidence.

`tests/visual/bookmarks/battle_bench_v1.json` versions the mission, seed, 48-mech strike-force grid at the mc2_24 fortress, capture time and cameras. The mission-start command channel uses `MC2_TEST_SPAWN`, checks each spawn's health, powers up the force, and orders real attacks on enemies and base buildings every two simulation seconds. Destruction must increase. Night uses the existing `set_light night` override on the same mission. Change the scenario version when changing its workload.

Performance uses real time, deterministic RNG, uncapped FPS and no vsync. Cameras change every 7.5 seconds while the battle continues. Capture uses the harness's fixed 1/30-second clock and freezes the battle at frame 240 for the bookmark sweep. Capture runs are excluded from performance distributions.

GPU tails come from individual `MC2_VK_GPU_TIMESTAMPS` query frames, never percentiles of window means. Missing scopes are **unmeasured**. GPU frame time is the first-to-last measured GPU scope span; JSON also retains every named pass, including VFX, HUD, sky and composite. CPU frame time measures elapsed work before the frame cap, including driver waits. Simulation samples measure each active `Mission::update`; the current combined engine is frame-coupled, and this benchmark does not pretend it has a global 30Hz simulation scheduler. Individual ticks over 1 ms and frames with multiple ticks are retained explicitly. The first two simulated seconds are warmup; spikes afterward are never discarded.

VRAM is DXGI process-local usage sampled every 120 frames, with VMA allocation peaks alongside it. Working set uses the OS process-lifetime peak. These are sampled VRAM peaks, not an allocation ledger. Load is startup-to-`mission_ready`; **cold means first process launch, warm means repeated process launch, with OS/driver caches uncontrolled**. This does not claim an empty OS cache. Run-to-run p95 spread is `(max-min)/mean`; inspect it before claiming an improvement. Keep other builds and GPU workloads idle for final performance measurements.

Identity includes the binary's embedded commit, exe hash, content/mod and shader byte hashes, resolved render options, full environment records and device/driver receipts. Default profile is stock with shipping features enabled and no active mod. The 512 MiB potato target is a separate profile and must not be applied to this stock run.

Compare two saved results:

```powershell
py -3 scripts/run_battle_bench.py --diff G:/mc2-proof/battle-bench/<before>/battle-bench.json G:/mc2-proof/battle-bench/<after>/battle-bench.json
```

The tiny diff prints before/after p95 CPU, tick and GPU times and refuses different scenario or content fingerprints. Budgets: total native 4K GPU 5 ms, 1080p CPU frame 1 ms; terrain p95 0.50 ms at 4K and 0.15 ms at 1080p on the reference RX 7900 XTX. Other pass timings identify where that total is spent; no invented per-pass budgets.
