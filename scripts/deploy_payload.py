#!/usr/bin/env python3
"""deploy_payload.py — deploy payload completeness gate (slice S8 seed, manifest v1).

Wraps/validates a deploy of the MC2 runtime payload from a source worktree
build to a runtime target dir (e.g. A:/Games/mc2-opengl/mc2-win64-v0.4 or
.../mc2-win64-0.4c). Payload, by target:
  both    : exe + pdb + ffmpeg DLLs + shaders/ + run-with-log.bat
  game    : + run-mc2.bat
  editor  : + run-editor.bat + tools/terrain_gen/** (authoring support — the
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
  rows = relpath,sha256,bytes,src_commit,timestamp

--verify-only re-checks an existing target against its manifest (post-hoc
staleness detector). Advisory: exit 0 with a report, unless --strict.

Python (not .sh) to match sibling conventions: scripts/ is python-dominated
(run_smoke.py, check-asset-manifests.py, ...) and we need sha256 + csv + robust
Windows file-lock probing, all stdlib here.

NO engine code. NO run_smoke verdict-path involvement. tier1 not required
(scripts-only, zero runtime change).
"""

import argparse
import csv
import ctypes
import datetime
import hashlib
import os
import shutil
import subprocess
import sys

try:
    import psutil
    _PSUTIL_OK = True
except ImportError:
    _PSUTIL_OK = False

MANIFEST_NAME = ".deployed_manifest.csv"
MANIFEST_VERSION = "v1"
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

# mc2-launcher.exe — the install front-door (campaign mod-picker GUI that
# launches mc2.exe with MC2_ACTIVE_MOD). A separate build target
# (tools/mc2_launcher -> build64/out/mc2_launcher/<cfg>/mc2-launcher.exe), so
# it lives outside build_dir; we locate it from the build64 root. Ships with
# both game and editor installs.
LAUNCHER_NAME = "mc2-launcher.exe"
LAUNCHER_FROM_BUILD64 = os.path.join("out", "mc2_launcher")  # + <cfg> + exe

# Editor authoring support trees (src_root-relative dirs copied recursively,
# structure preserved). The editor's -gen-map / Generate Mission path shells
# `py -3 tools\\terrain_gen\\terrain_gen.py`; without this tree the editor
# builds and launches but every gen/mission-load smoke case fails. Tying it to
# the deploy payload keeps "build -> deploy" self-complete (no manual copy).
EDITOR_SUPPORT_TREES = ["tools/terrain_gen"]

# Cook tools shipped with BOTH game and editor installs so modders/users can
# re-cook or inspect assets without a full source checkout.
#   tools/cook_bc6h_hdri.py  — EXR -> BC6H KTX2 via texconv+ktx (HDRI baking)
#   tools/examples/          — asset inspection helpers (list_mission_assets etc.)
GAME_COOK_TOOLS = ["tools/cook_bc6h_hdri.py", "tools/examples"]

# P1-G building PBR runtime payload. These files are source-tracked so HangarGLB
# and its CorrugatedSteel006A normal/ORM maps deploy through the verified payload
# path instead of by hand-copying release directories.
BUILDING_PBR_PAYLOAD = [
    "data/tgl/HangarGLB.glb",
    "data/tgl/HangarGLB.mcasset.json",
    "data/tgl/quonset.ini",
    "data/tgl/QuonsetGLB.glb",
    "data/tgl/QuonsetGLB.mcasset.json",
    "data/materials/pbr/corrugatedsteel006a_normal.ktx2",
    "data/materials/pbr/corrugatedsteel006a_orm.ktx2",
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
]

# TERRAIN-MATERIAL-TEXTURES-1: per-layer BC7 sRGB albedo KTX2s read off
# "data/terrain_layers/<channel>_albedo.ktx2" by the LOD-chunk albedo-array
# loader (gos_terrain_lod_chunk.cpp) when MC2_TERRAIN_MATERIAL_TEXTURES=1.
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
]

FFMPEG_DLLS = [
    "avcodec-61.dll",
    "avformat-61.dll",
    "avutil-59.dll",
    "swscale-8.dll",
    "swresample-5.dll",
]


def log(msg):
    print(f"[deploy_payload] {msg}")


