#!/usr/bin/env python3
"""
asset_edit.py — BLENDER-ASSET-EDIT-LOOP-1 human/agent-in-the-loop edit round-trip.

Interactive counterpart to the headless batch_driver.py pipeline:

    open ASSET     locate stock .ase/.ini -> extract to GLB (tools/ase_to_glb.py)
                   in work/<asset>/<asset>.glb, then launch Blender GUI
                   (detached, NOT --background) importing it via
                   asset_edit_open.py. Edit, then File > Export > glTF 2.0
                   back over the SAME path.
    export ASSET   re-attach stock baseColor texture URIs to the (re-exported)
                   GLB (batch_driver.carry_textures against the pristine
                   work/<asset>/<asset>.stock.glb copy) and inject an engine
                   override into out/<asset>/ (default out-kind tgl_ini).
    status ASSET   workdir state: extracted-at, GLB mtime changed since open?
                   -> "edited, ready to export".

Usage:
    py -3 tools/blender_pipeline/asset_edit.py open quonset
    py -3 tools/blender_pipeline/asset_edit.py open quonset --socket   # into a
        running Blender via the BlenderMCP addon socket (127.0.0.1:9876)
    py -3 tools/blender_pipeline/asset_edit.py open quonset --background-selftest
    py -3 tools/blender_pipeline/asset_edit.py status quonset
    py -3 tools/blender_pipeline/asset_edit.py export quonset
        [--out-kind tgl_ini|models_json|glb_only] [--source-name NAME]
        [--deploy-tgl-dir DIR]

Exit codes: 0 ok, 2 usage/lookup error, 1 stage failure. No pip deps.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import batch_driver as bd  # noqa: E402  (reuse GLB/ini/inject helpers)
from blender_runner import find_blender_exe, run_blender_script  # noqa: E402
from srcdata import SRCDATA_ROOT_PROBES  # noqa: E402

TOOL_DIR = Path(__file__).resolve().parent
ASE_TO_GLB = TOOL_DIR.parent / "ase_to_glb.py"
OPEN_SCRIPT = TOOL_DIR / "asset_edit_open.py"
DEFAULT_WORK_ROOT = TOOL_DIR / "work"
DEFAULT_OUT_ROOT = TOOL_DIR / "out"

OUT_KINDS = ("tgl_ini", "models_json", "glb_only")
SESSION_VERSION = 1

# Stock srcdata discovery: CLI --srcdata > MC2_SRCDATA env > known checkouts.
# find_stock_asset works in terms of srcdata ROOT (not the tgl dir) and
# resolves one named asset at a time, a different shape than srcdata.py's
# find_tgl_dir/find_srcdata — kept local, but sourced from the same shared
# probe list (srcdata.SRCDATA_ROOT_PROBES) so the checkout roots stay in sync.
_SRCDATA_PROBES = [str(p) for p in SRCDATA_ROOT_PROBES]


# ---------------------------------------------------------------------------
# Stock asset discovery (pure — pytest-covered)
# ---------------------------------------------------------------------------

def find_stock_asset(asset, srcdata=None):
    """Return (ase_path, ini_path) for a stock asset name, or raise
    RuntimeError with the probed locations."""
    roots = []
    if srcdata:
        roots.append(Path(srcdata))
    env = os.environ.get("MC2_SRCDATA")
    if env:
        roots.append(Path(env))
    roots.extend(Path(p) for p in _SRCDATA_PROBES)

    probed = []
    for root in roots:
        ase = root / "tgl" / f"{asset}.ase"
        probed.append(str(ase))
        if ase.is_file():
            ini = ase.with_suffix(".ini")
            if not ini.is_file():
                raise RuntimeError(
                    f"found {ase} but its FITini sibling is missing: {ini}")
            return ase, ini
    raise RuntimeError(
        f"stock asset {asset!r} not found. Probed:\n  " + "\n  ".join(probed)
        + "\nSet --srcdata or MC2_SRCDATA to the mc2srcdata checkout root.")


# ---------------------------------------------------------------------------
# Workdir layout + session state (pure — pytest-covered)
# ---------------------------------------------------------------------------

def paths_for(asset, work_root=DEFAULT_WORK_ROOT):
    work = Path(work_root) / asset
    return {
        "work": work,
        "glb": work / f"{asset}.glb",              # Blender edit target
        "stock_glb": work / f"{asset}.stock.glb",  # pristine extract (texture src)
        "sidecar": work / f"{asset}.mcasset.json",
        "session": work / "session.json",
    }


def save_session(paths, session):
    paths["session"].parent.mkdir(parents=True, exist_ok=True)
    paths["session"].write_text(json.dumps(session, indent=2) + "\n",
                                encoding="utf-8")


def load_session(paths):
    """Return the session dict, or None when the asset was never opened."""
    try:
        return json.loads(paths["session"].read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def glb_state(session, glb_path):
    """'missing' | 'unchanged' | 'edited' — mtime+size vs the open snapshot."""
    p = Path(glb_path)
    if not p.is_file():
        return "missing"
    st = p.stat()
    if (st.st_mtime_ns != session.get("glb_mtime_ns")
            or st.st_size != session.get("glb_size")):
        return "edited"
    return "unchanged"


def snapshot_glb(session, glb_path):
    st = Path(glb_path).stat()
    session["glb_mtime_ns"] = st.st_mtime_ns
    session["glb_size"] = st.st_size


# ---------------------------------------------------------------------------
# Blender launch (cmd builders pure — pytest-covered)
# ---------------------------------------------------------------------------

def build_gui_launch_cmd(blender_exe, glb_path, asset):
    """GUI launch: NO --background, NO --factory-startup (keeps user addons,
    e.g. the BlenderMCP socket). asset_edit_open.py imports the GLB and prints
    the edit/re-export instructions."""
    return [str(blender_exe), "--python", str(OPEN_SCRIPT), "--",
            f"--glb={glb_path}", f"--asset={asset}"]


def launch_detached(cmd):
    """Spawn without blocking or tying the child to this console. Returns pid."""
    kwargs = {"stdin": subprocess.DEVNULL, "stdout": subprocess.DEVNULL,
              "stderr": subprocess.DEVNULL}
    if os.name == "nt":
        kwargs["creationflags"] = (subprocess.DETACHED_PROCESS
                                   | subprocess.CREATE_NEW_PROCESS_GROUP)
    else:  # pragma: no cover — Windows-only project, kept portable
        kwargs["start_new_session"] = True
    # background-launch: allow Blender GUI the user asked to edit in (not a console child); already DETACHED_PROCESS via kwargs
    return subprocess.Popen(cmd, **kwargs).pid


def build_socket_import_code(glb_path, asset):
    """bpy code sent to an ALREADY-RUNNING Blender via the BlenderMCP addon
    socket (tools/blender_cmd.py, 127.0.0.1:9876). Non-destructive: imports
    into the current scene. Drops the Blender 5.x glTF-import 'custom_normal'
    corner attribute that breaks UV bookkeeping on re-export (README gotcha)."""
    glb = str(glb_path)
    return (
        "import bpy\n"
        f"bpy.ops.import_scene.gltf(filepath={glb!r})\n"
        "for _me in bpy.data.meshes:\n"
        "    _a = _me.attributes.get('custom_normal')\n"
        "    if _a is not None:\n"
        "        _me.attributes.remove(_a)\n"
        f"print('[asset_edit] imported', {glb!r})\n"
        "print('[asset_edit] edit, then File > Export > glTF 2.0 (glTF Binary) "
        "over the SAME path')\n"
        f"print('[asset_edit] then run: py -3 tools/blender_pipeline/"
        f"asset_edit.py export {asset}')\n"
    )


def socket_send_code(code, port=9876):
    """Reuse tools/blender_cmd.py's raw-socket sender."""
    sys.path.insert(0, str(TOOL_DIR.parent))
    import blender_cmd  # noqa: E402 — defs only at import time
    try:
        return blender_cmd.send("execute_code", {"code": code}, port=port)
    except OSError as exc:
        raise RuntimeError(
            f"no Blender listening on 127.0.0.1:{port} ({exc}). Start Blender "
            "with the BlenderMCP addon socket, or use plain 'open' to launch "
            "a fresh GUI.")


