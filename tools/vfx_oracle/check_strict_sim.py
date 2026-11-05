"""Check retained pre/post SIMID logs against an existing strict baseline; never write one."""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import sim_determinism as sim


def check(base: dict, runs: list[list[str]]) -> dict:
    failures = []
    count = base["frames"]
    if type(count) is not int or count < sim.MIN_FRAMES:
        raise ValueError("baseline must cover at least 300 frames")
    if len(runs) != 4:
        raise ValueError("two pre-change and two post-change logs required")
    digest = sim.chain if base.get("chain_schema") == 2 else sim.full_chain
    chains = [digest(run, count) for run in runs]
    if any(len(run) < count for run in runs):
        failures.append("short run")
    if any(value != base["chain"] for value in chains):
        failures.append("strict baseline chain differs")
    for run in runs[1:]:
        ok, detail = sim.compare(runs[0], run, min_frames=count)
        if not ok:
            failures.append(detail)
    return {"accepted": not failures, "failures": failures, "strict_chains": chains,
            "baseline_chain": base["chain"], "baseline_frames": count}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("logs", type=Path, nargs=4, metavar="PRE1_PRE2_POST1_POST2")
    parser.add_argument("--install", required=True)
    args = parser.parse_args(argv)
    install = sim.resolve_install(args.install)
    if len({path.resolve() for path in args.logs}) != 4:
        parser.error("four distinct process logs required")
    base = json.loads(args.baseline.read_text(encoding="utf-8"))
    result = check(base, [sim.load_simid(str(path)) for path in args.logs])
    if not base.get("seed") or any(sim._seed(str(path)) != base["seed"] for path in args.logs):
        result["failures"].append("missing or mismatched mission seed")
    if base.get("data_fingerprint") != sim.data_fingerprint(install):
        result["failures"].append("missing or mismatched installed data fingerprint")
    result["accepted"] = not result["failures"]
    print(json.dumps(result, indent=2))
    return 0 if result["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