def fail(msg, code=1):
    print(f"[deploy_payload] FAIL: {msg}", file=sys.stderr)
    sys.exit(code)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


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
        )
        if out.returncode == 0:
            return out.stdout.strip()
    except Exception:
        pass
    return "unknown"


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
            "(access denied / zombie) — not blocking deploy")
    for pid, path in other:
        log(f"INFO: running {exe_name} PID {pid} from OTHER folder "
            f"({path}) — deploy continues")

    if blocking:
        lines = "\n".join(f"  PID {pid}: {path}" for pid, path in blocking)
        return (
            f"target exe is running from THIS deploy folder:\n{lines}\n"
            "  Close that instance and re-run. "
            "This script will NEVER taskkill automatically."
        )
    return ""


def check_locks(target_dir, exe_name):
    target_exe = os.path.join(target_dir, exe_name)
    target_exe_resolved = os.path.normcase(os.path.abspath(target_exe))

    # Step 1: path-aware running-process check (psutil / wmic).
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
    """Locate the build64-family root (holds out/mc2_launcher and mc2.dir).

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


def check_exe_fresh(exe_src, build_dir, allow_stale_exe, build64_root=None):
    """Mistake E: stale exe deployed because the source exe was not relinked.

    The linked exe must be NEWER than every compiled .obj that feeds it. An incremental build
    that compiled objects but did not relink the exe — or a deploy racing a still-running build —
    leaves the exe OLDER than its objects, so deploy faithfully copies a stale binary (this
    actually happened: a deploy shipped a pre-slice exe ~2.5 KB smaller than the rebuilt one).
    Compare exe mtime against the newest .obj under build64/mc2.dir/<config>/.
    """
    if not os.path.isfile(exe_src):
        return
    exe_m = os.path.getmtime(exe_src)
    bd = os.path.abspath(build_dir).rstrip(os.sep)
    cfg = os.path.basename(bd)                      # e.g. RelWithDebInfo
    b64 = find_build_root(build_dir, build64_root)
    if not b64:
        return                                      # non-standard layout; skip the guard
    objdir = os.path.join(b64, "mc2.dir", cfg)
    newest_m, newest_p = 0.0, None
    for root, _dirs, files in os.walk(objdir):
        for f in files:
            if f.endswith(".obj"):
                m = os.path.getmtime(os.path.join(root, f))
                if m > newest_m:
                    newest_m, newest_p = m, os.path.join(root, f)
    if newest_p and newest_m > exe_m + 1.0:         # 1s slack for filesystem timestamp jitter
        msg = (f"STALE EXE (Mistake E): {exe_src}\n"
               f"  exe mtime {exe_m:.0f} is OLDER than object {newest_p}\n"
               f"  obj mtime {newest_m:.0f} — the build did not relink (incremental skip or a\n"
               f"  deploy racing an in-flight build). Rebuild the exe before deploying.")
        if allow_stale_exe:
            log(f"WARNING (--allow-stale-exe): {msg}")
        else:
            fail(msg + "\n  (override: --allow-stale-exe)")


def enumerate_payload(src_root, build_dir, exe_name, pdb_name,
                      require_build=True, build64_root=None):
    """Returns list of (src_abspath, target_relpath, kind).

    require_build=False (manifest-only): the build outputs may be gone; we
    only need the relpath list to know which target files to re-hash, so a
    missing source exe/dll is tolerated (the entry is still emitted so the
    deployed copy can be recorded).
    """
    items = []
    exe_src = os.path.join(build_dir, exe_name)
    if require_build and not os.path.isfile(exe_src):
        fail(f"source exe not found: {exe_src}")
    items.append((exe_src, exe_name, "exe"))

    pdb_src = os.path.join(build_dir, pdb_name)
    items.append((pdb_src, pdb_name, "pdb"))  # presence checked later

    for dll in FFMPEG_DLLS:
        p = os.path.join(build_dir, dll)
        if os.path.isfile(p):
            items.append((p, dll, "dll"))

    # mc2-launcher.exe: separate build target outside build_dir. Locate via the
    # build64 root (walk up from build_dir) using this deploy's config name.
    bd = os.path.abspath(build_dir).rstrip(os.sep)
    cfg = os.path.basename(bd)  # e.g. RelWithDebInfo
    b64 = find_build_root(build_dir, build64_root)
    launcher_src = (os.path.join(b64, LAUNCHER_FROM_BUILD64, cfg, LAUNCHER_NAME)
                    if b64 else "")
    if launcher_src and os.path.isfile(launcher_src):
        items.append((launcher_src, LAUNCHER_NAME, "exe"))
    elif require_build:
        fail(f"mc2-launcher.exe not found (looked: {launcher_src or '<no build64 root>'})\n"
             "  Build it as part of the release: cmake --build build64 "
             "--config RelWithDebInfo --target mc2_launcher")
    else:
        # manifest-only: source build output may be gone; still record the
        # relpath so an existing target copy gets hashed.
        items.append((launcher_src or LAUNCHER_NAME, LAUNCHER_NAME, "exe"))

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

    for rel in BUILDING_PBR_PAYLOAD:
        p = os.path.join(src_root, rel)
        if require_build and not os.path.isfile(p):
            fail(f"building PBR payload missing in source: {p}")
        items.append((p, rel, "support"))

    for rel in GAME_DATA_PAYLOAD:
        p = os.path.join(src_root, rel)
        if require_build and not os.path.isfile(p):
            fail(f"game data payload missing in source: {p}")
        items.append((p, rel, "support"))

    # TERRAIN-MATERIAL-TEXTURES-1: optional loose cooked sidecars -- skip (log)
    # when absent instead of failing; the engine fail-softs without them.
    # TERRAIN-MATERIAL-TEXTURES-1-FIX (fix D, MIPS ROOT CAUSE): the "levels=1
    # deployed" bug was a stale pre-mip cook getting picked up because deploy just
    # copies whatever _albedo.ktx2 is on disk. Guard it here: read the KTX2
    # levelCount and HARD-FAIL if a present layer is mip-incomplete, so a stale
    # cook can never reach the payload silently. Re-cook, then re-deploy.
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
    for script in SUPPORT_SCRIPTS["editor" if is_editor else "game"]:
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

def deploy(items, target_dir, allow_stale_pdb, preserve=()):
    src_hashes = {}
    shader_mismatches = []
    pdb_problem = None
    preserve = {p.replace(os.sep, "/") for p in preserve}

    for src, rel, kind in items:
        dst = os.path.join(target_dir, rel)
        if kind == "pdb" and not os.path.isfile(src):
            pdb_problem = f"source PDB missing: {src}"
            continue
        if rel.replace(os.sep, "/") in preserve and os.path.isfile(dst):
            # user-owned file at target (e.g. hand-edited run-with-log.bat):
            # keep the deployed copy, record its hash so the manifest matches
            # what's actually on disk.
            src_hashes[rel] = (sha256_file(dst), os.path.getsize(dst))
            log(f"preserved {rel} (existing target copy kept)")
            continue
        os.makedirs(os.path.dirname(dst) or target_dir, exist_ok=True)
        try:
            shutil.copyfile(src, dst)  # per-file copy, overwrite (never cp -r)
        except PermissionError:
            fail(f"copy refused (file locked?): {dst}\n"
                 "  A process is holding this file. Close it and re-run; "
                 "this script never taskkills.", 2)
        s_hash = sha256_file(src)
        d_hash = sha256_file(dst)
        src_hashes[rel] = (d_hash, os.path.getsize(dst))
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

    if shader_mismatches:
        fail("post-copy diff MISMATCH (stale files in target):\n  "
             + "\n  ".join(shader_mismatches), 4)

    if pdb_problem:
        if allow_stale_pdb:
            log(f"WARNING (--allow-stale-pdb): {pdb_problem}")
        else:
            fail(f"{pdb_problem}\n  Stale PDB = wrong Tracy symbols "
                 "(Mistake D). Use --allow-stale-pdb to override.", 5)
    else:
        log("PDB check OK: deployed PDB matches source hash")

    return src_hashes


def write_manifest(target_dir, hashes, src_commit):
    ts = datetime.datetime.now().isoformat(timespec="seconds")
    path = os.path.join(target_dir, MANIFEST_NAME)
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["manifest_version", MANIFEST_VERSION, "", "", ""])
        w.writerow(["relpath", "sha256", "bytes", "src_commit", "timestamp"])
        for rel in sorted(hashes):
            h, size = hashes[rel]
            w.writerow([rel, h, size, src_commit, ts])
    log(f"manifest written: {path} ({len(hashes)} rows, {MANIFEST_VERSION}, "
        f"src_commit {src_commit[:12]})")


def hash_present_payload(items, target_dir):
    """Re-hash the payload files that are ALREADY in target_dir (no copy).

    Used by --write-manifest-only: another deploy path (deploy-editor.sh,
    the manual recipe, etc.) already copied the binaries in; we just record
    them in the SAME .deployed_manifest.csv that --verify-only checks, so
    the CSV does not go stale behind the JSON manifest.
    Files missing from the target are skipped (not every payload item is
    present in every target — e.g. an editor-only deploy has no mc2.exe).
    """
    hashes = {}
    present, absent = 0, 0
    for src, rel, kind in items:
        dst = os.path.join(target_dir, rel)
        if os.path.isfile(dst):
            hashes[rel] = (sha256_file(dst), os.path.getsize(dst))
            present += 1
        else:
            absent += 1
    log(f"manifest-only: {present} payload files present in target, "
        f"{absent} absent (skipped)")
    return hashes


def write_manifest_only(src_root, build_dir, exe_name, pdb_name, target_dir,
                        build64_root=None):
    """Refresh <target>/.deployed_manifest.csv from files ALREADY in target.

    No copy, no lock check (we are not writing binaries — only the CSV).
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
                        hashes[rel] = (h, size)
        except Exception as e:
            log(f"WARNING: could not merge existing CSV manifest ({e}); replacing")

    if not hashes:
        fail("no payload files present in target — nothing to record")
    write_manifest(target_dir, hashes, git_head(src_root))
    log("manifest-only COMPLETE — .deployed_manifest.csv refreshed (no copy)")