# ---------------------------------------------------------------------------
# Export planning (pure — pytest-covered)
# ---------------------------------------------------------------------------

def plan_export(asset, session, paths, out_dir, out_kind, source_name,
                deploy_tgl_dir=None, override_class="staticprop"):
    """Ordered action list for the inject step (no side effects).

    Actions: {'action': 'copy', 'src', 'dst'}
             {'action': 'write_ini', 'stock_ini', 'dst', 'source_name'}
             {'action': 'models_json', 'path', 'entry'}
    """
    out_dir = Path(out_dir)
    glb = paths["glb"]
    actions = []

    if out_kind == "glb_only":
        actions.append({"action": "copy", "src": str(glb),
                        "dst": str(out_dir / f"{source_name}.glb")})

    elif out_kind == "tgl_ini":
        stock_ini = Path(session["ini"])
        actions.append({"action": "copy", "src": str(glb),
                        "dst": str(out_dir / f"{source_name}.glb")})
        actions.append({"action": "write_ini", "stock_ini": str(stock_ini),
                        "dst": str(out_dir / stock_ini.name),
                        "source_name": source_name})
        if paths["sidecar"].is_file():
            actions.append({"action": "copy", "src": str(paths["sidecar"]),
                            "dst": str(out_dir / f"{source_name}.mcasset.json")})
        if deploy_tgl_dir:
            deploy = Path(deploy_tgl_dir)
            for a in list(actions):
                dst = Path(a["dst"])
                if dst.suffix in (".glb", ".ini", ".json"):
                    actions.append({"action": "copy", "src": a["dst"],
                                    "dst": str(deploy / dst.name)})

    elif out_kind == "models_json":
        lod0 = f"{asset}_lod0.glb"
        actions.append({"action": "copy", "src": str(glb),
                        "dst": str(out_dir / lod0)})
        entry = bd.build_models_json_entry(
            {"override_class": override_class, "appearance_name": asset},
            [lod0], [])
        actions.append({"action": "models_json",
                        "path": str(out_dir / "models.json"), "entry": entry})
    else:
        raise ValueError(f"unknown out-kind {out_kind!r}")
    return actions


