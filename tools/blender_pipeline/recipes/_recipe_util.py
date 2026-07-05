"""
_recipe_util.py — shared helpers for headless Blender recipes.

BLENDER-ASSET-PIPELINE-1. Imported by recipes running INSIDE Blender
(`blender --background --factory-startup --python recipe.py -- --k=v`).
Keep `import bpy` inside functions guarded at call time so the arg-parsing
half stays importable by pytest outside Blender.
"""
import json
import sys


# ---------------------------------------------------------------------------
# Arg parsing (pure python — pytest-covered)
# ---------------------------------------------------------------------------

def script_args(argv=None):
    """Parse recipe args after the `--` separator. `--k=v` -> {'k': 'v'};
    bare `--flag` -> {'flag': True}. Returns {} when no `--` present."""
    argv = sys.argv if argv is None else argv
    if "--" not in argv:
        return {}
    out = {}
    for tok in argv[argv.index("--") + 1:]:
        if not tok.startswith("--"):
            continue
        body = tok[2:]
        if "=" in body:
            k, v = body.split("=", 1)
            out[k] = v
        else:
            out[body] = True
    return out


def arg(args, key, default=None, cast=str, required=False):
    """Fetch + cast one arg. Raises SystemExit with a clear message when a
    required arg is missing or a cast fails (visible in the driver log)."""
    if key not in args:
        if required:
            print(f"[recipe:error] missing required arg --{key}", flush=True)
            raise SystemExit(2)
        return default
    try:
        v = args[key]
        if cast is bool:
            return v is True or str(v).lower() in ("1", "true", "yes", "on")
        return cast(v)
    except (TypeError, ValueError):
        print(f"[recipe:error] bad value for --{key}: {args[key]!r}", flush=True)
        raise SystemExit(2)


def float_list(raw, sep=","):
    """'0.5,0.2' -> [0.5, 0.2]. Empty/None -> []."""
    if not raw or raw is True:
        return []
    return [float(x) for x in str(raw).split(sep) if x.strip()]


def emit_stats(tag, payload):
    """Print a machine-readable stats line the driver scrapes into its report."""
    print(f"[recipe:stats:{tag}] {json.dumps(payload, sort_keys=True)}", flush=True)


# ---------------------------------------------------------------------------
# bpy helpers (Blender-only)
# ---------------------------------------------------------------------------

def reset_scene():
    import bpy
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_glb(path):
    import bpy
    bpy.ops.import_scene.gltf(filepath=str(path))


def export_glb(path):
    import bpy
    bpy.ops.export_scene.gltf(filepath=str(path), export_format="GLB")


def mesh_objects():
    import bpy
    return [o for o in bpy.data.objects if o.type == "MESH"]


def mesh_counts():
    """Total (vertices, triangles) across all mesh objects (eval'd geometry)."""
    import bpy
    deps = bpy.context.evaluated_depsgraph_get()
    verts = tris = 0
    for o in mesh_objects():
        ev = o.evaluated_get(deps)
        me = ev.to_mesh()
        me.calc_loop_triangles()
        verts += len(me.vertices)
        tris += len(me.loop_triangles)
        ev.to_mesh_clear()
    return verts, tris


def weld_vertices(obj, dist):
    """Merge-by-distance. ASE->GLB meshes have per-(pos,uv) split verts —
    welding is near-mandatory before subdiv/decimate so modifiers see a
    connected surface. Loop UVs survive the weld."""
    import bmesh
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    before = len(bm.verts)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=dist)
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()
    return before, len(obj.data.vertices)


def crease_sharp_edges(obj, angle_deg):
    """Set edge crease 1.0 on edges whose face angle exceeds angle_deg so
    Catmull-Clark subdivision preserves hard silhouettes (buildings!).
    Blender 4.0+ stores creases in the 'crease_edge' float attribute."""
    import math
    import bmesh
    threshold = math.radians(angle_deg)
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    layer = bm.edges.layers.float.get("crease_edge")
    if layer is None:
        layer = bm.edges.layers.float.new("crease_edge")
    creased = 0
    for e in bm.edges:
        if len(e.link_faces) == 2:
            if e.calc_face_angle(0.0) > threshold:
                e[layer] = 1.0
                creased += 1
        elif len(e.link_faces) < 2:
            # Boundary edge — pin it so open shells keep their rim.
            e[layer] = 1.0
            creased += 1
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()
    return creased


def apply_all_modifiers(obj):
    import bpy
    with bpy.context.temp_override(object=obj, active_object=obj,
                                   selected_objects=[obj],
                                   selected_editable_objects=[obj]):
        for mod in list(obj.modifiers):
            bpy.ops.object.modifier_apply(modifier=mod.name)
