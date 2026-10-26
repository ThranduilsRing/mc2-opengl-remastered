"""Run the VFX MLR-vs-GPU-particle comparison under Vulkan through the existing capture harness (--install picks the lane install)."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import console_child
from smoke_lib import fingerprint
from smoke_lib import host_lease
import run_visual_capture

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from vfx_oracle.evidence import extract
INSTALL_ROOT = Path("A:/Games/mc2-opengl")
INSTALLS = ("lane2", "lane3")   # the lane installs the oracle may run against


def park_value(pose: dict) -> str:
    return ",".join(str(value) for value in
                    (pose["pos"][0], pose["pos"][1], pose["rot"], pose["proj"], pose["alt"])) + "@0"


def command(bookmarks: Path, out: Path, frame: int, arm: str, fixture: str = "loop",
            camera_park: str | None = None, experiment: str = "master",
            install: str = "lane3") -> list[str]:
    if arm not in {"mlr", "gpu"} or frame < 0 or fixture not in {"loop", "1"}:
        raise ValueError("unknown arm/fixture or negative frame")
    install_dir = Path(install) if Path(install).is_absolute() else INSTALL_ROOT / f"mc2-win64-{install}"
    cmd = [sys.executable, str(ROOT / "scripts/run_visual_capture.py"),
           "--no-kill", "--exe", str(install_dir / "mc2.exe"), "--bookmarks", str(bookmarks),
           "--out-dir", str(out), "--trigger-frame", str(frame),
           "--settle", "8", "--duration", "90", "--runs", "2"]
    # The selector alone makes the engine force GL; the umbrella is what makes the capture a Vulkan one.
    env = {"MC2_RENDER_BACKEND": "vulkan", "MC2_VK_SHARED_GBUFFER": "1", "MC2_SMOKE_FREEZE_ANIM": "1",
           "MC2_DETERMINISTIC_RNG": "1", "MC2_RNG_SEED": "0xC0FFEE",
           "MC2_FX_FORCE_SPAWN": fixture, "MC2_GPU_PARTICLES_LOG": "1",
           "MC2_FX_TRACE": "1", "MC2_SIM_IDENTITY_TRACE": "1",
           "MC2_VULKAN_VALIDATION": "1", "MC2_DEBUG_STATE_DUMP": "1",
           "MC2_GPU_PARTICLES": "1" if arm == "gpu" else "0"}
    env.update(gate_vector(arm, experiment))
    if camera_park is not None:
        env["MC2_CAMERA_PARK"] = camera_park
    for key, value in env.items():
        cmd += ["--env-declare", f"{key}={value}"]
    return cmd


def gate_vector(arm: str, experiment: str) -> dict[str, str]:
    if arm not in {"mlr", "gpu"} or experiment not in {"master", "harvested"}:
        raise ValueError("unknown arm or experiment")
    enabled = "1" if arm == "gpu" else "0"
    harvest = enabled if experiment == "harvested" else "0"
    return {"MC2_GPU_PARTICLES": enabled,
            "MC2_VFX_ORACLE_TUBE": harvest, "MC2_VFX_ORACLE_SHAPE": harvest}


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
    parser.add_argument("--experiment", choices=("master", "harvested"), default="master")
    parser.add_argument("--install", default="lane3")
    args = parser.parse_args(argv)
    if args.install not in INSTALLS and not Path(args.install).is_absolute():
        print(f"[capture_vfx_oracle] unknown --install {args.install!r}; known installs: {', '.join(INSTALLS)}",
              file=sys.stderr)
        return 2
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
                            "--out", str(args.out / name), "--arm", args.arm,
                            "--experiment", args.experiment,
                            "--install", args.install]) != 0
        return int(failed)
    if args.frame is None:
        parser.error("--frame is required for a bookmark file")
    if args.out.exists() and any(args.out.iterdir()):
        parser.error("capture output must be empty; stale evidence cannot be reused")
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
                      args.fixture, camera_park, args.experiment, args.install)
        (args.out / f"{arm}-command.json").write_text(json.dumps(cmd, indent=2) + "\n", encoding="utf-8")
        rc = capture(cmd)
        failed |= rc != 0
        for repeat in (1, 2):
            log = args.out / arm / f"r{repeat}" / f"{mission}_capture.log"
            log_text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
            diagnostic = {**extract(log_text), "log": str(log.resolve()),
                          "log_sha256": hashlib.sha256(log.read_bytes()).hexdigest() if log.exists() else None,
                          "experiment": args.experiment, "backend": "vulkan",
                          "gates": gate_vector(arm, args.experiment)}
            (args.out / f"{arm}-r{repeat}-runtime.json").write_text(
                json.dumps(diagnostic, indent=2) + "\n", encoding="utf-8")
            lines, bad = validate_fingerprint(log_text, head)
            for line in lines:
                print(f"{arm}/r{repeat} {line}", flush=True)
            fingerprint_checks.append({"arm": arm, "repeat": repeat, "log": str(log),
                                       "lines": lines, "failed": bad,
                                       "experiment": args.experiment,
                                       "gates": gate_vector(arm, args.experiment)})
            failed |= bad
        (args.out / "fingerprint-checks.json").write_text(
            json.dumps(fingerprint_checks, indent=2) + "\n", encoding="utf-8")
    return int(failed)


def capture(cmd: list[str]) -> int:
    # Acquire the lease before timed capture so contention cannot exhaust its deadline.
    host_lease.ensure_held("VFX oracle")
    previous = sys.argv
    try:
        sys.argv = cmd[1:]
        return run_visual_capture.main()
    finally:
        sys.argv = previous


if __name__ == "__main__":
    raise SystemExit(main())