def run_export_plan(actions):
    """Execute plan actions. write_ini copies are ordered after the write in
    plan_export, so copy-of-a-written-file always sees the fresh content."""
    written = []
    for a in actions:
        dst = Path(a["dst"] if "dst" in a else a["path"])
        dst.parent.mkdir(parents=True, exist_ok=True)
        if a["action"] == "copy":
            shutil.copyfile(a["src"], dst)
        elif a["action"] == "write_ini":
            stock = Path(a["stock_ini"]).read_text(encoding="utf-8",
                                                   errors="replace")
            dst.write_text(bd.build_override_ini(stock, a["source_name"]),
                           encoding="utf-8")
        elif a["action"] == "models_json":
            existing = []
            if dst.is_file():
                try:
                    existing = json.loads(dst.read_text(encoding="utf-8"))
                except json.JSONDecodeError:
                    existing = []
            merged = bd.merge_models_json(existing, a["entry"])
            dst.write_text(json.dumps(merged, indent=2) + "\n",
                           encoding="utf-8")
        else:  # pragma: no cover — plan_export only emits the three above
            raise RuntimeError(f"unknown action {a['action']}")
        written.append(str(dst))
    return written


# ---------------------------------------------------------------------------
# Post-export GLB validation (pure — pytest-covered)
# ---------------------------------------------------------------------------

MAX_SANE_TRI_COUNT = 5_000_000


