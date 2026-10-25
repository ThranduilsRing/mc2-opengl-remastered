#!/usr/bin/env python3
"""Dump and compare engine building cells, rubble, LOS and bounds for both art modes."""
from __future__ import annotations

import argparse
from collections import Counter
import json
import os
import re
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import console_child

PREFIX = "[BLDG_GAMEPLAY] "
NUMBER = r"-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?"
CELL = r"\d+,\d+;"
STATE = rf"(?:absent|(?:\d+,\d+,[01],[01],\d+,-?\d+,-?\d+;)*/los=(?:\d+,\d+,{NUMBER};)*/end)"
ROW = re.compile(rf"[^|\r\n]+\|{NUMBER},{NUMBER},{NUMBER}\|{NUMBER}\|-?\d+\|{NUMBER},{NUMBER}"
                 rf"\|covered=(?:{CELL})*\|kind=\d+\|state={STATE}\|state={STATE}")


def records(log: str) -> Counter:
    rows = Counter(line.split(PREFIX, 1)[1] for line in log.splitlines() if PREFIX in line)
    if not rows or not any("|covered=" in row for row in rows):
        raise ValueError("no building gameplay observations")
    if any(not ROW.fullmatch(row) for row in rows):
        raise ValueError("incomplete intact/destroyed observation")
    return rows


def compare(classic: str, remastered: str) -> dict:
    a, b = records(classic), records(remastered)
    removed, added = a - b, b - a
    return {"observations": a.total(), "diffs": removed.total() + added.total(),
            "classic_only": list(removed)[:3], "remastered_only": list(added)[:3]}


def coverage(classic: str, remastered: str, eligible: set[str]) -> dict:
    identities = dict(re.findall(r"\[BLDG_IDENTITY\] ([^|\r\n]+)\|key=([^\r\n]*)", classic))
    placements = Counter(row.split("|kind=", 1)[0] for row in records(classic).elements())
    identities = {name.lower(): key.lower() for name, key in identities.items()}
    names = {p: p.split("|", 1)[0].lower() for p in placements}
    missing_ids = set(names.values()) - identities.keys()
    if missing_ids:
        raise ValueError(f"missing appearance identities: {sorted(missing_ids)}")
    expected = {p: identities[n] for p, n in names.items() if identities[n] in eligible}
    applied = dict((placement, key.lower()) for placement, key in
                   re.findall(r"\[BLDG_ART\] ([^\r\n]+)\|applied=([^\r\n]+)", remastered))
    missing = {p: key for p, key in expected.items() if applied.get(p) != key}
    if missing:
        raise ValueError(f"missing observed render replacements: {missing}")
    return {"expected_replacements": {names[p]: key for p, key in expected.items()},
            "replaced_observations": sum(placements[p] for p in expected)}


def run(exe: Path, out: Path, missions: list[str]) -> int:
    out.mkdir(parents=True, exist_ok=True)
    results = {}
    eligible = set()
    for manifest in (exe.parent / "data/model_overrides/models.json",
                     exe.parent / "mods/mc2-industrial-kit/data/model_overrides/models.json"):
        if manifest.is_file():
            eligible.update(row["replaces"].split(":", 1)[1].lower()
                            for row in json.loads(manifest.read_text(encoding="utf-8"))["overrides"]
                            if row.get("replaces", "").lower().startswith("staticprop:"))
    if not eligible:
        raise ValueError("no expected building replacements in install manifests")
    for mission in missions:
        logs = {}
        for mode in ("Classic", "Remastered"):
            env = dict(os.environ, MC2_DEBUG_STATE_DUMP="1",
                       MC2_OPTIONS_OVERRIDE=f"RemasteredBuildings={'FALSE' if mode == 'Classic' else 'TRUE'}")
            result = subprocess.run(
                [sys.executable, str(ROOT / "scripts/run_smoke.py"), "--exe", str(exe),
                 "--mission", mission, "--duration", "15", "--keep-logs"],
                cwd=ROOT, env=env, capture_output=True, text=True, errors="replace",
                **console_child.child_kwargs(redirected=True))
            (out / f"{mission}-{mode}-runner.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            artifacts = [line.removeprefix("[runner] artifacts: ").strip()
                         for line in result.stdout.splitlines() if line.startswith("[runner] artifacts: ")]
            if not artifacts:
                raise RuntimeError(f"{mission} {mode}: no artifacts (runner rc={result.returncode})")
            log = (Path(artifacts[-1]) / f"{mission}.log").read_text(encoding="utf-8", errors="replace")
            (out / f"{mission}-{mode}.log").write_text(log, encoding="utf-8")
            logs[mode] = log
            if result.returncode:
                raise RuntimeError(f"{mission} {mode}: smoke failed rc={result.returncode}; see runner log")
        row = compare(logs["Classic"], logs["Remastered"])
        row.update(coverage(logs["Classic"], logs["Remastered"], eligible))
        row["render_overrides"] = logs["Remastered"].count("render override applied")
        results[mission] = row
        (out / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
        print(f"{mission}: {row['observations']} observations, {row['diffs']} diffs, "
              f"{row['render_overrides']} render overrides", flush=True)
    if not any(row["replaced_observations"] for row in results.values()):
        raise RuntimeError("Remastered loaded no render overrides; comparison is not an art A/B")
    return int(any(row["diffs"] for row in results.values()))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--missions", nargs="+", default=[f"mc2_{i:02}" for i in range(1, 25)])
    args = ap.parse_args()
    return run(args.exe.resolve(), args.out, args.missions)


if __name__ == "__main__":
    sys.exit(main())
