"""Pinned Remastered building payload; binary releases live outside git."""
import hashlib
import json
import os
import re
from pathlib import Path, PureWindowsPath
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

import console_child

KIT_REL = Path("mods/mc2-industrial-kit")
MANIFEST = Path("data/kit/industrial-kit.json")
IMPORT_ARGS = ["--no-ground", "--static-prop"]


def reconcile_stock_textures(target_dir, stock_dir, jobs=1, stock_hashes=None):
    """Receipt only byte-proven stock sources and deterministic stock sidecars.

    The read-only stock reference is the same golden install used for deploy
    materials, never the target's possibly edited sources or MOD overlays.
    Unknown recipes and mismatched bytes are left untouched.
    """
    tools = Path(__file__).resolve().parents[1] / "tools/mc2texcook"
    sys.path.insert(0, str(tools))
    import batch_cook
    import mc2texcook
    target, stock = Path(target_dir), Path(stock_dir)
    if stock_hashes is None:
        stock_hashes = json.loads((Path(__file__).resolve().parents[1] /
            "data/kit/stock-textures.json").read_text())["sha256"]
    if not (stock / "data/tgl").is_dir():
        raise ValueError(f"stock texture reference missing: {stock}")

    def reconcile(source):
        rel = source.relative_to(stock)
        installed = target / rel
        stock_bytes = source.read_bytes()
        if hashlib.sha256(stock_bytes).hexdigest() != stock_hashes.get(rel.as_posix()):
            return 0
        if not installed.is_file() or installed.read_bytes() != stock_bytes:
            return 0
        # Stock TGAs can be loose-only (not present in an FST). Record their
        # identity too, so later TGA edits still invalidate priority exemptions.
        mc2texcook.record_cook(installed, verified_bytes=stock_bytes)
        output = installed.with_suffix(".ktx2")
        if not output.is_file():
            return 0
        data = output.read_bytes()
        stamp = Path(str(output) + ".cookstamp")
        if stamp.is_file():
            # Never bless a modified cooked file, including on repeat deploys.
            return 0
        import struct
        if len(data) < 80 or data[:12] != mc2texcook.KTX2_MAGIC:
            return 0
        fmt, = struct.unpack_from("<I", data, 12)
        levels, = struct.unpack_from("<I", data, 40)
        with tempfile.TemporaryDirectory(prefix="stock-cook-", dir=target) as td:
            expected = Path(td) / "expected.ktx2"
            if fmt == 146:  # legacy albedo BC7 sRGB
                batch_cook._cook_one_bc7(source, expected, batch_cook._DEFAULT_KTX_TOOL,
                                         0, gen_mips=levels > 1)
            elif fmt == 43:  # RGBA8 sRGB, full mip chain
                batch_cook._cook_one(source, expected, "albedo", 0)
            else:
                return 0
            if data != expected.read_bytes() or output.read_bytes() != data:
                return 0
            mc2texcook.record_cook(output, verified_bytes=data)
            return 1

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        count = sum(pool.map(reconcile, sorted((stock / "data/tgl").rglob("*.tga"))))
    print(f"[deploy_payload] stock texture receipts backfilled: {count}", flush=True)
    return count


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def contained_path(root, rel):
    """Require a canonical relative member and a contained resolved destination."""
    win = PureWindowsPath(rel)
    if (not rel or win.drive or win.root or "\\" in rel
            or any(p in ("", ".", "..") for p in rel.split("/"))):
        raise ValueError(f"unsafe kit path: {rel}")
    root = Path(root).resolve()
    path = (root / rel).resolve()
    if not path.is_relative_to(root):
        raise ValueError(f"unsafe kit path: {rel}")
    return path


def checked_files(root, rows):
    """Reject absent, altered and unsafe package members before copying any bytes."""
    root = Path(root).resolve()
    seen = set()
    for row in rows:
        rel = row["path"]
        path = contained_path(root, rel)
        if rel.casefold() in {p.casefold() for p in seen}:
            raise ValueError(f"unsafe/duplicate kit path: {rel}")
        seen.add(rel)
        if not path.is_file() or path.stat().st_size != row["bytes"] or sha(path) != row["sha256"]:
            raise ValueError(f"missing/mismatched pinned kit file: {path}")
    return seen


