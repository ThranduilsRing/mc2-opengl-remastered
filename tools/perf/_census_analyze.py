import json, sys, statistics
from pathlib import Path
sys.path.insert(0, "A:/Games/mc2-lane10/tools/perf")
import main_busy as mb

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from smoke_lib.paths import PROOF_ROOT

OUT = Path((PROOF_ROOT + '/census2'))


def rows_of(d):
    rows = [json.loads(l) for l in (d / "frames.jsonl").read_text().splitlines() if l.strip()]
    cpu = [r for r in rows if r.get("type") == "cpu" and "wait_sealed_ms" in r]
    t, out = 0.0, []
    for r in cpu:
        t += r["ms"]
        if t > 3000 and r["ms"] < 200:
            out.append(r)
    return out, [r for r in rows if r.get("type") == "tick"], [r for r in rows if r.get("type") == "world"]


def mean(xs):
    return sum(xs) / len(xs) if xs else 0.0


for mission in ["mc2_01", "mc2_03", "mc2_10", "mc2_17", "mc2_24"]:
    runs = sorted(OUT.glob(f"{mission}_r*"))
    per = []
    ext = []
    for d in runs:
        if not (d / "frames.jsonl").exists():
            continue
        cpu, ticks, worlds = rows_of(d)
        if not cpu:
            continue
        tab = mb.table(cpu)
        noise = json.loads((d / "noise.json").read_text())
        ext.append((noise["external_cpu_p95"], noise["whole_machine_external_cpu_p95"]))
        tf = [r for r in cpu if r.get("ticks", 0) > 0]
        nt = [r for r in cpu if r.get("ticks", 0) == 0]
        def ms(rs, k): return mean([r[k] for r in rs])
        per.append({
            "n": len(cpu), "med": tab["all"]["MAIN_BUSY"]["median"], "p95": tab["all"]["MAIN_BUSY"]["p95"],
            "frame_med": tab["all"]["frame_ms"]["median"], "frame_mean": mean([r["ms"] for r in cpu]),
            "exec_med": tab["all"]["main_exec"]["median"], "sealed_med": tab["all"]["wait_sealed"]["median"],
            "sim_med": tab["all"]["wait_sim"]["median"], "gpu_med": tab["all"]["wait_gpu_main"]["median"],
            "tick_frames": len(tf), "tick_med": tab["tick_frames"]["MAIN_BUSY"]["median"], "tick_p95": tab["tick_frames"]["MAIN_BUSY"]["p95"],
            "nontick_med": mb.pct([mb.frame_columns(r)["MAIN_BUSY"] for r in nt], .5),
            "nontick_p95": mb.pct([mb.frame_columns(r)["MAIN_BUSY"] for r in nt], .95),
            "sealed_mean": ms(cpu, "wait_sealed_ms"), "sealed_tick": ms(tf, "wait_sealed_ms"), "sealed_nontick": ms(nt, "wait_sealed_ms"),
            "sim_mean": ms(cpu, "wait_sim_ms"), "sim_tick_mean": ms(tf, "wait_sim_ms"),
            "draws": {k: mean([r["draws"][k] for r in cpu]) for k in cpu[0]["draws"]},
            "walk": {k: mean([r["walk"][k] for r in cpu]) for k in cpu[0]["walk"]},
            "extract_bytes": ms(cpu, "extract_bytes"), "heap": ms(cpu, "heap_allocs"),
            "tick_ms_p50": statistics.median([t["ms"] for t in ticks]) if ticks else 0,
            "tick_ms_p95": mb.pct([t["ms"] for t in ticks], .95), "tick_ms_max": max([t["ms"] for t in ticks] or [0]),
            "movers": worlds[-1].get("movers") if worlds else None, "active": worlds[-1].get("active") if worlds else None,
            "dyn_leaves": ms(cpu, "dynamic_prop_leaves"),
        })
    if not per:
        print(mission, "no data"); continue
    def med(k): return statistics.median([p[k] for p in per])
    def rng(k): return f"{min(p[k] for p in per):.2f}-{max(p[k] for p in per):.2f}"
    print(f"== {mission} runs={len(per)} frames/run~{int(med('n'))} tickframes/run~{int(med('tick_frames'))}")
    for k in ["med", "p95", "frame_med", "frame_mean", "exec_med", "sealed_med", "sim_med", "gpu_med", "tick_med", "tick_p95", "nontick_med", "nontick_p95",
              "sealed_mean", "sealed_tick", "sealed_nontick", "sim_mean", "sim_tick_mean", "extract_bytes", "heap", "tick_ms_p50", "tick_ms_p95", "tick_ms_max", "dyn_leaves"]:
        print(f"  {k:15s} median-of-runs {med(k):9.3f}  range {rng(k)}")
    for g in ("draws", "walk"):
        print("  ", g, {k: round(statistics.median([p[g][k] for p in per]), 1) for k in per[0][g]})
    print("   movers/active", per[0]["movers"], per[0]["active"])
    print("   ext_cpu_p95 reserved / whole:", [(round(a or 0, 1), round(b or 0, 1)) for a, b in ext])
