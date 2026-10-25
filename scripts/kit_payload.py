"""Pinned Remastered building payload; binary releases live outside git."""
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import sys

import console_child

KIT_REL = Path("mods/mc2-industrial-kit")
MANIFEST = Path("data/kit/industrial-kit.json")


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def checked_files(root, rows):
    """Reject absent, altered and unsafe package members before copying any bytes."""
    root = Path(root).resolve()
    seen = set()
    for row in rows:
        rel = row["path"]
        path = (root / rel).resolve()
        if not path.is_relative_to(root) or rel in seen:
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
    identity = hashlib.sha256((sha(tool) + json.dumps(manifest, sort_keys=True)).encode()).hexdigest()
    cache = target / "cache/content-model"
    receipt = cache / "industrial-cook.json"
    try:
        previous = json.loads(receipt.read_text())
        if previous["identity"] == identity and previous["files"]:
            checked_files(cache, previous["files"])
            return
    except (OSError, ValueError, KeyError):
        pass
    models = sorted(p for p in paths if p.endswith(".glb"))
    outputs = set()
    for rel in models:
        result = cooker([str(tool), (KIT_REL / rel).as_posix(), "--no-ground", "--static-prop"],
                        cwd=target, capture_output=True, text=True, timeout=120,
                        **console_child.child_kwargs(redirected=True))
        if result.returncode:
            raise ValueError(f"industrial native cook failed: {rel}: {result.stderr[-300:]}")
        keys = re.findall(r"\[CONTENT_COOK\].*?key=([0-9a-f]{16})", result.stdout + result.stderr)
        if len(keys) != 1:
            raise ValueError(f"industrial importer did not report one native recipe: {rel}")
        outputs.add(cache / f"model-{keys[0]}.mci")
    outputs = sorted(outputs)
    if len(outputs) != len(models) or any(not p.is_file() or p.stat().st_size == 0 for p in outputs):
        raise ValueError(f"industrial cook incomplete: {len(outputs)} recipes for {len(models)} GLBs")
    rows = [dict(path=p.name, bytes=p.stat().st_size, sha256=sha(p)) for p in outputs]
    checked_files(cache, rows)
    receipt.write_text(json.dumps(dict(identity=identity, files=rows), indent=2) + "\n")
    print(f"[deploy_payload] pinned industrial kit verified: {len(models)} GLBs cooked")
