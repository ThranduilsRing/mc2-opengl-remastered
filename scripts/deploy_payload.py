#!/usr/bin/env python3
"""deploy_payload.py -- deploy payload completeness gate (slice S8 seed, manifest v1).

Wraps/validates a deploy of the MC2 runtime payload from a source worktree
build to a runtime target dir (e.g. A:/Games/mc2-opengl/mc2-win64-v0.4 or
.../mc2-win64-0.4c). Payload, by target:
  both    : exe + pdb + ffmpeg DLLs + shaders/ + run-with-log.bat
  game    : + run-mc2.bat
  editor  : + run-editor.bat + tools/terrain_gen/** (authoring support -- the
            editor's Generate-Mission / -gen-map path shells `py -3
            tools\\terrain_gen\\terrain_gen.py`; without it the editor builds
            and launches but every gen/mission-load smoke case fails)
Support files (launch .bat, terrain_gen tree) are source-tracked, not build
outputs, so build -> deploy stays self-complete with no manual copy step.

Hardens against the four documented stale-deploy failure classes
(docs/superpowers/recon/scratch-accel-1-deploy.md):
  A. stale exe via process lock          -> Step 0 lock check, HARD FAIL, never taskkill
  B. stale shaders via cp -r no-overwrite -> per-file copy + post-copy byte diff, HARD FAIL
  C. wrong deploy target                  -> .deployed_manifest.csv identity record
  D. stale PDB                            -> PDB present + hash == source, HARD FAIL
                                             (override: --allow-stale-pdb)

After a successful deploy, writes <target>/.deployed_manifest.csv (manifest v1):
  rows = relpath,sha256,bytes,src_commit,timestamp,mtime_ns

Unchanged files (source size AND mtime_ns match the previous row, and the
deployed copy still has the recorded size) are not re-copied or re-hashed.
--full-verify disables that fast path and copies/hashes every payload file.

--verify-only re-checks an existing target against its manifest (post-hoc
staleness detector). Advisory: exit 0 with a report, unless --strict.

Python (not .sh) to match sibling conventions: scripts/ is python-dominated
(run_smoke.py, check-asset-manifests.py, ...) and we need sha256 + csv + robust
Windows file-lock probing, all stdlib here.

NO engine code. NO run_smoke verdict-path involvement. tier1 not required
(scripts-only, zero runtime change).
"""

import argparse
import atexit
import csv
import ctypes
import datetime
import glob
import hashlib
import importlib.util
import json
import os
import ntpath
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import threading
from contextlib import contextmanager
import console_child  # noqa: E402  (stdio-inheriting children keep their output; scripts/console_child.py)

try:
    import psutil
    _PSUTIL_OK = True
except ImportError:
    _PSUTIL_OK = False

MANIFEST_NAME = ".deployed_manifest.csv"
MANIFEST_VERSION = "v1"
REUSE_NAME = ".deploy-inputs.json"


@contextmanager
def deploy_phase(name, timeout=600):
    """Report progress and terminate this deploy's children if a phase stalls."""
    if not _PSUTIL_OK:
        fail("bounded deploy phases require psutil (child cleanup on timeout)")
    started = time.monotonic()
    done = threading.Event()

    def watch():
        while not done.wait(min(15, timeout)):
            elapsed = time.monotonic() - started
            log(f"phase {name}: running {elapsed:.1f}s / {timeout}s")
            if elapsed >= timeout:
                log(f"ERROR: phase {name} timed out after {elapsed:.1f}s")
                # Only descendants of this deploy, never another lane's processes.
                children = psutil.Process().children(recursive=True)
                for child in reversed(children):
                    try:
                        child.kill()
                    except psutil.Error:
                        pass
                psutil.wait_procs(children, timeout=5)
                os._exit(8)

    log(f"phase {name}: start (timeout {timeout}s)")
    worker = threading.Thread(target=watch, daemon=True)
    worker.start()
    try:
        yield
    finally:
        done.set()
        worker.join()
        log(f"phase {name}: finished {time.monotonic() - started:.3f}s")


def deploy_inputs(src_root):
    """Hash authored inputs, never the multi-GB kit store or cooked outputs."""
    roots = ["data", "mods", "examples", "assets", "scripts", "tools", "cmake", "CMakeLists.txt",
             "run-mc2.bat", "run-editor.bat", "run-with-log.bat", "3rdparty/smaa", "3rdparty/cmaa2"]
    result = subprocess.run(["git", "-C", str(src_root), "ls-files", "-z", "--cached",
                             "--others", "--exclude-standard", "--", *roots],
                            capture_output=True, check=True, timeout=30,
                            **console_child.child_kwargs(redirected=True))
    names = set(os.fsdecode(p) for p in result.stdout.split(b"\0") if p)
    # Loose deploy members may be ignored by Git, but are still live inputs.
    names.update(rel for rel in IMPORTED_STRUCTURE_PAYLOAD + GAME_DATA_PAYLOAD + TERRAIN_LAYER_PAYLOAD
                 if (Path(src_root) / rel).is_file())
    names.update(p.relative_to(src_root).as_posix()
                 for p in (Path(src_root) / EXAMPLE_MODS_SRC).rglob("*") if p.is_file())
    # The importer consumes engine sources too; changes to its source closure
    # invalidate the cook even when the pinned package JSON did not change.
    cmake = Path(src_root) / "tools/tg_import_dump/CMakeLists.txt"
    if cmake.is_file():
        names.update(p for p in re.findall(r"\$\{MC2_ROOT\}/([\w./-]+)", cmake.read_text())
                     if Path(p).suffix in (".cpp", ".h", ".c", ".hpp"))
    for directory in ("mclib", "GameOS"):
        names.update(p.relative_to(src_root).as_posix()
                     for p in (Path(src_root) / directory).rglob("*") if p.suffix in (".h", ".hpp"))
    return {name: sha256_file(Path(src_root) / name) for name in sorted(names)} | {
        "environment": [os.environ.get(k, "") for k in ("MC2_KIT_STORE", "MC2_REF_ROOT")]}


def reusable_payload(src_root, target, inputs):
    """Fail closed on missing inputs, manifests, outputs or sampled byte drift."""
    import random
    from kit_payload import contained_path
    try:
        parents = {}
        saved = json.loads((Path(target) / REUSE_NAME).read_text())
        if saved["inputs"] != inputs:
            return False, "authored kit/mod or deploy inputs changed"
        if saved["manifest"] != sha256_file(Path(target) / MANIFEST_NAME):
            return False, "payload manifest changed"
        manifest = read_manifest(target)
        if not manifest or not saved["outputs"]:
            return False, "missing payload manifest or output inventory"
        for rel, stamp in saved["outputs"].items():
            p = contained_path(target, rel, parents=parents)
            s = p.stat()
            if [s.st_size, s.st_mtime_ns, s.st_ctime_ns] != stamp:
                return False, f"output missing or changed: {rel}"
        for src, (stamp, expected) in saved["sources"].items():
            s = Path(src).stat()
            if [s.st_size, s.st_mtime_ns, s.st_ctime_ns] != stamp:
                return False, f"kit source changed: {src}"
        for kit in ("mods/mc2-industrial-kit", "mods/mc2-vehicle-kit"):
            expected = {rel for rel in saved["outputs"] if rel.startswith(kit + "/")}
            if expected:
                actual = {p.relative_to(target).as_posix() for p in (Path(target) / kit).rglob("*")
                          if p.is_file() and p.name != ".modindex-cache"}
                if actual != expected:
                    return False, f"kit inventory changed: {kit}"
        for src in random.SystemRandom().sample(list(saved["sources"]), min(6, len(saved["sources"]))):
            content = Path(src).read_bytes()
            if src.endswith(".mcasset.json"):
                content = content.replace(b"\r\n", b"\n")
            if hashlib.sha256(content).hexdigest() != saved["sources"][src][1]:
                return False, f"sampled kit source mismatch: {src}"
        # Always include both kit packages and their native recipes. Rotate the
        # sample so repeated deploys don't leave the same files unchecked.
        groups = {}
        for rel in saved["outputs"]:
            group = rel.split("/", 2)[:2] if rel.startswith("mods/") else rel.split("/")[:1]
            groups.setdefault("/".join(group), []).append(rel)
        for paths in groups.values():
            for rel in random.SystemRandom().sample(paths, min(3, len(paths))):
                expected = saved["hashes"][rel]
                if sha256_file(contained_path(target, rel, parents=parents)) != expected:
                    return False, f"sampled hash mismatch: {rel}"
        return True, "authored input hashes and recorded payload match; output samples verified"
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return False, "missing or invalid reuse/manifest evidence"


def record_deploy_inputs(target, inputs, hashes, items):
    """Publish reuse evidence only after the full deploy and all cooks succeed."""
    from kit_payload import contained_path
    inventory = {rel: row[0] for rel, row in hashes.items()}
    for name in ("industrial-cook.json", "vehicle-cook.json"):
        receipt = Path(target) / "cache/content-model" / name
        if receipt.is_file():
            inventory[receipt.relative_to(target).as_posix()] = sha256_file(receipt)
            for row in json.loads(receipt.read_text())["files"]:
                inventory["cache/content-model/" + row["path"]] = row["sha256"]
    outputs = {}
    parents = {}
    for rel in inventory:
        s = contained_path(target, rel, parents=parents).stat()
        outputs[rel] = [s.st_size, s.st_mtime_ns, s.st_ctime_ns]
    sources = {}
    for src, rel, kind in items:
        if kind not in ("exe", "pdb", "shader", "dll"):
            s = Path(src).stat()
            sources[str(src)] = [[s.st_size, s.st_mtime_ns, s.st_ctime_ns], hashes[rel][0]]
    value = dict(inputs=inputs, manifest=sha256_file(Path(target) / MANIFEST_NAME),
                 outputs=outputs, hashes=inventory, sources=sources)
    pending = Path(target) / (REUSE_NAME + ".tmp")
    pending.write_text(json.dumps(value, sort_keys=True), encoding="utf-8")
    os.replace(pending, Path(target) / REUSE_NAME)
SHADER_EXTS = (".vert", ".frag", ".tesc", ".tese", ".comp")

# Editor target marker (exe_name match, case-insensitive).
EDITOR_EXE = "mission editor.exe"

# Launch scripts that belong in a user download, by target. Sourced from
# src_root (tracked at repo root). run-with-log.bat is the generic stderr-
# capturing launcher and ships with both.
SUPPORT_SCRIPTS = {
    "game":   ["run-mc2.bat", "run-with-log.bat"],
    "editor": ["run-editor.bat", "run-with-log.bat"],
}

# Obsolete payload excluded from runtime bootstraps.
RETIRED_LAUNCHER = "mc2-launcher.exe"

# Editor authoring support trees (src_root-relative dirs copied recursively,
# structure preserved). The editor's -gen-map / Generate Mission path shells
# `py -3 tools\\terrain_gen\\terrain_gen.py`; without this tree the editor
# builds and launches but every gen/mission-load smoke case fails. Tying it to
# the deploy payload keeps "build -> deploy" self-complete (no manual copy).
EDITOR_SUPPORT_TREES = ["tools/terrain_gen"]

# Cook tools shipped with BOTH game and editor installs so modders/users can
# re-cook or inspect assets without a full source checkout.
#   tools/cook_bc6h_hdri.py  -- EXR -> BC6H KTX2 via texconv+ktx (HDRI baking)
#   tools/examples/          -- asset inspection helpers (list_mission_assets etc.)
GAME_COOK_TOOLS = ["tools/cook_bc6h_hdri.py", "tools/examples"]

# Repo-shipped imported structure (TERRAIN-CLIFF-MESH-DRESSING-P0): data/tgl is enumerated
# file-by-file, not copied wholesale, so these reach an install only through this list.
# Building/prop replacements ship as mounted content packages (tools/content_pipeline), not here.
IMPORTED_STRUCTURE_PAYLOAD = [
    "data/tgl/spotlight.ini",
    "data/tgl/marblecliff.ini",
    "data/tgl/MarbleCliffGLB.glb",
]

# Renderer config JSON read directly off "data/<name>.json" relative paths by
# GameOS/gameos (terrain_material_lib.cpp, visual_tuning_profile.cpp) -- both
# game AND editor installs load these (shared renderer core), so they must
# ship even though neither is a build output. Missing file = silent no-op at
# runtime (both readers tolerate absence), but shipping without them silently
# drops per-mission/material tuning that was authored assuming the file loads.
GAME_DATA_PAYLOAD = [
    "data/terrain_materials.json",
    "data/visual_tuning.json",
    # Native vegetation packs these tracked cards into its 4x2 atlas at load.
    "data/vegetation/cards/grass_01.png",
    "data/vegetation/cards/grass_02.png",
    "data/vegetation/cards/grass_03.png",
    "data/vegetation/cards/grass_04.png",
    "data/vegetation/cards/bush_round.png",
    "data/vegetation/cards/bush_heath.png",
    "data/vegetation/cards/sapling_leaf.png",
    "data/vegetation/cards/sapling_pine.png",
    "data/vegetation/cooked/grass_01.ktx2",
    "data/vegetation/cooked/grass_02.ktx2",
    "data/vegetation/cooked/grass_03.ktx2",
    "data/vegetation/cooked/grass_04.ktx2",
    "data/vegetation/cooked/bush_round.ktx2",
    "data/vegetation/cooked/bush_heath.ktx2",
    "data/vegetation/cooked/sapling_leaf.ktx2",
    "data/vegetation/cooked/sapling_pine.ktx2",
    "data/vegetation/schemas/grasslands.json",
    "data/vegetation/schemas/snow.json",
    "data/vegetation/schemas/desert.json",
    "data/vegetation/schemas/swamp.json",


]

# TERRAIN-MATERIAL-TEXTURES-1: per-layer BC7 sRGB albedo KTX2s read off
# "data/terrain_layers/<channel>_albedo.ktx2" by the LOD-chunk albedo-array
# loader (none today: the GL consumer was retired; a Vulkan one is on the backlog).
# Loose cooked sidecars (gitignored; regenerate via
# tools/mc2texcook/cook_terrain_layers.py -- must be mip-complete BC7 sRGB).
# OPTIONAL at deploy time: a missing layer file only logs and skips (the
# engine fail-softs to the legacy tint path), matching the runtime contract.
TERRAIN_LAYER_PAYLOAD = [
    "data/terrain_layers/rock_albedo.ktx2",
    "data/terrain_layers/grass_albedo.ktx2",
    "data/terrain_layers/dirt_albedo.ktx2",
    "data/terrain_layers/concrete_albedo.ktx2",
    "data/terrain_layers/snow_albedo.ktx2",
    "data/terrain_layers/cliff_albedo.ktx2",
    "data/terrain_layers/concrete_detail_pad.ktx2",    # CONCRETE-ROAD-PBR-DETAIL (cook_concrete_detail.py)
    "data/terrain_layers/concrete_detail_road.ktx2",
    "data/terrain_layers/cliff_rock_ah.ktx2",          # TERRAIN-CLIFF-ROCK (cook_cliff_rock.py --select riverbed)
    "data/terrain_layers/cliff_rock_nra.ktx2",
]

# The two concrete/road detail KTX2s are generated, not tracked (about 1.4 MB each): cooked on demand at deploy time when absent.
CONCRETE_DETAIL_COOK = "tools/mc2texcook/cook_concrete_detail.py"
CONCRETE_DETAIL_SRC = "G:/mc2-assets/pbr/concrete-road"


