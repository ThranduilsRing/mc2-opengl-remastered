"""
decimate_lods.py — headless Blender recipe: LOD-chain generation.

BLENDER-ASSET-PIPELINE-1 quick-win (capability-survey #2): a pure bpy
Decimate loop that fills the LOD slots the engine ALREADY consumes —
appearance-ini FileName0..N/Distance0..N (mclib/bdactor.cpp TGLData) and
model_overrides/models.json ModelOverrideLod chains (TREE-OVERRIDE-LOD-MVP-1).
LOD0 is the input GLB itself (copied verbatim by the driver, no Blender
round-trip degradation); this recipe emits LOD1..N.

Run (normally via batch_driver.py):
  blender --background --factory-startup --python decimate_lods.py -- \
      --in=maple1.glb --out-dir=lods/ --name=maple1 --ratios=0.5,0.2

Args:
  --in=PATH        input GLB (required)
  --out-dir=DIR    output directory (required)
  --name=BASE      output base name -> <BASE>_lod1.glb, <BASE>_lod2.glb ...
  --ratios=CSV     decimate ratios per LOD, descending (e.g. 0.5,0.2,0.05)
  --weld=DIST      merge-by-distance before decimate (default 0.0001; 0 = off)
  --min-tris=N     never decimate below N triangles (default 8) — MC2 trees are
                   already 15t; a ratio that would go below floor is clamped
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _recipe_util import (  # noqa: E402
    apply_all_modifiers, arg, emit_stats, export_glb, float_list, import_glb,
    mesh_counts, mesh_objects, reset_scene, script_args, weld_vertices,
)


def main():
    args = script_args()
    in_glb = arg(args, "in", required=True)
    out_dir = arg(args, "out-dir", required=True)
    name = arg(args, "name", required=True)
    ratios = float_list(arg(args, "ratios", required=True))
    weld = arg(args, "weld", 0.0001, float)
    min_tris = arg(args, "min-tris", 8, int)

    if not ratios:
        print("[recipe:error] --ratios is empty", flush=True)
        raise SystemExit(2)

    os.makedirs(out_dir, exist_ok=True)
    lods = []

    for lod_index, ratio in enumerate(ratios, start=1):
        # Fresh import per LOD: each LOD decimates from the ORIGINAL mesh, not
        # cumulatively, so ratios in the manifest mean what they say.
        reset_scene()
        import_glb(in_glb)
        if not mesh_objects():
            print("[recipe:error] no mesh objects in input GLB", flush=True)
            raise SystemExit(2)

        _, tris_src = mesh_counts()

        for obj in mesh_objects():
            if weld > 0.0:
                weld_vertices(obj, weld)
            me = obj.data
            me.calc_loop_triangles()
            obj_tris = len(me.loop_triangles)
            eff_ratio = ratio
            if obj_tris > 0 and obj_tris * ratio < min_tris:
                eff_ratio = min(1.0, min_tris / obj_tris)
            mod = obj.modifiers.new(name="mc2_decimate", type="DECIMATE")
            mod.decimate_type = "COLLAPSE"
            mod.ratio = eff_ratio
            mod.use_collapse_triangulate = True
            apply_all_modifiers(obj)

        verts, tris = mesh_counts()
        out_glb = os.path.join(out_dir, f"{name}_lod{lod_index}.glb")
        export_glb(out_glb)
        lods.append({"lod": lod_index, "ratio": ratio,
                     "tris_src": tris_src, "tris": tris, "verts": verts,
                     "file": os.path.basename(out_glb)})
        print(f"[recipe] wrote {out_glb} ({tris} tris)", flush=True)

    emit_stats("decimate_lods", {"name": name, "lods": lods})


if __name__ == "__main__":
    main()
