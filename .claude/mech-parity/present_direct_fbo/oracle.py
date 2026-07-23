#!/usr/bin/env python3
"""Rung-1 oracle: MC2_VK_PRESENT_DIRECT_FBO copy-1 elimination.
Whole-frame parity: VK backend, gate OFF (copy-1 present) vs gate ON
(direct-FBO present, copy-1 skipped). Same seed/pose/duration; only the
present plumbing differs -> pixels must be identical (this IS the oracle,
per VK-P14b spec: "copy-1-elim ... proof: existing verifyReadbackContent()
... backend-compare bookmarks").

Local run() (not the shared terrain_near_oracle.run) because that harness
launches wShowWindow=7 (SW_SHOWMINNOACTIVE) -- fine for GL/terrain-island
oracles, but the VK present path bails to GL fail-soft on a minimized
window (zero surface extent). This oracle needs the VK present path to
actually come up, so it launches SW_SHOWNORMAL instead.
"""
import os, sys, subprocess, time
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import terrain_near_oracle as T

def run(tag, gates, duration=30):
    out_dir = T.OUT / tag
    out_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(T.BASE)
    env["MC2_VISUAL_CAPTURE_DIR"] = str(out_dir)
    for k, v in gates.items():
        env[k] = v
    si = None
    if os.name == "nt":
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 1  # SW_SHOWNORMAL -- VK present needs a real drawable
    args = [str(T.EXE), "--profile", "stock", "--mission", T.MISSION, "--duration", str(duration)]
    log = out_dir / f"{tag}.log"
    vkg = {k: v for k, v in gates.items() if k.startswith("MC2_VK")}
    print(f"[pdfbo-oracle] run {tag} vk_gates={vkg}")
    with open(log, "w", encoding="utf-8", errors="replace") as lf:
        p = subprocess.Popen(args, stdout=lf, stderr=subprocess.STDOUT,
                              cwd=str(T.EXE.parent), env=env, startupinfo=si)
        deadline = time.time() + duration + 40
        while time.time() < deadline:
            if p.poll() is not None:
                break
            time.sleep(0.5)
        try:
            p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait(timeout=10)
    pngs = sorted(out_dir.glob(f"{T.MISSION}_*.png"))
    print(f"[pdfbo-oracle] {tag}: {len(pngs)} PNGs -> {[q.name for q in pngs]}")
    return out_dir, pngs

VK_OFF = dict(T.COMMON_VK)  # gate absent -> default OFF, copy-1 present path
VK_ON  = dict(T.COMMON_VK, MC2_VK_PRESENT_DIRECT_FBO="1")

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "compare"
    if mode == "selfdiff":
        a = [run(f"pdfbo2_off_{i}", VK_OFF)[0] for i in range(2)]
        b = [run(f"pdfbo2_on_{i}", VK_ON)[0] for i in range(2)]
        T.pair_report("gate-OFF self-diff r0-r1 -- MUST be 0.000", a[0], a[1])
        T.pair_report("gate-ON  self-diff r0-r1 -- MUST be 0.000", b[0], b[1])
        T.pair_report("gate-OFF vs gate-ON -- MUST be 0.000 (whole-frame parity)", a[0], b[0])
    else:
        off = run("pdfbo2_off", VK_OFF)[0]
        on  = run("pdfbo2_on", VK_ON)[0]
        T.pair_report("gate-OFF vs gate-ON -- MUST be 0.000 (whole-frame parity)", off, on)
