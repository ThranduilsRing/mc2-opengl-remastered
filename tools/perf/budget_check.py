"""Advisory mission budgets and a standalone HTML cockpit. No merge-gate wiring."""
import argparse
import hashlib
from datetime import datetime, timezone
from html import escape
import json
import math
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
BUDGET = ROOT / "docs/perf-budget.json"
OUT = Path("E:/mc2-archive-2026-10/proofs/perf-report")
MISSIONS = {"mc2_01", "mc2_03", "mc2_10", "mc2_24", "battle"}
METRICS = {"cpu_frame_ms", "draw_screen_ms", "gpu_frame_ms", "mover_tick_ms"}
CAMERA_MISSIONS = {"mc2_01", "mc2_10", "mc2_24"}
CAMERA_NAMES = {*(f"low_horizon_{r}" for r in (0, 90, 180, 270)), "cinematic_close",
                "oblique_default", "wolfman", "topdown", "orbit", "sun_facing_low"}


def number(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def validate_budget(data):
    """Validate budgets and explicit per-bookmark measurement gaps, without a game."""
    def require(ok, message):
        if not ok:
            raise ValueError(message)
    require(set(data) == {"version", "baseline_source", "metrics", "missions"}, "budget fields")
    require(type(data["version"]) is int and data["version"] == 2, "budget version")
    require(isinstance(data["baseline_source"], str) and bool(data["baseline_source"]), "baseline source")
    require(set(data["metrics"]) == METRICS, "metric names")
    for name, metric in data["metrics"].items():
        require(set(metric) == {"goal", "strict", "context"}, f"{name}: fields")
        require(number(metric["goal"]) and metric["goal"] > 0, f"{name}: goal")
        require(type(metric["strict"]) is bool, f"{name}: strict")
        require(isinstance(metric["context"], str) and bool(metric["context"]), f"{name}: context")
    require(set(data["missions"]) == MISSIONS, "mission names")
    for name, mission in data["missions"].items():
        require(set(mission) == {"baseline_status", "baseline", "bookmarks"}, f"{name}: fields")
        require(mission["baseline_status"] in ("quiet", "NOISY"), f"{name}: baseline status")
        require(set(mission["baseline"]) == METRICS, f"{name}: baseline metrics")
        require(all(v is None or number(v) for v in mission["baseline"].values()), f"{name}: baseline values")
        require(set(mission["bookmarks"]) == (CAMERA_NAMES if name in CAMERA_MISSIONS else {"default"}), f"{name}: bookmarks")
        for mark, values in mission["bookmarks"].items():
            require(set(values) == METRICS, f"{name}/{mark}: metrics")
            require(all(v == "unmeasured" or number(v) for v in values.values()), f"{name}/{mark}: values")
        if name in CAMERA_MISSIONS:
            document = json.loads((ROOT / f"tests/visual/bookmarks/{name}_camera.json").read_text())
            require(document["schema_v"] == 1 and document["mission"] == name, f"{name}: bookmark identity")
            marks = document["bookmarks"]
            require(len(marks) == len(CAMERA_NAMES) and {b["name"] for b in marks} == CAMERA_NAMES, f"{name}: camera coverage")
            for mark in marks:
                require(len(mark["pos"]) == 3 and all(type(v) in (int, float) and math.isfinite(v) for v in mark["pos"]), f"{name}: position")
                require(-20 <= mark["proj"] <= 88 and 10 <= mark["alt"] <= 6400, f"{name}: camera envelope")
                require(type(mark["rot"]) in (int, float) and math.isfinite(mark["rot"]), f"{name}: rotation")
                if mark["name"] == "orbit":
                    require(mark.get("sweep") == {"degrees": 360, "seconds": 10}, f"{name}: orbit")


def measurements(run):
    if run.get("bookmarks"):
        samples = {name: measurements(mark) for name, mark in run["bookmarks"].items()}
        # Missing bookmarks/metrics never disappear behind a measured best view.
        return {metric: max(values[metric] for values in samples.values()) for metric in METRICS
                if all(metric in values for values in samples.values())}
    zones = run.get("zones", {})
    frame = zones.get("Frame", {})
    count = frame.get("count")
    values = {}
    cpu = run.get("cpu_frame", {})
    if isinstance(cpu, dict) and number(cpu.get("mean_ms")):
        values["cpu_frame_ms"] = cpu["mean_ms"]
    if number(count) and count > 0:
        for metric, zone in (("cpu_frame_ms", "Frame"), ("draw_screen_ms", "DrawScreen")):
            total = zones.get(zone, {}).get("total_ms")
            if number(total):
                values[metric] = total / count
    # Optional supplementary measurements must attest the metric's context.
    # The CPU profiler cannot measure GPU time or isolated 200-mover ticks.
    for metric in ("gpu_frame_ms", "mover_tick_ms"):
        sample = run.get("measurements", {}).get(metric, {})
        valid_context = (sample.get("resolution") == [3840, 2160] if metric == "gpu_frame_ms"
                         else sample.get("movers") == 200 and sample.get("ticks_per_sample") == 1)
        if valid_context and number(sample.get("value")):
            values[metric] = sample["value"]
    gpu = run.get("gpu_span", {})
    if run.get("resolution") == [3840, 2160] and isinstance(gpu, dict) and number(gpu.get("mean_ms")):
        values["gpu_frame_ms"] = gpu["mean_ms"]
    return values


def compare(profile, budget, previous=None):
    results = {}
    for mission, reference in budget["missions"].items():
        run = profile.get("runs", {}).get(mission, {})
        noise = run.get("noise", {})
        quiet = (noise.get("status") == "quiet" and not noise.get("reasons")
                 and noise.get("external_cpu_scope") == "reserved_cores")
        external = noise.get("external_cpu_p95")
        if not number(external) or external >= 15:
            quiet = False
        if mission in profile.get("errors", {}):
            quiet = False
        expected = set(reference["bookmarks"])
        marks = run.get("bookmarks", {})
        complete = set(marks) == expected if mission in CAMERA_MISSIONS else True
        if any(mark.get("pose_matches") is False or mark.get("visual_review") == "rejected" for mark in marks.values()):
            complete = False
        values = measurements(run)
        old_run = (previous or {}).get("runs", {}).get(mission, {})
        old_values = measurements(old_run)
        rows = {}
        for metric, policy in budget["metrics"].items():
            value = values.get(metric)
            base = reference["baseline"][metric]
            old = old_values.get(metric)
            status = "INCONCLUSIVE"
            if quiet and complete and value is not None:
                meets = value < policy["goal"] if policy["strict"] else value <= policy["goal"]
                status = "PASS" if meets else "FAIL"
            worst = max(marks, key=lambda n: measurements(marks[n]).get(metric, -1)) if marks and value is not None else None
            if mission in CAMERA_MISSIONS:
                baselines = [mark[metric] for mark in reference["bookmarks"].values()]
                base = max(baselines) if all(number(v) for v in baselines) else None
            rows[metric] = {"value": value, "worst_bookmark": worst, "goal": policy["goal"], "status": status,
                            "delta_baseline_ms": value - base if value is not None and base is not None else None,
                            "distance_goal_ms": value - policy["goal"] if value is not None else None,
                            "delta_previous_ms": value - old if value is not None and old is not None else None}
        statuses = {r["status"] for r in rows.values()}
        verdict = "INCONCLUSIVE" if not quiet else ("FAIL" if "FAIL" in statuses else
                   "INCONCLUSIVE" if "INCONCLUSIVE" in statuses else "PASS")
        results[mission] = {"status": verdict, "noise": noise, "metrics": rows,
                            "bookmarks": {n: measurements(m) for n, m in marks.items()},
                            "baseline_status": reference["baseline_status"],
                            "previous_noise": old_run.get("noise", {}).get("status", "missing")}
    return results


def fmt(value):
    return "unmeasured" if value is None else f"{value:+.3f}"


def write_html(path, results, profile, budget, previous=None):
    chunks = ['<!doctype html><html lang="en"><meta charset="utf-8">',
              '<meta name="viewport" content="width=device-width,initial-scale=1">',
              '<title>MC2 performance budgets</title><style>body{font:16px system-ui;margin:auto;max-width:1000px;padding:16px;background:#101820;color:#eef}section{background:#203040;padding:12px;margin:16px 0;border-radius:10px}.scroll{overflow-x:auto}table{border-collapse:collapse;width:100%}td,th{text-align:left;padding:8px;border-bottom:1px solid #567}small{color:#bcd}</style>',
              '<h1>MC2 performance budgets</h1>',
              '<p>Advisory only. NOISY or missing evidence is INCONCLUSIVE. Negative deltas mean faster. '
              'CPU zones include waits and overlap; do not sum nested zones. GPU 5 ms is a throughput goal, not proof of end-to-end 200 fps.</p>',
              f'<p>Executable SHA: {escape(str(profile.get("exe_sha256", "unknown")))}</p>']
    for mission, result in results.items():
        noise = result['noise']
        reserved = noise.get('external_cpu_p95') if noise.get('external_cpu_scope') == 'reserved_cores' else None
        whole = noise.get('whole_machine_external_cpu_p95', noise.get('external_cpu_p95') if reserved is None else None)
        chunks += [f'<section><h2>{mission}: {result["status"]}</h2>',
                   f'<p>Current noise: {escape(str(result["noise"].get("status", "missing")))}; {escape(str(result["noise"].get("reasons", [])))}. '
                   f'External CPU p95: reserved cores {fmt(reserved)}%; '
                   f'whole machine {fmt(whole)}%. '
                   f'<br>Baseline: {result["baseline_status"]}; previous: {escape(str(result["previous_noise"]))}. Deltas involving noise are descriptive only.</p>',
                   '<div class="scroll"><table><tr><th>Metric (ms)</th><th>Value</th><th>Goal</th><th>Δ baseline</th><th>Distance to goal</th><th>Δ previous</th><th>Verdict</th></tr>']
        for metric, row in result["metrics"].items():
            chunks.append(f'<tr><td>{metric} ({escape(row["worst_bookmark"] or "unmeasured")})</td><td>{fmt(row["value"])}</td><td>{row["goal"]}</td><td>{fmt(row["delta_baseline_ms"])}</td><td>{fmt(row["distance_goal_ms"])}</td><td>{fmt(row["delta_previous_ms"])}</td><td>{row["status"]}</td></tr>')
        for name, values in result["bookmarks"].items():
            mark = profile["runs"][mission]["bookmarks"][name]
            chunks.append(f'<tr><td>{escape(name)}</td><td colspan="6">{escape(str(values))}; '
                          f'CPU mean/p95/worst: {escape(str(mark.get("cpu_frame", "unmeasured")))}; '
                          f'GPU span mean/p95/worst at {escape(str(mark.get("resolution", "unknown")))}: '
                          f'{escape(str(mark.get("gpu_span", "unmeasured")))}</td></tr>')
        chunks += ['</table></div><h3>Top zones (inclusive ms/frame)</h3><ol>']
        run = profile.get("runs", {}).get(mission, {})
        zones = run.get("zones", {})
        count = zones.get("Frame", {}).get("count", 0)
        old_zones = (previous or {}).get("runs", {}).get(mission, {}).get("zones", {})
        old_count = old_zones.get("Frame", {}).get("count", 0)
        ranked = sorted(((n, z) for n, z in zones.items() if n != "Frame"),
                        key=lambda item: item[1]["total_ms"], reverse=True)[:5] if count else []
        for name, zone in ranked:
            value = zone["total_ms"] / count
            old = old_zones.get(name)
            delta = value - old["total_ms"] / old_count if old and old_count else None
            chunks.append(f'<li>{escape(name)}: {value:.3f}; event p95 {zone["p95_ms"]:.3f}; Δ previous {fmt(delta)}</li>')
        chunks.append('</ol></section>')
    chunks.append('<h2>Measurement contexts</h2>')
    chunks.extend(f'<p>{name}: {escape(policy["context"])}</p>' for name, policy in budget["metrics"].items())
    chunks.append('</html>')
    path.write_text('\n'.join(chunks), encoding="utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--budget', type=Path, default=BUDGET)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--validate-schema', action='store_true')
    source.add_argument('--profile', type=Path)
    source.add_argument('--install', type=Path, help='run mission_cpu_profile with its standard defaults')
    parser.add_argument('--previous', type=Path, help='previous raw profile.json, for trend comparison')
    parser.add_argument('--out', type=Path, default=OUT)
    args = parser.parse_args(argv)
    try:
        budget = json.loads(args.budget.read_text(encoding='utf-8'))
        validate_budget(budget)
        if args.validate_schema:
            print('perf_budget_schema: PASS')
            return 0
        previous = json.loads(args.previous.read_text(encoding='utf-8')) if args.previous else None
        if args.install:
            import mission_cpu_profile
            import psutil
            if sys.platform == 'win32':
                psutil.Process().nice(psutil.IDLE_PRIORITY_CLASS)
            from types import SimpleNamespace
            directory = args.out / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
            directory.mkdir(parents=True)
            options = SimpleNamespace(install=args.install.resolve(), seconds=20)
            exe = options.install / 'mc2.exe'
            profile = {'exe': str(exe), 'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
                       'seconds': options.seconds, 'runs': {}, 'errors': {}}
            for name in sorted(MISSIONS):
                try:
                    profile['runs'][name] = mission_cpu_profile.profile(options, name, directory / name, psutil)
                except (Exception, SystemExit) as exc:
                    profile['errors'][name] = str(exc)
            (directory / 'profile.json').write_text(json.dumps(profile, indent=2), encoding='utf-8')
        else:
            profile = json.loads(args.profile.read_text(encoding='utf-8'))
            directory = args.out
            directory.mkdir(parents=True, exist_ok=True)
        results = compare(profile, budget, previous)
        (directory / 'budget.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
        write_html(directory / 'index.html', results, profile, budget, previous)
        for name, result in results.items():
            print(f'{name}: {result["status"]} (baseline {result["baseline_status"]})')
            for metric, row in result['metrics'].items():
                print(f'  {metric}: {fmt(row["value"])} ms; delta baseline {fmt(row["delta_baseline_ms"])}; distance goal {fmt(row["distance_goal_ms"])}; delta previous {fmt(row["delta_previous_ms"])}; {row["status"]}')
        print(f'Report: {directory / "index.html"}')
        statuses = {r['status'] for r in results.values()}
        return 1 if 'FAIL' in statuses else 2 if 'INCONCLUSIVE' in statuses else 0
    except (OSError, ValueError, TypeError, KeyError) as exc:
        print(f'budget_check: {exc}', file=sys.stderr)
        return 3


if __name__ == '__main__':
    raise SystemExit(main())
