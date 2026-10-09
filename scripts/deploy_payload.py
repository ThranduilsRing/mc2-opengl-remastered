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
import re
import shutil
import subprocess
import sys
import tempfile

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

# mc2-launcher.exe -- the install front-door (campaign mod-picker GUI that
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
#   tools/cook_bc6h_hdri.py  -- EXR -> BC6H KTX2 via texconv+ktx (HDRI baking)
#   tools/examples/          -- asset inspection helpers (list_mission_assets etc.)
GAME_COOK_TOOLS = ["tools/cook_bc6h_hdri.py", "tools/examples"]

# P1-G building PBR runtime payload. These files are source-tracked so HangarGLB
# and its CorrugatedSteel006A normal/ORM maps deploy through the verified payload
# path instead of by hand-copying release directories.
BUILDING_PBR_PAYLOAD = [
    "data/tgl/HangarGLB.glb",
    "data/tgl/HangarGLB.mcasset.json",
    # CONTRACT-HARNESS-REPAIR-1: marble cliff shipped in 5ded0f19 with repo
    # assets + ini wiring but was never added here, so it never reached an
    # install tree (data/tgl is enumerated file-by-file, not copied wholesale).
    "data/tgl/marblecliff.ini",
    "data/tgl/MarbleCliffGLB.glb",
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
    # Cement-pad proper asset fix (deferred item 3, supersedes the ead760df
    # "asset cook" TODO): the painted-concrete NORMAL map loaded into
    # matNormalArray layer 6 / MAT_LAYER_PAINTED_CONC by terrtxm2.cpp's
    # materialArrays block and sampled AS A NORMAL for cement-pad relief
    # (terrain_lod_chunk.frag ~1031, gos_terrain.frag ~720). Installs deployed
    # between 2026-06-14 and this fix carry an ALBEDO image in this slot
    # (the CEMENT-DETAIL-COLOR era literal swap; backup kept on installs as
    # mat6_normal.tga.orig_normal), which corrupts pad relief lighting.
    # Tracked in-repo + shipped here so every deploy mechanically restores the
    # true normal map. Cement pad ALBEDO does NOT come from this file -- it is
    # sampled from the cement diffuse atlas (tex3 / u_cementAtlas) since
    # ead760df, which is the permanent path, not a stopgap.
    "data/textures/mat6_normal.tga",
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
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
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


def log(msg):
    print(f"[deploy_payload] {msg}")


def fail(msg, code=1):
    print(f"[deploy_payload] FAIL: {msg}", file=sys.stderr)
    sys.exit(code)


def check_free_space_for_payload(items, target):
    """DISK-GUARD-1: refuse to start a deploy that cannot fit.

    Motivated by the 2026-07-29 ENOSPC incident: a write that runs out of space
    part-way through TRUNCATES its target. A deploy that dies mid-copy leaves a
    half-written mc2.exe in a deploy dir that other lanes then smoke, and the
    manifest hash check only catches it *after* the damage.

    The required figure is MEASURED from the enumerated payload, not guessed.
    +15% headroom covers filesystem slack, the manifest, and the fact that
    overwriting an existing file can transiently need space for both copies.
    Verdict handling (DISK-GUARD-3, 2026-07-29):
      OK (0)            -> proceed silently.
      LOW (1)           -> warn and proceed. This is the genuine advisory case:
                           the guard RAN, measured the volume, and said "tight
                           but it fits."
      CRITICAL (3) /
      ERROR (2)         -> fatal, nothing written.
      guard missing, guard un-runnable, or guard returned an unrecognised
      code -> ALSO FATAL.

    That last line is a reversal of DISK-GUARD-2, which warned and proceeded
    UNGUARDED. The reasoning: this guard's entire purpose is to prevent a
    silent, hard-to-attribute corruption (an ENOSPC-truncated mc2.exe that
    other lanes then smoke). Degrading to unguarded on a missing dependency
    means the protection evaporates exactly when the checkout is already in a
    state nobody has verified -- and it does so in a WARNING line, in the
    middle of a deploy's normal chatter, which is precisely the kind of
    diagnostic the 2026-07-29 incident proved nobody reads. A guard that
    fails open is not a guard; it is a log line.
    check-disk-space.py is a sibling file in this same repo, so its absence
    means a broken or partial checkout -- not a condition to shrug at.

    There is a deliberate, LOUD escape hatch: MC2_DEPLOY_SKIP_DISK_GUARD=1.
    Bypassing is a decision someone makes on purpose and can be found in the
    environment afterwards, rather than something the tool does quietly on
    their behalf.
    """
    if os.environ.get("MC2_DEPLOY_SKIP_DISK_GUARD") == "1":
        log("WARNING: MC2_DEPLOY_SKIP_DISK_GUARD=1 -- free-space preflight "
            "BYPASSED BY REQUEST. A mid-copy ENOSPC will truncate its target "
            "and the manifest check only catches that afterwards.")
        return

    total = 0
    missing = 0
    for src, _rel, _kind in items:
        try:
            total += os.path.getsize(src)
        except OSError:
            missing += 1  # optional payload entry (e.g. absent pdb) -- fine
    need = int(total * 1.15)
    log(f"disk guard: payload {total} bytes across {len(items) - missing} "
        f"present files; requiring {need} bytes (+15% headroom) on {target}")

    guard = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "check-disk-space.py")
    if not os.path.isfile(guard):
        fail(f"disk guard script not found at {guard} — refusing to deploy "
             "UNGUARDED. check-disk-space.py is a sibling of this script in "
             "the same repo; its absence means a broken or partial checkout. "
             "Restore it, or set MC2_DEPLOY_SKIP_DISK_GUARD=1 to bypass "
             "deliberately.")
    try:
        r = subprocess.run(
            [sys.executable, guard, "--path", target,
             "--need-bytes", str(need), "--label", "deploy payload"],
            timeout=60)
        rc = r.returncode
    except Exception as e:  # noqa: BLE001
        fail(f"disk guard could not run ({e}) — refusing to deploy UNGUARDED. "
             "Fix the guard invocation, or set MC2_DEPLOY_SKIP_DISK_GUARD=1 "
             "to bypass deliberately.")
    if rc in (2, 3):
        fail("insufficient free space for this deploy payload — NOTHING was "
             "written. See the [DISK_GUARD] lines above, reclaim space "
             "(.claude/DISK-RECLAIM-PLAN.md), then re-run.")
    if rc == 1:
        log("WARNING: disk guard verdict LOW — the payload fits but headroom "
            "is thin. Proceeding.")
    elif rc != 0:
        fail(f"disk guard returned unrecognised exit code {rc} — refusing to "
             "deploy on an unreadable verdict. Set "
             "MC2_DEPLOY_SKIP_DISK_GUARD=1 to bypass deliberately.")


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
                timeout=120)
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


