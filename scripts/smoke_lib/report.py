# scripts/smoke_lib/report.py
"""Markdown + JSON report rendering."""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

from .logparse import LogSummary
from .gates import Verdict


@dataclass
class Row:
    stem: str
    verdict: Verdict
    summary: LogSummary
    destroys_delta: int = 0


def render_markdown(rows: list[Row], *, tier: str, profile: str,
                    timestamp: str) -> str:
    passed = sum(1 for r in rows if r.verdict.passed)
    total = len(rows)
    head = (f"# Smoke run {timestamp}  tier={tier}  profile={profile}  "
            f"result={'PASS' if passed == total else 'FAIL'} ({passed}/{total} passed)\n\n")
    table = [
        "| Mission | Result | Bucket                 | Frames | Avg FPS | p1% | Load ms | Δ destroys |",
        "|---------|--------|------------------------|--------|---------|-----|---------|-----------|"
    ]
    failures = []
    for r in rows:
        status = "PASS" if r.verdict.passed else "FAIL"
        bucket = ",".join(r.verdict.buckets)
        frames = r.summary.perf.samples
        avg = f"{r.summary.perf.avg_fps:.0f}" if r.summary.perf.avg_fps else "-"
        p1 = f"{r.summary.perf.p1low_fps:.0f}" if r.summary.perf.p1low_fps else "-"
        # Load column = ms to gameplay-ready (mission_ready), NOT first_frame,
        # which can fire during the logistics screen before gameplay starts.
        load = f"{r.summary.mission_ready_ms:.0f}" if r.summary.mission_ready_ms else "-"
        d = f"{r.destroys_delta:+d}" if r.destroys_delta is not None else "-"
        table.append(f"| {r.stem:<7} | {status:<6} | {bucket:<22} | "
                     f"{frames:<6} | {avg:<7} | {p1:<3} | {load:<7} | {d:<9} |")
        if not r.verdict.passed:
            failures.append(f"### {r.stem} — {bucket}\n" +
                            "\n".join(r.verdict.details))
    body = "\n".join(table) + "\n"
    if failures:
        body += "\n## Failures\n" + "\n\n".join(failures) + "\n"
    return head + body


def render_json(rows: list[Row], *, tier: str, profile: str,
                timestamp: str) -> dict:
    return {
        "timestamp": timestamp,
        "tier": tier,
        "profile": profile,
        "rows": [
            {
                "stem": r.stem,
                "result": "PASS" if r.verdict.passed else "FAIL",
                "buckets": r.verdict.buckets,
                "details": r.verdict.details,
                "avg_fps": r.summary.perf.avg_fps,
                "p1low_fps": r.summary.perf.p1low_fps,
                "mission_ready_ms": r.summary.mission_ready_ms,
                "destroys_delta": r.destroys_delta,
            }
            for r in rows
        ],
    }
