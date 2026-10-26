"""Pinned Remastered building payload; binary releases live outside git."""
import hashlib
import json
import os
import re
from pathlib import Path, PureWindowsPath
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

import console_child

KIT_REL = Path("mods/mc2-industrial-kit")
MANIFEST = Path("data/kit/industrial-kit.json")
IMPORT_ARGS = ["--no-ground", "--static-prop"]


def reconcile_stock_textures(target_dir, inventory=None, jobs=1):
    """Receipt only byte-proven stock sources and pinned stock sidecars.

    The pins include current batch_cook output and verified golden legacy cooks. Deployment
    never trusts paired source existence, target art, or MOD overlay contents.
    Unknown recipes and mismatched bytes retain user priority.
    """
    target = Path(target_dir)
    if inventory is None:
        inventory = json.loads((Path(__file__).resolve().parents[1] /
            "data/kit/stock-textures.json").read_text())
    sources = inventory["sha256"]
    cooks = inventory["cooked_sha256"]

    # Invalidate legacy cooker stamps and receipts for removed/unpinned files.
    for stamp in (target / "data/tgl").rglob("*"):
        if stamp.is_file() and stamp.name.casefold().endswith(".cookstamp"):
            stamp.unlink()

    def reconcile(item):
        rel, expected = item
        path = contained_path(target, rel)
        if not path.is_file():
            return 0
        data = path.read_bytes()
        accepted = [expected] if isinstance(expected, str) else expected
        if hashlib.sha256(data).hexdigest() not in accepted:
            return 0
        value = 14695981039346656037
        for byte in data:
            value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
        Path(str(path) + ".cookstamp").write_text(
            f"mc2-stock-v1 {value:016x}\n", encoding="ascii")
        return int(path.suffix.casefold() == ".ktx2")

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        count = sum(pool.map(reconcile, sorted({**sources, **cooks}.items())))
    print(f"[deploy_payload] stock texture receipts verified/backfilled: {count}", flush=True)
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
    # Identical small-prop LODs share a content-addressed native recipe.
    unique_models = len({r["sha256"] for r in manifest["files"] if r["path"].endswith(".glb")})
    identity = hashlib.sha256((sha(tool) + json.dumps(manifest, sort_keys=True)
                               + json.dumps(IMPORT_ARGS)).encode()).hexdigest()
    cache = target / "cache/content-model"
    receipt = cache / "industrial-cook.json"
    try:
        previous = json.loads(receipt.read_text())
        recipes = previous["recipes"]
        rows = previous["files"]
        if (previous["identity"] == identity and isinstance(recipes, dict) and set(recipes) == set(models)
                and len(rows) == unique_models
                and set(recipes.values()) == {r["path"] for r in rows}):
            checked_files(cache, rows)
            print(f"[deploy_payload] pinned industrial kit verified: {len(models)} GLBs cached; cook skipped")
            return
    except (OSError, ValueError, KeyError, TypeError):
        pass
    # Record the importer's source-to-recipe mapping with the verified inventory.
    recipes = {}
    recipes_by_sha = {}
    model_shas = {r["path"]: r["sha256"] for r in manifest["files"] if r["path"].endswith(".glb")}
    outputs = set()
    for rel in models:
        digest = model_shas[rel]
        if digest in recipes_by_sha:
            recipes[rel] = recipes_by_sha[digest]
            continue
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
        recipes_by_sha[digest] = recipes[rel]
    outputs = sorted(outputs)
    if len(outputs) != unique_models or any(not p.is_file() or p.stat().st_size == 0 for p in outputs):
        raise ValueError(f"industrial cook incomplete: {len(outputs)} recipes for {unique_models} distinct GLBs")
    rows = [dict(path=p.name, bytes=p.stat().st_size, sha256=sha(p)) for p in outputs]
    checked_files(cache, rows)
    checked_files(kit, manifest["files"])
    receipt.write_text(json.dumps(dict(identity=identity, recipes=recipes, files=rows), indent=2) + "\n")
    print(f"[deploy_payload] pinned industrial kit verified: {len(models)} GLBs cooked")
