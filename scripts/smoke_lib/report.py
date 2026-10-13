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
    #: The render options the engine ran with (scripts/smoke_lib/render_options_pin.py record()["values"]); None when not recorded.
    render_options: Optional[dict] = None


def _options_line(rows: list[Row]) -> str:
    """One report line naming the render options of the run: 'all defaults', or the keys that differ from the table defaults."""
    from . import render_options_pin
    recorded = [r.render_options for r in rows if r.render_options]
    if not recorded:
        return "> **Render options:** not recorded\n\n"
    shown = []
    for values in recorded:
        text = render_options_pin.describe(values)
        if text not in shown:
            shown.append(text)
    return "> **Render options:** " + " | ".join(shown) + "\n\n"


def _vram_line(rows: list[Row]) -> str:
    """One report line with the GPU memory of every mission that printed `[VRAM v1]` (load end -> run end); nothing when no row did."""
    from . import vram
    parts = []
    for r in rows:
        samples = getattr(r.summary, "vram", None) or []
        if samples:
            parts.append(f"{r.stem} " + "; ".join(vram.describe(s) for s in samples))
    return ("> **VRAM:** " + " | ".join(parts) + "\n\n") if parts else ""


def _vk_profile_line(rows: list[Row]) -> str:
    """One report line with the Vulkan device profile verdict of the run (the first mission that printed one): which routes the device offers."""
    from . import vk_profile
    for r in rows:
        profiles = getattr(r.summary, "vk_profile", None) or []
        if profiles:
            return "> **Vulkan profile:** " + vk_profile.describe(profiles[0]) + "\n\n"
    return ""


def _resolution_line(tier: str) -> str:
    """The size a pinned tier ran at (the fps numbers mean this size); other tiers take the install's options.cfg size and say nothing."""
    from . import render_resolution
    size = render_resolution.asked_size(tier)
    if not size:
        return ""
    return f"> **Resolution:** {size[0]}x{size[1]} (pinned for {tier}, scripts/smoke_lib/render_resolution.py)\n\n"


def render_markdown(rows: list[Row], *, tier: str, profile: str,
                    timestamp: str, show_window: bool = False,
                    run_failures: Optional[list[str]] = None) -> str:
    """Render the smoke report.

    SMOKE-DEPLOY-TRAP-1: `run_failures` carries RUN-LEVEL failures that are not
    attributable to any single mission -- above all a DEPLOY_FINGERPRINT
    mismatch, i.e. "every mission passed, but on the wrong binary". Without it
    the header said `result=PASS (5/5 passed)` while the runner exited 1, and
    that header is what report.md / report.json / the get_latest_smoke_report()
    MCP tool serve to the next reader. The exit code was honest; the artifact
    everyone actually quotes was not.
    """
    run_failures = list(run_failures or [])
    passed = sum(1 for r in rows if r.verdict.passed)
    total = len(rows)
    run_ok = not run_failures
    fps_note = (
        "window starts visible; FPS reflects foreground throughput subject to "
        "the active MC2_FPS_CAP / MC2_VSYNC settings."
        if show_window else
        "game window starts minimized (SW_SHOWMINNOACTIVE). SDL throttles "
        "rendering to ~100 fps when minimized; Avg FPS / p1% reflect that cap, "
        "not GPU throughput. Not a perf regression."
    )
    _verdict = "PASS" if (passed == total and run_ok) else "FAIL"
    head = (f"# Smoke run {timestamp}  tier={tier}  profile={profile}  "
            f"result={_verdict} ({passed}/{total} passed)\n\n"
            f"> **FPS note:** {fps_note}\n\n")
    if run_failures:
        head += ("> **RUN INVALID — the missions' results do not count.**\n>\n"
                 + "".join(f"> - {f}\n" for f in run_failures) + "\n")
    head += _options_line(rows)
    head += _resolution_line(tier)
    head += _vram_line(rows)
    head += _vk_profile_line(rows)
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
                timestamp: str, show_window: bool = False,
                run_failures: Optional[list[str]] = None) -> dict:
    run_failures = list(run_failures or [])
    _all_missions_passed = all(r.verdict.passed for r in rows) and bool(rows)
    return {
        "timestamp": timestamp,
        "tier": tier,
        "profile": profile,
        # SMOKE-DEPLOY-TRAP-1: an explicit top-level verdict that accounts for
        # run-level failures (chiefly DEPLOY_FINGERPRINT mismatch). Consumers
        # used to derive PASS from the per-mission rows alone, which is how a
        # green report got quoted for a run that smoked the wrong binary.
        "overall_result": "PASS" if (_all_missions_passed and not run_failures) else "FAIL",
        "run_failures": run_failures,
        "fps_note": ("window visible; fps reflects foreground throughput subject to MC2_FPS_CAP/MC2_VSYNC"
                     if show_window else
                     "window minimized; SDL throttles rendering; fps reflects ~100fps cap not GPU throughput"),
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
                "simid_frames": r.summary.simid_frames,
                "simid_chain": r.summary.simid_chain,
                "render_options": r.render_options,
                "vram": [{"phase": v.phase, "backend": v.backend, "values": v.values} for v in (getattr(r.summary, "vram", None) or [])],
            }
            for r in rows
        ],
    }
