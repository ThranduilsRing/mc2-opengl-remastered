"""Hard merge gate: Remastered mc2_10 must instantiate bundled building art."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import console_child


def grade(text):
    applied = re.findall(r"\[BLDG_ART\].*?\|applied=([^\s]+)", text)
    count = sum(name != "-" for name in applied)
    priority = re.findall(r"building '([^']+)': user assets take priority", text)
    return dict(bldg_art_rows=len(applied), render_override_instances=count,
                user_priority_types=priority,
                must_check_pass=count > 0 and not priority)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--install", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    run_out = Path(tempfile.mkdtemp(prefix="run-", dir=args.out))
    env = dict(os.environ, MC2_DEBUG_STATE_DUMP="1",
               MC2_OPTIONS_OVERRIDE="RemasteredBuildings=TRUE",
               MC2_SMOKE_ARTIFACT_ROOT=str(run_out.resolve()))
    result = subprocess.run([sys.executable, str(Path(__file__).with_name("run_smoke.py")),
                             "--mission", "mc2_10", "--duration", "10", "--keep-logs",
                             "--exe", str(args.install / "mc2.exe")], env=env,
                            **console_child.child_kwargs())
    logs = sorted(run_out.glob("*/mc2_10.log"), key=lambda p: p.stat().st_mtime)
    row = grade(logs[-1].read_text(encoding="utf-8", errors="replace") if logs else "")
    row["smoke_returncode"] = result.returncode
    row["must_check_pass"] &= result.returncode == 0
    (args.out / "kit-visibility.json").write_text(json.dumps(row, indent=2) + "\n")
    print(f"kit-visibility: {'PASS' if row['must_check_pass'] else 'FAIL'} "
          f"mc2_10 overrides={row['render_override_instances']}")
    return 0 if row["must_check_pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
