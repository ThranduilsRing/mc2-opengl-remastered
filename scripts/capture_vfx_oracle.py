"""Run the Q2 GL master-gate comparison on lane3 through the existing harness."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys

import console_child
from smoke_lib import fingerprint
from smoke_lib import host_lease
import run_visual_capture

ROOT = Path(__file__).resolve().parents[1]
INSTALL = Path("A:/Games/mc2-opengl/mc2-win64-lane3")


def park_value(pose: dict) -> str:
    return ",".join(str(value) for value in
                    (pose["pos"][0], pose["pos"][1], pose["rot"], pose["proj"], pose["alt"])) + "@0"


def command(bookmarks: Path, out: Path, frame: int, arm: str, fixture: str = "loop",
            camera_park: str | None = None) -> list[str]:
    if arm not in {"mlr", "gpu"} or frame < 0 or fixture not in {"loop", "1"}:
        raise ValueError("unknown arm/fixture or negative frame")
    cmd = [sys.executable, str(ROOT / "scripts/run_visual_capture.py"),
           "--exe", str(INSTALL / "mc2.exe"), "--bookmarks", str(bookmarks),
           "--out-dir", str(out), "--trigger-frame", str(frame),
           "--settle", "8", "--duration", "30", "--runs", "2"]
    env = {"MC2_RENDER_BACKEND": "gl", "MC2_SMOKE_FREEZE_ANIM": "1",
           "MC2_DETERMINISTIC_RNG": "1", "MC2_RNG_SEED": "0xC0FFEE",
           "MC2_FX_FORCE_SPAWN": fixture, "MC2_GPU_PARTICLES_LOG": "1",
           "MC2_GPU_PARTICLES": "1" if arm == "gpu" else "0"}
    if camera_park is not None:
        env["MC2_CAMERA_PARK"] = camera_park
    for key, value in env.items():
        cmd += ["--env-declare", f"{key}={value}"]
    return cmd


def validate_fingerprint(log_text: str, head: str) -> tuple[list[str], bool]:
    fp = fingerprint.parse_fingerprint(log_text)
    lines, bad = fingerprint.check_fingerprint(fp, head)
    return lines, bad or bool(fp and fp["dirty"])


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bookmarks", type=Path, nargs="?")
    parser.add_argument("--plan", type=Path)
    parser.add_argument("--frame", type=int)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--arm", choices=("mlr", "gpu", "both"), default="both")
    parser.add_argument("--fixture", choices=("loop", "1"), default="loop")
    args = parser.parse_args(argv)
    if bool(args.bookmarks) == bool(args.plan):
        parser.error("provide one bookmark file or --plan")
    if args.plan:
        failed = False
        scenes = json.loads(args.plan.read_text(encoding="utf-8"))["scenes"]
        if not scenes:
            parser.error("empty capture plan")
        seen = set()
        for scene in scenes:
            name = scene["id"]
            if name in seen or not name or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789_-" for c in name):
                parser.error("plan scene IDs must be unique safe directory names")
            seen.add(name)
        for scene in scenes:
            name = scene["id"]
            failed |= main([str(args.plan.parent / scene["bookmarks"]),
                            "--frame", str(scene["frame"]), "--fixture", scene["fixture"],
                            "--out", str(args.out / name), "--arm", args.arm]) != 0
        return int(failed)
    if args.frame is None:
        parser.error("--frame is required for a bookmark file")
    args.out.mkdir(parents=True, exist_ok=True)
    bookmarks = json.loads(args.bookmarks.read_text(encoding="utf-8"))
    mission = bookmarks["mission"]
    camera_park = park_value(bookmarks["bookmarks"][0])
    if any(park_value(pose) != camera_park for pose in bookmarks["bookmarks"]):
        raise ValueError("oracle bookmarks must duplicate one camera pose")
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                   text=True, **console_child.child_kwargs()).strip()
    failed = False
    fingerprint_checks = []
    for arm in (("mlr", "gpu") if args.arm == "both" else (args.arm,)):
        cmd = command(args.bookmarks.resolve(), (args.out / arm).resolve(), args.frame, arm,
                      args.fixture, camera_park)
        (args.out / f"{arm}-command.json").write_text(json.dumps(cmd, indent=2) + "\n", encoding="utf-8")
        rc = capture(cmd)
        failed |= rc != 0
        for repeat in (1, 2):
            log = args.out / arm / f"r{repeat}" / f"{mission}_capture.log"
            log_text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
            lines, bad = validate_fingerprint(log_text, head)
            for line in lines:
                print(f"{arm}/r{repeat} {line}", flush=True)
            fingerprint_checks.append({"arm": arm, "repeat": repeat, "log": str(log),
                                       "lines": lines, "failed": bad})
            failed |= bad
        (args.out / "fingerprint-checks.json").write_text(
            json.dumps(fingerprint_checks, indent=2) + "\n", encoding="utf-8")
    return int(failed)


def capture(cmd: list[str]) -> int:
    # Acquire the lease before timed capture so contention cannot exhaust its deadline.
    host_lease.ensure_held("Q2 VFX oracle lane3")
    previous = sys.argv
    try:
        sys.argv = cmd[1:]
        return run_visual_capture.main()
    finally:
        sys.argv = previous


if __name__ == "__main__":
    raise SystemExit(main())
