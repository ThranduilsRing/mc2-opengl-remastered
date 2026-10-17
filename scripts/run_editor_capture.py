#!/usr/bin/env python3
"""Capture the Mission Editor window (scene + ImGui panels) to a TGA, background-only.

Launches `Mission Editor.exe -mission=<pak> -capture=<tga>` through nofocus.spawn (host lease,
minimized, no activation) and waits for the editor's `[EDITOR_CAPTURE v1] ... ok=1` line.
`--backend gl|vulkan` sets MC2_RENDER_BACKEND for the run (the editor default is vulkan).

  py -3 scripts/run_editor_capture.py --exe "<install>/Mission Editor.exe" --mission stock:mc2_01 --out shot.tga [--backend gl]
"""
import argparse, os, re, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from smoke_lib import nofocus  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--mission", required=True, help="pak path or stock:<name> (loose stock pak in the install)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--backend", choices=("gl", "vulkan"), default=None)
    ap.add_argument("--frame", type=int, default=150)
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--env", action="append", default=[], help="KEY=VALUE for the editor process")
    args = ap.parse_args()
    exe = Path(args.exe)
    deploy = exe.parent
    mission = args.mission
    if mission.lower().startswith("stock:"):
        mission = str(deploy / "data" / "missions" / (mission.split(":", 1)[1] + ".pak"))
    out = Path(args.out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()
    env = dict(os.environ)
    env["MC2_EDITOR_TRACE"] = "1"
    if args.backend:
        env["MC2_RENDER_BACKEND"] = args.backend
    for kv in args.env:
        k, v = kv.split("=", 1)
        env[k] = v
    argv = [str(exe), f"-mission={mission}", f"-capture={out}", f"-capture-frame={args.frame}",
            "-smoke-exit-sec=15", "-exit-on-load-fail"]
    t0 = time.time()
    proc = nofocus.spawn(argv, cwd=str(deploy), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        text, _ = proc.communicate(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        text, _ = proc.communicate()
        print("TIMEOUT")
    log = deploy / "editor-startup.log"
    logtext = log.read_text(errors="replace") if log.exists() else ""
    combined = (text or "") + "\n" + logtext
    m = re.findall(r"\[EDITOR_CAPTURE v1\] .*", combined)
    ok = bool(m) and "ok=1" in m[-1] and out.exists()
    print(f"rc={proc.returncode} sec={time.time() - t0:.1f} capture={m[-1] if m else 'none'} file={'yes' if out.exists() else 'no'}")
    (out.with_suffix(".log")).write_text(combined, encoding="utf-8", errors="replace")
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