def validate_exported_glb(gltf):
    """Cheap sanity check on a re-exported GLB's parsed glTF JSON. Raises
    RuntimeError (loudly, before the asset leaves work/) if the Blender 5.x
    'custom_normal' UV-corruption bug (README gotcha) or another gross
    round-trip failure slipped through: missing TEXCOORD_0, lost texture
    URIs, or a zero/absurd triangle count. Returns None on success."""
    meshes = gltf.get("meshes", [])
    # Note: no meshes at all means there's no geometry to validate (e.g. a
    # texture-only GLB fragment in tests) — that's a different failure mode
    # than a round-trip that lost UVs, so it's not flagged here.

    tri_count = 0
    accessors = gltf.get("accessors", [])
    for mesh in meshes:
        for prim in mesh.get("primitives", []):
            attrs = prim.get("attributes", {})
            if "TEXCOORD_0" not in attrs:
                raise RuntimeError(
                    "export validation: primitive missing TEXCOORD_0 — "
                    "likely the Blender 5.x 'custom_normal' UV-corruption "
                    "bug (see README); re-open with the scrub applied")
            idx_acc = prim.get("indices")
            if idx_acc is not None and idx_acc < len(accessors):
                tri_count += accessors[idx_acc].get("count", 0) // 3
            elif idx_acc is None and "POSITION" in attrs:
                pos_acc = attrs["POSITION"]
                if pos_acc < len(accessors):
                    tri_count += accessors[pos_acc].get("count", 0) // 3

    if meshes and tri_count <= 0:
        raise RuntimeError(
            f"export validation: triangle count is {tri_count} (expected > 0)")
    if tri_count > MAX_SANE_TRI_COUNT:
        raise RuntimeError(
            f"export validation: triangle count {tri_count} exceeds sanity "
            f"ceiling {MAX_SANE_TRI_COUNT} — likely a corrupt export")

    materials = gltf.get("materials", [])
    images = gltf.get("images", [])
    if materials and any("baseColorTexture" in m.get("pbrMetallicRoughness", {})
                         for m in materials) and not images:
        raise RuntimeError(
            "export validation: material references a baseColorTexture but "
            "the GLB has no images/URIs — texture carry-over likely failed")
    return None


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def _extract(ase, ini, paths, asset_class):
    paths["work"].mkdir(parents=True, exist_ok=True)
    cmd = [sys.executable, str(ASE_TO_GLB),
           "--ase", str(ase), "--ini", str(ini),
           "--out-glb", str(paths["glb"]),
           "--out-sidecar", str(paths["sidecar"]),
           "--asset-class", asset_class]
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if proc.returncode != 0:
        raise RuntimeError(
            f"extract failed:\n{proc.stdout[-1500:]}\n{proc.stderr[-1500:]}")


def cmd_open(args):
    paths = paths_for(args.asset, args.work_root)
    old = load_session(paths)
    if old and glb_state(old, paths["glb"]) == "edited" and not args.force:
        print(f"REFUSING: {paths['glb']} was edited since it was opened.\n"
              f"  export it first (asset_edit.py export {args.asset}) or "
              "re-open with --force to discard the edit.", file=sys.stderr)
        return 2

    ase, ini = find_stock_asset(args.asset, args.srcdata)
    print(f"[open] stock: {ase}")
    _extract(ase, ini, paths, args.asset_class)
    shutil.copyfile(paths["glb"], paths["stock_glb"])

    session = {"session_version": SESSION_VERSION, "asset": args.asset,
               "ase": str(ase), "ini": str(ini),
               "asset_class": args.asset_class,
               "extracted_at": time.strftime("%Y-%m-%d %H:%M:%S")}
    snapshot_glb(session, paths["glb"])
    save_session(paths, session)
    print(f"[open] extracted: {paths['glb']}")

    if args.background_selftest:
        exe = find_blender_exe(args.blender)
        log = paths["work"] / "blender_selftest.log"
        run_blender_script(exe, OPEN_SCRIPT,
                           {"glb": str(paths["glb"]), "asset": args.asset,
                            "selftest": True}, log_path=str(log))
        print(f"[open] selftest edit applied (scale Z x1.1), log: {log}")
        print(f"[open] status: {glb_state(session, paths['glb'])}")
        return 0

    if args.socket:
        resp = socket_send_code(
            build_socket_import_code(paths["glb"], args.asset), args.port)
        print(f"[open] socket import response: {resp[:300]}")
    else:
        exe = find_blender_exe(args.blender)
        pid = launch_detached(build_gui_launch_cmd(exe, paths["glb"],
                                                   args.asset))
        session["launched_pid"] = pid
        save_session(paths, session)
        print(f"[open] Blender GUI launched detached (pid {pid})")
    print(f"[open] edit, File > Export > glTF 2.0 over {paths['glb']},\n"
          f"       then: py -3 tools/blender_pipeline/asset_edit.py "
          f"export {args.asset}")
    return 0


def cmd_status(args):
    paths = paths_for(args.asset, args.work_root)
    session = load_session(paths)
    if session is None:
        print(f"{args.asset}: not opened (no {paths['session']})")
        return 0
    state = glb_state(session, paths["glb"])
    print(f"{args.asset}: extracted {session.get('extracted_at')} "
          f"from {session.get('ase')}")
    print(f"  work GLB: {paths['glb']}")
    if state == "edited":
        print("  state: edited, ready to export -> py -3 "
              f"tools/blender_pipeline/asset_edit.py export {args.asset}")
    elif state == "unchanged":
        print("  state: unchanged since open (edit + re-export the GLB in "
              "Blender first)")
    else:
        print("  state: work GLB MISSING — re-run open")
    out_dir = Path(args.out_root) / args.asset
    if out_dir.is_dir():
        files = sorted(p.name for p in out_dir.iterdir())
        print(f"  exported: {out_dir} -> {', '.join(files) or '(empty)'}")
    return 0