def ensure_concrete_detail(src_root, cooker=subprocess.run):
    """Cook data/terrain_layers/concrete_detail_{pad,road}.ktx2 when missing (soft: the engine fail-softs without them)."""
    out = os.path.join(src_root, "data", "terrain_layers")
    if all(os.path.isfile(os.path.join(src_root, r)) for r in TERRAIN_LAYER_PAYLOAD if "concrete_detail" in r):
        return
    if not os.path.isdir(CONCRETE_DETAIL_SRC):
        log(f"[DEPLOY] WARN: concrete detail KTX2s absent and no cook source at {CONCRETE_DETAIL_SRC}: "
            "concrete/road detail will be missing from this install")
        return
    os.makedirs(out, exist_ok=True)
    log("concrete detail KTX2s absent: cooking them (about 15 s)")
    try:
        r = cooker([sys.executable, os.path.join(src_root, CONCRETE_DETAIL_COOK), "--src", CONCRETE_DETAIL_SRC, "--out", out],
                   capture_output=True, text=True, timeout=300, env=dict(os.environ, OMP_NUM_THREADS="1"), **console_child.child_kwargs(redirected=True))
    except subprocess.TimeoutExpired:
        log("[DEPLOY] WARN: concrete detail cook timed out after 300 s (skipped)")
        return
    if r.returncode:
        log("[DEPLOY] WARN: concrete detail cook failed (skipped): " + (r.stdout + r.stderr).strip()[-300:])


# TERRAIN-CLIFF-ROCK: the cliff rock pair is generated too (CC0 Poly Haven sets; licences in tools/mc2texcook/cliff_rock_provenance.json).
CLIFF_ROCK_COOK = "tools/mc2texcook/cook_cliff_rock.py"
CLIFF_ROCK_SRC = "C:/Users/Joe/Downloads/GameAsset/Terrain"
CLIFF_ROCK_SET = "riverbed"


def ensure_cliff_rock(src_root, cooker=subprocess.run):
    """Cook data/terrain_layers/cliff_rock_{ah,nra}.ktx2 when missing (soft: steep faces keep the base look without them)."""
    out = os.path.join(src_root, "data", "terrain_layers")
    if all(os.path.isfile(os.path.join(src_root, r)) for r in TERRAIN_LAYER_PAYLOAD if "cliff_rock" in r):
        return
    if not os.path.isdir(CLIFF_ROCK_SRC):
        log(f"[DEPLOY] WARN: cliff rock KTX2s absent and no cook source at {CLIFF_ROCK_SRC}: cliffs keep the base look")
        return
    os.makedirs(out, exist_ok=True)
    log("cliff rock KTX2s absent: cooking them")
    try:
        r = cooker([sys.executable, os.path.join(src_root, CLIFF_ROCK_COOK), "--src", CLIFF_ROCK_SRC, "--out", out,
                    "--sets", CLIFF_ROCK_SET, "--select", CLIFF_ROCK_SET],
                   capture_output=True, text=True, timeout=300, env=dict(os.environ, OMP_NUM_THREADS="1"), **console_child.child_kwargs(redirected=True))
    except subprocess.TimeoutExpired:
        log("[DEPLOY] WARN: cliff rock cook timed out after 300 s (skipped)")
        return
    if r.returncode:
        log("[DEPLOY] WARN: cliff rock cook failed (skipped): " + (r.stdout + r.stderr).strip()[-300:])

# EXAMPLE-MODS-1: worked example checkbox mods, source-tracked under
# examples/mods/. Ship to <target>/mods/<id>/ so the launcher enumerates them
# as add-on checkboxes (a mods/ folder needs mod.json AND a data/ overlay dir
# to count as a stackable add-on; data-less examples like sample_bad_mod /
# core_unlocks are lint/doc fixtures and are NOT shipped).
EXAMPLE_MODS_SRC = "examples/mods"

# UNIT/EQUIPMENT-DEFS deploy: generated definition sidecars consumed by the
# source-flip loaders (MC2_UNIT_DEFINITION_SOURCE=generated reads
# data/defs/units/core/..., MC2_EQUIPMENT_DEFINITION_SOURCE=generated reads
# data/defs/equipment/core/...). NOT committed in the repo -- regenerated at
# deploy time by the offline converters (tools/unit_converter/) from stock
# mc2srcdata, into <build64>/generated_defs/, then shipped like any payload.
# Converters are deterministic (tested); stock srcdata is the single source of
# truth, so deploy-time generation cannot drift from the converter the way a
# committed snapshot could. Soft-skips (with a loud log) when mc2srcdata or
# python is unavailable, since the engine defaults to the legacy path anyway.
GENERATED_DEFS = [
    # (converter script, extra args, out subdir, target rel root)
    ("tools/unit_converter/unit_converter.py", ["--batch"], "units",
     "data/defs/units"),
    ("tools/unit_converter/equipment_converter.py", [], "equipment",
     "data/defs/equipment"),
]

def find_srcdata(src_root):
    """Locate the stock mc2srcdata root (untracked, lives at the main repo
    root, not inside worktrees). Candidates, first hit wins:
    $MC2_SRCDATA, <src_root>/mc2srcdata, <src_root>/../../../mc2srcdata
    (worktree layout: .claude/worktrees/<name>), A:/Games/mc2-opengl-src/mc2srcdata."""
    cands = []
    env = os.environ.get("MC2_SRCDATA")
    if env:
        cands.append(env)
    cands.append(os.path.join(src_root, "mc2srcdata"))
    cands.append(os.path.abspath(os.path.join(src_root, "..", "..", "..",
                                              "mc2srcdata")))
    cands.append("A:/Games/mc2-opengl-src/mc2srcdata")
    for c in cands:
        if os.path.isdir(c):
            return os.path.abspath(c)
    return None


def generate_defs_payload(src_root, build64_root):
    """Run the offline converters --batch into <build64>/generated_defs/ and
    return payload items [(abs, rel, 'support')]. Empty list (loud log) when
    srcdata is missing or a converter fails -- the engine default is the legacy
    path, so a defs-less install still works; only the generated-source toggle
    goes inert."""
    srcdata = find_srcdata(src_root)
    if not srcdata:
        log("generated defs SKIPPED: mc2srcdata not found (set MC2_SRCDATA); "
            "the launcher's 'Data-driven definitions' toggle will be inert")
        return []
    gen_root = os.path.join(build64_root or os.path.join(src_root, "build64"),
                            "generated_defs")
    items = []
    for script, extra, sub, rel_root in GENERATED_DEFS:
        out_dir = os.path.join(gen_root, sub)
        cmd = [sys.executable, os.path.join(src_root, script),
               "--srcdata", srcdata, "--out", out_dir] + extra
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=600, **console_child.child_kwargs(redirected=True))
        if r.returncode != 0:
            log(f"generated defs SKIPPED: {script} failed rc={r.returncode}\n"
                f"{(r.stderr or r.stdout or '').strip()[:2000]}")
            return []
        n = 0
        for dirpath, _dirs, files in os.walk(out_dir):
            for fn in sorted(files):
                sp = os.path.join(dirpath, fn)
                rel = rel_root + "/" + os.path.relpath(sp, out_dir).replace(os.sep, "/")
                items.append((sp, rel, "support"))
                n += 1
        log(f"generated defs: {script} -> {n} files under {rel_root}/")
    return items


def example_mods_payload(src_root):
    """Enumerate shippable example mods (mod.json + data/ overlay) from
    examples/mods/ -> <target>/mods/<id>/ payload items."""
    root = os.path.join(src_root, EXAMPLE_MODS_SRC)
    items = []
    if not os.path.isdir(root):
        return items
    for entry in sorted(os.listdir(root)):
        mod_dir = os.path.join(root, entry)
        if not os.path.isdir(mod_dir):
            continue
        if not os.path.isfile(os.path.join(mod_dir, "mod.json")):
            continue
        if not os.path.isdir(os.path.join(mod_dir, "data")):
            log(f"example mod '{entry}' has no data/ overlay (lint/doc fixture) -- not shipped")
            continue
        n = 0
        for dirpath, _dirs, files in os.walk(mod_dir):
            for fn in sorted(files):
                sp = os.path.join(dirpath, fn)
                rel = "mods/" + entry + "/" + os.path.relpath(sp, mod_dir).replace(os.sep, "/")
                items.append((sp, rel, "support"))
                n += 1
        log(f"example mod '{entry}': {n} files -> mods/{entry}/")
    return items


FFMPEG_DLLS = [
    "avcodec-61.dll",
    "avformat-61.dll",
    "avutil-59.dll",
    "swscale-8.dll",
    "swresample-5.dll",
]
RUNTIME_DLLS = Path(__file__).with_name("release-dlls.txt").read_text().splitlines()


def log(msg):
    print(f"[deploy_payload] {msg}", flush=True)


_NINJA_SPV = re.compile(r"shaders[\\/]vulkan[\\/]([A-Za-z0-9_.\-]+\.spv)")


def baked_spv_files(b64):
    """The build's own SPIR-V outputs under <b64>/shaders/vulkan. A Ninja tree names every output in build.ninja; a .spv
    left in the directory by a retired CMake rule is not an output and never ships (W129: 20 dead .spv, 2026-10-09..11-04,
    shipped to v0.4 on every PC deploy). A tree whose build.ninja names no .spv (VS tree, test fixture) falls back to the
    directory listing."""
    spv_dir = Path(b64) / "shaders" / "vulkan"
    files = sorted(p for p in spv_dir.glob("*.spv") if p.is_file()) if spv_dir.is_dir() else []
    ninja = Path(b64) / "build.ninja"
    produced = set(_NINJA_SPV.findall(ninja.read_text(encoding="utf-8", errors="replace"))) if ninja.is_file() else set()
    if not produced:
        return files
    stale = [p.name for p in files if p.name not in produced]
    if stale:
        log(f"skipped {len(stale)} stale .spv no build rule produces: {', '.join(stale)}")
    return [p for p in files if p.name in produced]


_last_failure = []


def _reprint_failure_last():
    # Cleanup logs (lease release) print after fail(); repeat the refusal as the LAST line so `| tail` shows it.
    if _last_failure:
        print(f"[deploy_payload] FAIL (exit {_last_failure[1]}): {_last_failure[0].splitlines()[0]}", file=sys.stderr)


atexit.register(_reprint_failure_last)


def fail(msg, code=1):
    print(f"[deploy_payload] FAIL: {msg}", file=sys.stderr)
    _last_failure[:] = [msg, code]
    sys.exit(code)


def disk_space(path, need_bytes, warn_bytes=0, label=None, quiet=False):
    """Measure the destination volume before a write can truncate its files."""
    if need_bytes < 0 or warn_bytes < 0:
        raise ValueError("disk-space thresholds must be nonnegative")
    probe = os.path.abspath(path)
    while not os.path.exists(probe):
        parent = os.path.dirname(probe)
        if parent == probe:
            raise OSError(f"no existing ancestor of {path}")
        probe = parent
    if not os.path.isdir(probe):
        probe = os.path.dirname(probe)
    usage = shutil.disk_usage(probe)
    rc = 3 if usage.free < need_bytes else 1 if usage.free < need_bytes + warn_bytes else 0
    verdict = {0: "OK", 1: "LOW", 3: "CRITICAL"}[rc]
    if rc or not quiet:
        print(f"[DISK_GUARD] {verdict}: {usage.free} bytes free, {need_bytes} needed "
              f"for {label or 'write'} on {probe}", file=sys.stderr if rc == 3 else sys.stdout)
    return rc, {"verdict": verdict, "path": probe, "free_bytes": usage.free,
                "need_bytes": need_bytes, "warn_margin_bytes": warn_bytes, "op": label}


def disk_space_main(argv=None):
    """Read-only entry point shared by release, isolated deploy and capture callers."""
    import json
    import math
    parser = argparse.ArgumentParser(description="Check free space before writing")
    parser.add_argument("--path", default=repo_root())
    need = parser.add_mutually_exclusive_group(required=True)
    need.add_argument("--need-bytes", type=int)
    need.add_argument("--need-gb", type=float)
    parser.add_argument("--warn-gb", type=float, default=5.0)
    parser.add_argument("--label")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)
    try:
        if not math.isfinite(args.warn_gb) or (args.need_gb is not None and not math.isfinite(args.need_gb)):
            raise ValueError("disk-space thresholds must be finite")
        if args.warn_gb < 0 or (args.need_gb is not None and args.need_gb < 0):
            raise ValueError("disk-space thresholds must be nonnegative")
        required = args.need_bytes if args.need_bytes is not None else int(args.need_gb * 1024 ** 3)
        rc, facts = disk_space(args.path, required, int(args.warn_gb * 1024 ** 3), args.label, args.quiet)
        if args.json:
            print(json.dumps(facts))
        return rc
    except (OSError, ValueError) as exc:
        print(f"[DISK_GUARD] ERROR: {exc}", file=sys.stderr)
        return 2


