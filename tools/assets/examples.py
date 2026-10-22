"""Rebuild the hangar/fence selection evidence from local data; no downloads.

py -3 tools/assets/examples.py --rework
Existing complete rework packages are read, never overwritten.
"""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.assets.common import OUT_ROOT, parse_tgl, stock_install, write_json
from tools.assets.render import comparison, mesh_textures, render
from tools.assets.replace import search, select, target_spec
from tools.assets.rework import offline_upscalers, rework


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--catalog", type=Path, default=Path("docs/assets/catalog.json"))
    p.add_argument("--rework", action="store_true")
    args = p.parse_args(argv)
    data = json.loads(args.catalog.read_text(encoding="utf-8"))
    stock = stock_install()
    out = Path("docs/assets/examples")
    results = []
    for name in ("hangar", "wirefence", "wirefencediag", "razorwirefence", "razorwirefencediag", "vehiclehangar"):
        entry = select(data, name)
        for state in ("intact", "destroyed"):
            spec = target_spec(entry, state)
            prefix = name if state == "intact" else name+"-destroyed"
            write_json(out/(prefix+".spec.json"), spec)
            found = search(spec)
            write_json(out/(prefix+".search.json"), found)
            mesh = parse_tgl(stock.read(spec["stock_tgl"])[0])
            thumb = render(mesh, mesh_textures(mesh, stock), size=(512, 360))
            thumb.save(out/(prefix+"-stock.png"))
            results.append({"selection": name, "state": state, "instances": spec["instances"],
                "peak_mission_instances": spec["peak_mission_instances"], "footprint_wu": spec["footprint_wu"],
                "height_wu": spec["height_wu"], "triangles": mesh.triangles,
                "budget_triangles": spec["budgets"]["max_triangles_lod0"],
                "texture_edge_budget": spec["budgets"]["max_texture_edge"],
                "candidate_count": len(found["results"]), "library_exists": found["library_exists"]})
        if name in ("hangar", "wirefence") and args.rework:
            mod_id = "asset-rework-"+name
            destination = OUT_ROOT/mod_id
            if not destination.exists():
                rework(entry, mod_id)
            report = json.loads((destination/"rework-report.json").read_text(encoding="utf-8"))
            write_json(out/(name+"-rework-report.json"), report)
            (out/(name+"-modeller-brief.md")).write_text((destination/"modeller-brief.md").read_text(encoding="utf-8"), encoding="utf-8")
            from PIL import Image
            renamed = {t["stock"]: t["cooked"] for t in report["textures"]}
            for state in ("intact", "destroyed"):
                spec = target_spec(entry, state)
                original = parse_tgl(stock.read(spec["stock_tgl"])[0])
                cooked = parse_tgl((destination/spec["stock_tgl"]).read_bytes())
                tex = mesh_textures(original, stock)
                for i, texture in enumerate(original.textures):
                    if texture in renamed:
                        tex[i] = Image.open(destination/"data"/"tgl"/"128"/renamed[texture]).convert("RGBA")
                comparison(original, cooked, mesh_textures(original, stock), tex, out/f"{name}-{state}-rework.png")
                comparison(original, cooked, mesh_textures(original, stock), tex, destination/f"preview-{state}.png")
    write_json(out/"results.json", results)
    write_json(out/"offline-upscalers.json", offline_upscalers())
    top = sorted((r for r in data["objects"] if r["category"] not in ("tree", "mech")), key=lambda r: (-r["priority"], r["id"]))[:20]
    write_json(Path("docs/assets/top20.json"), [{"rank": i+1, "id": r["id"], "category": r["category"], "instances": r["instances"], "priority": r["priority"]} for i, r in enumerate(top)])
    rows = ["# Top 20 modernization priorities", "", "Vegetation and mechs are listed separately in [catalog.md](catalog.md). Scores use the documented usage/size/quality heuristic.", "",
            "| Rank | Selection | Category | Instances | Score |", "|---:|---|---|---:|---:|"]
    rows += [f"| {i+1} | {r['id']} | {r['category']} | {r['instances']:,} | {r['priority']:,.1f} |" for i, r in enumerate(top)]
    Path("docs/assets/top20.md").write_text("\n".join(rows)+"\n", encoding="utf-8")
    missing = [r for r in data["objects"] if r["coverage"] != "ok"]
    model_errors = {m["tgl"]: m["error"] for r in data["objects"] for m in r["models"] if "error" in m}
    write_json(out/"coverage-details.json", {"coverage": data["coverage"],
        "missing_base_by_category": dict(Counter(r["category"] for r in missing)),
        "missing_base_appearances": [r["id"] for r in missing], "model_errors": model_errors,
        "unresolved_instances": {label: sum(s["unresolved_type_ids"].values()) for label, s in data["installs"].items()},
        "custom_or_missing_instances": {label: s["custom_or_missing_instances"] for label, s in data["installs"].items()}})
    print(f"Wrote {len(results)} target/search/stock-preview sets; rework={args.rework}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