def cmd_export(args):
    paths = paths_for(args.asset, args.work_root)
    session = load_session(paths)
    if session is None:
        print(f"ERROR: {args.asset} was never opened — run "
              f"asset_edit.py open {args.asset} first.", file=sys.stderr)
        return 2
    state = glb_state(session, paths["glb"])
    if state == "missing":
        print(f"ERROR: work GLB missing: {paths['glb']} — re-run open.",
              file=sys.stderr)
        return 2
    if state == "unchanged":
        print("WARNING: GLB unchanged since open — exporting the un-edited "
              "extract anyway.")

    # Texture carry-over: stock URIs from the pristine extract, in place
    # (same semantics as batch_driver's patch_textures stage; idempotent).
    src_gltf, _ = bd.read_glb(paths["stock_glb"])
    dst_gltf, dst_bin = bd.read_glb(paths["glb"])
    patched = bd.carry_textures(src_gltf, dst_gltf)
    if patched:
        bd.write_glb(dst_gltf, dst_bin, paths["glb"])
        snapshot_glb(session, paths["glb"])  # patch write is not a user edit
        save_session(paths, session)
    print(f"[export] texture carry-over: {patched} material(s) patched")

    final_gltf, _ = bd.read_glb(paths["glb"])
    validate_exported_glb(final_gltf)
    print("[export] validation: TEXCOORD_0 present, textures intact, "
          "triangle count sane")

    source_name = args.source_name or f"{args.asset.capitalize()}Edit"
    out_dir = Path(args.out_root) / args.asset
    actions = plan_export(args.asset, session, paths, out_dir, args.out_kind,
                          source_name, deploy_tgl_dir=args.deploy_tgl_dir,
                          override_class=args.override_class)
    written = run_export_plan(actions)
    for w in written:
        print(f"[export] wrote {w}")
    if args.out_kind == "tgl_ini" and not args.deploy_tgl_dir:
        print("[export] to test in-game, copy the .glb/.ini into a deploy "
              "lane's data/tgl (or re-run with --deploy-tgl-dir).")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    p.add_argument("--work-root", default=str(DEFAULT_WORK_ROOT),
                   help=argparse.SUPPRESS)  # test seam
    p.add_argument("--out-root", default=str(DEFAULT_OUT_ROOT),
                   help=argparse.SUPPRESS)  # test seam
    sub = p.add_subparsers(dest="command", required=True)

    po = sub.add_parser("open", help="extract stock asset + launch Blender")
    po.add_argument("asset")
    po.add_argument("--asset-class", default="building",
                    choices=("mech", "prop", "building", "vehicle", "tree"))
    po.add_argument("--srcdata", help="mc2srcdata checkout root "
                    "(else MC2_SRCDATA env, then known locations)")
    po.add_argument("--blender", help="explicit blender.exe path")
    po.add_argument("--socket", action="store_true",
                    help="import into an already-running Blender via the "
                    "BlenderMCP addon socket instead of launching a GUI")
    po.add_argument("--port", type=int, default=9876,
                    help="BlenderMCP socket port (default 9876)")
    po.add_argument("--background-selftest", action="store_true",
                    help="headless round-trip proof: import, scale Z x1.1, "
                    "re-export over the same GLB (no GUI)")
    po.add_argument("--force", action="store_true",
                    help="re-extract even if the work GLB has unexported edits")
    po.set_defaults(func=cmd_open)

    ps = sub.add_parser("status", help="show workdir state for an asset")
    ps.add_argument("asset")
    ps.set_defaults(func=cmd_status)

    pe = sub.add_parser("export", help="patch textures + inject override")
    pe.add_argument("asset")
    pe.add_argument("--out-kind", default="tgl_ini", choices=OUT_KINDS)
    pe.add_argument("--source-name",
                    help="override GLB source name (default <Asset>Edit)")
    pe.add_argument("--deploy-tgl-dir",
                    help="also copy tgl_ini outputs into this deploy data/tgl "
                    "dir (no default)")
    pe.add_argument("--override-class", default="staticprop",
                    choices=("staticprop", "tree"),
                    help="models_json override class")
    pe.set_defaults(func=cmd_export)

    args = p.parse_args(argv)
    try:
        return args.func(args)
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
