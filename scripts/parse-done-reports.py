#!/usr/bin/env python3
"""parse-done-reports.py — scan worktrees for LANE_COMPLETION_REPORT.md files
and emit a merge-ready summary table.

Searches:
  .claude/worktrees/*/LANE_COMPLETION_REPORT.md   (active lane worktrees)
  Additional paths passed via --report

Usage:
  py -3 scripts/parse-done-reports.py
  py -3 scripts/parse-done-reports.py --report path/to/LANE_COMPLETION_REPORT.md
  py -3 scripts/parse-done-reports.py --json
  py -3 scripts/parse-done-reports.py --filter READY
  py -3 scripts/parse-done-reports.py --strict   # exit 1 if any required field missing

Exit codes:
  0  all reports parsed, no missing required fields (or no reports found)
  1  --strict: one or more reports have missing required fields
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

REPORT_NAME = "LANE_COMPLETION_REPORT.md"
TEMPLATE_NAME = "docs/LANE_COMPLETION_REPORT.template.md"


def _find_main_worktree() -> Path:
    """Return the main worktree root (first entry of git worktree list)."""
    here = str(Path(__file__).resolve().parent)
    result = subprocess.run(
        ["git", "worktree", "list", "--porcelain"],
        capture_output=True, text=True, cwd=here,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    for line in result.stdout.splitlines():
        if line.startswith("worktree "):
            return Path(line[len("worktree "):].strip())
    # Fallback: two levels up from scripts/ (nifty-mendeleev worktree root).
    return Path(__file__).resolve().parent.parent


MAIN_ROOT = _find_main_worktree()
WORKTREES_DIR = MAIN_ROOT / ".claude" / "worktrees"

REQUIRED_FIELDS = [
    "branch",
    "commit",
    "base_commit",
    "files_changed",
    "risk_class",
    "build_run",
    "smoke_run",
    "deploy_state",
    "merge_recommendation",
]

VALID = {
    "risk_class":           {"low", "medium", "high"},
    "build_run":            {"PASS", "FAIL", "skipped"},
    "smoke_run":            {"PASS", "FAIL", "skipped"},
    "deploy_state":         {"deployed", "not-deployed", "N/A"},
    "merge_recommendation": {"READY", "HOLD", "NEEDS-REVIEW"},
}

# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

_FRONTMATTER_RE = re.compile(r"^---\s*\n(.*?)\n---", re.DOTALL)
_KV_RE = re.compile(r"^([a-z_]+)\s*:\s*(.+)$", re.MULTILINE)


def parse_report(path: Path) -> dict:
    """Parse a LANE_COMPLETION_REPORT.md, return a dict of fields + metadata."""
    text = path.read_text(encoding="utf-8", errors="replace")
    result: dict = {"_path": str(path), "_errors": [], "_warnings": []}

    m = _FRONTMATTER_RE.search(text)
    if not m:
        result["_errors"].append("no --- frontmatter block found")
        return result

    block = m.group(1)
    # Strip comment lines (lines starting with #) before parsing kv.
    block_lines = [ln for ln in block.splitlines() if not ln.lstrip().startswith("#")]
    for kv in _KV_RE.finditer("\n".join(block_lines)):
        key, val = kv.group(1).strip(), kv.group(2).strip()
        # Strip inline comments.
        val = re.sub(r"\s+#.*$", "", val).strip()
        result[key] = val

    # Check required fields.
    for f in REQUIRED_FIELDS:
        if f not in result:
            result["_errors"].append(f"missing required field: {f}")
        elif result[f] in {"<full sha>", f"<{f}>", "", "0"} and f not in {"files_changed"}:
            result["_warnings"].append(f"field '{f}' looks unfilled: {result[f]!r}")

    # Validate enum fields.
    for field, allowed in VALID.items():
        if field in result and result[field] not in allowed:
            result["_warnings"].append(
                f"field '{field}' value {result[field]!r} not in {sorted(allowed)}"
            )

    return result


def find_reports(extra: list[Path]) -> list[Path]:
    found: list[Path] = []
    if WORKTREES_DIR.is_dir():
        for wt in sorted(WORKTREES_DIR.iterdir()):
            r = wt / REPORT_NAME
            if r.is_file():
                found.append(r)
    for p in extra:
        if p.is_file() and p not in found:
            found.append(p)
    return found


# ---------------------------------------------------------------------------
# Formatting
# ---------------------------------------------------------------------------

COL_WIDTHS = {
    "branch":              32,
    "commit":              10,
    "risk_class":           7,
    "build_run":            8,
    "smoke_run":            8,
    "deploy_state":        14,
    "merge_recommendation": 14,
    "files_changed":        6,
}

STATUS_MARKERS = {
    "READY":        "✓",
    "HOLD":         "⏸",
    "NEEDS-REVIEW": "?",
    "PASS":         "✓",
    "FAIL":         "✗",
    "skipped":      "-",
    "deployed":     "✓",
    "not-deployed": "-",
    "N/A":          "-",
}


def _col(val: str, width: int) -> str:
    val = val or "—"
    return val[:width].ljust(width)


def print_table(reports: list[dict], filter_rec: str | None) -> None:
    if filter_rec:
        reports = [r for r in reports if r.get("merge_recommendation") == filter_rec]

    if not reports:
        print("[done-reports] no reports found" + (f" matching --filter {filter_rec}" if filter_rec else ""))
        return

    header = (
        f"{'BRANCH':<32}  {'COMMIT':<10}  {'RISK':<7}  "
        f"{'BUILD':<8}  {'SMOKE':<8}  {'DEPLOY':<14}  {'MERGE':<14}  {'FILES':>5}"
    )
    sep = "-" * len(header)
    print(sep)
    print(header)
    print(sep)

    for r in reports:
        branch  = r.get("branch", "?")
        commit  = r.get("commit", "?")[:10]
        risk    = r.get("risk_class", "?")
        build   = r.get("build_run", "?")
        smoke   = r.get("smoke_run", "?")
        deploy  = r.get("deploy_state", "?")
        rec     = r.get("merge_recommendation", "?")
        files   = r.get("files_changed", "?")
        errors  = r["_errors"]
        warnings = r["_warnings"]

        row = (
            f"{_col(branch, 32)}  {_col(commit, 10)}  {_col(risk, 7)}  "
            f"{_col(build, 8)}  {_col(smoke, 8)}  {_col(deploy, 14)}  "
            f"{_col(rec, 14)}  {files:>5}"
        )
        print(row)

        if errors:
            for e in errors:
                print(f"  ERROR: {e}")
        if warnings:
            for w in warnings:
                print(f"  WARN:  {w}")

    print(sep)
    ready = sum(1 for r in reports if r.get("merge_recommendation") == "READY")
    hold  = sum(1 for r in reports if r.get("merge_recommendation") == "HOLD")
    nr    = sum(1 for r in reports if r.get("merge_recommendation") == "NEEDS-REVIEW")
    print(f"  {len(reports)} report(s): {ready} READY  {hold} HOLD  {nr} NEEDS-REVIEW")
    print(sep)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(
        description="Parse LANE_COMPLETION_REPORT.md files and emit a summary table."
    )
    ap.add_argument(
        "--report", action="append", type=Path, default=[],
        metavar="PATH",
        help="Additional LANE_COMPLETION_REPORT.md path(s) to include.",
    )
    ap.add_argument(
        "--filter",
        metavar="REC",
        help="Only show reports with this merge_recommendation (READY|HOLD|NEEDS-REVIEW).",
    )
    ap.add_argument(
        "--json", dest="as_json", action="store_true",
        help="Emit JSON array instead of table.",
    )
    ap.add_argument(
        "--strict", action="store_true",
        help="Exit 1 if any report has missing required fields.",
    )
    args = ap.parse_args()

    paths = find_reports(args.report)
    if not paths:
        print(f"[done-reports] no {REPORT_NAME} files found under {WORKTREES_DIR}")
        print(f"[done-reports] create one with: cp {TEMPLATE_NAME} "
              f"<worktree-root>/{REPORT_NAME}")
        return

    reports = [parse_report(p) for p in paths]

    if args.as_json:
        # Strip internal keys for clean output.
        out = []
        for r in reports:
            d = {k: v for k, v in r.items() if not k.startswith("_")}
            d["_path"] = r["_path"]
            d["_errors"] = r["_errors"]
            d["_warnings"] = r["_warnings"]
            out.append(d)
        print(json.dumps(out, indent=2))
    else:
        print_table(reports, args.filter)

    if args.strict:
        has_errors = any(r["_errors"] for r in reports)
        if has_errors:
            sys.exit(1)


if __name__ == "__main__":
    main()