def check_free_space_for_payload(items, target):
    """Require the measured payload plus headroom, with a 2 GiB floor."""
    if os.environ.get("MC2_DEPLOY_SKIP_DISK_GUARD") == "1":
        log("WARNING: MC2_DEPLOY_SKIP_DISK_GUARD=1 -- free-space preflight BYPASSED BY REQUEST.")
        return
    total = 0
    for src, _rel, _kind in items:
        try:
            total += os.path.getsize(src)
        except OSError:
            pass  # Optional payload entries need no space when absent.
    required = max(2 * 1024 ** 3, (total * 115 + 99) // 100)
    try:
        rc, _facts = disk_space(target, required, label="deploy payload")
    except (OSError, ValueError) as exc:
        fail(f"cannot measure deploy free space: {exc} -- nothing was written")
    if rc == 3:
        fail("insufficient free space for deploy payload -- nothing was written")


def check_hardlink_isolation(items, target):
    """LINK-GUARD-1: refuse to deploy into an install that is somebody else's.

    Motivated by the 2026-07-30 shared-install incident. Four deploy dirs under
    A:/Games/mc2-opengl shared one hardlinked mc2.exe, one shader tree, and --
    as the guard's first real run revealed -- ~1530 files each, including
    "Mission Editor.exe", every DLL, .deployed_manifest.csv, and
    .mc2_deploy_lane.json (the lane lock itself). deploy() below copies with
    shutil.copyfile, which opens the destination "wb": that TRUNCATES AND
    REWRITES THE SHARED INODE rather than replacing the directory entry, so a
    deploy into one of those dirs rewrote the other three in place.

    Nothing already in this script could see it. check_locks() asks whether a
    PROCESS holds the target; the lane lock and smoke lease ask whether another
    lane holds THIS PATH; DEPLOY_ALLOWLIST asks whether the path is canonical
    (mc2-win64-v0.4d-rc1 is BOTH allowlisted AND in the shared group, so the
    allowlist actively steered lanes into it). All path-keyed -- and a hardlink
    is exactly the case where two paths are one object. Every one of those
    guards returned a correct answer to its own question and a deploy still
    silently destroyed another lane's comparison baseline.

    The payload item list is passed through verbatim, so this is not a
    heuristic at the one site where being wrong costs evidence: the guard
    checks precisely the relpaths deploy() is about to overwrite, no more and
    no fewer.

    Verdict handling follows the DISK-GUARD-3 reasoning (c5d934d5):
      OK (0)           -> proceed silently.
      LINKED-DATA (1)  -> proceed. Read-only data sharing is CORRECT and is
                          what makes a private install affordable; it is only
                          reachable here via --data-links, which we do not pass.
      SHARED (3) /
      ERROR (2)        -> fatal, nothing written.
      guard missing, un-runnable, or an unrecognised code -> ALSO FATAL.

    Failing open was rejected for the same reason it was rejected for the disk
    guard: this guard's whole job is to prevent a silent, hard-to-attribute
    corruption, and degrading to unguarded on a missing dependency retires the
    protection exactly when the checkout is in a state nobody has verified.
    check-hardlink-isolation.py is a sibling file in this same repo.

    LOUD escape hatch: MC2_DEPLOY_ALLOW_SHARED_LINKS=1.
    """
    if os.environ.get("MC2_DEPLOY_ALLOW_SHARED_LINKS") == "1":
        log("WARNING: MC2_DEPLOY_ALLOW_SHARED_LINKS=1 -- hardlink-isolation "
            "preflight BYPASSED BY REQUEST. If this target shares inodes with "
            "another install, the deploy below rewrites that install's files "
            "in place and nothing afterwards will report it.")
        return

    guard = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "check-hardlink-isolation.py")
    if not os.path.isfile(guard):
        fail(f"hardlink-isolation guard not found at {guard} — refusing to "
             "deploy UNGUARDED. check-hardlink-isolation.py is a sibling of "
             "this script in the same repo; its absence means a broken or "
             "partial checkout. Restore it, or set "
             "MC2_DEPLOY_ALLOW_SHARED_LINKS=1 to bypass deliberately.")

    # Hand the guard the EXACT relpaths about to be written, via a temp file
    # (a 1500-entry argv is not portable).
    listfile = None
    try:
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False,
                                         encoding="utf-8", newline="\n") as fh:
            listfile = fh.name
            for _src, rel, _kind in items:
                fh.write(rel.replace(os.sep, "/") + "\n")
        log(f"link guard: checking {len(items)} payload relpath(s) in {target}")
        try:
            r = subprocess.run(
                [sys.executable, guard, "--target", target,
                 "--payload-list", listfile, "--quiet",
                 "--label", "deploy payload"],
                timeout=120, **console_child.child_kwargs())
            rc = r.returncode
        except Exception as e:  # noqa: BLE001
            fail(f"hardlink-isolation guard could not run ({e}) — refusing to "
                 "deploy UNGUARDED. Fix the guard invocation, or set "
                 "MC2_DEPLOY_ALLOW_SHARED_LINKS=1 to bypass deliberately.")
    finally:
        if listfile:
            try:
                os.unlink(listfile)
            except OSError:
                pass

    if rc == 3:
        fail("deploy target is NOT hardlink-isolated — NOTHING was written. "
             "See the [LINK_GUARD] lines above for the other dirs in this "
             "target's link group and the make-isolated-deploy.py remedy.")
    if rc == 2:
        fail("hardlink-isolation guard reported ERROR — refusing to deploy "
             "without a real verdict. See the [LINK_GUARD] lines above.")
    if rc == 1:
        log("link guard: read-only data is shared with another install "
            "(expected and desirable); written artifacts are private.")
    elif rc != 0:
        fail(f"hardlink-isolation guard returned unrecognised exit code {rc} "
             "— refusing to deploy on an unreadable verdict. Set "
             "MC2_DEPLOY_ALLOW_SHARED_LINKS=1 to bypass deliberately.")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def read_manifest(target_dir):
    """relpath -> (sha256, bytes, mtime_ns) from the target's CSV, or {}.

    Rows written before the mtime_ns column have no cache entry (mtime_ns
    None), so the first deploy after the upgrade copies and hashes as before.
    """
    path = os.path.join(target_dir, MANIFEST_NAME)
    if not os.path.isfile(path):
        return {}
    out = {}
    try:
        with open(path, newline="", encoding="utf-8") as f:
            rows = list(csv.reader(f))
    except (OSError, csv.Error):
        return {}
    if not rows or rows[0][:2] != ["manifest_version", MANIFEST_VERSION]:
        return {}
    for row in rows[2:]:
        if len(row) < 3 or not row[0]:
            continue
        try:
            size = int(row[2])
        except ValueError:
            continue
        mtime_ns = None
        if len(row) >= 6 and row[5].isdigit():
            mtime_ns = int(row[5])
        out[row[0]] = (row[1], size, mtime_ns)
    return out


def source_stamp(src):
    """(size, mtime_ns) of a source file, or None when it does not exist."""
    try:
        st = os.stat(src)
    except OSError:
        return None
    return (st.st_size, st.st_mtime_ns)


# TERRAIN-MATERIAL-TEXTURES-1-FIX (fix D): read a KTX2 file's levelCount from the
# header. The KTX2 v2 layout is a 12-byte identifier followed by a fixed header:
# vkFormat(u32) typeSize(u32) pixelWidth(u32) pixelHeight(u32) pixelDepth(u32)
# layerCount(u32) faceCount(u32) levelCount(u32) ... -- so levelCount is the 8th
# u32 after the identifier (byte offset 12 + 7*4 = 40). Returns the level count,
# or None if the file is not a recognizable KTX2 (never blocks a non-KTX2 asset).
def _ktx2_level_count(path):
    import struct
    KTX2_MAGIC = b"\xabKTX 20\xbb\r\n\x1a\n"
    try:
        with open(path, "rb") as f:
            head = f.read(44)
        if len(head) < 44 or head[:12] != KTX2_MAGIC:
            return None
        # 8 u32 fields at offset 12; levelCount is the 8th (index 7).
        fields = struct.unpack("<8I", head[12:44])
        return fields[7]
    except OSError:
        return None