def staleness_report(target_dir):
    """Pure: compare a deployed tree against its .deployed_manifest.csv by
    re-hashing every listed file (single-source sha256_file). Returns a dict —
    NO printing, NO sys.exit — so callers other than the CLI (run_smoke) can
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


def verify_only(target_dir, strict):
    rep = staleness_report(target_dir)
    if not rep["has_manifest"]:
        msg = (f"no manifest at {rep['manifest_path']} — target never deployed "
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
#   editor -> live editor install A:/Games/mc2-opengl/mc2-win64-0.4c,
#             "Mission Editor.exe", build64/out/editor/RelWithDebInfo (EditRel)
TARGET_PRESETS = {
    "game": {
        "target_dir": "A:/Games/mc2-opengl/mc2-win64-v0.4",
        "exe_name": "mc2.exe",
        "build_subdir": os.path.join("build64", "RelWithDebInfo"),
    },
    "editor": {
        "target_dir": "A:/Games/mc2-opengl/mc2-win64-0.4c",
        "exe_name": "Mission Editor.exe",
        "build_subdir": os.path.join("build64", "out", "editor", "RelWithDebInfo"),
    },
}

# DEPLOY-TARGET-GUARD: deploy ONLY to one of the canonical deploy folders. Each full
# install is ~5GB; spinning up a per-lane/per-experiment deploy folder (e.g.
# mc2-<lane>-test) bloats A:/Games/mc2-opengl by tens of GB. Deploy is REFUSED to any
# target not in this set unless --allow-new-target is passed (deliberate new install /
# release cut). The 5 folders below are the shared dev deploy targets — pick a free one
# (md5-verify after; a sibling may hold the lease).
DEPLOY_ALLOWLIST = {
    "mc2-win64-v0.4",
    "mc2-win64-v0.4c",
    "mc2-win64-0.4c",
    "mc2-win64-abl-validate",
    "mc2-win64-v0.3",
}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        epilog="Presets: --target game   = A:/Games/mc2-opengl/mc2-win64-v0.4 "
               "(mc2.exe, build64/RelWithDebInfo); "
               "--target editor = A:/Games/mc2-opengl/mc2-win64-0.4c "
               "(\"Mission Editor.exe\", build64/out/editor/RelWithDebInfo). "
               "Explicit target dir / --build-dir / --exe-name override preset "
               "defaults.")
    ap.add_argument("target_dir", nargs="?", default=None,
                    help="deploy target dir (e.g. "
                    "A:/Games/mc2-opengl/mc2-win64-v0.4); optional when "
                    "--target preset is given (and then overrides the preset dir)")
    ap.add_argument("--target", choices=sorted(TARGET_PRESETS),
                    help="canned target preset: 'game' (v0.4, mc2.exe) or "
                    "'editor' (0.4c, Mission Editor.exe, EditRel build dir). "
                    "Sets defaults only; explicit flags still override.")
    ap.add_argument("--source-root", default=repo_root(),
                    help="source worktree root (default: this script's repo)")
    ap.add_argument("--build-dir", default=None,
                    help="build output dir (default: <source-root>/build64/RelWithDebInfo, "
                    "or the preset's build dir with --target)")
    ap.add_argument("--exe-name", default=None,
                    help="exe filename (default mc2.exe, or the preset's exe "
                    "with --target)")
    ap.add_argument("--build64-root", default=None,
                    help="explicit build64-family root that holds out/mc2_launcher "
                    "and mc2.dir (e.g. an absolute path to build64_island). Default: "
                    "walk up from --build-dir for an ancestor whose name starts with "
                    "'build64' (so build64_island/build64_editor are auto-found).")
    ap.add_argument("--allow-stale-pdb", action="store_true",
                    help="downgrade PDB staleness to a warning")
    ap.add_argument("--allow-stale-exe", action="store_true",
                    help="downgrade stale-exe (exe older than its .obj files) to a warning")
    ap.add_argument("--verify-only", action="store_true",
                    help="re-check existing target against its manifest; no copy")
    ap.add_argument("--write-manifest-only", action="store_true",
                    help="re-hash payload files ALREADY in the target and "
                    "rewrite .deployed_manifest.csv; no copy, no lock check. "
                    "Use as the final step of any OTHER deploy path "
                    "(deploy-editor.sh, manual recipe) so the CSV manifest "
                    "that --verify-only checks does not go stale.")
    ap.add_argument("--strict", action="store_true",
                    help="with --verify-only: drift = nonzero exit")
    ap.add_argument("--allow-new-target", action="store_true",
                    help="DELIBERATELY deploy to a NON-canonical / new ~5GB install "
                         "(release cut, fresh install). Off by default — deploys are "
                         "forced to the canonical DEPLOY_ALLOWLIST to stop per-lane bloat.")
    ap.add_argument("--preserve", action="append", default=[],
                    metavar="RELPATH",
                    help="payload file (repeatable) whose EXISTING target copy "
                         "is kept instead of overwritten (e.g. a hand-edited "
                         "run-with-log.bat). Only skips if the target copy "
                         "exists; fresh targets still get the source file.")
    args = ap.parse_args()

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
    default_subdir = (preset["build_subdir"] if preset
                      else os.path.join("build64", "RelWithDebInfo"))
    build_dir = args.build_dir or os.path.join(src_root, default_subdir)
    pdb_name = os.path.splitext(exe_name)[0] + ".pdb"

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
                 f"  (under A:/Games/mc2-opengl). Each full install is ~5GB — do NOT spin "
                 f"up per-lane/per-experiment deploy folders; pick a FREE canonical folder "
                 f"and md5-verify after.\n"
                 f"  If you REALLY need a new install (release cut), pass --allow-new-target.")

    if not os.path.isdir(target):
        fail(f"target dir does not exist: {target} (refusing to create deploy "
             "targets — wrong-target trap, Mistake C). Pass --allow-new-target only "
             "for a deliberate new install.")

    log(f"source root: {src_root}")
    log(f"build dir:   {build_dir}")
    log(f"target:      {target}")

    check_locks(target, exe_name)
    check_exe_fresh(os.path.join(build_dir, exe_name), build_dir, args.allow_stale_exe,
                    build64_root=args.build64_root)
    items = enumerate_payload(src_root, build_dir, exe_name, pdb_name,
                              build64_root=args.build64_root)
    log(f"payload: {len(items)} files "
        f"({sum(1 for *_x, k in items if k == 'shader')} shaders)")
    hashes = deploy(items, target, args.allow_stale_pdb, preserve=args.preserve)
    write_manifest(target, hashes, git_head(src_root))
    log("deploy COMPLETE — payload verified, manifest written")

    # Post-deploy hygiene scan (Truth-First arc P1 #5/#7): the payload manifest
    # is a COMPLETENESS gate and never walks data/missions/*.beauty/, so a
    # generated-diagnostic sidecar that leaked into the target by MANUAL copy is
    # invisible to it. Run the standalone hygiene gate as a non-fatal warning so
    # every deploy surfaces leaked dev artifacts. Fully skippable / advisory.
    try:
        hygiene = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "check-deploy-hygiene.py")
        if os.path.isfile(hygiene):
            r = subprocess.run([sys.executable, hygiene, target],
                               capture_output=True, text=True, timeout=120)
            out = (r.stdout or "").strip()
            if out:
                for line in out.splitlines():
                    log(f"hygiene: {line}")
    except Exception as e:  # noqa: BLE001 — advisory only, never block a deploy
        log(f"hygiene scan skipped ({e})")


if __name__ == "__main__":
    main()
