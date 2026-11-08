# Farm desktop runner

Paired performance jobs use `host.py submit --exe <arm-A-build>/mc2.exe
--exe-b <arm-B-build>/mc2.exe --data-delta <delta> tools/farm/perf.json`.
Place the committed profiling harness under `delta/perf-harness/` (scripts,
tools/perf, their Python imports, Gates, tests/smoke and tests/visual/bookmarks).
Both payloads deploy privately on the laptop. The job alternates A/B five times
per mission with 20-second fixed views, returns raw profile JSONs and Tracy
captures, and prints the same paired confidence/verdict table as the host.
Tracy executables and psutil must be installed under C:/mc2farm; this job uses
the NVIDIA GPU and laptop-local leases. Noise annotates every result.
Fetch removes both private arm trees. Laptop absolute timings characterize
that machine; compare host and laptop deltas separately.

Fresh release proof: package with `scripts/build_release.sh`, then prepare a
data-delta directory containing `release-assets/mc2-remastered-engine.zip`,
`release-assets/mc2-gamedata.zip`, and the lane's interpreted harness under
`release-proof/` (the `harness/` entries returned by `host.seed_files`). Run
`py -3 tools/farm/host.py submit --exe <lane>/build64-ninja/mc2.exe --data-delta <delta> tools/farm/release-smoke.json`.
The release validator extracts only those zips into an empty temporary directory,
tests normal first-run preference creation with smoke mode unset, then runs
mc2_01 for 15 seconds. It rejects missing SPIR-V, missing/unreviewed DLLs and
crash dumps. `wait` then `fetch` the job: the result includes first-run.log,
the release manifest/report and the smoke logs. The temporary install is deleted
by the validator; fetch removes the per-job payload tree. Host temps belong on
G:; the owner's no-C: rule permits the farm's C:/mc2farm workspace.

The laptop is a separate, serial game slot. SSH is session 0; GPU jobs must run
through `MC2FarmRunner` in Kaitlyn's logged-in desktop session. All laptop files
belong under `C:\mc2farm`. No service or firewall changes are needed.

Copy `runner.py`, `queue_lock.py` (the one queue shared with the laptop gate's build; every `tools/laptop_gate.py` run re-provisions
both from the PC, sha-checked, so they never drift) and `install-task.cmd` to `C:\mc2farm`, then run
`C:\mc2farm\install-task.cmd` as Kaitlyn. It uses `schtasks /create /tn MC2FarmRunner
/sc ONLOGON /ru DESKTOP-Q11RABS\Kaitlyn /it /rl LIMITED /tr
"C:\Users\Kaitlyn\AppData\Local\Python\bin\python.exe C:\mc2farm\runner.py --root C:\mc2farm"`.
The task uses an interactive token and starts immediately and at logon. The runner
requires the seeded `C:/mc2farm/harness/scripts/console_child.py` helper.
It sets its own priority to BELOW_NORMAL before polling; children inherit it.
No PowerShell script or execution-policy change is needed. An installed Python
executable can be supplied as the command file's first argument.

The host helper defaults to `Kaitlyn@192.168.68.107` and the key under the SSH
`HOME` directory (on the main host, `C:\SPB_Data`, distinct from USERPROFILE).
Use `--host` or `--key` before the subcommand to override those defaults.

```powershell
py -3 tools/farm/host.py submit smoke.json
py -3 tools/farm/host.py wait <printed-id>
py -3 tools/farm/host.py fetch <printed-id> E:/mc2-archive-2026-10/proofs/farm-runner
```

After each merge window, reseed from the verified merge-gate install before
submitting jobs (main-host temporary files must use G:, never C:):

```powershell
$env:TMP = 'G:/mc2-tmp'; $env:TEMP = $env:TMP; $env:TMPDIR = $env:TMP
py -3 tools/farm/host.py reseed --install A:/Games/mc2-opengl/mc2-win64-v0.4 --harness A:/Games/mc2-main
```

`reseed` checks the source executable against `.deployed_manifest.csv`, copies
runtime payload and interpreted harness files over SSH to `C:/mc2farm`, and pins
`reference/mc2.exe` with its source commit and SHA-256 in `reference/seed.json`.
It transfers only files whose size or modification time changed, preserves
modification times, and reports transferred bytes/decimal GB. It excludes PDBs,
build trees, untracked harness files and game run outputs; it does not delete
existing laptop files. Run with the queue idle; do not reseed during a game job.
Fetch `reference/seed.json` alongside job proofs. Performance uses the paired
job above; label laptop absolute times separately from host measurements.

Example `smoke.json` (the harness and independent reference executable must
already have been seeded from the verified merge-gate output):

```json
{
  "argv": ["python", "C:/mc2farm/harness/scripts/run_smoke.py", "--lite", "--keep-logs", "--exe", "C:/mc2farm/install/mc2.exe", "--ref-exe", "C:/mc2farm/reference/mc2.exe"],
  "cwd": "C:/mc2farm/harness",
  "timeout": 600,
  "env": {
    "MC2_HOST_LEASE_PATH": "C:/mc2farm/host_game_lease.json",
    "MC2_DEPLOY_LEASE_PATH": "C:/mc2farm/deploy_leases.json",
    "MC2_CHILD_REGISTRY_DIR": "C:/mc2farm/children",
    "MC2_HOST_GAME_SLOTS": "1",
    "MC2_DEBUG_STATE_DUMP": "1",
    "MC2_WIDTH": "1920",
    "MC2_HEIGHT": "1080"
  }
}
```