def repo_root():
    """Source worktree root = parent of the scripts/ dir this file lives in."""
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def git_head(root):
    try:
        out = subprocess.run(
            ["git", "-C", root, "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=30,
            **console_child.child_kwargs(redirected=True),
        )
        if out.returncode == 0:
            return out.stdout.strip()
    except Exception:
        pass
    return "unknown"


def read_build_fingerprint(build_dir):
    """Parse generated/build_fingerprint.h next to the build dir.

    Returns {"sha": str, "dirty": bool} or None if the header is missing /
    unparsable. build_dir is usually <build64*>/RelWithDebInfo; the header
    lives at <build64*>/generated/build_fingerprint.h (also probed directly
    under build_dir for non-config layouts).

    WHY: the manifest's src_commit is git HEAD *now*, but the exe being
    deployed embeds the git state *at build time*. A dirty-tree build deploys
    an exe whose exact source state is unattributable -- the 2026-07-12 grey-
    regression P0 burned hours on exactly this (dirty pre-commit build,
    fingerprint said the prior sha). This reads the same header the exe
    compiled in, so the manifest can record what actually shipped.
    """
    for probe in (
        os.path.join(build_dir, "..", "generated", "build_fingerprint.h"),
        os.path.join(build_dir, "generated", "build_fingerprint.h"),
    ):
        pth = os.path.abspath(probe)
        if not os.path.isfile(pth):
            continue
        try:
            with open(pth, encoding="utf-8", errors="replace") as f:
                text = f.read()
            m_sha = re.search(r'#define MC2_BUILD_GIT_SHA\s+"([0-9a-fA-F]+)"', text)
            m_dirty = re.search(r"#define MC2_BUILD_GIT_DIRTY\s+(\d+)", text)
            if m_sha and m_dirty:
                return {"sha": m_sha.group(1), "dirty": m_dirty.group(1) != "0"}
        except OSError:
            pass
    return None


# ---------------------------------------------------------------------------
# Step 0: exe lock check (Mistake A). Windows: a running process holding the
# target exe makes overwrite silently fail / be refused. Probe by attempting
# an exclusive-open of the existing target binary. NEVER taskkill.
# ---------------------------------------------------------------------------

def is_file_locked_windows(path):
    if not os.path.exists(path):
        return False
    GENERIC_WRITE = 0x40000000
    OPEN_EXISTING = 3
    FILE_SHARE_NONE = 0
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    k32 = ctypes.windll.kernel32
    k32.CreateFileW.restype = ctypes.c_void_p  # default c_int truncates -1
    handle = k32.CreateFileW(path, GENERIC_WRITE, FILE_SHARE_NONE,
                             None, OPEN_EXISTING, 0, None)
    if handle is None or handle == INVALID_HANDLE_VALUE:
        return True  # ERROR_SHARING_VIOLATION et al -> locked
    k32.CloseHandle(ctypes.c_void_p(handle))
    return False


def _norm(p):
    """Normalize a path for case-insensitive Windows comparison."""
    return os.path.normcase(os.path.abspath(str(p)))


def _same_path(a, b):
    """True if a and b refer to the same file (os.path.samefile when possible)."""
    try:
        return os.path.samefile(a, b)
    except (OSError, ValueError):
        return _norm(a) == _norm(b)


def _proc_exe_paths_psutil(exe_name):
    """Yield (pid, exe_path_or_None) for every process whose name matches exe_name.

    exe_path is None when the path cannot be read (access denied, zombie, etc.).
    """
    name_lower = exe_name.lower()
    for proc in psutil.process_iter(["pid", "name"]):
        try:
            if proc.info["name"] and proc.info["name"].lower() == name_lower:
                try:
                    yield proc.pid, proc.exe()
                except (psutil.AccessDenied, psutil.ZombieProcess, OSError):
                    yield proc.pid, None
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass


def _proc_exe_paths_wmic(exe_name):
    """Fallback when psutil is unavailable: query WMIC for ExecutablePath.

    Yields (pid, exe_path_or_None).  pid is int when parseable, else str.
    """
    results = []
    try:
        out = subprocess.run(
            ["wmic", "process", "where",
             f"name='{exe_name}'", "get", "ProcessId,ExecutablePath", "/FORMAT:CSV"],
            capture_output=True, text=True, timeout=30,
            **console_child.child_kwargs(redirected=True),
        )
        for line in out.stdout.splitlines():
            line = line.strip()
            if not line or line.lower().startswith("node"):
                continue
            parts = line.split(",")
            # CSV columns: Node, ExecutablePath, ProcessId
            if len(parts) >= 3:
                exe_path = parts[1].strip() or None
                pid_str = parts[2].strip()
                try:
                    pid = int(pid_str)
                except ValueError:
                    pid = pid_str
                results.append((pid, exe_path))
    except Exception:
        pass
    return results


def _enumerate_matching_processes(exe_name):
    """Return list of (pid, exe_path_or_None) for all running processes named exe_name."""
    if _PSUTIL_OK:
        return list(_proc_exe_paths_psutil(exe_name))
    return _proc_exe_paths_wmic(exe_name)


def _check_running_processes(target_exe_resolved, exe_name):
    """Path-aware running-process check.

    - If a running process exe matches target_exe_resolved: HARD FAIL (returns
      a non-empty error string).
    - Processes from other folders: log info, allow.
    - Processes whose path cannot be read: log warning, allow (do NOT block
      unless the file-lock probe below catches them).

    Returns an error string if deploy must be blocked, or '' if clear.
    """
    procs = _enumerate_matching_processes(exe_name)
    if not procs:
        return ""

    blocking = []
    other = []
    unreadable = []

    for pid, proc_exe in procs:
        if proc_exe is None:
            unreadable.append(pid)
            continue
        proc_resolved = os.path.normcase(os.path.abspath(proc_exe))
        if _same_path(proc_exe, target_exe_resolved):
            blocking.append((pid, proc_exe))
        else:
            other.append((pid, proc_exe))

    for pid in unreadable:
        log(f"WARNING: running process PID {pid} ({exe_name}) path unreadable "
            "(access denied / zombie) -- not blocking deploy")
    for pid, path in other:
        log(f"INFO: running {exe_name} PID {pid} from OTHER folder "
            f"({path}) -- deploy continues")

    if blocking:
        lines = "\n".join(f"  PID {pid}: {path}" for pid, path in blocking)
        return (
            f"target exe is running from THIS deploy folder:\n{lines}\n"
            "  Close that instance and re-run. "
            "This script will NEVER taskkill automatically."
        )
    return ""


#: How long a deploy waits for a game running from its target folder to exit (another lane's smoke run, usually). MC2_DEPLOY_WAIT_S overrides.
DEFAULT_DEPLOY_WAIT_S = 30.0


def _deploy_lease_module():
    try:
        from smoke_lib import deploy_lease
    except ImportError:
        from scripts.smoke_lib import deploy_lease
    return deploy_lease


def take_folder_lease(target_dir, wait_s):
    """Hold the smoke folder lease while this script writes into the install.

    A smoke run holds the lease of the folder it plays from, and a deploy under it replaces the exe the run was started from; a lane that deploys into an
    install somebody else leased must wait for the holder, not overwrite it. Waits up to `wait_s` seconds with a visible countdown; a lease that is still
    live after that is fatal. Returns True when the lease is ours to release.
    """
    lease = _deploy_lease_module()
    try:
        lease.acquire_lease_waiting(target_dir, "deploy_payload", wait_s)
    except lease.LeaseError as exc:
        fail(f"{exc}\n  Another session leased this install (a smoke run, or a lane deploying); this script waited {wait_s:.0f}s and never overwrites a leased install.",
             code=2)
    log(f"folder lease taken: {target_dir}")
    return True


def release_folder_lease(target_dir):
    try:
        _deploy_lease_module().release_lease(target_dir)
        log(f"folder lease released: {target_dir}")
    except Exception as exc:  # noqa: BLE001 -- the deploy is done; a stale entry expires by TTL
        log(f"folder lease release failed ({exc}); it expires by TTL")


def check_locks(target_dir, exe_name, wait_s=0.0):
    target_exe = os.path.join(target_dir, exe_name)
    target_exe_resolved = os.path.normcase(os.path.abspath(target_exe))

    # Step 1: path-aware running-process check (psutil / wmic). A game running from this folder is waited for (bounded, visible countdown): it is
    # normally a smoke run that ends by itself; this script never ends it.
    err = _check_running_processes(target_exe_resolved, exe_name)
    if err and wait_s > 0:
        try:
            from smoke_lib import bounded_wait
        except ImportError:
            from scripts.smoke_lib import bounded_wait

        def still_running():
            lines = _check_running_processes(target_exe_resolved, exe_name).splitlines()
            return " ".join(s.strip() for s in lines[:2]) if lines else ""

        bounded_wait.wait_until(still_running, f"deploy into {target_dir}", wait_s)
        err = _check_running_processes(target_exe_resolved, exe_name)
    if err:
        fail(
            f"{err}\n"
            "  Copy-to-locked-exe silently fails on Windows (stale-exe trap, "
            "Mistake A).",
            code=2,
        )

    # Step 2: file-lock probe as a safety net (catches cases where psutil
    # could not enumerate the process but the file handle is still held).
    if sys.platform == "win32" and is_file_locked_windows(target_exe):
        fail(
            f"target exe is LOCKED by an unidentified process: {target_exe}\n"
            "  psutil/wmic found no matching process by name, but the file "
            "cannot be opened for writing. Close any process holding it and re-run. "
            "This script will NEVER taskkill automatically.",
            code=2,
        )

    log(f"lock check OK: {target_exe} not held by a running process from this deploy folder")


# ---------------------------------------------------------------------------
# Payload enumeration
# ---------------------------------------------------------------------------

def find_build_root(build_dir, explicit_root=None):
    """Locate the build64-family root (holds mc2.dir).

    Historically this walked up for an ancestor named literally 'build64', which
    fails for sibling build dirs like build64_island / build64_editor. Now:
      1. explicit_root (--build64-root) wins if given;
      2. else walk up accepting any ancestor whose basename STARTS WITH 'build64'
         (build64, build64_island, ...);
      3. else '' (non-standard layout; callers skip the launcher/freshness guard).
    """
    if explicit_root:
        return os.path.abspath(explicit_root).rstrip(os.sep)
    bd = os.path.abspath(build_dir).rstrip(os.sep)
    b64 = bd
    while b64 and not os.path.basename(b64).startswith("build64"):
        parent = os.path.dirname(b64)
        if parent == b64:
            return ""
        b64 = parent
    return b64


def is_single_config(build_dir, b64):
    """True for a Ninja-style tree: outputs land directly in the build root
    (no <config> subdir), because there is exactly one config per tree.

    WHY: the VS tree nests everything under a config name (RelWithDebInfo/,
    mc2.dir/RelWithDebInfo/); Ninja trees
    (build64-ninja) do not. Detecting the layout from the tree itself keeps
    both usable without forcing either into the other's directory shape.
    """
    if not b64:
        return False
    return os.path.isfile(os.path.join(b64, "build.ninja"))


def mc2_obj_dir(b64, cfg, single_config):
    """Where the mc2 target's .obj files live (the exe-freshness reference)."""
    if single_config:
        return os.path.join(b64, "CMakeFiles", "mc2.dir")
    return os.path.join(b64, "mc2.dir", cfg)


def check_exe_fresh(exe_src, build_dir, allow_stale_exe, build64_root=None):
    """Mistake E: stale exe deployed because the source exe was not relinked.

    The linked exe must be NEWER than every compiled .obj that feeds it. An incremental build
    that compiled objects but did not relink the exe -- or a deploy racing a still-running build --
    leaves the exe OLDER than its objects, so deploy faithfully copies a stale binary (this
    actually happened: a deploy shipped a pre-slice exe ~2.5 KB smaller than the rebuilt one).
    Compare exe mtime against the newest .obj under build64/mc2.dir/<config>/
    (VS) or build64-ninja/CMakeFiles/mc2.dir/ (Ninja).
    """
    if not os.path.isfile(exe_src):
        return
    exe_m = os.path.getmtime(exe_src)
    bd = os.path.abspath(build_dir).rstrip(os.sep)
    cfg = os.path.basename(bd)                      # e.g. RelWithDebInfo
    b64 = find_build_root(build_dir, build64_root)
    if not b64:
        return                                      # non-standard layout; skip the guard
    objdir = mc2_obj_dir(b64, cfg, is_single_config(build_dir, b64))
    newest_m, newest_p = 0.0, None
    for root, _dirs, files in os.walk(objdir):
        for f in files:
            if f.endswith((".obj", ".o")):
                m = os.path.getmtime(os.path.join(root, f))
                if m > newest_m:
                    newest_m, newest_p = m, os.path.join(root, f)
    if newest_p and newest_m > exe_m + 1.0:         # 1s slack for filesystem timestamp jitter
        msg = (f"STALE EXE (Mistake E): {exe_src}\n"
               f"  exe mtime {exe_m:.0f} is OLDER than object {newest_p}\n"
               f"  obj mtime {newest_m:.0f} -- the build did not relink (incremental skip or a\n"
               f"  deploy racing an in-flight build). Rebuild the exe before deploying.")
        if allow_stale_exe:
            log(f"WARNING (--allow-stale-exe): {msg}")
        else:
            fail(msg + "\n  (override: --allow-stale-exe)")


def enumerate_payload(src_root, build_dir, exe_name, pdb_name,
                      require_build=True, build64_root=None, code_only=False):
    """Returns list of (src_abspath, target_relpath, kind).

    require_build=False (manifest-only): the build outputs may be gone; we
    only need the relpath list to know which target files to re-hash, so a
    missing source exe/dll is tolerated (the entry is still emitted so the
    deployed copy can be recorded).
    """
    items = []
    native = exe_name == "mc2"
    exe_src = os.path.join(build_dir, exe_name)
    if require_build and not os.path.isfile(exe_src):
        fail(f"source exe not found: {exe_src}")
    items.append((exe_src, exe_name, "exe"))

    # Out-of-process minidump writer (sentry-native crashpad backend): mc2 copies it next to itself at link
    # time and gos_crashpad.cpp starts it from the exe's folder. Without it the in-process dump stays in charge.
    if exe_name.lower() != EDITOR_EXE:
        handler_name = "crashpad_handler" if native else "crashpad_handler.exe"
        handler_src = os.path.join(build_dir, handler_name)
        if require_build and not os.path.isfile(handler_src):
            fail(f"crashpad handler not found: {handler_src}")
        items.append((handler_src, handler_name, "exe"))

    pdb_src = os.path.join(build_dir, pdb_name)
    if not native:
        items.append((pdb_src, pdb_name, "pdb"))  # presence checked later

    for dll in (() if native else RUNTIME_DLLS):
        p = os.path.join(build_dir, dll)
        if os.path.isfile(p):
            items.append((p, dll, "dll"))
        elif require_build:
            fail(f"required runtime DLL missing from build: {p}")

    b64 = find_build_root(build_dir, build64_root)
    # Baked Vulkan SPIR-V (VULKAN-SHADER-TOOLCHAIN-1): glslangValidator/glslc
    # compiles shaders/vulkan/*.{vert,frag} to <build64-root>/shaders/vulkan/*.spv
    # (CMAKE_BINARY_DIR, i.e. the build64 root, NOT the per-config build_dir --
    # the CMake build root). The Vulkan island
    # loaders (vulkan_*_island.cpp) read
    # MC2_VULKAN_SPV_DIR if set, else default to "shaders/vulkan" relative to
    # the exe's CWD -- so deployed .spv must land at <target>/shaders/vulkan/.
    # Without this, any deployed install hits "cannot open spv" ->
    # vk_device_init_failed for every Vulkan region/island, even though the
    # exe + GL shaders deployed fine (found by VK-REGION-SSAO-1 content pass).
    # Vulkan is mandatory. A release/deploy without baked shaders cannot run.
    vk_spv_dir = os.path.join(b64, "shaders", "vulkan") if b64 else ""
    if vk_spv_dir and os.path.isdir(vk_spv_dir):
        for p in baked_spv_files(b64):
            items.append((str(p), f"shaders/vulkan/{p.name}", "shader"))
    if require_build and not any(rel.startswith("shaders/vulkan/") and rel.endswith(".spv")
                                 for _src, rel, _kind in items):
        fail(f"No baked Vulkan SPIR-V at {vk_spv_dir or '<no build root>'}; build shaders before deploying")

    # Deploy the ENTIRE shaders/ tree recursively (every file, all extensions),
    # not an extension allowlist. Shaders #include each other across stages and
    # extensions (.comp pulls .glsl/.hglsl; .frag pulls include/*.hglsl); a
    # per-ext allowlist silently dropped .comp + root .glsl and the GPU-driven
    # terrain cull/lighting/water compute then failed to compile -> NaN frustum
    # planes -> terrain culled to black, while smoke still PASSED (compile-fail
    # is non-fatal sticky-disable, not a crash). Only skip test fixtures.
    shader_dir = os.path.join(src_root, "shaders")
    if not os.path.isdir(shader_dir):
        fail(f"source shader dir not found: {shader_dir}")
    for dirpath, dirs, files in os.walk(shader_dir):
        dirs[:] = [d for d in dirs if d.lower() != "fixtures"]
        for fn in sorted(files):
            p = os.path.join(dirpath, fn)
            # GUARD (2026-07-02): refuse to ship shader sources containing git
            # conflict markers. A mid-merge deploy shipped a frag with <<<<<<<
            # markers -> GLSL compile fail -> shader_error smoke FAIL on the
            # user's viewing lane. Cheap scan; text shader sources only.
            if fn.lower().endswith((".frag", ".vert", ".comp", ".glsl", ".hglsl",
                                    ".tesc", ".tese", ".geom")):
                try:
                    with open(p, "rb") as fh:
                        blob = fh.read()
                    if b"<<<<<<<" in blob or b">>>>>>>" in blob:
                        fail(f"shader source contains git conflict markers: {p}")
                except OSError as e:
                    fail(f"cannot read shader source {p}: {e}")
            rel = os.path.relpath(p, src_root).replace(os.sep, "/")
            items.append((p, rel, "shader"))

    if code_only:
        return items

    # Lookup textures are embedded in the exe; deploy notices, not reference sources.
    for tree in ("3rdparty/smaa", "3rdparty/cmaa2"):
        if require_build and not os.path.isdir(os.path.join(src_root, tree)):
            fail(f"spatial AA payload tree missing in source: {tree}")
        for directory, _dirs, files in os.walk(os.path.join(src_root, tree)):
            for fn in sorted(files):
                if not fn.lower().startswith(("license", "notice", "upstream")):
                    continue
                path = os.path.join(directory, fn)
                rel = os.path.relpath(path, src_root).replace(os.sep, "/")
                items.append((path, rel, "support"))

    # Portable community importer and its recipes/shims; no Win32 launcher needed.
    for tree in ("tools/mc2_import", "tools/mc2x_import", "tools/mco_import"):
        for directory, dirs, files in os.walk(os.path.join(src_root, tree)):
            dirs[:] = [d for d in dirs if d != "__pycache__"]
            for fn in sorted(files):
                if fn.endswith((".pyc", ".pyo")):
                    continue
                path = os.path.join(directory, fn)
                items.append((path, os.path.relpath(path, src_root).replace(os.sep, "/"), "support"))

    items.append((os.path.join(src_root, "scripts", "console_child.py"), "scripts/console_child.py", "support"))
    for rel in IMPORTED_STRUCTURE_PAYLOAD:
        p = os.path.join(src_root, rel)
        if require_build and not os.path.isfile(p):
            fail(f"imported structure payload missing in source: {p}")
        items.append((p, rel, "support"))

    for rel in GAME_DATA_PAYLOAD:
        p = os.path.join(src_root, rel)
        if require_build and not os.path.isfile(p):
            fail(f"game data payload missing in source: {p}")
        items.append((p, rel, "support"))

    # Optional cooked logistics overrides; missing art retains the original PAK/atlas.
    for folder in ("data/art/mechicons", "data/art/missionmaps", "data/art/pilots"):
        for path in sorted((Path(src_root) / folder).rglob("*.png")):
            items.append((str(path), path.relative_to(src_root).as_posix(), "support"))

    # UI-DEFS-LOCKSTEP-HARDEN-1 (BLACK-MENU-BISECT-1): data/defs/** ships in
    # lockstep with the exe, exactly like shaders/. Since the ui-phase1 merge
    # the front-end menus ARE defs/ui pages; an exe running against a
    # version-skewed defs/ui package rendered an opaque black menu, silently.
    # Hard requirements:
    #   - data/defs/ must exist in source (else FAIL -- an exe without its
    #     defs is a broken deploy, not an optional extra)
    #   - data/defs/ui/PACKAGE.fit must exist (the runtime version key the
    #     exe checks at boot; a deploy without it fail-softs every menu to
    #     the legacy path on arrival)
    #   - post-copy byte diff hard-fails on mismatch (kind "defs" falls into
    #     the same hard-fail bucket as shaders in deploy())
    defs_dir = os.path.join(src_root, "data", "defs")
    if not os.path.isdir(defs_dir):
        fail(f"source data/defs dir not found: {defs_dir}\n"
             "  The front-end UI is defs/ui pages; an exe deployed without "
             "matching data/defs shows legacy-fallback menus at best.")
    pkg_fit = os.path.join(defs_dir, "ui", "PACKAGE.fit")
    if not os.path.isfile(pkg_fit):
        fail(f"data/defs/ui/PACKAGE.fit missing in source: {pkg_fit}\n"
             "  The exe checks this version key at boot (UI-DEFS-LOCKSTEP-"
             "HARDEN-1); deploying without it makes every defs/ui page load "
             "fail-soft to legacy on the target.")
    for dirpath, _dirs, files in os.walk(defs_dir):
        for fn in sorted(files):
            sp = os.path.join(dirpath, fn)
            rel = os.path.relpath(sp, src_root).replace(os.sep, "/")
            items.append((sp, rel, "defs"))

    # TERRAIN-MATERIAL-TEXTURES-1: optional loose cooked sidecars -- skip (log)
    # when absent instead of failing; the engine fail-softs without them.
    # TERRAIN-MATERIAL-TEXTURES-1-FIX (fix D, MIPS ROOT CAUSE): the "levels=1
    # deployed" bug was a stale pre-mip cook getting picked up because deploy just
    # copies whatever _albedo.ktx2 is on disk. Guard it here: read the KTX2
    # levelCount and HARD-FAIL if a present layer is mip-incomplete, so a stale
    # cook can never reach the payload silently. Re-cook, then re-deploy.
    ensure_concrete_detail(src_root)
    ensure_cliff_rock(src_root)
    for rel in TERRAIN_LAYER_PAYLOAD:
        p = os.path.join(src_root, rel)
        if not os.path.isfile(p):
            log(f"terrain layer payload absent (skipped, engine fail-softs): {rel}")
            continue
        lvls = _ktx2_level_count(p)
        if lvls is not None and lvls <= 1:
            fail(f"terrain layer payload is mip-incomplete (levelCount={lvls}, expected >1): "
                 f"{rel} -- re-cook with tools/mc2texcook/cook_terrain_layers.py (mips=True) "
                 f"BEFORE deploying, or the engine ships a shimmering single-level albedo array.")
        items.append((p, rel, "support"))

    # Support payload: launch scripts (per target) + editor authoring trees.
    # These are source-tracked, not build outputs, so they ship even though
    # they never appear in build_dir. require_build is irrelevant here: a
    # missing SOURCE support file is a real error (loud copy failure), while
    # manifest-only tolerates a missing TARGET copy downstream as usual.
    is_editor = exe_name.lower() == EDITOR_EXE
    for script in (() if native else SUPPORT_SCRIPTS["editor" if is_editor else "game"]):
        sp = os.path.join(src_root, script)
        items.append((sp, script, "support"))
    if is_editor:
        for tree in EDITOR_SUPPORT_TREES:
            tree_abs = os.path.join(src_root, tree)
            if require_build and not os.path.isdir(tree_abs):
                fail(f"editor support tree not found in source: {tree_abs}\n"
                     "  The editor's Generate-Mission path needs this tree; "
                     "a deploy without it builds a non-functional editor.")
            for dirpath, _dirs, files in os.walk(tree_abs):
                for fn in sorted(files):
                    sp = os.path.join(dirpath, fn)
                    rel = os.path.relpath(sp, src_root).replace(os.sep, "/")
                    items.append((sp, rel, "support"))
    # Example mods (EXAMPLE-MODS-1) -> <target>/mods/<id>/ (launcher add-on
    # checkboxes). Ships with both game and editor installs.
    items.extend(example_mods_payload(src_root))

    # Generated unit/equipment definitions -> <target>/data/defs/{units,equipment}/
    # so MC2_*_DEFINITION_SOURCE=generated works from a normal install. Converter
    # runs are skipped on manifest-only passes (require_build=False): no fresh
    # generation side effects, existing target copies keep their hashes.
    if require_build:
        items.extend(generate_defs_payload(src_root, b64))

    # Cook tools ship with both game and editor installs (modder/user asset pipeline).
    for entry in GAME_COOK_TOOLS:
        entry_abs = os.path.join(src_root, entry)
        if os.path.isfile(entry_abs):
            items.append((entry_abs, entry.replace(os.sep, "/"), "support"))
        elif os.path.isdir(entry_abs):
            for dirpath, _dirs, files in os.walk(entry_abs):
                for fn in sorted(files):
                    sp = os.path.join(dirpath, fn)
                    rel = os.path.relpath(sp, src_root).replace(os.sep, "/")
                    items.append((sp, rel, "support"))
    return items


# ---------------------------------------------------------------------------
# Deploy + verify
# ---------------------------------------------------------------------------


# The detail-normal layers terrain_tier1.frag samples get lossless RGBA8 mip-complete sidecars next to the deployed TGAs, cooked in the
# target from the verified TGAs (generated, about 22 MB each, never in git). Soft: without them the engine imports the TGAs.
ENGINE_NORMAL_COOK = Path(__file__).resolve().parents[1] / "tools" / "mc2texcook" / "cook_terrain_layers.py"
# Bump when the cooked texels change meaning; the stamp in the target forces one recook. 2 = RGBA normal order (1 was TGA B, G, R). 3 = lossless RGBA8 + box mips (2 was BC7).
ENGINE_NORMAL_COOK_VERSION = "3"


def ensure_engine_normal_cooks(target_dir, cooker=subprocess.run):
    tex = os.path.join(target_dir, "data", "textures")
    stamp = os.path.join(tex, "engine_normals.cookver")
    try:
        with open(stamp, encoding="utf-8") as f:
            current = f.read().strip() == ENGINE_NORMAL_COOK_VERSION
    except OSError:
        current = False
    stale = [i for i in range(6) if not current or not os.path.isfile(os.path.join(tex, f"mat{i}_normal.ktx2")) or
             os.path.getmtime(os.path.join(tex, f"mat{i}_normal.ktx2")) < os.path.getmtime(os.path.join(tex, f"mat{i}_normal.tga"))]
    if not stale:
        return
    log(f"detail-normal sidecars absent or stale for layers {stale}: cooking them (a few seconds)")
    try:
        r = cooker([sys.executable, str(ENGINE_NORMAL_COOK), "--engine-normals", tex] + ([] if current else ["--force"]),
                   capture_output=True, text=True, timeout=900, env=dict(os.environ, OMP_NUM_THREADS="1"), **console_child.child_kwargs(redirected=True))
    except subprocess.TimeoutExpired:
        log("[DEPLOY] WARN: detail-normal cook timed out after 900 s (skipped; the engine imports the TGAs)")
        return
    if r.returncode:
        log("[DEPLOY] WARN: detail-normal cook failed (skipped; the engine imports the TGAs): " + (r.stdout + r.stderr).strip()[-300:])
        return
    with open(stamp, "w", encoding="utf-8") as f:
        f.write(ENGINE_NORMAL_COOK_VERSION + "\n")


# MC2_GOLDEN_MATERIAL_ROOT: a copy of the golden install's 14 material files (the Linux box has no A: drive); hashes are still checked.
GOLDEN_MATERIAL_ROOT = Path(os.environ.get("MC2_GOLDEN_MATERIAL_ROOT") or "A:/Games/mc2-opengl/mc2-win64-v0.4c")
MATERIAL_MANIFEST = Path(__file__).with_name("terrain_materials.json")


def ensure_terrain_materials(target_dir):
    """Verify required modern normals and displacement maps; read golden assets without modifying them.

    Replace destination entries atomically so shared data hardlinks stay intact.
    Correct files are never rewritten. Refuse unavailable or changed golden bytes.
    """
    hashes = {}
    try:
        manifest = json.loads(MATERIAL_MANIFEST.read_text(encoding="utf-8"))
        expected_paths = ({f"data/textures/mat{i}_normal.tga" for i in range(9)} |
                          {f"data/textures/mat{i}_displacement.tga" for i in range(4)} |
                          {"data/textures/detail_displacement.tga"})
        if set(manifest) != expected_paths or any(
                not re.fullmatch(r"[0-9a-f]{40}", h) for h in manifest.values()):
            raise ValueError("invalid terrain material manifest")
        for rel, expected in manifest.items():
            dst = Path(target_dir) / rel
            if not dst.is_file() or hashlib.sha1(dst.read_bytes()).hexdigest() != expected:
                src = GOLDEN_MATERIAL_ROOT / rel
                content = src.read_bytes()
                if hashlib.sha1(content).hexdigest() != expected:
                    raise ValueError(f"golden terrain material hash mismatch: {src}")
                dst.parent.mkdir(parents=True, exist_ok=True)
                temporary = None
                try:
                    with tempfile.NamedTemporaryFile(dir=dst.parent, delete=False) as out:
                        temporary = Path(out.name)
                        out.write(content)
                    os.replace(temporary, dst)
                finally:
                    if temporary is not None and temporary.exists():
                        temporary.unlink()
                log(f"restored required terrain material: {rel}")
            content = dst.read_bytes()
            if hashlib.sha1(content).hexdigest() != expected:
                raise ValueError(f"deployed terrain material hash mismatch: {dst}")
            hashes[rel] = (hashlib.sha256(content).hexdigest(), len(content))
        log("terrain materials verified: 14/14")
    except (OSError, ValueError, TypeError) as exc:
        fail(f"required terrain materials unavailable: {exc}")
    return hashes


def deploy(items, target_dir, allow_stale_pdb, preserve=(), cache=None,
           full_verify=False, code_only=False, verified=None):
    """Copy + verify the payload.

    Hash receipts retain source and destination metadata plus the hash verified
    at write time. Metadata drift rehashes bytes, including same-size corruption.
    Executables are always hashed on both sides. --full-verify copies everything.
    """
    # Retention must reach retire_unowned(), before it can delete static files.
    # Shader inventories still belong entirely to the current build.
    src_hashes = {rel: row for rel, row in (cache or {}).items()
                  if not rel.startswith(("shaders/", "shaders_vk/"))} if code_only else ensure_terrain_materials(target_dir)
    if not code_only:
        ensure_engine_normal_cooks(target_dir)
    shader_mismatches = []
    pdb_problem = None
    preserve = {p.replace(os.sep, "/") for p in preserve}
    cache = cache or {}
    from kit_payload import HashReceipts, contained_path
    receipts = HashReceipts(Path(target_dir) / ".deploy-receipts.json")
    kit_root = Path(target_dir).resolve()
    kit_parents = {}
    skipped = 0
    pdb_skipped = False
    copy_seconds = 0.0
    verify_started = time.perf_counter()

    for src, rel, kind in items:
        dst = os.path.join(target_dir, rel)
        if verified and rel in verified:
            # Lane seed: source hash and the verified seed file identity plus
            # NTFS ChangeTime match. No write, no second read of unchanged bytes.
            stamp = source_stamp(src)
            src_hashes[rel] = (verified[rel], os.path.getsize(dst), stamp[1] if stamp else None)
            skipped += 1
            continue
        if kind == "kit":
            dst = str(contained_path(kit_root, rel, parents=kit_parents))
        if kind == "pdb" and not os.path.isfile(src):
            pdb_problem = f"source PDB missing: {src}"
            continue
        if rel.replace(os.sep, "/") in preserve and os.path.isfile(dst):
            if kind == "exe" and sha256_file(src) != sha256_file(dst):
                fail(f"REFUSING preserved executable differs from source: {dst}")
            # user-owned file at target (e.g. hand-edited run-with-log.bat):
            # keep the deployed copy, record its hash so the manifest matches
            # what's actually on disk.
            src_hashes[rel] = (sha256_file(dst), os.path.getsize(dst))
            log(f"preserved {rel} (existing target copy kept)")
            continue
        def source_hash(path):
            if rel.endswith(".mcasset.json"):
                return hashlib.sha256(Path(path).read_bytes().replace(b"\r\n", b"\n")).hexdigest()
            return sha256_file(path)

        s_hash = receipts.digest(src, force=full_verify or kind == "exe", hasher=source_hash,
                                 variant='lf' if rel.endswith('.mcasset.json') else '')
        if not full_verify and os.path.isfile(dst):
            d_hash = receipts.digest(dst, force=kind == "exe", hasher=sha256_file)
            if s_hash == d_hash:
                stamp = source_stamp(src)
                src_hashes[rel] = (d_hash, os.path.getsize(dst), stamp[1])
                skipped += 1
                pdb_skipped = pdb_skipped or kind == "pdb"
                continue
        os.makedirs(os.path.dirname(dst) or target_dir, exist_ok=True)
        try:
            copy_started = time.perf_counter()
            if rel.endswith(".mcasset.json"):
                # Authored sidecars ship unchanged except for checkout line endings;
                # mixed CRLF/LF copies must not change the pinned data fingerprint.
                with open(src, "rb") as fh:
                    content = fh.read().replace(b"\r\n", b"\n")
                with open(dst, "wb") as fh:
                    fh.write(content)
                copy_seconds += time.perf_counter() - copy_started
            else:
                shutil.copyfile(src, dst)
                if kind == "exe" and os.name != "nt":
                    shutil.copymode(src, dst)
                copy_seconds += time.perf_counter() - copy_started
        except PermissionError:
            fail(f"copy refused (file locked?): {dst}\n"
                 "  A process is holding this file. Close it and re-run; "
                 "this script never taskkills.", 2)
        d_hash = receipts.digest(dst, force=True, hasher=sha256_file)
        stamp = source_stamp(src)
        src_hashes[rel] = (d_hash, os.path.getsize(dst),
                           stamp[1] if stamp is not None else None)
        if s_hash != d_hash:
            if kind == "exe":
                fail(f"post-copy exe hash MISMATCH: {rel}\n"
                     f"  source   {s_hash}\n  deployed {d_hash}\n"
                     "  Deployed exe is STALE (locked file / copy failure).", 3)
            elif kind == "shader":
                shader_mismatches.append(rel)
            elif kind == "pdb":
                pdb_problem = f"deployed PDB hash != source ({rel})"
            else:
                shader_mismatches.append(rel)  # DLLs hard-fail too
        log(f"deployed {rel} ({'OK' if s_hash == d_hash else 'MISMATCH'})")

    log(f"phase hash/verify seconds={time.perf_counter() - verify_started - copy_seconds:.3f}")
    log(f"phase copy seconds={copy_seconds:.3f}")
    if skipped:
        log(f"skipped {skipped} unchanged file(s) "
            f"(verified hash receipts match; --full-verify to re-copy)")

    if shader_mismatches:
        fail("post-copy diff MISMATCH (stale files in target):\n  "
             + "\n  ".join(shader_mismatches), 4)

    if pdb_problem:
        if allow_stale_pdb:
            log(f"WARNING (--allow-stale-pdb): {pdb_problem}")
        else:
            fail(f"{pdb_problem}\n  Stale PDB = wrong Tracy symbols "
                 "(Mistake D). Use --allow-stale-pdb to override.", 5)
    elif pdb_skipped:
        log("PDB check OK: unchanged (size+mtime_ns match the manifest)")
    else:
        log("PDB check OK: deployed PDB matches source hash")

    retire_unowned(target_dir, src_hashes, preserve,
                   own_shaders=any(kind == "shader" and rel.startswith("shaders/vulkan/") and rel.endswith(".spv")
                                   for _src, rel, kind in items))
    receipts.save()
    return src_hashes


def retire_unowned(target_dir, keep, preserve=(), own_shaders=False):
    """Retire files the new payload (`keep`, its relpaths) no longer carries.

    Manifest rows of the previous deploy are retired only when unchanged. Paths are resolved before unlinking so a malformed
    manifest or a directory junction cannot escape the target. With `own_shaders` (the payload carries the baked SPIR-V) the
    build's output list owns shaders/: any file there the payload does not carry is retired, manifest row or not (W129:
    merge_gate.stage_published copies v0.4's shaders/ and _forget_swapped_rows drops their rows, so dead .spv slipped past the
    manifest loop and verify_published_identity failed the shader inventory)."""
    root = Path(target_dir).resolve()
    for rel, (digest, _size, _mtime) in read_manifest(target_dir).items():
        if rel in keep or rel in preserve:
            continue
        retired = root / rel
        resolved = retired.resolve()
        if not resolved.is_relative_to(root) or resolved == root:
            fail(f"retired payload path escapes target: {rel}")
        parts = tuple(part.casefold() for part in Path(rel).parts)
        if parts[0] in {"mods", "savegame"} or parts[:2] == ("data", "savegame"):
            continue
        if retired.is_file():
            if sha256_file(retired) != digest:
                log(f"preserved modified retired payload: {rel}")
                continue
            retired.unlink()
            log(f"pruned retired payload: {rel}")
    # W130 review P1: the sweep owns only the baked SPIR-V (shaders/vulkan/*.spv, the W129 defect), never owner or mod files
    # elsewhere under shaders/, and it resolves like the manifest loop so a junction cannot take it outside the target.
    spv_root = root / "shaders" / "vulkan"
    if own_shaders and spv_root.is_dir():
        for path in sorted(spv_root.glob("*.spv")):
            rel = path.relative_to(root).as_posix()
            resolved = path.resolve()
            if not resolved.is_relative_to(spv_root.resolve()):
                fail(f"retired shader path escapes target: {rel}")
            if path.is_file() and rel not in keep and rel not in preserve:
                path.unlink()
                log(f"pruned retired payload: {rel}")


def write_manifest(target_dir, hashes, src_commit):
    ts = datetime.datetime.now().isoformat(timespec="seconds")
    path = os.path.join(target_dir, MANIFEST_NAME)
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["manifest_version", MANIFEST_VERSION, "", "", "", ""])
        w.writerow(["relpath", "sha256", "bytes", "src_commit", "timestamp",
                    "mtime_ns"])
        for rel in sorted(hashes):
            entry = tuple(hashes[rel])
            h, size = entry[0], entry[1]
            mtime_ns = entry[2] if len(entry) > 2 else None
            w.writerow([rel, h, size, src_commit, ts,
                        "" if mtime_ns is None else mtime_ns])
    log(f"manifest written: {path} ({len(hashes)} rows, {MANIFEST_VERSION}, "
        f"src_commit {src_commit[:12]})")


