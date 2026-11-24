#!/usr/bin/env python3
"""Mod chassis icons: every chassis a mod adds gets its own HUD/logistics icon.

Stock MC2 chassis use their shipped stock art (data/art/mechicons/hud/front/<stem>.png).
A mod chassis without its own art gets <stem>_hud_<W>x<H>.tga cooked from its own .tgl model by
the stock-style renderer (render_icons.py) into the mod's data/art, at mod-build time
(build_mco_compat.py) or on demand here. Generated files live in the install, never in git.

  py -3 -m tools.bt2018_mechs.mod_icons --install <root>          # list stock / own / cooked / fallback
  py -3 -m tools.bt2018_mechs.mod_icons --install <root> --cook   # cook what is missing, then list
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.bt2018_mechs import render_icons as R  # noqa: E402


def _ci(directory: Path, name: str):
    """Case-insensitive child lookup (mod trees keep the author's casing)."""
    if not directory.is_dir():
        return None
    want = name.lower()
    return next((p for p in directory.iterdir() if p.name.lower() == want), None)


def chassis_csvs(mod: Path):
    """{stem: MechName} for the chassis CSVs a mod ships (the BaseChassisInfo block)."""
    out = {}
    objects = _ci(mod / "data", "objects")
    for csv in sorted(objects.glob("*.csv")) if objects else []:
        try:
            head = csv.read_text("latin1", errors="replace").splitlines()[:4]
        except OSError:
            continue
        if not head or not head[0].startswith("BaseChassisInfo"):
            continue
        name = next((line.split(",")[1] for line in head if line.startswith("MechName,")), csv.stem)
        out[csv.stem.lower()] = name.strip()
    return out


def model_of(mod: Path, stem: str):
    """The intact shape the chassis' appearance INI names (FileName0), else <stem>.tgl."""
    tgl = _ci(mod / "data", "tgl")
    ini = _ci(tgl, stem + ".ini") if tgl else None
    name = stem
    if ini:
        m = re.search(r'(?mi)^\s*st\s+FileName0?\s*=\s*"([^"]+)"', ini.read_text("latin1", errors="replace"))
        if m:
            name = Path(m.group(1)).stem
    return _ci(tgl, name + ".tgl") if tgl else None


ALIASES = {k: v for k, v in json.loads((Path(__file__).with_name("stock_aliases.json")).read_text()).items() if not k.startswith("_")}
COOKED = ".cooked_icons.txt"   # stems this tool cooked into a mod's data/art (re-cooked on every --cook)


def stock_strip(install: Path, target: str, stem: str, art: Path):
    """Write <stem>_hud_<W>x<H>.tga from the stock front/back art of `target` (an alias is the stock chassis)."""
    hud = install / "data" / "art" / "mechicons" / "hud"
    for w, h in R.HUD_SIZES:
        front, back = (Image.open(hud / side / f"{target}@{w}x{h}.png").convert("RGBA") for side in ("front", "back"))
        strip = Image.new("RGBA", (w, 2 * h))
        strip.paste(front, (0, 0))
        strip.paste(back, (0, h))
        art.mkdir(parents=True, exist_ok=True)
        strip.save(art / f"{stem}_hud_{w}x{h}.tga")


def classify(install: Path, cook: bool = False, only: str = ""):
    """[(mod, stem, status, detail)] for every chassis a mod adds; status stock|alias|own|cooked|fallback."""
    stock_art = install / "data" / "art" / "mechicons" / "hud" / "front"
    stock = {p.stem.split("@")[0].lower() for p in stock_art.glob("*.png")}
    mods = sorted(p for p in (install / "mods").iterdir() if p.is_dir()) if (install / "mods").is_dir() else []
    cooked = {m.name: set((m / "data" / "art" / COOKED).read_text().split()) if (m / "data" / "art" / COOKED).is_file() else set() for m in mods}
    strips = {p.name.lower() for m in mods for p in ((_ci(m / "data", "art") or Path("-")).glob("*_hud_40x48.tga"))
              if p.name.lower()[: -len("_hud_40x48.tga")] not in cooked[m.name]}
    rows = []
    for mod in mods:
        art = mod / "data" / "art"
        writes = cook and not (only and mod.name != only)
        for stem, name in chassis_csvs(mod).items():
            key = name.lower().replace(" ", "")
            base = stem[len("bt2018_"):] if stem.startswith("bt2018_") else stem
            target = ALIASES.get(base) or ALIASES.get(key) or (base if base in stock else None)
            if stem in stock or key in stock:
                rows.append((mod.name, stem, "stock", "own stock art"))
            elif target in stock:
                if writes:
                    stock_strip(install, target, stem, art)
                    cooked[mod.name].discard(stem)
                rows.append((mod.name, stem, "alias", f"stock art: {target}"))
            elif {f"{stem}_hud_40x48.tga", f"bt2018_{stem}_hud_40x48.tga"} & strips:
                rows.append((mod.name, stem, "own", "strip in a mod"))
            elif not writes:
                rows.append((mod.name, stem, "cooked" if stem in cooked[mod.name] else "fallback",
                             "cooked earlier" if stem in cooked[mod.name] else "no art yet (run --cook)"))
            elif (model := model_of(mod, stem)) is None:
                rows.append((mod.name, stem, "fallback", "no .tgl model in the mod"))
            else:
                try:
                    R.build_icons(model, stem, art, hud_only=True)
                    cooked[mod.name].add(stem)
                    rows.append((mod.name, stem, "cooked", model.name))
                except (ValueError, SystemExit, OSError) as e:
                    rows.append((mod.name, stem, "fallback", f"{model.name}: {e}"))
        if writes and art.is_dir():
            (art / COOKED).write_text("\n".join(sorted(cooked[mod.name])) + "\n")
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--install", required=True, help="game root holding data/ and mods/")
    ap.add_argument("--cook", action="store_true", help="cook strips for mod chassis that have no art")
    a = ap.parse_args()
    rows = classify(Path(a.install), a.cook)
    for row in rows:
        print("%-16s %-20s %-8s %s" % row)
    counts = {s: sum(r[2] == s for r in rows) for s in ("stock", "alias", "own", "cooked", "fallback")}
    print("summary " + " ".join(f"{k}={v}" for k, v in counts.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
