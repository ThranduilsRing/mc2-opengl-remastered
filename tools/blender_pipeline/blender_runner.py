"""
blender_runner.py — locate Blender and run a headless bpy script.

BLENDER-ASSET-PIPELINE-1. Same discovery/invocation pattern as
tools/mech_import/blender_runner.py (BT2018-BLENDER-RUNNER-1), vendored here
so the pipeline is self-contained in worktrees that lack tools/mech_import.

Probe order: BLENDER_EXECUTABLE env var wins, then known install locations
(Blender 5.1 confirmed on this machine), then PATH.

Usage (as a module):
    from blender_runner import find_blender_exe, run_blender_script
    exe = find_blender_exe()
    run_blender_script(exe, "recipes/upscale_mesh.py", {"in": "x.glb", "out": "y.glb"})

Usage (CLI smoke):
    py -3 blender_runner.py --version      # prints located blender + its version
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

_FALLBACK_PROBES = [
    r"C:\Program Files\Blender Foundation\Blender 5.1\blender.exe",
    r"C:\Program Files\Blender Foundation\Blender 5.0\blender.exe",
    r"C:\Program Files\Blender Foundation\Blender 4.2\blender.exe",
    r"C:\Program Files\Blender\blender.exe",
]


def find_blender_exe(explicit=None):
    """Return a Path to blender.exe or raise RuntimeError with actionable text.

    Order: explicit arg > BLENDER_EXECUTABLE env var > known installs > PATH.
    """
    if explicit:
        p = Path(explicit)
        if p.is_file():
            return p
        raise RuntimeError(f"--blender {explicit!r} does not point at a file.")

    env = os.environ.get("BLENDER_EXECUTABLE")
    if env:
        p = Path(env)
        if p.is_file():
            return p
        raise RuntimeError(
            f"BLENDER_EXECUTABLE={env!r} is set but does not point at a file."
        )

    for cand in _FALLBACK_PROBES:
        p = Path(cand)
        if p.is_file():
            return p

    on_path = shutil.which("blender")
    if on_path:
        return Path(on_path)

    raise RuntimeError(
        "Could not locate blender.exe. Set BLENDER_EXECUTABLE to the full path, "
        "or install Blender to a standard location. Probed:\n  "
        + "\n  ".join(_FALLBACK_PROBES)
    )


def build_blender_cmd(blender_exe, script_path, args):
    """Build the headless command line (pure — unit-testable without Blender).

    Recipe args are passed after `--` as --key=value pairs; bool True becomes a
    bare flag, None/False keys are omitted.
    """
    cmd = [str(blender_exe), "--background", "--factory-startup",
           "--python", str(Path(script_path).resolve()), "--"]
    for k, v in args.items():
        if v is None or v is False:
            continue
        if v is True:
            cmd.append(f"--{k}")
        else:
            cmd.append(f"--{k}={v}")
    return cmd


def run_blender_script(blender_exe, script_path, args, cwd=None, log_path=None):
    """Run `blender --background --factory-startup --python script -- --k=v ...`.

    Returns stdout. Raises RuntimeError on non-zero exit, including the tail of
    stderr/stdout so the failing bpy line is visible without re-running.
    """
    cmd = build_blender_cmd(blender_exe, script_path, args)
    proc = subprocess.run(
        cmd, capture_output=True, text=True, cwd=cwd,
        encoding="utf-8", errors="replace",
    )
    if log_path:
        try:
            with open(log_path, "w", encoding="utf-8") as f:
                f.write("$ " + " ".join(cmd) + "\n\n")
                f.write(proc.stdout or "")
                f.write("\n--- stderr ---\n")
                f.write(proc.stderr or "")
        except OSError:
            pass
    if proc.returncode != 0:
        tail = (proc.stdout or "")[-2000:] + "\n--- stderr ---\n" + (proc.stderr or "")[-2000:]
        raise RuntimeError(
            f"blender exited {proc.returncode} on {Path(script_path).name}\n{tail}"
        )
    return proc.stdout


if __name__ == "__main__":
    exe = find_blender_exe()
    print(f"[blender_runner] found: {exe}", flush=True)
    if "--version" in sys.argv:
        out = subprocess.run(
            [str(exe), "--background", "--version"],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        print(out.stdout.strip().splitlines()[0] if out.stdout else "(no version output)")