def hash_present_payload(items, target_dir):
    """Re-hash the payload files that are ALREADY in target_dir (no copy).

    Used by --write-manifest-only: another deploy path (deploy-editor.sh,
    the manual recipe, etc.) already copied the binaries in; we just record
    them in the SAME .deployed_manifest.csv that --verify-only checks, so
    the CSV does not go stale behind the JSON manifest.
    Files missing from the target are skipped (not every payload item is
    present in every target -- e.g. an editor-only deploy has no mc2.exe).
    """
    hashes = {}
    present, absent = 0, 0
    for src, rel, kind in items:
        dst = os.path.join(target_dir, rel)
        if os.path.isfile(dst):
            # No mtime_ns: this path cannot prove the deployed bytes came from
            # the current source (another deploy path copied them), so the row
            # must never authorize a future skip. A real deploy fills it in.
            hashes[rel] = (sha256_file(dst), os.path.getsize(dst), None)
            present += 1
        else:
            absent += 1
    log(f"manifest-only: {present} payload files present in target, "
        f"{absent} absent (skipped)")
    return hashes


def write_manifest_only(src_root, build_dir, exe_name, pdb_name, target_dir,
                        build64_root=None):
    """Refresh <target>/.deployed_manifest.csv from files ALREADY in target.

    No copy, no lock check (we are not writing binaries -- only the CSV).
    Merges with an existing CSV so a partial deploy (editor-only into a
    shared install) does not drop the game's rows, mirroring the JSON
    manifest's --merge behaviour.
    """
    if not os.path.isdir(target_dir):
        fail(f"target dir does not exist: {target_dir}")
    items = enumerate_payload(src_root, build_dir, exe_name, pdb_name,
                              require_build=False, build64_root=build64_root)
    hashes = hash_present_payload(items, target_dir)

    # Merge: keep prior rows for payload files we did not just re-hash but
    # that still exist on disk (e.g. game exe rows when refreshing after an
    # editor-only copy).
    existing = os.path.join(target_dir, MANIFEST_NAME)
    if os.path.isfile(existing):
        try:
            with open(existing, newline="") as f:
                rows = list(csv.reader(f))
            if rows and rows[0][:2] == ["manifest_version", MANIFEST_VERSION]:
                for row in rows[2:]:
                    if len(row) < 3:
                        continue
                    rel, h, size = row[0], row[1], row[2]
                    if rel in hashes:
                        continue  # freshly re-hashed wins
                    p = os.path.join(target_dir, rel)
                    if os.path.isfile(p):
                        hashes[rel] = (h, size, None)
        except Exception as e:
            log(f"WARNING: could not merge existing CSV manifest ({e}); replacing")

    if not hashes:
        fail("no payload files present in target -- nothing to record")

    # LINK-GUARD-1: this path writes no binaries, but it DOES write
    # .deployed_manifest.csv -- and in the 2026-07-30 incident that file was
    # itself hardlinked four ways (nlink=4). write_manifest() below opens it
    # "w", which truncates the shared inode, so a manifest refresh here would
    # silently overwrite three other installs' manifests with this tree's
    # hashes: every one of them would then read as coherent while describing
    # somebody else's payload. Guarding only the copy path would have left the
    # cheapest way to forge a clean manifest wide open.
    check_hardlink_isolation([(None, MANIFEST_NAME, "manifest")], target_dir)

    write_manifest(target_dir, hashes, git_head(src_root))
    log("manifest-only COMPLETE -- .deployed_manifest.csv refreshed (no copy)")


