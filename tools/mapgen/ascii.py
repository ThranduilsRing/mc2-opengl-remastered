"""Render extracted or generated JSON as an ASCII map, north up."""
from __future__ import annotations

import argparse
import math
import re
import sys
from collections import Counter
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.mapgen.common import building_role, load_json, output_path

SYMBOLS = {"tree": "t", "prop": ",", "terrain_object": ",", "rock": "%", "fence": "#",
           "wall": "#", "gate": "+", "turret": "T", "turret_control": "C",
           "turret_generator": "G", "gate_control": "c", "bridge": "=",
           "unknown": "?", "light": ","}
LEGEND = ("@ player start; O visible objective; B military; h town; I industry; H HQ/sensor; A airfield\n"
          "# wall/fence; + gate; T turret; C turret control; G generator; c gate control\n"
          "= road/bridge; : runway; . cement; ~ water; t trees; ^ slope>=35deg; % rock; , prop; ? unresolved")


def render(layout, cells_per_char=3, crop=None):
    if cells_per_char < 1:
        raise ValueError("cells-per-char must be positive")
    rows, cols = layout["size"]["rows"], layout["size"]["cols"]
    r0, c0, r1, c1 = crop or (0, 0, rows, cols)
    if not (0 <= r0 < r1 <= rows and 0 <= c0 < c1 <= cols):
        raise ValueError("crop must be within map: first_row first_col end_row end_col")
    nr, nc = math.ceil((r1-r0)/cells_per_char), math.ceil((c1-c0)/cells_per_char)
    grid = [[" " for _ in range(nc)] for _ in range(nr)]
    priority = [[-1 for _ in range(nc)] for _ in range(nr)]

    def put(cell, char, weight):
        rr, cc = cell
        if r0 <= rr < r1 and c0 <= cc < c1:
            r, c = (rr-r0)//cells_per_char, (cc-c0)//cells_per_char
            if weight > priority[r][c]:
                grid[r][c], priority[r][c] = char, weight

    for name, char, weight in (("cement", ".", 0), ("cliffs", "^", 1), ("water", "~", 2),
                               ("roads", "=", 5), ("runway", ":", 6)):
        for r, first, end in layout["layers"].get(name, []):
            if r0 <= r < r1 and end > c0 and first < c1:
                first, end = max(first, c0), min(end, c1)
                for cc in range((first-c0)//cells_per_char, (end-1-c0)//cells_per_char + 1):
                    put((r, c0+cc*cells_per_char), char, weight)
    for obj in layout["objects"]:
        cls = obj["class"]
        symbol = SYMBOLS.get(cls, "B")
        if cls == "building":
            symbol = {"town": "h", "industrial": "I", "airfield": "A", "hq": "H"}.get(building_role(obj), "B")
        weight = 3 if cls in ("tree", "prop", "terrain_object", "light", "rock") else 10
        weight += {"wall": 1, "gate": 4, "turret": 3, "turret_control": 2, "turret_generator": 2}.get(cls, 0)
        put(obj["cell"], symbol, weight)
    visible = [o for o in layout.get("objectives", []) if o.get("display") and not o.get("hidden") and o.get("cell")]
    for obj in visible:
        put(obj["cell"], "O", 20)
    if layout.get("player_start") is not None:
        put(layout["player_start"], "@", 30)
    teams = Counter(o["team"] for o in layout["objects"] if o["class"] not in ("tree", "prop", "rock"))
    heading = [f"{layout['source']['corpus']}/{layout['name']} | {rows}x{cols} cells | {cells_per_char} cells/char",
               f"North up; cell crop [{r0},{c0})..[{r1},{c1}); objects by team: {dict(sorted(teams.items()))}", LEGEND,
               "+" + "-"*nc + "+"]
    lines = heading + ["|"+"".join(line)+"|" for line in grid] + ["+"+"-"*nc+"+"]
    if visible:
        lines += ["Visible objectives (O; symbols may share a character):"]
        lines += [f"  {o['cell']}: {o['title']}" for o in visible]
    lines += ["Coarse preview: highest-priority feature wins each character; object footprints are not drawn."]
    if layout.get("warnings"):
        lines += ["Source warnings: " + "; ".join(layout["warnings"])]
    return "\n".join(lines) + "\n"


def examples(corpus, out, cells_per_char):
    index = load_json(corpus / "index.json")
    selected = []
    for row in index["missions"]:
        if row["corpus"] == "stock" and row["name"] in ("mc2_01", "mc2_03", "mc2_06", "mc2_10", "mc2_17", "mc2_24"):
            selected.append(row)
    for label in sorted({row["corpus"] for row in index["missions"]} - {"stock"}):
        candidates = [r for r in index["missions"] if r["corpus"] == label and not re.fullmatch(r"mc2_\d+", r["name"])]
        candidates = [r for r in candidates if r["objects"] > 100 and r["size"]["rows"] <= 1024]
        if not candidates:
            candidates = [r for r in index["missions"] if r["corpus"] == label]
        if candidates:
            # Stable selection: first and middle substantial custom maps.
            selected.extend([candidates[0], candidates[len(candidates)//2]] if len(candidates) > 1 else candidates)
    for row in selected:
        path = output_path(out / row["corpus"] / (row["name"] + ".txt"))
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(render(load_json(corpus / row["file"]), cells_per_char), encoding="ascii", errors="backslashreplace")
        print(path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mission", nargs="?", type=Path)
    parser.add_argument("--cells-per-char", type=int, default=3)
    parser.add_argument("--crop", type=int, nargs=4)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--examples", action="store_true")
    parser.add_argument("--corpus", type=Path, default=Path(__file__).resolve().parent / "corpus")
    args = parser.parse_args(argv)
    if args.examples:
        examples(args.corpus, args.out or Path("docs/mapgen/examples"), args.cells_per_char)
    elif args.mission:
        result = render(load_json(args.mission), args.cells_per_char, args.crop)
        if args.out:
            path = output_path(args.out)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(result, encoding="ascii", errors="backslashreplace")
        else:
            print(result, end="")
    else:
        parser.error("provide a mission JSON or --examples")


if __name__ == "__main__":
    main()
