"""VK-*-FENCE-DEFER A/B measurement harness (template: .claude/props-probe/ppmcap.py).

Launches the v0.4c lane exe in the FIELD env (mc2_01, fixed airport view) with the
island's [VK_*_PERF] receipt gate armed, once per A/B arm:
  arm "inline" : MC2_VK_<ISLAND>_FENCE_DEFER=0  (killswitch -> old inline wait)
  arm "defer"  : gate unset                      (default -> deferred retire)
Parses the receipt lines, drops warmup, prints avg/min/max fence_wait_us +
island_total_us. Also dumps a PPM per arm (MC2_VK_PRESENT_VERIFY oracle) for the
pixel A/B.

Usage: py -3 measure.py terrain|prop|mech|water [wait_seconds]
"""
import os, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(r"A:/Games/mc2-vk-interop-bridge/.claude/fence-defer")
INSTALL = Path(r"A:/Games/mc2-opengl/mc2-win64-0.4c")
EXE = INSTALL / "mc2.exe"
VIEW = ROOT / "airport_view.txt"

ISLANDS = {
    "terrain": ("MC2_VK_TERRAIN_PERF", "MC2_VK_TERRAIN_FENCE_DEFER", "VK_TERRAIN_PERF"),
    "prop":    ("MC2_VK_PROP_PERF",    "MC2_VK_PROP_FENCE_DEFER",    "VK_PROPS_PERF"),
    "mech":    ("MC2_VK_MECH_PERF",    "MC2_VK_MECH_FENCE_DEFER",    "VK_MECH_PERF"),
    "water":   ("MC2_VK_WATER_PERF",   "MC2_VK_WATER_FENCE_DEFER",   "VK_WATER_PERF"),
}

BASE = {
    "MC2_NO_LAUNCHER": "1", "MC2_DEBUG_STATE_DUMP": "1", "MC2_SHADOW_CSM": "1",
    "MC2_STATIC_PROP_PBR_V1": "1", "MC2_STATIC_PROP_AMBIENT_V1": "1",
    "MC2_STATIC_PROP_IBL_SH": "1", "MC2_HZB_VIEW_FILE": str(VIEW), "MC2_LOG": "1",
    "MC2_VK_PRESENT_VERIFY": "1",
    "MC2_RENDER_BACKEND": "vulkan", "MC2_VK_SHARED_GBUFFER": "1",
}


def run(tag: str, extra: dict, wait: int) -> Path:
    env = os.environ.copy()
    for k in list(env):
        if k.startswith("MC2_VK") or k == "MC2_RENDER_BACKEND":
            env.pop(k)
    env.update(BASE)
    env.update(extra)
    env["MC2_VK_PRESENT_DUMP_PPM"] = str(ROOT / f"{tag}.ppm")
    log = ROOT / f"{tag}.log"
    with open(log, "w", encoding="utf-8", errors="replace") as lf:
        p = subprocess.Popen([str(EXE), "-mission", "mc2_01"], cwd=str(INSTALL),
                             env=env, stdout=lf, stderr=subprocess.STDOUT)
        print(f"[{tag}] PID {p.pid} (own child; wait {wait}s)")
        try:
            time.sleep(wait)
        finally:
            p.terminate()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
    return log


def stats(log: Path, receipt_tag: str):
    fw, tot = [], []
    pat = re.compile(r"\[" + receipt_tag + r"\].*?fence_wait_us=([0-9.]+).*?island_total_us=([0-9.]+)")
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        m = pat.search(line)
        if m:
            fw.append(float(m.group(1)))
            tot.append(float(m.group(2)))
    if not fw:
        return None
    n0 = max(len(fw) // 4, 30)          # drop warmup (first quarter, >=30 frames)
    fw2, tot2 = fw[n0:], tot[n0:]
    if not fw2:
        fw2, tot2 = fw, tot
    def s(v):
        return f"avg={sum(v)/len(v):.1f} min={min(v):.1f} max={max(v):.1f} n={len(v)}"
    return f"fence_wait_us {s(fw2)} | island_total_us {s(tot2)} (raw frames={len(fw)})"


def main():
    island = sys.argv[1]
    wait = int(sys.argv[2]) if len(sys.argv) > 2 else 35
    perf_var, kill_var, receipt = ISLANDS[island]
    ROOT.mkdir(parents=True, exist_ok=True)
    VIEW.write_text("X=3200.0 Y=100.0 Z=399.588 ROT=110.0 ANGLE=28.0 ALT=1000.0\n")

    for arm, extra in (("inline", {perf_var: "1", kill_var: "0"}),
                       ("defer",  {perf_var: "1"})):
        tag = f"{island}_{arm}"
        log = run(tag, extra, wait)
        r = stats(log, receipt)
        print(f"[{tag}] {r if r else 'NO RECEIPT LINES — check log ' + str(log)}")
        ppm = ROOT / f"{tag}.ppm"
        print(f"[{tag}] ppm exists={ppm.exists()} size={ppm.stat().st_size if ppm.exists() else 0}")


if __name__ == "__main__":
    main()