def is_single_config(build_dir, b64):
    """True for a Ninja-style tree: outputs land directly in the build root
    (no <config> subdir), because there is exactly one config per tree.

    WHY: the VS tree nests everything under a config name (RelWithDebInfo/,
    mc2.dir/RelWithDebInfo/, out/mc2_launcher/RelWithDebInfo/); Ninja trees
    (build64-ninja) do not. Detecting the layout from the tree itself keeps
    both usable without forcing either into the other's directory shape.
    """
    if not b64:
        return False
    return os.path.isfile(os.path.join(b64, "build.ninja"))


def launcher_path(b64, cfg, single_config):
    base = os.path.join(b64, LAUNCHER_FROM_BUILD64)
    return os.path.join(base, LAUNCHER_NAME if single_config
                        else os.path.join(cfg, LAUNCHER_NAME))


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
            if f.endswith(".obj"):
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
    launcher_src = (launcher_path(b64, cfg, is_single_config(build_dir, b64))
                    if b64 else "")
    if launcher_src and os.path.isfile(launcher_src):
        items.append((launcher_src, LAUNCHER_NAME, "exe"))
    elif require_build:
        fail(f"mc2-launcher.exe not found (looked: {launcher_src or '<no build64 root>'})\n"
             "  Build it as part of the release: cmake --build build64 "
             "--config RelWithDebInfo --target mc2_launcher "
             "(Ninja tree: ninja -C build64-ninja mc2_launcher)")
    else:
        # manifest-only: source build output may be gone; still record the
        # relpath so an existing target copy gets hashed.
        items.append((launcher_src or LAUNCHER_NAME, LAUNCHER_NAME, "exe"))

    # Baked Vulkan SPIR-V (VULKAN-SHADER-TOOLCHAIN-1): glslangValidator/glslc
    # compiles shaders/vulkan/*.{vert,frag} to <build64-root>/shaders/vulkan/*.spv
    # (CMAKE_BINARY_DIR, i.e. the build64 root, NOT the per-config build_dir --
    # same non-standard location as mc2-launcher.exe above). The Vulkan island
    # loaders (vulkan_*_island.cpp, vulkan_backend_skeleton.cpp) read
    # MC2_VULKAN_SPV_DIR if set, else default to "shaders/vulkan" relative to
    # the exe's CWD -- so deployed .spv must land at <target>/shaders/vulkan/.
    # Without this, any deployed install hits "cannot open spv" ->
    # vk_device_init_failed for every Vulkan region/island, even though the
    # exe + GL shaders deployed fine (found by VK-REGION-SSAO-1 content pass).
    # Soft: MC2_VULKAN is an opt-in CMake flag, so most builds never produce
    # these; only enforce the lockstep hash-match discipline (kind="shader")
    # for whichever ones ARE present in the build output.
    vk_spv_dir = os.path.join(b64, "shaders", "vulkan") if b64 else ""
    if vk_spv_dir and os.path.isdir(vk_spv_dir):
        for fn in sorted(os.listdir(vk_spv_dir)):
            if fn.endswith(".spv"):
                p = os.path.join(vk_spv_dir, fn)
                if os.path.isfile(p):
                    items.append((p, f"shaders/vulkan/{fn}", "shader"))
    elif require_build:
        log(f"INFO: no baked Vulkan SPIR-V at {vk_spv_dir or '<no build64 root>'} "
            "-- skipping (MC2_VULKAN not compiled in this build; Vulkan "
            "islands will fail-soft to GL in the deployed install)")

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
    present in every target -- e.g. an editor-only deploy has no mc2.exe).
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
                        hashes[rel] = (h, size)
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
        "exe_name": "mc2.exe",
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
# release cut). The 5 folders below are the shared dev deploy targets -- pick a free one
# (md5-verify after; a sibling may hold the lease).
DEPLOY_ALLOWLIST = {
    "mc2-win64-v0.4",
    "mc2-win64-v0.4c",
    "mc2-win64-0.4c",
}


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
    ap.add_argument("--target", choices=sorted(TARGET_PRESETS),
                    help="canned target preset: 'game' (v0.4, mc2.exe) or "
                    "'editor' (dev v0.4, Mission Editor.exe, EditRel build dir). "
                    "Sets defaults only; explicit flags still override.")
    ap.add_argument("--source-root", default=repo_root(),
                    help="source worktree root (default: this script's repo)")
    ap.add_argument("--build-dir", default=None,
                    help="build output dir (default: <source-root>/build64/RelWithDebInfo, "
                    "or the preset's build dir with --target)")
    ap.add_argument("--exe-name", default=None,
                    help="exe filename (default mc2.exe, or the preset's exe "
                    "with --target)")
    ap.add_argument("--ninja", action="store_true",
                    help="use the Ninja single-config dev tree (build64-ninja/, "
                    "configured by scripts/configure_ninja.py) instead of the VS "
                    "tree. Equivalent to --build-dir <src>/build64-ninja for "
                    "--target game. An explicit --build-dir wins.")
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
    ap.add_argument("--require-clean", action="store_true",
                    help="refuse to deploy a build whose build_fingerprint.h says "
                         "MC2_BUILD_GIT_DIRTY=1 (built from an uncommitted tree)")
    ap.add_argument("--allow-new-target", action="store_true",
                    help="DELIBERATELY deploy to a NON-canonical / new ~5GB install "
                         "(release cut, fresh install). Off by default -- deploys are "
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
    if args.ninja:
        default_subdir = (preset["ninja_subdir"] if preset else "build64-ninja")
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
                 f"  (under A:/Games/mc2-opengl). Each full install is ~5GB -- do NOT spin "
                 f"up per-lane/per-experiment deploy folders; pick a FREE canonical folder "
                 f"and md5-verify after.\n"
                 f"  If you REALLY need a new install (release cut), pass --allow-new-target.")

    if not os.path.isdir(target):
        fail(f"target dir does not exist: {target} (refusing to create deploy "
             "targets -- wrong-target trap, Mistake C). Pass --allow-new-target only "
             "for a deliberate new install.")

    log(f"source root: {src_root}")
    log(f"build dir:   {build_dir}")
    log(f"target:      {target}")

    check_locks(target, exe_name)
    check_exe_fresh(os.path.join(build_dir, exe_name), build_dir, args.allow_stale_exe,
                    build64_root=args.build64_root)

    # DIRTY-BUILD GUARD: record what the exe was actually BUILT from, not just
    # git HEAD at deploy time. A dirty-tree build is unattributable to any
    # exact source state (2026-07-12 grey-regression P0 mis-diagnosis class).
    src_commit = git_head(src_root)
    fp = read_build_fingerprint(build_dir)
    if fp:
        if fp["sha"] and not src_commit.startswith(fp["sha"]):
            log(f"WARNING: build fingerprint sha {fp['sha']} != git HEAD "
                f"{src_commit[:12]} — the exe was built from a DIFFERENT commit "
                f"than the tree you are deploying from. Manifest records the "
                f"BUILD sha.")
            src_commit = fp["sha"]
        if fp["dirty"]:
            msg = (f"build_fingerprint.h says MC2_BUILD_GIT_DIRTY=1 — this exe "
                   f"was built from an UNCOMMITTED tree at {fp['sha']}. Its exact "
                   f"source state cannot be reproduced. COMMIT FIRST, rebuild, "
                   f"then deploy.")
            if args.require_clean:
                fail(msg)
            log("=" * 72)
            log(f"WARNING (dirty build): {msg}")
            log("=" * 72)
            src_commit = src_commit + "-dirty"
    else:
        log("note: no build_fingerprint.h found next to the build dir; "
            "manifest src_commit falls back to git HEAD (dirty state unknown)")

    items = enumerate_payload(src_root, build_dir, exe_name, pdb_name,
                              build64_root=args.build64_root)
    log(f"payload: {len(items)} files "
        f"({sum(1 for *_x, k in items if k == 'shader')} shaders, "
        f"{sum(1 for *_x, k in items if k == 'defs')} defs)")

    # DISK-GUARD-1 (2026-07-29 ENOSPC incident) -- checked HERE, immediately
    # after enumeration, because this is the first moment the required byte
    # count is actually KNOWN. Every byte below is measured from the real
    # payload; nothing is guessed. Still before deploy() copies anything.
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
    check_hardlink_isolation(items + [(None, MANIFEST_NAME, "manifest")],
                             target)

    hashes = deploy(items, target, args.allow_stale_pdb, preserve=args.preserve)
    write_manifest(target, hashes, src_commit)
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
    except Exception as e:  # noqa: BLE001 -- advisory only, never block a deploy
        log(f"hygiene scan skipped ({e})")


if __name__ == "__main__":
    main()
