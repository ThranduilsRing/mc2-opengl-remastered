"""
upscale_mesh.py — headless Blender recipe: mesh upscale / reimagine.

BLENDER-ASSET-PIPELINE-1. Weld split verts -> crease hard edges -> subdivision
surface -> shade smooth -> GLB out. Designed for MC2-era low-poly assets
(buildings median 76v/82t) where Catmull-Clark with creased hard edges rounds
bevels/curves without collapsing the silhouette.

Run (normally via batch_driver.py):
  blender --background --factory-startup --python upscale_mesh.py -- \
      --in=src.glb --out=dst.glb --subdiv=1 --crease-angle=40 --weld=0.0001

Args:
  --in=PATH            input GLB (required)
  --out=PATH           output GLB (required)
  --subdiv=N           subdivision levels (default 1)
  --subdiv-type=T      CATMULL_CLARK (default) | SIMPLE (densify only, no smoothing;
                       use SIMPLE when the mesh will get displacement later)
  --weld=DIST          merge-by-distance before subdiv (default 0.0001; 0 = off)
  --crease-angle=DEG   crease edges sharper than DEG so subdiv keeps them
                       (default 40; 0 = off — full smooth)
  --shade-smooth       shade smooth after subdiv (default on; --shade-smooth=0 off)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _recipe_util import (  # noqa: E402
    apply_all_modifiers, arg, crease_sharp_edges, emit_stats, export_glb,
    import_glb, mesh_counts, mesh_objects, reset_scene, script_args,
    weld_vertices,
)


def main():
    import bpy

    args = script_args()
    in_glb = arg(args, "in", required=True)
    out_glb = arg(args, "out", required=True)
    subdiv = arg(args, "subdiv", 1, int)
    subdiv_type = arg(args, "subdiv-type", "CATMULL_CLARK")
    weld = arg(args, "weld", 0.0001, float)
    crease_angle = arg(args, "crease-angle", 40.0, float)
    shade_smooth = arg(args, "shade-smooth", True, bool)

    if subdiv_type not in ("CATMULL_CLARK", "SIMPLE"):
        print(f"[recipe:error] bad --subdiv-type {subdiv_type}", flush=True)
        raise SystemExit(2)

    reset_scene()
    import_glb(in_glb)

    if not mesh_objects():
        print("[recipe:error] no mesh objects in input GLB", flush=True)
        raise SystemExit(2)

    v0, t0 = mesh_counts()

    welded = creased = 0
    for obj in mesh_objects():
        if weld > 0.0:
            before, after = weld_vertices(obj, weld)
            welded += before - after
        if crease_angle > 0.0 and subdiv_type == "CATMULL_CLARK":
            creased += crease_sharp_edges(obj, crease_angle)

        if subdiv > 0:
            mod = obj.modifiers.new(name="mc2_subdiv", type="SUBSURF")
            mod.subdivision_type = subdiv_type
            mod.levels = subdiv
            mod.render_levels = subdiv
            mod.use_creases = True
            apply_all_modifiers(obj)

        if shade_smooth:
            for poly in obj.data.polygons:
                poly.use_smooth = True

    v1, t1 = mesh_counts()
    emit_stats("upscale_mesh", {
        "in": os.path.basename(in_glb), "out": os.path.basename(out_glb),
        "subdiv": subdiv, "subdiv_type": subdiv_type,
        "welded_verts": welded, "creased_edges": creased,
        "verts_before": v0, "tris_before": t0,
        "verts_after": v1, "tris_after": t1,
    })

    os.makedirs(os.path.dirname(os.path.abspath(out_glb)), exist_ok=True)
    export_glb(out_glb)
    print(f"[recipe] wrote {out_glb}", flush=True)


if __name__ == "__main__":
    main()