`--ref-exe` checks byte identity against the separately pinned merge-gate exe;
retain its original source commit and SHA-256 in the proof directory. Do not
disable the fingerprint gate. The private host lease stays on the laptop and
avoids the harness's hardcoded main-host `A:` path. Slot zero uses logical CPUs
4-7, which exist on the laptop's 4C/8T CPU. Record actual host load separately
because the harness does not sample load when using a private lease path.

For Intel runs set `MC2_VK_DEVICE=Intel` (the loader ignores `VK_LOADER_DRIVERS_SELECT`; run_smoke now fails a run whose
logged `[VK_PROFILE]` device differs) and set `MC2_HARNESS_GPU=any` in the job env on a host with one GPU (farm jobs always do; the implicit NVIDIA hint is also not asserted when the engine logs that it matched no device). Launcher Low is
`MC2_OPTIONS_OVERRIDE=ShadowQuality=0,SpatialAA=0`, with `MC2_VEGETATION_MAX=5000000`,
`MC2_SHADOW_ENABLE=0`, `MC2_VK_POST_SCREENFX_SHADOW=0`, `MC2_CLOUD_SHADOW=0`,
`MC2_SHADOW_MOVER_SAMPLES=4`, and `MC2_RENDER_SCALE=0.5` (source:
`tools/mc2_launcher/quality_presets.h`). Thus output sizes 1280x720 and
1920x1080 use half-scale scene rendering. Disable the frame cap and vsync for
timing, record 60 seconds in each mission, and keep GPU timestamps and required
device-profile lines. Never report a validation pass if the validation layer
is absent. Visual captures must include `--no-kill`.

Jobs are argv arrays, never shell strings. Publish through the helper: it first
copies `.upload`, then atomically renames to `.json`. The runner claims
`.running`, executes serially at below-normal priority, and writes
`results/<id>/job.log` and `result.json`. Jobs can write artifacts into the
`MC2_FARM_RESULT_DIR` environment path; fetch copies that whole directory.
Smoke artifacts default to its `smoke` subdirectory, so fetch includes them.
`wait` exits nonzero for failed, interrupted, or timed-out jobs; its own timeout
does not cancel the remote job. Runner timeouts terminate only their own PID
and descendants. After a reboot, claimed jobs are marked interrupted, never
replayed. Do not submit jobs that detach processes.

To stop polling after the current job, create `C:\mc2farm\stop`; remove it before
starting the task again. Queue uploads must have unique IDs; completed `.done`
files are retained. The task and file lock prevent overlapping runners.

Offline verification:
`bash scripts/check-contracts.sh --rows '^farm_runner$' -j6`.

Nightly host job: `nightly_dual_gpu.json` is for a runner on the **main PC**,
whose GPUs are the RX 7900 XTX and RTX 2070 SUPER (not the laptop queue).
Run its argv nightly at 03:00 in the logged-in desktop session, or register
after merging with Windows Task Scheduler (daily, interactive, limited user):
`schtasks /Create /TN MC2NightlyDualGPU /SC DAILY /ST 03:00 /IT /RL LIMITED /TR "py -3 A:/Games/mc2-main/tools/farm/nightly_dual_gpu.py --exe A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"`.
The script runs both smoke-lite missions serially per GPU through the existing
lease/focus-safe harness. Each GPU gets a report line, logs and proofs under
`E:/mc2-archive-2026-10/proofs/nightly-dual-gpu` (or the farm result directory).
Missing validation, wrong GPU, residency refusals, denials, peak beyond the
declared low budget or VMA usage above 512 MiB fail the job. Successful texture
mip/resample reductions implement low quality and are allowed; terminal
placeholder/off/skipped-draw refusals fail. The low preset uses half-scale scene
rendering at 1080p within a **512 MiB logical residency budget**, including staging
and flight copies. Physical heaps still retain the OS/driver safety margin.
On UMA, the startup `vram_mib` is the device-local shared heap, not dedicated VRAM.

Lane exe: `submit --exe <worktree/build64-ninja/mc2.exe> [--data-delta <dir>] job.json` stages the exe and deploy's matching shaders (including baked SPIR-V), DLLs, definitions and support files into a per-job tree `C:\mc2farm\work\<id>`. PDBs stay on the host. A payload manifest replaces the seed manifest; payload files take precedence over the optional data overlay. Touched trees are copied so the seed is unchanged. `{EXE}` / `{ROOT}` in the job JSON become the staged paths; use `{EXE}` for `--ref-exe` to verify the submitted binary. `fetch` deletes the tree. Still no perf numbers or owner captures from the farm.

`{RESULT}` becomes the per-job result directory, which `fetch` retrieves.
Merge gates can use `--farm-determinism` to run the trio there alongside local
save/load checks; failures and missing farm evidence still fail the gate.
