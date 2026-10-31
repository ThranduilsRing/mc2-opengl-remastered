"""Interleaved A/B mission profiles; noise is evidence, never an exclusion."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import random
from statistics import median
import subprocess
import sys
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mission_cpu_profile as cpu

ZONES = ("Frame", "GameLogic", "DrawScreen", "SwapWindow")


def paired_delta(a, b):
    """Resample whole adjacent pairs, not correlated events within captures."""
    if len(a) != len(b) or not a or not all(math.isfinite(x) for x in [*a, *b]):
        raise ValueError("finite equal-length paired samples required")
    deltas = [y - x for x, y in zip(a, b)]
    rng = random.Random(20261030)
    boot = sorted(median(rng.choices(deltas, k=len(deltas))) for _ in range(10000))
    lo, hi = boot[499], boot[9499]
    verdict = ("INCONCLUSIVE" if len(deltas) < 5 else
               "WIN" if hi < 0 else "REGRESSION" if lo > 0 else
               "NO CHANGE" if lo >= -.02 and hi <= .02 else "INCONCLUSIVE")
    return {"pairs": len(deltas), "pair_deltas_ms": deltas,
            "delta_ms": median(deltas), "ci90_ms": [lo, hi], "verdict": verdict}


def comparison(pairs):
    result = {}
    valid = [p for p in pairs if all(a in p and not p[a].get("error") for a in ("a", "b"))]
    names = set(ZONES) & set.intersection(*(set(p[a]["zones"]) for p in valid for a in ("a", "b"))) if valid else set()
    for name in sorted(names):
        row = paired_delta(*[[p[a]["zones"][name]["median_ms"] for p in valid] for a in ("a", "b")])
        for arm in ("a", "b"):
            rows = [p[arm]["zones"][name] for p in valid]
            row[arm] = {"mean_ms": median(r["total_ms"] / r["count"] for r in rows),
                        **{k: median(r[k] for r in rows) for k in ("median_ms", "p95_ms", "min_ms", "q25_ms")},
                        "reserved_external_cpu_p95": [p[arm]["noise"].get("external_cpu_p95") for p in valid]}
        result[name] = row
    return result


def table(report):
    lines = ["| Mission | Zone | A mean/p95 ms | B mean/p95 ms | Delta median [90% CI] ms | Verdict | Reserved external CPU p95 A/B % |",
             "|---|---|---:|---:|---:|---|---|"]
    for mission, pairs in report["runs"].items():
        for zone, r in comparison(pairs).items():
            if zone not in ZONES:
                continue
            a, b = r["a"], r["b"]
            lo, hi = r["ci90_ms"]
            lines.append(f"| {mission} | {zone} | {a['mean_ms']:.3f}/{a['p95_ms']:.3f} | {b['mean_ms']:.3f}/{b['p95_ms']:.3f} | {r['delta_ms']:+.3f} [{lo:+.3f}, {hi:+.3f}] | {r['verdict']} N={r['pairs']} | {a['reserved_external_cpu_p95']}/{b['reserved_external_cpu_p95']} |")
    return "\n".join(lines)


def run(args, psutil):
    args.out.mkdir(parents=True, exist_ok=True)
    report = {"arms": {}, "runs": {}, "errors": [], "policy": "B-A; paired median bootstrap 90%; equivalence +/-0.02 ms; noise annotates"}
    for arm in ("a", "b"):
        exe = getattr(args, "exe_" + arm)
        install = getattr(args, "install_" + arm)
        path = exe or install / "mc2.exe"
        report["arms"][arm] = {"exe": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    for mission in args.missions:
        pairs = report["runs"].setdefault(mission, [])
        for i in range(args.pairs):
            pair = {}
            pairs.append(pair)
            for arm in ("a", "b"):
                directory = args.out / mission / f"pair{i + 1}-{arm}"
                exe = getattr(args, "exe_" + arm)
                try:
                    install = getattr(args, "install_" + arm) or args.install
                    if exe:
                        source = next(p for p in exe.resolve().parents if (p / "scripts/deploy_payload.py").is_file())
                        with (args.out / "deploy.log").open("a", encoding="utf-8") as log:
                            subprocess.run([sys.executable, str(cpu.ROOT / "scripts/deploy_payload.py"),
                                str(install), "--target", "game", "--source-root", str(source),
                                "--build-dir", str(exe.parent), "--allow-new-target", "--skip-kit"],
                                stdout=log, stderr=subprocess.STDOUT, check=True)
                    options = SimpleNamespace(install=install, seconds=args.seconds, short_view=True)
                    pair[arm] = cpu.profile(options, mission, directory, psutil)
                except (Exception, SystemExit) as exc:
                    pair[arm] = {"error": str(exc)}
                    report["errors"].append(f"{mission}/{i + 1}/{arm}: {exc}")
                (args.out / "profile.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
            print(f"{mission} pair {i + 1}: " + ", ".join(f"{a}={pair[a].get('error', 'OK')}" for a in ("a", "b")), flush=True)
        report.setdefault("comparisons", {})[mission] = comparison(pairs)
        (args.out / "profile.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        (args.out / "profile.md").write_text(table(report) + "\n", encoding="utf-8")
    print(table(report), flush=True)
    return 1 if report["errors"] else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for arm in ("a", "b"):
        group = parser.add_mutually_exclusive_group(required=True)
        group.add_argument("--exe-" + arm, type=Path, help="deploy this build's payload before each host arm")
        group.add_argument("--install-" + arm, type=Path, help="already deployed private arm on farm")
    parser.add_argument("--install", type=Path, help="single host lane install, required with --exe")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--pairs", type=int, default=5)
    parser.add_argument("--seconds", type=int, default=20)
    parser.add_argument("--missions", nargs="+", choices=cpu.DEFAULT_MISSIONS, default=cpu.DEFAULT_MISSIONS)
    args = parser.parse_args(argv)
    if args.pairs < 5 or not 15 <= args.seconds <= 20:
        parser.error("at least five pairs and 15-20 second captures required")
    if (args.exe_a or args.exe_b) and not args.install:
        parser.error("--exe arms require --install")
    import psutil
    if os.name == "nt":
        psutil.Process().nice(psutil.IDLE_PRIORITY_CLASS)
    return run(args, psutil)


if __name__ == "__main__":
    raise SystemExit(main())
