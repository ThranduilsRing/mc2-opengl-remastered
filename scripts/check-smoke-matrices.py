#!/usr/bin/env python3
"""check-smoke-matrices.py -- validate smoke/capture matrix definitions.

VALIDATION-SCAFFOLD-PREFLIGHT-1 / CAPTURE-TOOLING-SAFETY-CHECKS-1.

Fails early on bad matrix JSON so a malformed capture matrix never reaches an
actual smoke run. Pure static check -- launches nothing, mutates nothing.

What it validates, per tests/smoke/matrices/*.json:
  * JSON parses
  * required fields exist (matrix_id, entries[]; each entry has a unique id)
  * matrix_id == filename stem (run_smoke_matrix.py resolves matrices by stem)
  * every entry env key is a syntactically valid MC2_* name AND is a row of the typed gate table
    (Gates/gate_table.json, generated from Gates/gate_table.cpp)
  * env values are scalars (str / int / bool), missions[] are known stems,
    duration is a positive int
  * mission stems exist in tests/smoke/smoke_missions.txt

It ALSO guards the matrix EXECUTION PATH (scripts/run_smoke_matrix.py):
  * must NOT inject --kill-existing (forbidden: taskkills concurrent mc2.exe ->
    false crash_silent; run_smoke.py already holds a concurrency-safe lock)
  * must expand missions as repeated --mission, never the nonexistent --missions

Usage:
  py -3 scripts/check-smoke-matrices.py            # check all matrices
  py -3 scripts/check-smoke-matrices.py --verbose  # also print known env set

Exit 0 = all clean. Exit 1 = at least one violation (CI-blocking).
"""

import json
import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent
MATRICES_DIR = ROOT / "tests" / "smoke" / "matrices"
GATE_TABLE_JSON = ROOT / "Gates" / "gate_table.json"
MISSIONS_TXT = ROOT / "tests" / "smoke" / "smoke_missions.txt"
RUN_SMOKE_MATRIX = SCRIPT_DIR / "run_smoke_matrix.py"

ENV_NAME_RE = re.compile(r"^MC2_[A-Z0-9_]+$")
MC2_TOKEN_RE = re.compile(r"MC2_[A-Z0-9_]+")


def load_known_env_vars() -> set:
    """Every gate name of the compiled gate table: the one authority for which MC2_* variables exist."""
    if not GATE_TABLE_JSON.exists():
        return set()
    return {g["name"] for g in json.loads(GATE_TABLE_JSON.read_text(encoding="utf-8"))["gates"]}


def load_known_missions() -> set:
    """Mission stems from smoke_missions.txt (any tier, excluding 'skip')."""
    stems = set()
    if not MISSIONS_TXT.exists():
        return stems
    for line in MISSIONS_TXT.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) >= 2 and parts[0] in ("tier1", "tier2", "tier3"):
            stems.add(parts[1])
    return stems


def check_matrix(path: Path, known_env: set, known_missions: set) -> list:
    """Return a list of error strings for one matrix file (empty = clean)."""
    errs = []
    try:
        matrix = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as e:
        return [f"{path.name}: JSON parse failed: {e}"]

    if not isinstance(matrix, dict):
        return [f"{path.name}: top-level must be an object"]

    mid = matrix.get("matrix_id")
    if not isinstance(mid, str) or not mid:
        errs.append(f"{path.name}: missing/empty required field 'matrix_id'")
    elif mid != path.stem:
        errs.append(f"{path.name}: matrix_id '{mid}' != filename stem '{path.stem}' "
                    f"(run_smoke_matrix.py resolves by stem)")

    entries = matrix.get("entries")
    if not isinstance(entries, list) or not entries:
        return errs + [f"{path.name}: missing/empty required field 'entries' (array)"]

    seen_ids = set()
    for i, entry in enumerate(entries):
        loc = f"{path.name} entry[{i}]"
        if not isinstance(entry, dict):
            errs.append(f"{loc}: entry must be an object")
            continue
        eid = entry.get("id")
        if not isinstance(eid, str) or not eid:
            errs.append(f"{loc}: missing/empty required field 'id'")
        else:
            loc = f"{path.name} entry '{eid}'"
            if eid in seen_ids:
                errs.append(f"{loc}: duplicate entry id")
            seen_ids.add(eid)

        env = entry.get("env", {})
        if not isinstance(env, dict):
            errs.append(f"{loc}: 'env' must be an object")
        else:
            for k, v in env.items():
                if not ENV_NAME_RE.match(k):
                    errs.append(f"{loc}: env key '{k}' is not a valid MC2_* name")
                elif k not in known_env:
                    errs.append(f"{loc}: env key '{k}' is not a row of Gates/gate_table.cpp")
                if not isinstance(v, (str, int, bool)):
                    errs.append(f"{loc}: env['{k}'] must be a scalar (str/int/bool), got {type(v).__name__}")

        missions = entry.get("missions")
        if missions is not None:
            if not isinstance(missions, list):
                errs.append(f"{loc}: 'missions' must be an array")
            else:
                for m in missions:
                    if known_missions and m not in known_missions:
                        errs.append(f"{loc}: mission '{m}' not in smoke_missions.txt")

        dur = entry.get("duration")
        if dur is not None and (not isinstance(dur, int) or isinstance(dur, bool) or dur <= 0):
            errs.append(f"{loc}: 'duration' must be a positive int")

    dm = matrix.get("default_missions")
    if dm is not None and isinstance(dm, list):
        for m in dm:
            if known_missions and m not in known_missions:
                errs.append(f"{path.name}: default_missions '{m}' not in smoke_missions.txt")
    return errs


def check_execution_path() -> list:
    """Guard run_smoke_matrix.py against forbidden CLI flags."""
    errs = []
    if not RUN_SMOKE_MATRIX.exists():
        return errs
    src = RUN_SMOKE_MATRIX.read_text(encoding="utf-8")
    if '"--kill-existing"' in src or "'--kill-existing'" in src:
        errs.append("run_smoke_matrix.py injects --kill-existing (forbidden: "
                    "taskkills concurrent mc2.exe -> false crash_silent)")
    if '"--missions"' in src or "'--missions'" in src:
        errs.append("run_smoke_matrix.py uses --missions (does not exist; "
                    "run_smoke.py takes repeated --mission)")
    return errs


def main() -> int:
    verbose = "--verbose" in sys.argv[1:]
    known_env = load_known_env_vars()
    known_missions = load_known_missions()
    if verbose:
        print(f"[smoke-matrices] known env vars: {len(known_env)}  "
              f"missions: {sorted(known_missions)}")

    if not MATRICES_DIR.is_dir():
        print(f"[smoke-matrices] FAIL: matrices dir not found: {MATRICES_DIR}", file=sys.stderr)
        return 1

    matrices = sorted(MATRICES_DIR.glob("*.json"))
    all_errs = []
    for path in matrices:
        errs = check_matrix(path, known_env, known_missions)
        if errs:
            all_errs += errs
        else:
            print(f"[smoke-matrices] PASS: {path.name}")

    exec_errs = check_execution_path()
    all_errs += exec_errs

    if all_errs:
        print(f"\n[smoke-matrices] FAIL: {len(all_errs)} violation(s):", file=sys.stderr)
        for e in all_errs:
            print(f"  - {e}", file=sys.stderr)
        return 1

    print(f"\n[smoke-matrices] OK: {len(matrices)} matrices clean; "
          f"execution path uses repeated --mission, no --kill-existing.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
