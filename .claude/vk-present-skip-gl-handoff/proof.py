#!/usr/bin/env python3
import os
import subprocess
import sys
import time
from pathlib import Path

# OWNER RULE: the engine is launched through nofocus.spawn ONLY (background launch:
# non-activating, offscreen, no cursor grab). These oracles read engine PNGs.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from smoke_lib import nofocus  # noqa: E402

EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe")
BOOK = Path(r"A:/Games/mc2-vk-interop-bridge/tests/visual/bookmarks/mc2_01_buildings.json")
OUT = Path(r"A:/Games/mc2-vk-interop-bridge/.claude/vk-present-skip-gl-handoff")
MISSION = "mc2_01"

ENV = {
    "MC2_SMOKE_MODE": "1",
    "MC2_LOG": "1",
    "MC2_SMOKE_SEED": "0xC0FFEE",
    "MC2_SMOKE_FIXED_TIMESTEP": "1",
    "MC2_HEARTBEAT": "1",
    "MC2_NO_LAUNCHER": "1",
    "MC2_VISUAL_BOOKMARK_CAPTURE": str(BOOK),
    "MC2_VISUAL_CAPTURE_DIR": str(OUT),
    "MC2_VISUAL_CAPTURE_FRAME": "120",
    "MC2_VISUAL_SETTLE": "30",
    "MC2_RENDER_BACKEND": "vulkan",
    "MC2_VK_SHARED_GBUFFER": "1",
    "MC2_VK_POST_COMPOSITE": "1",
    "MC2_VK_PRESENT_DIRECT_FBO": "1",
    "MC2_VK_PRESENT_SKIP_GL_HANDOFF": "1",
    "MC2_VK_PRESENT_TAIL_HOOK": "1",
    "MC2_VK_HUD": "1",
    "MC2_VK_HUD_COLOR_PROOF": "1",
    "MC2_VK_HUD_SKIP_GL": "1",
    "MC2_VK_IMGUI": "1",
}

def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(ENV)
    log = OUT / "visible.log"
    args = [str(EXE), "--profile", "stock", "--mission", MISSION, "--duration", "35"]
    with open(log, "w", encoding="utf-8", errors="replace") as lf:
        p = nofocus.spawn(args, cwd=str(EXE.parent), env=env, stdout=lf,
                           stderr=subprocess.STDOUT, show_window=True, small_window=False)
        deadline = time.time() + 90
        while time.time() < deadline and p.poll() is None:
            time.sleep(0.5)
        if p.poll() is None:
            p.kill()
        rc = p.wait()
    print(f"log={log} rc={rc}")
    print("pngs=" + ",".join(p.name for p in sorted(OUT.glob(f"{MISSION}_*.png"))))
    return rc

if __name__ == "__main__":
    raise SystemExit(main())
