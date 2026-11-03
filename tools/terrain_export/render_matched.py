"""Render Blender frames from the game's own bookmark cameras and montage them next to in-game captures.
  py -3 tools/terrain_export/render_matched.py --ingame DIR --glb mc2_01_as_rendered.glb --out DIR
       [--blender EXE] [--labels a,b] [--samples 48] [--scale-down 2]
Camera comes from each capture sidecar (cap_pos/cap_rot/cap_proj/cap_alt, 40 deg HORIZONTAL fov).  The in-game
frame is letterboxed; the non-black row band is detected and used as the render aspect.  Blender runs at
BELOW_NORMAL priority.  Eye terrain clamping (AltitudeMinimum) is NOT modelled."""
import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
BLENDER = "C:/Program Files/Blender Foundation/Blender 5.1/blender.exe"
BELOW_NORMAL = 0x00004000


def letterbox_band(im: Image.Image) -> tuple[int, int]:
    a = np.asarray(im.convert("RGB")).astype(int)
    rows = np.where(a.reshape(a.shape[0], -1).max(1) > 8)[0]
    return int(rows.min()), int(rows.max()) + 1


def cam_args(side: dict) -> list[str]:
    x, y, z = side["cap_pos"]
    return [str(v) for v in (x, y, z, side["cap_rot"], side["cap_proj"], side["cap_alt"])]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ingame", required=True, help="dir of <mission>_<label>.png/.json from run_visual_capture")
    ap.add_argument("--glb", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--blender", default=BLENDER)
    ap.add_argument("--labels", help="comma list; default all sidecars in --ingame")
    ap.add_argument("--samples", type=int, default=48)
    ap.add_argument("--scale-down", type=int, default=2)
    ap.add_argument("--extra", nargs="*", default=[], help="extra render_blender.py args (e.g. --hide-props)")
    a = ap.parse_args()
    ing, out = Path(a.ingame), Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    sides = sorted(p for p in ing.glob("*.json") if "manifest" not in p.name)
    want = set(a.labels.split(",")) if a.labels else None
    for sp in sides:
        side = json.loads(sp.read_text())
        label = side.get("label")
        if not label or (want and label not in want):
            continue
        game = Image.open(sp.with_suffix(".png")).convert("RGB")
        r0, r1 = letterbox_band(game)
        w, h = game.width // a.scale_down, (r1 - r0) // a.scale_down
        bl = out / f"blender_{label}.png"
        cmd = [a.blender, "-b", "--python", str(HERE / "render_blender.py"), "--", "--glb", a.glb, "--out", str(bl),
               "--res", str(w), str(h), "--samples", str(a.samples), "--cam-bookmark", *cam_args(side), *a.extra]
        subprocess.run(cmd, check=True, creationflags=BELOW_NORMAL, stdout=subprocess.DEVNULL)
        g = game.crop((0, r0, game.width, r1)).resize((w, h), Image.LANCZOS)
        b = Image.open(bl).convert("RGB")
        m = Image.new("RGB", (w, h * 2 + 28), (20, 20, 20))
        m.paste(g, (0, 14))
        m.paste(b, (0, h + 28))
        d = ImageDraw.Draw(m)
        d.text((6, 1), f"{label}: IN-GAME (Vulkan) pos={side['cap_pos']} rot={side['cap_rot']} proj={side['cap_proj']} alt={side['cap_alt']}", fill=(235, 235, 235))
        d.text((6, h + 15), f"BLENDER, same camera ({Path(a.glb).name}); 40 deg HFOV, {w}x{h}", fill=(235, 235, 235))
        m.save(out / f"matched_{label}.png")
        print("wrote", out / f"matched_{label}.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
