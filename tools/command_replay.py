#!/usr/bin/env python3
"""Record accepted orders and replay their tick hashes through the existing smoke harness."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import console_child


def fingerprint(install: Path, mission: str) -> str:
    """Mission and gameplay/mod data identity; engine revisions are compared by their state hashes."""
    digest = hashlib.sha256(mission.encode())
    extensions = {".fst", ".pak", ".fit", ".csv", ".abl", ".abx", ".abi", ".ini", ".json", ".cfg",
                  ".ase", ".glb", ".gltf", ".tgl", ".agl"}
    files = [p for p in install.iterdir() if p.is_file() and
             (p.suffix.lower() in {".fst", ".pak"} or p.name.lower() in {"mods.cfg", "mods_active.cfg", "mod.json"})]
    for directory in (install / "data", install / "mods"):
        if directory.exists():
            files.extend(p for p in directory.rglob("*") if p.is_file() and p.suffix.lower() in extensions)
    for path in sorted(set(files), key=lambda p: p.relative_to(install).as_posix()):
        digest.update(path.relative_to(install).as_posix().encode())
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
    return digest.hexdigest()


def inspect(path: Path) -> dict:
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or not lines[0].startswith("MC2REPLAY "):
        raise ValueError("invalid replay header")
    version = lines[0].split()[1]
    if version != "2":
        raise ValueError(f"unsupported replay format version {version}; expected 2")
    commands = [line for line in lines[1:] if line.startswith("C ")]
    hashes = [line for line in lines[1:] if line.startswith("H ")]
    if not hashes or lines[-1] != f"E {len(commands)} {len(hashes)}":
        raise ValueError("incomplete replay recording")
    ticks = [int(line.split()[1]) for line in hashes]
    if ticks != list(range(ticks[0], ticks[0] + len(ticks))):
        raise ValueError("missing replay tick")
    return {"commands": len(commands), "checkpoints": len(hashes), "first_tick": ticks[0], "last_tick": ticks[-1]}


def validate_commands(script: str) -> None:
    """Only selection and recorded order ingress are supported; fixtures bypass it."""
    for number, line in enumerate(script.splitlines(), 1):
        line = line.rstrip()
        if line and not re.fullmatch(
                r"select .+|order (?:stop|hold_fire|powerup|powerdown|withdraw)|order (?:move|attack|jump) .+", line):
            raise ValueError(f"command script line {number}: unsupported replay command {line!r}; "
                             "only select and order stop/move/attack/jump/hold_fire/powerup/powerdown/withdraw are recorded; "
                             "fixture mutations such as teleport are not replayed")


def run(args, label: str, extra: dict[str, str], expect: str) -> dict:
    output = args.out / label
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, MC2_SMOKE_FIXED_TIMESTEP="1", MC2_DETERMINISTIC_RNG="1",
               MC2_RNG_SEED=str(args.seed), MC2_REPLAY_CONTENT=args.content,
               MC2_SMOKE_REQUIRE_FINGERPRINT="1", MC2_MISSION_SPLIT="1")
    env.pop("MC2_UI_COMMAND_FILE", None)  # Playback must consume envelopes, not inherited fixtures.
    env.update(extra)
    executable = args.install / ("mc2.exe" if sys.platform == "win32" else "mc2")
    command = [sys.executable, str(ROOT / "scripts/run_smoke.py"), "--exe", str(executable),
               "--mission", args.mission, "--duration", str(args.duration), "--keep-logs"]
    result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True,
                            errors="replace", **console_child.child_kwargs(redirected=True))
    (output / "runner.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    artifacts = re.findall(r"^\[runner\] artifacts: (.+)$", result.stdout, re.MULTILINE)
    log = ""
    if artifacts:
        source = Path(artifacts[-1].strip()) / (args.mission + ".log")
        if source.exists():
            log = source.read_text(errors="replace")
            (output / "engine.log").write_text(log, encoding="utf-8")
    divergence = re.search(r"\[REPLAY_DIVERGED v1\] ([^\n]+)", log)
    ok = result.returncode == 0 and expect in log and not divergence
    report = {"label": label, "ok": ok, "smoke_exit": result.returncode,
              "first_divergence": divergence.group(1) if divergence else None, "env": extra}
    (output / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"{label}: {'PASS' if ok else 'FAIL'}" + (f" {divergence.group(1)}" if divergence else ""), flush=True)
    if not ok:
        raise RuntimeError(f"replay run failed; see {output}")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("record", "play", "proof", "inspect"))
    parser.add_argument("replay", type=Path)
    parser.add_argument("--install", type=Path)
    parser.add_argument("--mission", default="mc2_10")
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0xC0FFEE)
    parser.add_argument("--ticks", type=int, default=180)
    parser.add_argument("--duration", type=int, default=30)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--commands", type=Path, help="existing UI command channel script for the recording")
    args = parser.parse_args()
    if args.mode == "inspect":
        print(json.dumps(inspect(args.replay), indent=2))
        return 0
    if not args.install or not args.out:
        parser.error("--install and --out are required")
    args.install = args.install.resolve()
    args.replay = args.replay.resolve()
    args.out = args.out.resolve()
    script_text = (args.commands.read_text(encoding="utf-8") if args.commands else
                   "select all\norder move -4900 -5400\norder attack 706\n")
    if args.mode in ("record", "proof"):
        validate_commands(script_text)
    args.out.mkdir(parents=True, exist_ok=False)
    args.content = fingerprint(args.install, args.mission)
    reports = []
    if args.mode in ("record", "proof"):
        if args.replay.exists():
            parser.error("recording already exists")
        env = {"MC2_COMMAND_REPLAY_RECORD": str(args.replay), "MC2_COMMAND_REPLAY_TICKS": str(args.ticks),
               "MC2_FPS_CAP": "30"}
        script = args.out / "commands.txt"
        script.write_text(script_text, encoding="utf-8")
        env["MC2_UI_COMMAND_FILE"] = str(script)
        reports.append(run(args, "record", env, "[REPLAY_RECORDED v1]"))
    metadata = inspect(args.replay)
    if not metadata["commands"]:
        raise RuntimeError("recording contains no accepted commands")
    if args.mode in ("play", "proof"):
        matrix = [("fps30", {"MC2_FPS_CAP": "30"})]
        if args.mode == "proof":
            matrix += [("fps144", {"MC2_FPS_CAP": "144"}),
                       ("camera_far", {"MC2_FPS_CAP": "144", "MC2_CAMERA_PARK": "0,0,135,60,3000@1"}),
                       ("presentation_off", {"MC2_FPS_CAP": "144", "MC2_PRESENTATION_DISABLED": "1"})]
        for label, env in matrix:
            reports.append(run(args, label, {"MC2_COMMAND_REPLAY_PLAY": str(args.replay), **env}, "[REPLAY_COMPLETE v1]"))
    (args.out / "proof.json").write_text(json.dumps({"replay": str(args.replay), "fingerprint": args.content,
        **metadata, "runs": reports}, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print(f"command_replay: {error}", file=sys.stderr)
        raise SystemExit(1)
