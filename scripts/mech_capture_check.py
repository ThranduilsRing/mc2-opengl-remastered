#!/usr/bin/env python3
"""Per-mech capture manifest, pixel check and contact sheet for MC2_CAPTURE_FOCUS=mech:home|enemy|all walks.

  gen      --mission M --count N --out bookmarks.json [--pos x,y --empty-ground]
           N walk bookmarks (bookmark k frames the kth live mech); poses are placeholders, the engine
           moves each onto its mech. --empty-ground appends one extra bookmark at a fixed bare spot.
  check    <capture_dir> [--empty-ref PNG] [--sheet OUT.png] [--out captures.json]
           reads every sidecar *.json with a focus_walk object plus its PNG, runs the pixel check,
           writes the manifest list consumed by campaign_roster.py --captures.

Pixel check: edge energy (mean absolute luma gradient) of the centre square of the frame, where the
focused mech sits. A mech is a dense, high-detail object; bare ground is smooth. passed = energy
>= MIN_ENERGY and, when an empty-ground reference at the same zoom is given, >= REF_RATIO x its energy.
Hidden-mech proof: the same check on the empty-ground capture must FAIL (nothing drawn there).
"""
import json
import sys
from pathlib import Path

# Calibrated on live mc2_01 captures at MC2_CAPTURE_FOCUS_ZOOM=500: real mechs 3.9-4.4, bare ground 1.1-2.1.
MIN_ENERGY = 3.0
REF_RATIO = 1.5
CENTER_FRAC = 0.3


def edge_energy(img):
    """Mean absolute horizontal+vertical luma gradient of the centre square. img: PIL image."""
    g = img.convert("L")
    w, h = g.size
    s = max(8, int(min(w, h) * CENTER_FRAC))
    x0, y0 = (w - s) // 2, (h - s) // 2
    px = g.crop((x0, y0, x0 + s, y0 + s)).load()
    tot = n = 0
    for y in range(s - 1):
        for x in range(s - 1):
            v = px[x, y]
            tot += abs(px[x + 1, y] - v) + abs(px[x, y + 1] - v)
            n += 2
    return tot / n if n else 0.0


def pixel_check(img, ref_img=None):
    e = edge_energy(img)
    ok = e >= MIN_ENERGY
    if ok and ref_img is not None:
        ok = e >= REF_RATIO * max(edge_energy(ref_img), 0.01)
    return {"energy": round(e, 3), "passed": bool(ok)}


def gen_bookmarks(mission, count, empty_ground=False, empty_pos=(0.0, 0.0)):
    bms = [{"name": f"walk_{k:02d}", "pos": [0.0, 0.0, 0.0], "rot": 0.0, "proj": 22.0, "alt": 1200.0,
            "covers": ["mech_walk"]} for k in range(count)]
    if empty_ground:
        bms.append({"name": "empty_ground", "pos": [empty_pos[0], empty_pos[1], 0.0], "rot": 0.0,
                    "proj": 22.0, "alt": 1200.0, "covers": ["mech_walk_ref"]})
    return {"schema_v": 1, "mission": mission, "bookmarks": bms}


def collect(capture_dir, empty_ref=None):
    """Sidecars with focus_walk -> manifest rows (+ pixel check). Missing mech or PNG is 'failed'."""
    from PIL import Image
    d = Path(capture_dir)
    ref = Image.open(empty_ref) if empty_ref else None
    rows = []
    for js in sorted(d.rglob("*.json")):
        try:
            doc = json.loads(js.read_text(encoding="utf-8", errors="replace"))
        except ValueError:
            continue
        fw = doc.get("focus_walk") if isinstance(doc, dict) else None
        if not fw:
            continue
        png = js.with_suffix(".png")
        row = {"id": fw.get("id"), "name": fw.get("name"), "team": fw.get("team"),
               "index": fw.get("index"), "total": fw.get("total"), "png": str(png),
               "contact": fw.get("contact", "n/a")}
        if fw.get("id", -1) < 0 or not (doc.get("focus") or {}).get("found", True):
            continue   # surplus bookmark beyond the live mech count: not a mech, not a failure
        elif not png.exists():
            row.update(passed=False, energy=0.0, why="png missing")
        else:
            row.update(pixel_check(Image.open(png), ref), why="")
            if row["contact"] == "fogged":   # enemy hidden by the stock fog gate: the frame cannot show it
                row.update(passed=False, why="fogged")
        rows.append(row)
    return rows


def contact_sheet(rows, out, cols=4, thumb=240):
    from PIL import Image, ImageDraw
    rows = [r for r in rows if Path(r["png"]).exists()]
    if not rows:
        return False
    nr = (len(rows) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * thumb, nr * (thumb + 16)), (20, 20, 20))
    dr = ImageDraw.Draw(sheet)
    for i, r in enumerate(rows):
        im = Image.open(r["png"]).convert("RGB")
        w, h = im.size
        s = int(min(w, h) * 0.5)
        im = im.crop(((w - s) // 2, (h - s) // 2, (w + s) // 2, (h + s) // 2)).resize((thumb, thumb))
        x, y = (i % cols) * thumb, (i // cols) * (thumb + 16)
        sheet.paste(im, (x, y + 16))
        dr.text((x + 2, y + 2), f"{'OK ' if r['passed'] else ('FOGGED ' if r.get('why') == 'fogged' else 'FAIL ')}{r['name']} t{r['team']}",
                fill=(0, 255, 0) if r["passed"] else (255, 60, 60))
    sheet.save(out)
    return True


def main(argv):
    if not argv or argv[0] not in ("gen", "check"):
        print(__doc__)
        return 2
    a = argv[1:]

    def opt(flag, default=None):
        if flag in a:
            i = a.index(flag)
            v = a[i + 1]
            del a[i:i + 2]
            return v
        return default

    if argv[0] == "gen":
        mission, count, out = opt("--mission"), int(opt("--count", "0")), opt("--out")
        pos = tuple(float(v) for v in opt("--pos", "0,0").split(","))
        eg = "--empty-ground" in a
        Path(out).write_text(json.dumps(gen_bookmarks(mission, count, eg, pos), indent=2))
        return 0
    ref, sheet, out = opt("--empty-ref"), opt("--sheet"), opt("--out")
    rows = collect(a[0], ref)
    if out:
        Path(out).write_text(json.dumps(rows, indent=2))
    if sheet:
        contact_sheet(rows, sheet)
    print(f"{sum(r['passed'] for r in rows)}/{len(rows)} passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
