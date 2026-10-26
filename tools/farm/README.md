# Farm desktop runner

The laptop is a separate, serial game slot. SSH is session 0; GPU jobs must run
through `MC2FarmRunner` in Kaitlyn's logged-in desktop session. All laptop files
belong under `C:\mc2farm`. No service or firewall changes are needed.

Copy `runner.py` and `install-task.cmd` to `C:\mc2farm`, then run
`C:\mc2farm\install-task.cmd` as Kaitlyn. It uses `schtasks /create /tn MC2FarmRunner
/sc ONLOGON /ru DESKTOP-Q11RABS\Kaitlyn /it /rl LIMITED /tr
"C:\Users\Kaitlyn\AppData\Local\Python\bin\python.exe C:\mc2farm\runner.py --root C:\mc2farm"`.
The task uses an interactive token and starts immediately and at logon. The runner
sets its own priority to BELOW_NORMAL before polling; children inherit it.
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

For Intel runs add `VK_LOADER_DRIVERS_SELECT=*igvk*` and verify the selected
device in the engine log before drawing any Intel conclusions. Launcher Low is
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