def payload(src_root):
    manifest = json.loads((Path(src_root) / MANIFEST).read_text(encoding="utf-8"))
    if manifest["schema"] != 1 or not manifest["assets"] or not manifest["files"]:
        raise ValueError("invalid pinned kit manifest")
    store = Path(os.environ.get("MC2_KIT_STORE", manifest["store"]))
    checked_files(store, manifest["files"])
    models = {p["path"] for p in manifest["files"] if p["path"].endswith(".glb")}
    expected = {f"data/model_overrides/{a['id']}/pristine_intact_lod{i}.glb"
                for a in manifest["assets"] for i in range(3)}
    if models != expected:
        raise ValueError("pinned kit asset list differs from its GLB payload")
    return manifest, [(str(store / r["path"]), (KIT_REL / r["path"]).as_posix(), "kit")
                      for r in manifest["files"]]


def build_importer(src_root):
    """Ninja checks the real source/header closure even when an old exe exists."""
    import configure_ninja as cn
    from bootstrap_worktree_build import find_cmake
    root = Path(src_root).resolve()
    tree = root / "build-cc-tgdump"
    cmake = find_cmake()
    jobs = Path("A:/Games/mc2-ops/JOBS").read_text().strip()
    commands = []
    if not (tree / "build.ninja").is_file():
        flags, env = cn.cache_cmake_args(str(root))
        commands.append(([cmake, "-S", str(root / "tools/tg_import_dump"), "-B", str(tree),
                          "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={cn.NINJA}",
                          "-DCMAKE_BUILD_TYPE=RelWithDebInfo", *flags], env))
    commands.append(([cmake, "--build", str(tree), "--config", "RelWithDebInfo",
                      "--target", "tg_import_dump", "--parallel", jobs], None))
    for command, env in commands:
        if cn.under_vcvars(subprocess.list2cmdline(command), env):
            raise ValueError("pinned kit importer build failed")
    tool = tree / "tg_import_dump.exe"
    if not tool.is_file():
        raise ValueError(f"kit importer build produced no executable: {tool}")
    return tool


def finish(target_dir, manifest, tool, cooker=subprocess.run):
    """Verify installed source, cook native recipes and verify the cache receipt."""
    target = Path(target_dir)
    kit = target / KIT_REL
    paths = checked_files(kit, manifest["files"])
    extra = {p.relative_to(kit).as_posix() for p in kit.rglob("*") if p.is_file()}
    extra -= paths | {".modindex-cache"}
    if extra:
        raise ValueError(f"unpinned kit files in install: {sorted(extra)}")
    models = sorted(p for p in paths if p.endswith(".glb"))
    identity = hashlib.sha256((sha(tool) + json.dumps(manifest, sort_keys=True)
                               + json.dumps(IMPORT_ARGS)).encode()).hexdigest()
    cache = target / "cache/content-model"
    receipt = cache / "industrial-cook.json"
    try:
        previous = json.loads(receipt.read_text())
        recipes = previous["recipes"]
        rows = previous["files"]
        if (previous["identity"] == identity and isinstance(recipes, dict) and set(recipes) == set(models)
                and len(rows) == len(models)
                and set(recipes.values()) == {r["path"] for r in rows}):
            checked_files(cache, rows)
            print(f"[deploy_payload] pinned industrial kit verified: {len(models)} GLBs cached; cook skipped")
            return
    except (OSError, ValueError, KeyError, TypeError):
        pass
    # Record the importer's source-to-recipe mapping with the verified inventory.
    recipes = {}
    outputs = set()
    for rel in models:
        result = cooker([str(tool), (KIT_REL / rel).as_posix(), *IMPORT_ARGS],
                        cwd=target, capture_output=True, text=True, timeout=120,
                        **console_child.child_kwargs(redirected=True))
        if result.returncode:
            raise ValueError(f"industrial native cook failed: {rel}: {result.stderr[-300:]}")
        keys = re.findall(r"\[CONTENT_COOK\].*?key=([0-9a-f]{16})", result.stdout + result.stderr)
        if len(keys) != 1:
            raise ValueError(f"industrial importer did not report one native recipe: {rel}")
        outputs.add(cache / f"model-{keys[0]}.mci")
        recipes[rel] = f"model-{keys[0]}.mci"
    outputs = sorted(outputs)
    if len(outputs) != len(models) or any(not p.is_file() or p.stat().st_size == 0 for p in outputs):
        raise ValueError(f"industrial cook incomplete: {len(outputs)} recipes for {len(models)} GLBs")
    rows = [dict(path=p.name, bytes=p.stat().st_size, sha256=sha(p)) for p in outputs]
    checked_files(cache, rows)
    checked_files(kit, manifest["files"])
    receipt.write_text(json.dumps(dict(identity=identity, recipes=recipes, files=rows), indent=2) + "\n")
    print(f"[deploy_payload] pinned industrial kit verified: {len(models)} GLBs cooked")
