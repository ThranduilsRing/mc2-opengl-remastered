"""capture-before reuse (p29 cut 2): the old build's reference captures are restored instead of re-shot when the COMPLETE identity of
the reference run matches an earlier one and every artifact still hashes to what the sweep recorded.

Enabled by default; MC2_CAPTURE_REUSE=0 is a temporary kill switch. The key carries shader and DLL bytes, not manifest text:
merge_gate stages the candidate by removing the shader rows from .deployed_manifest.csv, so the manifest cannot describe them.

Identity = exe sha256, deployed-file content (the install's .deployed_manifest.csv rows: relpath, sha256, bytes; shaders and data
included), options/prefs files, poses (bookmark names and their file hashes), capture arguments, harness source, MC2_* environment
and GPU + driver. Any missing piece (no manifest, no GPU answer) means no reuse: a miss only costs the capture it replaces.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

CACHE_ROOT = Path("G:/mc2-tmp/capture-reuse")
KEEP = 3   # newest reference sets kept
SHADER_DIRS = ("shaders", "shaders_vk")   # install-relative; every file under them is hashed by content
ENABLE_ENV = "MC2_CAPTURE_REUSE"
OPTION_FILES = ("options.cfg", "system.cfg", "prefs.cfg")
HARNESS = ("tools/visual_verify/capture.py", "scripts/run_visual_capture.py", "tools/visual_verify/ab.py")


def _sha(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def gpu_identity() -> str | None:
    """GPU name + driver version + date, or None when the host cannot say (no reuse then)."""
    try:
        sys_path = Path(__file__).resolve().parent
        import sys
        sys.path.insert(0, str(sys_path))
        from smoke_lib import crash_evidence
        info = crash_evidence._gpu_info(20)
    except Exception:
        return None
    return json.dumps(info, sort_keys=True) if info else None


def enabled() -> bool:
    return os.environ.get(ENABLE_ENV, "1") != "0"


def deployed_bytes(install: Path) -> dict | None:
    """relpath -> sha256 of the shader files and root DLLs actually on disk, or None when there are no shaders (cannot be identified)."""
    out = {}
    for d in SHADER_DIRS:
        for p in sorted((install / d).rglob("*")) if (install / d).is_dir() else []:
            if p.is_file():
                out[p.relative_to(install).as_posix()] = _sha(p)
    if not any(k.split("/", 1)[0] in SHADER_DIRS for k in out):
        return None
    for p in sorted(install.glob("*.dll")):
        out[p.name] = _sha(p)
    return out


def _arg(command: list, flag: str) -> str | None:
    return command[command.index(flag) + 1] if flag in command and command.index(flag) + 1 < len(command) else None


def identity(command: list, root: Path, gpu: str | None = None, env=None) -> dict | None:
    """The reference-run identity for a capture command, or None when any component cannot be established."""
    install = _arg(command, "--install")
    bookmarks = _arg(command, "--bookmarks")
    if not install or not bookmarks:
        return None
    install = Path(install)
    manifest = install / ".deployed_manifest.csv"
    exe = install / "mc2.exe"
    gpu = gpu_identity() if gpu is None else gpu
    if not (manifest.is_file() and exe.is_file() and gpu):
        return None
    rows = []
    for line in manifest.read_text(encoding="utf-8", errors="replace").splitlines()[2:]:
        parts = line.split(",")
        if len(parts) >= 3:
            rows.append(",".join(parts[:3]))
    try:
        poses = {}
        for name in bookmarks.split(","):
            hits = list((root / "tests").rglob(name + ".json")) + list(install.rglob(name + ".json"))
            poses[name] = sorted({_sha(p) for p in hits}) if hits else []
        helpers = [p for d in ("scripts/smoke_lib", "tools/visual_verify") for p in (root / d).rglob("*.py")]
        harness = {h: _sha(root / h) for h in HARNESS}
        harness.update({p.relative_to(root).as_posix(): _sha(p) for p in helpers})
        options = {f: _sha(install / f) for f in OPTION_FILES if (install / f).is_file()}
        exe_sha = _sha(exe)
        files = deployed_bytes(install)
    except OSError:
        return None
    if files is None:
        return None
    if not all(poses.values()):
        return None   # a pose that cannot be hashed cannot be identified
    return {
        "exe": exe_sha,
        "deployed": hashlib.sha256("\n".join(sorted(rows)).encode()).hexdigest(),
        "shader_bytes": files,
        "options": options,
        "poses": poses,
        "args": [a for i, a in enumerate(command[2:], 2) if a not in ("--out", "--install") and command[i - 1] not in ("--out", "--install")],
        "harness": harness,
        "env": {k: v for k, v in sorted((os.environ if env is None else env).items()) if k.startswith("MC2_") and k not in ("MC2_GATE_BASE", "MC2_CAPTURE_REUSE", "MC2_GATE_REASON", "MC2_GATE_TIER", "MC2_SMOKE_ARTIFACT_ROOT", "MC2_LANE_SCRATCH")},
        "gpu": gpu,
    }


def key_of(ident: dict) -> str:
    return hashlib.sha256(json.dumps(ident, sort_keys=True).encode()).hexdigest()


def verify_artifacts(directory: Path) -> str | None:
    """None when the capture set is whole and every primary PNG hashes to what the sweep recorded, else the reason it is not."""
    try:
        manifest = json.loads((directory / "capture_run.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return "capture_run.json missing or unreadable"
    poses = manifest.get("poses") or []
    if not poses or manifest.get("n_captured") != manifest.get("n_requested") or manifest.get("n_captured") != len(poses):
        return "sweep incomplete"
    for pose in poses:
        png, want = pose.get("primary_png"), pose.get("primary_sha256")
        if not png or not want:
            return f"{pose.get('bookmark')}: no primary png recorded"
        path = directory / Path(pose.get("pose_dir", "")).name / Path(png).name   # the set's own copy, never the original it was cut from
        if not path.is_file() or path.stat().st_size == 0 or _sha(path) != want:
            return f"{pose.get('bookmark')}: png missing, empty or hash differs"
    return None


def _copy(src: Path, dst: Path) -> None:
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst)


def available(command: list, root: Path, cache: Path = CACHE_ROOT, gpu: str | None = None, env=None) -> Path | None:
    """Read-only lookup of a matching, verified reference set."""
    if not enabled():
        return None
    ident = identity(command, root, gpu, env)
    if not _arg(command, "--out") or ident is None:
        return None
    hit = cache / key_of(ident)
    if not hit.is_dir() or verify_artifacts(hit) is not None:
        return None
    return hit


def restore(command: list, root: Path, cache: Path = CACHE_ROOT, gpu: str | None = None, env=None) -> str | None:
    """Copy a matching verified reference set to the command's --out. Returns a one-line note on reuse, None on a miss."""
    hit = available(command, root, cache=cache, gpu=gpu, env=env)
    if hit is None:
        return None
    out = _arg(command, "--out")
    _copy(hit, Path(out))
    # Poses record absolute paths inside the stored set; the restored copy must verify in its own place too.
    manifest_path = Path(out) / "capture_run.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for pose in manifest.get("poses", []):
        pose["pose_dir"] = str(Path(out) / Path(pose["pose_dir"]).name)
        if pose.get("primary_png"):
            pose["primary_png"] = str(Path(pose["pose_dir"]) / Path(pose["primary_png"]).name)
    manifest["reused_from_identity"] = hit.name
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    if verify_artifacts(Path(out)) is not None:
        shutil.rmtree(out, ignore_errors=True)
        return None
    return f"capture-before reused: identity {hit.name[:16]} (exe, deployed files, options, poses, harness, GPU/driver all match)"


def store(command: list, root: Path, cache: Path = CACHE_ROOT, gpu: str | None = None, env=None) -> bool:
    """Keep a freshly verified reference set under its identity (newest KEEP sets survive)."""
    out = _arg(command, "--out")
    if not enabled():
        return False
    ident = identity(command, root, gpu, env)
    if not out or ident is None or verify_artifacts(Path(out)) is not None:
        return False
    cache.mkdir(parents=True, exist_ok=True)
    _copy(Path(out), cache / key_of(ident))
    sets = sorted((p for p in cache.iterdir() if p.is_dir()), key=lambda p: p.stat().st_mtime, reverse=True)
    for old in sets[KEEP:]:
        shutil.rmtree(old, ignore_errors=True)
    return True