def staleness_report(target_dir):
    """Pure: compare a deployed tree against its .deployed_manifest.csv by
    re-hashing every listed file (single-source sha256_file). Returns a dict --
    NO printing, NO sys.exit -- so callers other than the CLI (run_smoke) can
    surface drift without reimplementing the hashing/compare:

      {has_manifest, version_ok, ok (int), stale [rel...], missing [rel...],
       src_commit, manifest_path}
    """
    path = os.path.join(target_dir, MANIFEST_NAME)
    rep = {"manifest_path": path, "has_manifest": False, "version_ok": True,
           "ok": 0, "stale": [], "missing": [], "src_commit": None}
    if not os.path.isfile(path):
        return rep
    rep["has_manifest"] = True
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    if not rows or rows[0][:2] != ["manifest_version", MANIFEST_VERSION]:
        rep["version_ok"] = False
        return rep
    for row in rows[2:]:
        if len(row) < 3:
            continue
        rel, expect_hash = row[0], row[1]
        if rep["src_commit"] is None and len(row) >= 4:
            rep["src_commit"] = row[3]
        p = os.path.join(target_dir, rel)
        if not os.path.isfile(p):
            rep["missing"].append(rel)
        elif sha256_file(p) != expect_hash:
            rep["stale"].append(rel)
        else:
            rep["ok"] += 1
    return rep


REQUIRED_DIRS = ("assets", "data", "shaders")


