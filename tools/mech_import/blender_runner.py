"""
blender_runner.py — locate Blender and run a headless bpy script.

Part of the BT2018 mech-import arc (BT2018-BLENDER-RUNNER-1).
No Blender runner existed in the repo before this; the cook pipeline only
shelled out to ktx.exe. This is the missing glue that lets mech_prep.py
(and any future bpy cook step) run from plain Python.

Usage (as a module):
    from blender_runner import find_blender_exe, run_blender_script
    exe = find_blender_exe()
    run_blender_script(exe, "mech_prep.py", {"src": "x.fbx", "out_glb": "y.glb"})

Usage (CLI smoke):
    py -3 blender_runner.py --version      # prints located blender + its version
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

# Probe order: env var wins, then known install locations (Blender 5.1 confirmed
# on this machine), then PATH. Keep the explicit version dirs newest-first.
_FALLBACK_PROBES = [
    r"C:\Program Files\Blender Foundation\Blender 5.1\blender.exe",
    r"C:\Program Files\Blender Foundation\Blender 5.0\blender.exe",
    r"C:\Program Files\Blender Foundation\Blender 4.2\blender.exe",
    r"C:\Program Files\Blender\blender.exe",
]


def find_blender_exe() -> Path:
    """Return a path to blender.exe or raise RuntimeError with actionable text.

    Override with the BLENDER_EXECUTABLE env var (full path to blender.exe).
    """
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


def run_blender_script(
    blender_exe: Path,
    script_path: str,
    args: dict,
    cwd: str = None,
) -> str:
    """Run `blender --background --python script -- --k=v ...` and return stdout.

    Raises RuntimeError on non-zero exit, including the tail of stderr/stdout so
    the failing bpy line is visible without re-running.
    """
    script_path = str(Path(script_path).resolve())
    cmd = [str(blender_exe), "--background", "--python", script_path, "--"]
    for k, v in args.items():
        cmd.append(f"--{k}={v}")

    proc = subprocess.run(
        cmd, capture_output=True, text=True, cwd=cwd, encoding="utf-8", errors="replace",
        **console_child.child_kwargs(redirected=True),
    )
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
            **console_child.child_kwargs(redirected=True),
        )
        print(out.stdout.strip().splitlines()[0] if out.stdout else "(no version output)")
