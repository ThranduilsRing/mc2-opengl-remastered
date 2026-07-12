"""
asset_edit_open.py — bpy-side bootstrap for asset_edit.py (runs INSIDE Blender).

GUI open (launched detached by asset_edit.py; .glb is not a native Blender
open format, so we import glTF into an empty scene and print instructions):

    blender --python asset_edit_open.py -- --glb=PATH --asset=NAME

Headless round-trip selftest (import, trivial visible edit, re-export over
the same path — proves the loop with no GUI):

    blender -b --factory-startup --python asset_edit_open.py -- \
        --glb=PATH --asset=NAME --selftest

Kept standalone (no _recipe_util import) so the GUI launch has zero sys.path
requirements inside Blender.
"""
import sys

SELFTEST_SCALE_Z = 1.1


def script_args(argv=None):
    """--k=v -> {'k': 'v'}; bare --flag -> {'flag': True} (after the `--`)."""
    argv = sys.argv if argv is None else argv
    if "--" not in argv:
        return {}
    out = {}
    for tok in argv[argv.index("--") + 1:]:
        if not tok.startswith("--"):
            continue
        key, sep, val = tok[2:].partition("=")
        out[key] = val if sep else True
    return out


def _drop_custom_normals():
    """Blender 5.x glTF import creates a 'custom_normal' float2 corner
    attribute that leaks into mesh.uv_layers and silently breaks UV
    bookkeeping — re-export loses TEXCOORD_0 (README gotcha; same removal
    as recipes/rebake_pbr.py). Normals re-derive from geometry."""
    import bpy
    removed = 0
    for me in bpy.data.meshes:
        stale = me.attributes.get("custom_normal")
        if stale is not None:
            me.attributes.remove(stale)
            removed += 1
    return removed


def main():
    import bpy
    args = script_args()
    glb = args.get("glb")
    asset = args.get("asset", "asset")
    selftest = bool(args.get("selftest"))
    if not glb:
        print("[asset_edit_open] ERROR: missing --glb=PATH", flush=True)
        if selftest:
            sys.exit(2)
        return

    # Fresh empty scene (keeps user prefs/addons in GUI mode), then import.
    bpy.ops.wm.read_homefile(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=glb)
    dropped = _drop_custom_normals()
    meshes = [ob for ob in bpy.data.objects if ob.type == "MESH"]
    print(f"[asset_edit_open] imported {glb}: {len(meshes)} mesh object(s), "
          f"dropped {dropped} custom_normal attr(s)", flush=True)

    if selftest:
        for ob in meshes:
            ob.scale.z *= SELFTEST_SCALE_Z
        bpy.ops.export_scene.gltf(filepath=glb, export_format="GLB")
        print(f"[asset_edit_open] selftest: scaled Z x{SELFTEST_SCALE_Z} and "
              f"re-exported over {glb}", flush=True)
        return

    msg = (
        f"MC2 ASSET EDIT LOOP — {asset}\n"
        f"  1. Edit the model.\n"
        f"  2. File > Export > glTF 2.0, format 'glTF Binary (.glb)',\n"
        f"     EXPORT OVER THE SAME PATH:\n"
        f"       {glb}\n"
        f"  3. Then run:\n"
        f"       py -3 tools/blender_pipeline/asset_edit.py export {asset}\n"
    )
    print(msg, flush=True)
    # Also drop the instructions where they survive a lost console: a text
    # datablock visible in Blender's Text Editor.
    txt = bpy.data.texts.new(f"MC2_EDIT_{asset}")
    txt.write(msg)


if __name__ == "__main__":
    main()