def _pe_imports(path):
    """Imported + delay-loaded DLL names (lowercase) of a PE file. Raises RuntimeError when pefile is missing (py -3 -m pip
    install pefile) or the file is not a readable PE: an empty answer would let the completeness check pass having parsed nothing."""
    try:
        import pefile
    except ImportError as e:
        raise RuntimeError("the install completeness check needs pefile (py -3 -m pip install pefile)") from e
    try:
        pe = pefile.PE(path, fast_load=True)
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
                                               pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT"]])
        names = {e.dll.decode().lower() for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", [])}
        names |= {e.dll.decode().lower() for e in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", [])}
        pe.close()
        return names
    except Exception as e:  # noqa: BLE001
        raise RuntimeError(f"cannot read PE imports of {path}: {e}") from e


def check_install_complete(target, exe_name, build_dir, fix=True):
    """Every non-system DLL the exe (transitively) imports must sit next to it, and assets/ data/ shaders/ must exist and be non-empty.
    Missing DLLs are copied from build_dir (fix=True); returns a sorted list of what is still missing (DLL names and 'dir/')."""
    if exe_name == "mc2":
        exe = os.path.join(target, exe_name)
        if not os.path.isfile(exe):
            return [f"{exe_name} (not found in target)"]
        result = subprocess.run(["ldd", exe], capture_output=True, text=True,
                                **console_child.child_kwargs(redirected=True))
        if result.returncode:
            return [f"cannot read ELF dependencies: {result.stderr.strip() or result.stdout.strip()}"]
        missing = [line.split()[0] for line in result.stdout.splitlines() if "=> not found" in line]
        for name in REQUIRED_DIRS:
            directory = os.path.join(target, name)
            if not (os.path.isdir(directory) and any(os.scandir(directory))):
                missing.append(name + "/")
        return sorted(missing)
    sysdirs = [os.path.join(os.environ.get("SystemRoot", "C:/Windows"), d) for d in ("System32", "SysWOW64")]
    runtimes = {d.lower() for d in RUNTIME_DLLS}

    def is_system(n):
        return n not in runtimes and (n.startswith(("api-ms-", "ext-ms-")) or any(os.path.exists(os.path.join(d, n)) for d in sysdirs))

    exe = next((c for c in (os.path.join(target, exe_name), os.path.join(build_dir, exe_name)) if os.path.isfile(c)), None)
    if exe is None:
        return [f"{exe_name} (not found in target or build dir)"]
    try:
        todo = (_pe_imports(exe) if exe else set()) | runtimes
        for name in ("mc2_asset_viewer.exe", "ui_editor.exe", "mc2fx_preview.exe", "mc2weapon_gui.exe"):
            tool = os.path.join(target, name)
            if os.path.isfile(tool):
                todo |= _pe_imports(tool)
    except RuntimeError as e:
        return [str(e)]
    if fix:
        seed_build_dlls(build_dir, repo_root())
    seen, missing = set(), []
    while todo:
        n = todo.pop()
        if n in seen or is_system(n):
            continue
        seen.add(n)
        dst, src = os.path.join(target, n), os.path.join(build_dir, n)
        if not os.path.isfile(dst):
            if fix and os.path.isfile(src):
                shutil.copy2(src, dst)
                log(f"completeness: copied missing {n} from build dir")
            else:
                missing.append(n)
                continue
        try:
            todo |= _pe_imports(dst)
        except RuntimeError as e:
            missing.append(str(e))
    for d in REQUIRED_DIRS:
        dpath = os.path.join(target, d)
        if not (os.path.isdir(dpath) and any(os.scandir(dpath))):
            missing.append(d + "/")
    return sorted(missing)


def verify_only(target_dir, strict):
    rep = staleness_report(target_dir)
    if not rep["has_manifest"]:
        msg = (f"no manifest at {rep['manifest_path']} -- target never deployed "
               "via this tool")
        if strict:
            fail(msg)
        log(f"ADVISORY: {msg}")
        return 0
    if not rep["version_ok"]:
        fail(f"unrecognized manifest version in {rep['manifest_path']}")
    stale, missing, ok = rep["stale"], rep["missing"], rep["ok"]
    log(f"verify: {ok} match, {len(stale)} stale, {len(missing)} missing")
    for r in stale:
        log(f"  STALE:   {r}")
    for r in missing:
        log(f"  MISSING: {r}")
    if (stale or missing):
        if strict:
            fail("target drifted from manifest (--strict)", 6)
        log("ADVISORY: target drifted from manifest (exit 0; use --strict to gate)")
    else:
        log("target matches manifest")
    return 0


# Canned target presets (--target). Presets only SET DEFAULTS; explicit
# positional target / --build-dir / --exe-name always override.
#   game   -> live game install   A:/Games/mc2-opengl/mc2-win64-v0.4, mc2.exe,
#             build64/RelWithDebInfo
#   editor -> live editor install A:/Games/mc2-opengl/mc2-win64-v0.4 (dev install; 2026-10-05 reset),
#             "Mission Editor.exe", build64/out/editor/RelWithDebInfo (EditRel)
TARGET_PRESETS = {
    "game": {
        "target_dir": "A:/Games/mc2-opengl/mc2-win64-v0.4",
        "exe_name": "mc2.exe" if sys.platform == "win32" else "mc2",
        "build_subdir": os.path.join("build64", "RelWithDebInfo"),
        "ninja_subdir": "build64-ninja",
    },
    "editor": {
        "target_dir": "A:/Games/mc2-opengl/mc2-win64-v0.4",
        "exe_name": "Mission Editor.exe",
        "build_subdir": os.path.join("build64", "out", "editor", "RelWithDebInfo"),
        "ninja_subdir": os.path.join("build64-ninja", "out", "editor"),
    },
}

# DEPLOY-TARGET-GUARD: deploy ONLY to one of the canonical deploy folders. Each full
# install is ~5GB; spinning up a per-lane/per-experiment deploy folder (e.g.
# mc2-<lane>-test) bloats A:/Games/mc2-opengl by tens of GB. Deploy is REFUSED to any
# target not in this set unless --allow-new-target is passed (deliberate new install /
# release cut). The folders below are the shared dev deploy targets -- pick a free one
# (md5-verify after; a sibling may hold the lease).
DEPLOY_ALLOWLIST = {
    "mc2-win64-v0.4",
    "mc2-win64-lane2",
    "mc2-win64-lane3",
    "mc2-win64-v0.4c",
    "mc2-win64-0.4c",
}


def implicit_target_refusal(src_root, explicit_target):
    """A lane worktree (A:/Games/mc2-<lane>, not mc2-main) must name its own install; None when the deploy may proceed."""
    if explicit_target:
        return None
    root = (ntpath.normpath(src_root) if ntpath.splitdrive(src_root)[0]
            else os.path.normpath(os.path.abspath(src_root))).replace("\\", "/")
    parent, name = os.path.split(root)
    if parent.lower() == "a:/games" and name.lower().startswith("mc2-") and name.lower() != "mc2-main":
        return (f"no deploy target named, and {name} is a lane worktree: the implicit default install is mc2-main's. "
                f"Name your own install, e.g. E:/mc2-installs/<lane> with --allow-new-target.")
    return None


BOOTSTRAP_SKIP = {"cache", "crashes", "shaders", "debug_state", "_pngcap", "_rdoc", MANIFEST_NAME, RETIRED_LAUNCHER}


def bootstrap_install(src, target, exe_name, pdb_name):
    """One-time creation of a NEW lane install: copy the base files (data, assets, fst archives, DLLs, cfg) from an existing
    install; the deploy that follows adds the exe, launcher, pdb, shaders and manifest. Real copies (never hardlinks: LINK-GUARD-1)."""
    src, target = os.path.abspath(src), os.path.abspath(target)
    if not (os.path.isdir(os.path.join(src, "data")) and os.path.isdir(os.path.join(src, "assets"))):
        fail(f"--bootstrap-from {src}: not an install (needs data/ and assets/)")
    if _same_path(src, target):
        fail("--bootstrap-from names the target itself")
    if os.path.isdir(target) and any(os.scandir(target)):
        log(f"bootstrap: {target} already has files; not copying from {src}")
        return
    skip = BOOTSTRAP_SKIP | {exe_name, pdb_name}
    names = [n for n in sorted(os.listdir(src)) if n not in skip and not n.lower().endswith((".log", ".pdb"))]
    if exe_name == "mc2":
        names = [n for n in names if n != ".mc2_deploy_lane.json"
                 and not n.lower().endswith((".exe", ".dll", ".bat"))]
    need = sum(os.path.getsize(p) for n in names for p in
               ([os.path.join(src, n)] if os.path.isfile(os.path.join(src, n)) else
                (os.path.join(d, f) for d, _, fs in os.walk(os.path.join(src, n)) for f in fs)))
    rc, _ = disk_space(os.path.dirname(target) or target, need, label="bootstrap")
    if rc == 3:
        fail(f"bootstrap: not enough free space for {need / 2**30:.1f} GiB", 3)
    log(f"bootstrap: copying {len(names)} entries ({need / 2**30:.1f} GiB) {src} -> {target}")
    partial = target + ".partial"
    if os.path.isdir(partial):
        shutil.rmtree(partial)
    os.makedirs(partial)
    for n in names:
        a, b = os.path.join(src, n), os.path.join(partial, n)
        if os.path.isdir(a):
            shutil.copytree(a, b, ignore=shutil.ignore_patterns("*.log"))
        else:
            shutil.copy2(a, b)
    if os.path.isdir(target):
        os.rmdir(target)  # empty (checked above)
    os.rename(partial, target)


def published_shader_sources(source_root, build):
    """The shaders/ inventory a published install must hold exactly: every source file except fixtures plus the build's baked SPIR-V."""
    source_root, build = Path(source_root), Path(build)
    expected = {p.relative_to(source_root).as_posix(): p
                for p in (source_root / 'shaders').rglob('*')
                if p.is_file() and 'fixtures' not in p.relative_to(source_root / 'shaders').parts}
    baked = baked_spv_files(build)
    if not baked:
        raise ValueError(f'{build}: missing baked Vulkan shaders')
    expected.update({'shaders/vulkan/' + p.name: p for p in baked})
    return expected


def shader_inventory_problems(source_root, published):
    """Dry run of the deploy row's shader half on a copy of the published install's shaders/ (about 2 MB): what the deploy
    ships, minus what it retires, must equal what verify_published_identity expects. Laptop-gate fast red (W129 run 2 found
    this mismatch only after contracts, harnesses and capture-before). Returns problem lines."""
    source_root, build = Path(source_root), Path(source_root) / 'build64-ninja'
    try:
        expected = published_shader_sources(source_root, build)
        items = enumerate_payload(str(source_root), str(build), 'mc2.exe', 'mc2.pdb', build64_root=str(build))
    except (ValueError, SystemExit) as exc:
        return [f'shader inventory: {exc}']
    shipped = {rel: src for src, rel, _kind in items if rel.startswith('shaders/')}
    bad = []
    if set(shipped) != set(expected):
        bad.append(f'shader inventory: deploy ships {sorted(set(shipped) - set(expected))[:5]} extra, '
                   f'{sorted(set(expected) - set(shipped))[:5]} missing vs verify_published_identity')
    with tempfile.TemporaryDirectory(prefix='shader-inventory-') as tmp:
        if (Path(published) / 'shaders').is_dir():
            shutil.copytree(Path(published) / 'shaders', Path(tmp) / 'shaders')
        for rel, src in shipped.items():
            (Path(tmp) / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, Path(tmp) / rel)
        retire_unowned(tmp, shipped, own_shaders=True)
        left = {p.relative_to(tmp).as_posix() for p in (Path(tmp) / 'shaders').rglob('*') if p.is_file()}
    if left != set(expected):
        bad.append(f'shader inventory: after deploy {published} would hold {sorted(left - set(expected))[:5]} extra, '
                   f'{sorted(set(expected) - left)[:5]} missing (published shader inventory != HEAD build)')
    return bad


def verify_published_identity(target, source_root, head):
    """Hash the published exe/shaders against the clean HEAD build, not its own CSV."""
    from exe_fingerprint import extract, sha_agrees, build_current
    target, source_root = Path(target), Path(source_root)
    build = source_root / 'build64-ninja'
    for exe in (build / 'mc2.exe', target / 'mc2.exe'):
        fp = extract(exe)
        if not fp.get('present') or not build_current(fp.get('sha'), head, source_root, bool(fp.get('dirty'))):
            raise ValueError(f'{exe}: fingerprint != clean HEAD {head}')
    expected = {'mc2.exe': build / 'mc2.exe'}
    expected.update(published_shader_sources(source_root, build))
    with (target / '.deployed_manifest.csv').open(encoding='utf-8-sig', newline='') as stream:
        if next(csv.reader(stream), [])[:2] != ['manifest_version', 'v1']:
            raise ValueError('unsupported published manifest')
        rows = {row['relpath']: row for row in csv.DictReader(stream)}
    for rel, src in expected.items():
        row = rows.get(rel, {})
        dst = target / rel
        if (not sha_agrees(row.get('src_commit'), head) or not dst.is_file()
                or int(row.get('bytes', -1)) != dst.stat().st_size
                or sha256_file(str(src)) != row.get('sha256') or sha256_file(str(dst)) != row.get('sha256')):
            raise ValueError(f'{dst}: published bytes/manifest != HEAD build')
    # Every other manifest row (DLLs, data/, 3rdparty) is byte-checked on the deployed file too: the deploy's fast path and the
    # seed/reuse paths trust size+mtime, so a swapped DLL or data file was invisible here (meta-0220 P1#5). ~15 s for 5.6 GB warm.
    for rel, row in rows.items():
        if rel in expected:
            continue
        dst = target / rel
        if not dst.is_file() or int(row.get('bytes', -1)) != dst.stat().st_size or sha256_file(str(dst)) != row.get('sha256'):
            raise ValueError(f'{dst}: published bytes != manifest row')
    deployed_shaders = {p.relative_to(target).as_posix() for p in (target / 'shaders').rglob('*') if p.is_file()}
    if deployed_shaders != {rel for rel in expected if rel.startswith('shaders/')}:
        raise ValueError(f'{target}: published shader inventory != HEAD build')


def deploy_from_payload(payload, target, src_root, head):
    """Publish a payload built and gated elsewhere (tools/laptop_gate.py): payload/files/<rel> holds every file
    that differs from this install, payload/.deployed_manifest.csv is the full manifest. Every copied byte is
    sha256-checked against its manifest row, untouched files are sha256-checked, the exe fingerprint must be a clean HEAD build."""
    from exe_fingerprint import extract, build_current
    payload, target = Path(payload), Path(target)
    manifest = payload / '.deployed_manifest.csv'
    with manifest.open(encoding='utf-8-sig', newline='') as stream:
        if next(csv.reader(stream), [])[:2] != ['manifest_version', 'v1']:
            fail('unsupported payload manifest')
        rows = {row['relpath']: row for row in csv.DictReader(stream)}
    staged = payload / 'files'
    for rel, row in rows.items():
        src, dst = staged / rel, target / rel
        if src.is_file():
            if sha256_file(str(src)) != row['sha256']:
                fail(f'payload file {rel}: sha256 != manifest')
            dst.parent.mkdir(parents=True, exist_ok=True)
            tmp = dst.with_name(dst.name + '.payload-tmp')
            shutil.copy2(src, tmp)
            os.replace(tmp, dst)
        elif not dst.is_file() or dst.stat().st_size != int(row['bytes']) or sha256_file(str(dst)) != row['sha256']:
            fail(f'{rel}: not in the payload and the target copy does not match the manifest (size or sha256); the PC v0.4 drifted since the delta was taken')
    exe = target / 'mc2.exe'
    fp = extract(exe)
    if sha256_file(str(exe)) != rows['mc2.exe']['sha256'] or not fp.get('present') \
            or not build_current(fp.get('sha'), head, Path(src_root), bool(fp.get('dirty'))):
        fail(f'{exe}: fingerprint != clean HEAD {head}')
    retire_unowned(str(target), rows, own_shaders=any(rel.startswith('shaders/vulkan/') and rel.endswith('.spv') for rel in rows))
    shutil.copy2(manifest, target / '.deployed_manifest.csv')
    log(f'payload published: {len(rows)} manifest rows, exe sha256 {rows["mc2.exe"]["sha256"]}, fingerprint matches {head[:9]}')


def _target_preset(value):
    if value not in TARGET_PRESETS:
        hint = (f" '{value}' looks like a directory: pass it as the positional target_dir "
                f"(deploy_payload.py {value} --ninja ... [--allow-new-target]), not as --target." if re.search(r"[\\/:]", value) else "")
        raise argparse.ArgumentTypeError(f"--target takes a preset name ({', '.join(sorted(TARGET_PRESETS))}).{hint}")
    return value


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        epilog="Presets: --target game   = A:/Games/mc2-opengl/mc2-win64-v0.4 "
               "(mc2.exe, build64/RelWithDebInfo); "
               "--target editor = A:/Games/mc2-opengl/mc2-win64-v0.4 "
               "(\"Mission Editor.exe\", build64/out/editor/RelWithDebInfo). "
               "Explicit target dir / --build-dir / --exe-name override preset "
               "defaults.")
    ap.add_argument("target_dir", nargs="?", default=None,
                    help="deploy target dir (e.g. "
                    "A:/Games/mc2-opengl/mc2-win64-v0.4); optional when "
                    "--target preset is given (and then overrides the preset dir)")
    ap.add_argument("--target", type=_target_preset,
                    help="canned target preset: 'game' (v0.4, mc2.exe) or "
                    "'editor' (dev v0.4, Mission Editor.exe, EditRel build dir). "
                    "Sets defaults only; explicit flags still override.")
    ap.add_argument("--classic-assets", action="store_true",
                    help="deploy without the Remastered building kit (also honors RemasteredBuildings=FALSE)")
    ap.add_argument('--seed-owner-test', action='store_true',
                    help='seed target_dir from --seed-source after verifying against --source-root HEAD; no build or cook')
    ap.add_argument('--seed-source', type=Path,
                    help='published install to verify and copy with --seed-owner-test')
    ap.add_argument("--source-root", default=repo_root(),
                    help="source worktree root (default: this script's repo)")
    ap.add_argument("--build-dir", default=None,
                    help="build output dir (default: <source-root>/build64/RelWithDebInfo, "
                    "or the preset's build dir with --target)")
    ap.add_argument("--exe-name", default=None,
                    help="exe filename (default mc2.exe, or the preset's exe "
                    "with --target)")
    ap.add_argument("--wait", type=float, default=None, metavar="SECONDS",
                    help="how long to wait for a game running from the target folder to exit before refusing (default %d, or MC2_DEPLOY_WAIT_S; "
                         "0 refuses at once)" % DEFAULT_DEPLOY_WAIT_S)
    ap.add_argument("--ninja", action="store_true",
                    help="use the Ninja single-config dev tree (build64-ninja/, "
                    "configured by scripts/configure_ninja.py) instead of the VS "
                    "tree. Equivalent to --build-dir <src>/build64-ninja for "
                    "--target game. An explicit --build-dir wins.")
    ap.add_argument("--build64-root", default=None,
                    help="explicit build64-family root that holds engine outputs "
                    "and mc2.dir (e.g. an absolute path to build64_island). Default: "
                    "walk up from --build-dir for an ancestor whose name starts with "
                    "'build64' (so build64_island/build64_editor are auto-found).")
    ap.add_argument("--allow-stale-pdb", action="store_true",
                    help="downgrade PDB staleness to a warning")
    ap.add_argument("--allow-stale-exe", action="store_true",
                    help="downgrade stale-exe (exe older than its .obj files) to a warning")
    ap.add_argument("--full-verify", action="store_true",
                    help="ignore the size+mtime_ns fast path: copy every payload "
                    "file and hash source and target as earlier versions did")
    ap.add_argument("--verify-only", action="store_true",
                    help="re-check existing target against its manifest; no copy")
    ap.add_argument("--write-manifest-only", action="store_true",
                    help="re-hash payload files ALREADY in the target and "
                    "rewrite .deployed_manifest.csv; no copy, no lock check. "
                    "Use as the final step of any OTHER deploy path "
                    "(deploy-editor.sh, manual recipe) so the CSV manifest "
                    "that --verify-only checks does not go stale.")
    ap.add_argument("--check-only", action="store_true",
                    help="only check the target has every runtime DLL the exe imports plus assets/ data/ shaders/; no copy; exit 7 if not")
    ap.add_argument("--strict", action="store_true",
                    help="with --verify-only: drift = nonzero exit")
    ap.add_argument("--require-clean", action="store_true",
                    help="refuse an executable whose embedded fingerprint says "
                         "dirty=1 (built from an uncommitted tree)")
    ap.add_argument("--skip-kit", action="store_true",
                    help="lane installs: leave the install's mods/ kit untouched (no pinned-kit payload, no kit reconcile)")
    ap.add_argument("--allow-unverified-exe", action="store_true",
                    help="--target game: publish an exe whose sha256 is not the one in the newest PASS gate verdict for HEAD")
    ap.add_argument("--allow-new-target", action="store_true",
                    help="DELIBERATELY deploy to a NON-canonical / new ~5GB install "
                         "(release cut, fresh install). Off by default -- deploys are "
                         "forced to the canonical DEPLOY_ALLOWLIST to stop per-lane bloat.")
    ap.add_argument("--bootstrap-from", metavar="INSTALL",
                    help="with --allow-new-target: when the target is missing or empty, first copy the base files (data, assets, "
                         "archives, DLLs) from this existing install once; the deploy then adds the exe, shaders and manifest.")
    ap.add_argument("--preserve", action="append", default=[],
                    metavar="RELPATH",
                    help="payload file (repeatable) whose EXISTING target copy "
                         "is kept instead of overwritten (e.g. a hand-edited "
                         "run-with-log.bat). Only skips if the target copy "
                         "exists; fresh targets still get the source file.")
    ap.add_argument("--from-payload", metavar="DIR", default=None,
                    help="publish a payload gated elsewhere (tools/laptop_gate.py pull-back: DIR/files + DIR/.deployed_manifest.csv) "
                         "instead of enumerating a local build; sha256-verified, no build needed. Pair with --payload-head.")
    ap.add_argument("--payload-head", metavar="SHA", default=None,
                    help="with --from-payload: the candidate head the exe must fingerprint as (default: --source-root HEAD)")
    args = ap.parse_args()

    if args.seed_owner_test:
        preset = TARGET_PRESETS.get(args.target) if args.target else None
        destination = args.target_dir or (preset and preset['target_dir'])
        if not args.seed_source or not destination:
            ap.error('--seed-owner-test requires --seed-source INSTALL and target_dir (or --target)')
        source, target = args.seed_source, Path(destination)
        head = subprocess.check_output(['git', '-C', args.source_root, 'rev-parse', 'HEAD'],
                                       text=True, **console_child.child_kwargs(redirected=True)).strip()
        verify_published_identity(source, args.source_root, head)
        check_locks(str(target), 'mc2.exe', 0)
        held = take_folder_lease(str(target), 0)
        try:
            import kit_payload
            kit_payload.seed_install(source, target)
        finally:
            if held:
                release_folder_lease(str(target))
        return

    preset = TARGET_PRESETS.get(args.target) if args.target else None
    target_dir = args.target_dir or (preset and preset["target_dir"])
    if not target_dir:
        ap.error("a target dir is required: pass it positionally or via "
                 "--target game|editor")
    exe_name = args.exe_name or (preset["exe_name"] if preset else "mc2.exe")
    if preset:
        log(f"preset '{args.target}': target {preset['target_dir']}, "
            f"exe {preset['exe_name']}, build {preset['build_subdir']}")

    target = os.path.abspath(target_dir)
    if args.verify_only:
        sys.exit(verify_only(target, args.strict))

    src_root = os.path.abspath(args.source_root)
    refusal = implicit_target_refusal(src_root, args.target_dir)
    if refusal:
        fail(refusal)
    default_subdir = (preset["build_subdir"] if preset
                      else os.path.join("build64", "RelWithDebInfo"))
    if args.ninja:
        default_subdir = (preset["ninja_subdir"] if preset else "build64-ninja")
    build_dir = args.build_dir or os.path.join(src_root, default_subdir)
    pdb_name = os.path.splitext(exe_name)[0] + ".pdb"

    if args.check_only:
        gaps = check_install_complete(target, exe_name, build_dir, fix=False)
        if gaps:
            fail("INSTALL INCOMPLETE: " + ", ".join(gaps), 7)
        log("install complete")
        return

    if args.write_manifest_only:
        write_manifest_only(src_root, build_dir, exe_name, pdb_name, target,
                            build64_root=args.build64_root)
        return

    # DEPLOY-TARGET-GUARD: force deploys to the canonical deploy folders; never create
    # or deploy into per-lane ~5GB installs that bloat A:/Games/mc2-opengl.
    if not args.allow_new_target:
        base = os.path.basename(os.path.normpath(target))
        if base not in DEPLOY_ALLOWLIST:
            fail(f"deploy target '{target}'\n"
                 f"  basename '{base}' is NOT a canonical deploy folder.\n"
                 f"  Deploy ONLY to: {sorted(DEPLOY_ALLOWLIST)}\n"
                 f"  (under A:/Games/mc2-opengl). Each full install is ~5GB -- do NOT spin "
                 f"up per-lane/per-experiment deploy folders; pick a FREE canonical folder "
                 f"and md5-verify after.\n"
                 f"  If you REALLY need a new install (release cut), pass --allow-new-target.")

    if args.bootstrap_from:
        if not args.allow_new_target:
            fail("--bootstrap-from creates a new install: pass --allow-new-target too")
        with deploy_phase("bootstrap seed", 600):
            bootstrap_install(args.bootstrap_from, target, exe_name, pdb_name)

    if not os.path.isdir(target):
        fail(f"target dir does not exist: {target} (refusing to create deploy "
             "targets -- wrong-target trap, Mistake C). Pass --allow-new-target only "
             "for a deliberate new install.")

    log(f"source root: {src_root}")
    log(f"build dir:   {build_dir}")
    log(f"target:      {target}")

    wait_s = args.wait if args.wait is not None else float(os.environ.get("MC2_DEPLOY_WAIT_S") or DEFAULT_DEPLOY_WAIT_S)
    with deploy_phase("locks and folder lease", max(120, wait_s + 30)):
        check_locks(target, exe_name, wait_s)
        lease_held = take_folder_lease(target, wait_s)
    try:
        if args.from_payload:
            head = args.payload_head or subprocess.check_output(['git', '-C', src_root, 'rev-parse', 'HEAD'],
                                                                text=True, **console_child.child_kwargs(redirected=True)).strip()
            with deploy_phase("published payload", 600):
                deploy_from_payload(args.from_payload, target, src_root, head)
        else:
            run_deploy(args, src_root, build_dir, target, exe_name, pdb_name)
    finally:
        if lease_held:
            release_folder_lease(target)


def rebuild_command():
    """The exact command that rebuilds this worktree's dev exe (what a stale/dirty refusal tells the lane to run)."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace("\\", "/")
    tree = os.environ.get("MC2_NINJA_TREE")
    return f"cd {root} && " + (f"MC2_NINJA_TREE={tree} " if tree else "") + "py -3 scripts/ninja_build.py"


def require_head_exe(exe, head, root=None):
    """Reject stale/missing provenance from the executable bytes, never a header. With root, a clean
    ancestor build is accepted when no build input changed since (data/test-only commits)."""
    from exe_fingerprint import extract, sha_agrees, build_current
    fp = extract(exe)
    current = (sha_agrees(fp.get("sha"), head) is True
               or (root is not None and build_current(fp.get("sha"), head, root, bool(fp.get("dirty")))))
    if not fp.get("present") or not current:
        fail(f"REFUSING stale/unattributable executable: {exe}\n"
             f"  embedded build sha={fp.get('sha', 'absent')} != HEAD {head} "
             f"({fp.get('reason', 'SHA mismatch')}).\n"
             f"  Commit first, rebuild this HEAD with: {rebuild_command()}\n"
             "  then deploy --ninja (or name that same tree with --build-dir). "
             "--allow-stale-exe only relaxes object timestamps, never provenance.")
    return fp


def viewer_payload(build_dir, build64_root, head):
    """Ship the rebuilt viewer and its complete runtime closure, never an install's old exe."""
    b64 = find_build_root(build_dir, build64_root)
    cfg = "" if is_single_config(build_dir, b64) else os.path.basename(build_dir)
    directory = os.path.join(b64, "out", "tools", "asset_viewer", cfg)
    exe = os.path.join(directory, "mc2_asset_viewer.exe")
    if require_head_exe(exe, head)["dirty"]:
        fail(f"REFUSING dirty viewer build: {exe}; commit first and rebuild")
    # Include loader-library objects too: a partial build must not ship an old link.
    for root, _dirs, files in os.walk(os.path.join(b64, "out", "tools", "asset_viewer")):
        for name in files:
            obj = os.path.join(root, name)
            if name.endswith((".obj", ".o")) and os.path.getmtime(obj) > os.path.getmtime(exe) + 1:
                fail(f"STALE viewer: {exe} is older than {obj}; rebuild mc2_asset_viewer")
    items = [(exe, "mc2_asset_viewer.exe", "exe")]
    todo, seen = [exe], set()
    while todo:
        for name in _pe_imports(todo.pop()):
            if name in seen:
                continue
            seen.add(name)
            system = os.path.join(os.environ.get("SystemRoot", "C:/Windows"), "System32", name)
            crt = name.startswith(("msvcp140", "vcruntime140", "concrt140"))
            if name.startswith(("api-ms-", "ext-ms-")) or (os.path.isfile(system) and not crt):
                continue
            search = (directory, build_dir, os.path.dirname(system)) if crt else (directory, build_dir)
            src = next((os.path.join(d, name) for d in search
                        if os.path.isfile(os.path.join(d, name))), None)
            if not src:
                fail(f"viewer runtime DLL missing from build: {name}")
            items.append((src, name, "dll"))
            todo.append(src)
    for name in ("mesh_lit.vert.spv", "mesh_lit.frag.spv", "pbr_sphere.vert.spv", "pbr_sphere.frag.spv"):
        src = os.path.join(directory, "shaders_vk", name)
        if not os.path.isfile(src):
            fail(f"viewer shader missing from build: {src}")
        items.append((src, "shaders_vk/" + name, "shader"))
    return items


def dll_source_dirs(src_root):
    """Real homes of the runtime DLLs a fresh Ninja tree lacks: the repo's 3rdparty/lib/x64 (SDL2, zlib1), the ffmpeg
    bin dir, and the Visual Studio redist CRT (msvcp140*, vcruntime140*). No other worktree is consulted."""
    dirs = [os.path.join(src_root, "3rdparty", "lib", "x64"),
            os.path.join(src_root, "3rdparty", "ffmpeg-lgpl-win64", "bin"),
            os.path.join(src_root, "3rdparty", "fidelityfx", "bin")]
    pf = os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")
    for edition in ("BuildTools", "Community", "Professional", "Enterprise"):
        base = os.path.join(pf, "Microsoft Visual Studio", "2022", edition, "VC", "Redist", "MSVC")
        for ver in sorted(glob.glob(os.path.join(base, "*")), reverse=True):
            dirs.extend(glob.glob(os.path.join(ver, "x64", "Microsoft.VC*.CRT")))
    return [d for d in dirs if os.path.isdir(d)]


def seed_build_dlls(build_dir, src_root, names=None):
    """Copy any runtime DLL missing from build_dir out of dll_source_dirs (a fresh Ninja tree has none of them); returns the copied names."""
    if sys.platform != "win32":
        return []
    copied = []
    for name in (RUNTIME_DLLS if names is None else names):
        dst = os.path.join(build_dir, name)
        if os.path.isfile(dst):
            continue
        src = next((os.path.join(d, name) for d in dll_source_dirs(src_root) if os.path.isfile(os.path.join(d, name))), None)
        if src:
            shutil.copy2(src, dst)
            copied.append(name)
    if copied:
        log(f"seeded build dir {build_dir} with {', '.join(copied)} from 3rdparty/redist")
    return copied


def verdict_exe_refusal(src_root, exe_src, head):
    """Why the v0.4 publish must stop: the exe on disk is not the one the gate PASSed for HEAD (verdict artifacts.exe_sha256), else None."""
    try:
        with open(os.path.join(src_root, "build64-ninja", "gate-verdict.json"), encoding="utf-8") as f:
            v = json.load(f)
        want = (v.get("artifacts") or {}).get("exe_sha256")
    except (OSError, ValueError, AttributeError):
        return "no readable build64-ninja/gate-verdict.json"
    if v.get("verdict") != "PASS" or v.get("head") != head:
        return f"newest gate verdict is {v.get('verdict')} for {str(v.get('head'))[:9]}, not PASS for HEAD {head[:9]}"
    have = sha256_file(exe_src) if os.path.isfile(exe_src) else None
    if not want or have != want:
        return f"exe sha256 {str(have)[:12]} is not the gated {str(want)[:12]} (rebuilt after the gate?)"
    return None


def prepare_seed_delta(items, target, full_verify=False, allow_legacy=False):
    """Keep verified shared lane bytes; detach changed writes before the guard.

    Legacy lane installs have no receipt: detach their exact write set when
    explicitly allowed. Published v0.4 behavior is unchanged. Replacement never
    writes through a shared inode; the isolation guard still runs afterwards.
    """
    receipt = Path(target) / '.lane-seed.json'
    if not receipt.is_file() and not allow_legacy:
        return {}
    spec = importlib.util.spec_from_file_location('lane_seed', Path(__file__).with_name('make-isolated-deploy.py'))
    iso = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(iso)
    shared = json.loads(receipt.read_text(encoding='utf-8'))['shared'] if receipt.is_file() else {}
    verified = {}
    for src, rel, kind in items:
        dst = Path(target) / rel
        if not dst.is_file() or dst.stat().st_nlink <= 1:
            continue
        row = shared.get(rel)
        if (not full_verify and src is not None and row and iso.content_stamp(dst) == row['stamp']
                and Path(src).is_file() and sha256_file(src) == row['sha256']):
            verified[rel] = row['sha256']
            continue
        # The normal deploy still checks the source and copied hash. Privatize
        # first so its in-place write cannot reach the seed or another lane.
        tmp = dst.with_name(dst.name + f'.private-{os.getpid()}')
        try:
            shutil.copy2(dst, tmp)
            os.replace(tmp, dst)
        finally:
            tmp.unlink(missing_ok=True)
    log(f'lane seed delta: {len(verified)} unchanged shared payload files retained')
    return verified


def run_deploy(args, src_root, build_dir, target, exe_name, pdb_name):
    with deploy_phase("deploy", 1800):
        return _run_deploy(args, src_root, build_dir, target, exe_name, pdb_name)


def _run_deploy(args, src_root, build_dir, target, exe_name, pdb_name):
    if getattr(args, "target", None) == "game" and not args.allow_new_target and not getattr(args, "allow_unverified_exe", False):
        why = verdict_exe_refusal(src_root, os.path.join(build_dir, exe_name), git_head(src_root))
        if why:
            fail(f"v0.4 publish refused: {why}. Publish the exe the gate tested, or pass --allow-unverified-exe.")
    seed_build_dlls(build_dir, src_root)
    check_exe_fresh(os.path.join(build_dir, exe_name), build_dir, args.allow_stale_exe,
                    build64_root=args.build64_root)

    # DIRTY-BUILD GUARD: record what the exe was actually BUILT from, not just
    # git HEAD at deploy time. A dirty-tree build is unattributable to any
    # exact source state (2026-07-12 grey-regression P0 mis-diagnosis class).
    source_head = git_head(src_root)
    src_commit = source_head
    fp = require_head_exe(os.path.join(build_dir, exe_name), src_commit, src_root)
    if fp["dirty"]:
        msg = (f"embedded fingerprint says dirty build at {fp['sha']}. "
               f"COMMIT FIRST, rebuild ({rebuild_command()}), then deploy.")
        if args.require_clean:
            fail("--require-clean refused deploy: " + msg +
                 " lane_merge verification must build the committed HEAD before the gate deploy stage.")
        log(f"WARNING (dirty build): {msg}")
        src_commit += "-dirty"

    with deploy_phase("input comparison", 120):
        inputs = None
        reuse, reason = False, "explicit full verification or asset override"
        if not args.classic_assets and not getattr(args, "skip_kit", False):
            inputs = deploy_inputs(src_root)
            reuse, reason = reusable_payload(src_root, target, inputs)
            if getattr(args, "full_verify", False):
                reuse, reason = False, "explicit full verification"
        log(f"kit {'skipped automatically' if reuse else 'full deploy'}: {reason}")
        previous = read_manifest(target)
        old_reuse = json.loads((Path(target) / REUSE_NAME).read_text()) if reuse else None
        # A failed copy/cook must not leave valid-looking evidence for a retry.
        (Path(target) / REUSE_NAME).unlink(missing_ok=True)
    with deploy_phase("payload enumeration", 300):
        items = enumerate_payload(src_root, build_dir, exe_name, pdb_name,
                                  build64_root=args.build64_root, code_only=reuse)
    if exe_name != "mc2":
        with deploy_phase("viewer payload", 120):
            items.extend(viewer_payload(build_dir, args.build64_root, source_head))
    import kit_payload
    from smoke_lib import render_options_pin
    kit_manifest = None
    vehicle_manifest = None
    kit_tool = None
    remastered = render_options_pin.record(Path(target), os.environ)["values"]["RemasteredBuildings"]
    kit_prepare_started = time.perf_counter()
    if not args.classic_assets and not getattr(args, "skip_kit", False) and not reuse and remastered:
        try:
            with deploy_phase("kit preparation and importer", 600):
                kit_receipts = kit_payload.HashReceipts(Path(target) / '.deploy-receipts.json')
                kit_manifest, kit_items = kit_payload.payload(src_root, receipts=kit_receipts)
                items.extend(kit_items)
                vehicle_manifest, vehicle_items = kit_payload.payload(src_root,
                    Path("data/kit/vehicle-kit.json"), Path("mods/mc2-vehicle-kit"), receipts=kit_receipts)
                items.extend(vehicle_items)
                kit_receipts.save()
                kit_tool = kit_payload.build_importer(src_root)
        except (OSError, ValueError, KeyError) as exc:
            fail(f"RemasteredBuildings requires the pinned kit: {exc}; use --classic-assets for Classic")
    kit_prepare_seconds = time.perf_counter() - kit_prepare_started
    log(f"payload: {len(items)} files "
        f"({sum(1 for *_x, k in items if k == 'shader')} shaders, "
        f"{sum(1 for *_x, k in items if k == 'defs')} defs)")

    # DISK-GUARD-1 (2026-07-29 ENOSPC incident) -- checked HERE, immediately
    # after enumeration, because this is the first moment the required byte
    # count is actually KNOWN. Every byte below is measured from the real
    # payload; nothing is guessed. Still before deploy() copies anything.
    with deploy_phase("disk space guard", 120):
        check_free_space_for_payload(items, target)

    # LINK-GUARD-1 (2026-07-30 shared-install incident) -- checked HERE for the
    # same reason as the disk guard: `items` is the first moment the exact set
    # of relpaths about to be overwritten is known, and it is still before
    # deploy() writes a single byte. Order relative to the disk guard is not
    # load-bearing (neither writes anything); both must pass.
    # write_manifest() below writes .deployed_manifest.csv, which
    # enumerate_payload() does not list (it is an output, not payload) -- and
    # that file was itself hardlinked four ways in the incident. Append it so
    # the guard covers every file this run will write, not just the copied
    # ones.
    write_set = items + [(None, MANIFEST_NAME, "manifest")]
    legacy_lane = (getattr(args, "allow_new_target", False) and
                   Path(target).resolve().is_relative_to(Path('E:/mc2-installs').resolve()))
    with deploy_phase("hardlink isolation", 300):
        verified = prepare_seed_delta(write_set, target, args.full_verify, allow_legacy=legacy_lane)
        check_hardlink_isolation([item for item in items if item[1] not in verified] + [(None, MANIFEST_NAME, "manifest")],
                                 target)

    cache = {} if args.full_verify else read_manifest(target)
    preserve = list(args.preserve)
    if getattr(args, "skip_kit", False):
        preserve.extend(rel for rel in previous if rel.startswith("mods/"))
    with deploy_phase("payload hash copy and verification", 600):
        hashes = deploy(items, target, args.allow_stale_pdb, preserve=preserve,
                        cache=cache, full_verify=args.full_verify, code_only=reuse, verified=verified)
    if getattr(args, "skip_kit", False):
        hashes = {rel: row for rel, row in previous.items() if rel.startswith("mods/")} | hashes
    if kit_manifest:
        try:
            phase_started = time.perf_counter()
            with deploy_phase("stock texture receipts", 300):
                kit_payload.reconcile_stock_textures(target,
                    jobs=int(Path("A:/Games/mc2-ops/JOBS").read_text().strip()))
            log(f"phase stock receipts seconds={time.perf_counter() - phase_started:.3f}")
            phase_started = time.perf_counter()
            with deploy_phase("kit cook", 900):
                kit_payload.finish(target, kit_manifest, kit_tool)
                if vehicle_manifest:
                    kit_payload.finish(target, vehicle_manifest, kit_tool, kit_rel=Path("mods/mc2-vehicle-kit"))
            log(f"phase kit cook seconds={kit_prepare_seconds + time.perf_counter() - phase_started:.3f}")
        except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
            fail(f"pinned industrial kit deploy failed: {exc}")
    else:
        log("phase stock receipts seconds=0.000 (skipped)")
        log("phase kit cook seconds=0.000 (skipped)")
    installed = os.path.join(target, exe_name)
    require_head_exe(installed, source_head, src_root)
    if sha256_file(installed) != sha256_file(os.path.join(build_dir, exe_name)):
        fail(f"REFUSING installed executable differs from validated source: {installed}")
    # Completeness BEFORE the manifest: a failure must not leave a manifest that says the deploy is complete.
    gaps = check_install_complete(target, exe_name, build_dir)
    if gaps:
        fail("INSTALL INCOMPLETE (game would die with 0xC0000135 / missing assets): " + ", ".join(gaps) +
             f"; copy them from a full install (e.g. {GOLDEN_MATERIAL_ROOT})", 7)
    write_manifest(target, hashes, src_commit)
    if reuse:
        # Preserve the verified external source inventory across code-only deploys.
        record_deploy_inputs(target, inputs, hashes, [])
        value = json.loads((Path(target) / REUSE_NAME).read_text())
        value["sources"] = old_reuse["sources"]
        (Path(target) / REUSE_NAME).write_text(json.dumps(value, sort_keys=True), encoding="utf-8")
    elif kit_manifest and inputs is not None:
        record_deploy_inputs(target, inputs, hashes, items)
    log("deploy COMPLETE — payload verified, manifest written")

    # Post-deploy hygiene scan (Truth-First arc P1 #5/#7): the payload manifest
    # is a COMPLETENESS gate and never walks data/missions/*.beauty/, so a
    # generated-diagnostic sidecar that leaked into the target by MANUAL copy is
    # invisible to it. Run the standalone hygiene gate as a non-fatal warning so
    # every deploy surfaces leaked dev artifacts. Fully skippable / advisory.
    try:
        hygiene = Path(__file__).resolve().parent / "check-deploy-hygiene.py"
        if os.path.isfile(hygiene):
            r = subprocess.run([sys.executable, hygiene, target],
                               capture_output=True, text=True, timeout=120, **console_child.child_kwargs(redirected=True))
            out = (r.stdout or "").strip()
            if out:
                for line in out.splitlines():
                    log(f"hygiene: {line}")
    except Exception as e:  # noqa: BLE001 -- advisory only, never block a deploy
        log(f"hygiene scan skipped ({e})")


if __name__ == "__main__":
    main()
