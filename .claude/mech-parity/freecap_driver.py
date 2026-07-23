#!/usr/bin/env python3
"""Free-capture (natural game camera) driver for co-covered mech parity.

Runs mc2 at the natural game-camera (no bookmark override) at a fixed frame with
fixed-step clock + seed, so the player lance is on-screen where GL ALSO draws
mechs (the bookmark free-camera poses frame terrain only). Emits
<mission>_frame<N>.png. Used for the item-1 co-covered GL-vs-VK mech parity number.
"""
import os, sys, subprocess, time, shutil
from pathlib import Path

EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe")

def run(tag, mission, frame, gates, out_dir, duration=40):
    out_dir = Path(out_dir).resolve(); out_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["MC2_SMOKE_MODE"] = "1"
    env["MC2_LOG"] = "1"
    env["MC2_SMOKE_SEED"] = "0xC0FFEE"
    env["MC2_SMOKE_FIXED_TIMESTEP"] = "1"
    env["MC2_HEARTBEAT"] = "1"
    env["MC2_VISUAL_CAPTURE_DIR"] = str(out_dir)
    env["MC2_VISUAL_CAPTURE_FRAME"] = str(frame)
    env["MC2_NO_LAUNCHER"] = "1"
    for k, v in gates.items():
        env[k] = v
    log = out_dir / f"{tag}.log"
    si = None
    if os.name == "nt":
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 7  # minimized, no focus steal
    args = [str(EXE), "--profile", "stock", "--mission", mission, "--duration", str(duration)]
    print(f"[freecap] {tag} gates={gates} frame={frame}")
    with open(log, "w", encoding="utf-8", errors="replace") as lf:
        p = subprocess.Popen(args, stdout=lf, stderr=subprocess.STDOUT,
                             cwd=str(EXE.parent), env=env, startupinfo=si)
        # frame*.png emitted at the trigger frame; wait for it or process exit.
        expected = out_dir / f"{mission}_frame{frame}.png"
        deadline = time.time() + duration + 30
        while time.time() < deadline:
            if expected.exists():
                time.sleep(1.0); break
            if p.poll() is not None:
                break
            time.sleep(0.5)
        try:
            p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait(timeout=10)
    # rename to tag
    dst = out_dir / f"{tag}.png"
    if expected.exists():
        shutil.copy(expected, dst)
        print(f"[freecap] {tag}: captured -> {dst}")
        return dst
    print(f"[freecap] {tag}: NO CAPTURE (see {log})")
    return None

if __name__ == "__main__":
    mission = sys.argv[1] if len(sys.argv) > 1 else "mc2_01"
    frame = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    out = fr".claude/mech-parity/freecap/{mission}_f{frame}"
    common = {"MC2_RENDER_BACKEND": "vulkan"}
    # Identical substrate (shared depth) in all 3; only the mech gates differ.
    gl = dict(common, MC2_VK_SHARED_SCENE_DEPTH="1")  # mech island OFF -> GL mech flush
    vk_flat = dict(common, MC2_VK_SHARED_SCENE_DEPTH="1", MC2_VK_MECH_TIER1="1")
    vk_mat = dict(vk_flat, MC2_VK_MECH_MATERIAL="1", MC2_VK_MECH_LIGHTING="1")
    run("gl",      mission, frame, gl,      out)
    run("vk_flat", mission, frame, vk_flat, out)
    run("vk_mat",  mission, frame, vk_mat,  out)
    print(f"[freecap] done. dir={out}")
