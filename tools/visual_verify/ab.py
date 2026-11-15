#!/usr/bin/env python3
"""VK before/after A/B: capture two builds on the Vulkan backend, then montage (A|B with a FLIP error map below) + crops + diff heatmap + FLIP/MAD table.

A is the left/"before" image, B the right/"after" one. Pick the two builds one way:
  --install-a DIR --install-b DIR     two installs (each has its own mc2.exe + data)
  --exe-a EXE --exe-b EXE [--install DIR]   two exes run against one install's data (staged by hard links; DLLs come from --install)
Usage:
  py -3 tools/visual_verify/ab.py --bookmarks a,b (--install-a DIR --install-b DIR | --exe-a EXE --exe-b EXE) [--env K=V ...]
        [--crop x0,y0,x1,y1 ...] [--out-root DIR]
  py -3 tools/visual_verify/ab.py --compare-only DIR_A DIR_B [--crop ...] [--out-root DIR]
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
# OWNER RULE: no window for any child process (hidden_kwargs: CREATE_NO_WINDOW + SW_HIDE).
from smoke_lib import nofocus  # noqa: E402
import mc2_env_pin as env_pin  # noqa: E402
import console_child  # noqa: E402  (windowless console children; scripts/console_child.py)
import asset_parity  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parent))
import pixel_hist  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cooked_outputs  # noqa: E402  (tools/cooked_outputs.py: the PC-cooked output manifest)

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEFAULT_INSTALL = "A:/Games/mc2-opengl/mc2-win64-v0.4"
THRESH = 24


def pin_stock_overlay_concrete(env):
    """Both sides of an A/B run with the same StockOverlayConcrete (default 0 = new concrete) regardless of either install's options.cfg; an explicit
    StockOverlayConcrete in --env (MC2_OPTIONS_OVERRIDE=StockOverlayConcrete=1) wins. An exe that predates the key logs and ignores the entry."""
    env = list(env)
    for i, kv in enumerate(env):
        if kv.startswith("MC2_OPTIONS_OVERRIDE="):
            if "stockoverlayconcrete" not in kv.lower():
                env[i] = kv + ",StockOverlayConcrete=0"
            return env
    return env + ["MC2_OPTIONS_OVERRIDE=StockOverlayConcrete=0"]


MANIFEST = ".deployed_manifest.csv"
EXECUTABLES = {"mc2.exe", "mc2", "mc2.elf"}


def payload_executable(files, install=None, windows=None):
    """The game executable a manifest names. Exactly one of EXECUTABLES -> it. Zero or several (a Windows-seeded box install can
    list mc2.exe and mc2, or a manifest can omit it): prefer the platform's name, and with `install` the one that exists on disk."""
    names = EXECUTABLES.intersection(files)
    if len(names) == 1:
        return next(iter(names))
    if not names and install is None:
        raise ValueError("payload must identify exactly one game executable")
    windows = (os.name == "nt") if windows is None else windows
    cands = sorted(names or EXECUTABLES)
    if install is not None:
        cands = [n for n in cands if (Path(install) / n).is_file()]
    pref = "mc2.exe" if windows else "mc2"
    if pref in cands:
        return pref
    if len(cands) == 1:
        return cands[0]
    raise ValueError("payload must identify exactly one game executable")


def game_executable(install):
    files, _ = read_manifest(install)
    return Path(install) / payload_executable(files, install)


def read_manifest(install):
    """{relpath: sha256} from <install>/.deployed_manifest.csv, plus the src_commit recorded for mc2.exe (or None); ({}, None) when absent."""
    files, exe_commit = {}, None
    try:
        with open(Path(install) / MANIFEST, newline="", encoding="utf-8") as f:
            for row in csv.reader(f):
                if len(row) >= 4 and row[0] not in ("manifest_version", "relpath"):
                    files[row[0]] = row[1]
                    if row[0] in EXECUTABLES:
                        exe_commit = row[3] or None
    except OSError:
        pass
    return files, exe_commit


def payload_identity(install):
    """Verify the executable and every deployed SPIR-V byte before attributing captures."""
    install = Path(install)
    files, commit = read_manifest(install)
    exe = payload_executable(files, install)
    payload ={p: sha for p, sha in files.items()
               if p == exe or (p.startswith("shaders/vulkan/") and p.endswith(".spv"))}
    actual = {p.relative_to(install).as_posix() for p in (install / "shaders/vulkan").rglob("*.spv")}
    if not re.fullmatch(r"[0-9a-f]{40}", commit or "") or not actual or actual != set(payload) - {exe}:
        raise ValueError(f"{install}: unknown or incomplete payload manifest identity")
    for rel, expected in payload.items():
        with (install / rel).open("rb") as stream:
            got = hashlib.file_digest(stream, "sha256").hexdigest()
        if got != expected:
            raise ValueError(f"{install}: manifest mismatch for {rel}: {got} != {expected} "
                             "(stale manifest: the file was replaced after the manifest was written; redeploy through box_run "
                             "build, which rewrites .deployed_manifest.csv from the installed files, instead of editing the install)")
    fingerprint = exe_commit(install / exe)
    if fingerprint and not commit.startswith(fingerprint):
        raise ValueError(f"{install}: executable commit mismatches manifest")
    _, asset_commit = asset_parity.manifest_assets(install)
    declared = asset_parity.intentional_changes(asset_commit, ROOT)
    # Stamp the declared bytes actually captured, including authored assets outside the deploy inventory.
    for rel, sha in declared.items():
        if asset_parity._sha(install / rel) != sha:
            raise ValueError(f"{install}: intentional asset hash mismatch: {rel}")
        print(f"[ab] INTENTIONAL ASSET ACCEPTED {rel} sha256={sha}", flush=True)
    files.update(declared)
    return dict(commit=commit, asset_commit=asset_commit, payload=payload, manifest=files, intentional_asset_changes=declared)


def stamp_identity(directory, identity):
    path = directory / "capture_run.json"
    sidecar = json.loads(path.read_text()) if path.exists() else {}
    sidecar["install_identity"] = identity
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(sidecar, indent=2), encoding="utf-8")


def captured_identity(directory):
    try:
        sidecar = json.loads((directory / "capture_run.json").read_text())
        identity = sidecar["install_identity"]
        payload = identity["payload"]
        exe = payload_executable(payload)
        expected = {p: sha for p, sha in identity["manifest"].items()
                    if p in EXECUTABLES or (p.startswith("shaders/vulkan/") and p.endswith(".spv"))}
        if not re.fullmatch(r"[0-9a-f]{40}", identity["commit"]) or len(payload) < 2:
            raise ValueError("incomplete payload identity")
        if payload != expected or any(not re.fullmatch(r"[0-9a-f]{64}", sha) for sha in payload.values()):
            raise ValueError("mismatched payload identity")
        captured_exe = sidecar.get("identity", {}).get("exe", {}).get("sha256")
        if captured_exe and captured_exe != payload[exe]:
            raise ValueError("capture executable mismatches payload identity")
        fingerprint = sidecar.get("identity", {}).get("exe", {}).get("embedded_fingerprint", {})
        if fingerprint.get("present") and not identity["commit"].startswith(fingerprint.get("sha") or "unknown"):
            raise ValueError("capture executable commit mismatches payload identity")
        return identity
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as exc:
        raise ValueError(f"{directory}: unknown or mismatched capture install identity: {exc}") from exc


def exe_commit(exe):
    """Commit sha embedded in an exe (build fingerprint), or None."""
    try:
        from exe_fingerprint import extract
        fp = extract(exe)
        sha = fp.get("sha") if fp.get("present") else None
        return sha if sha and sha != "unknown" else None
    except Exception:  # noqa: BLE001 -- an unreadable exe is "unknown", reported by the caller
        return None


def install_commit(install):
    """(commit, how) of an install: its mc2.exe fingerprint, else the manifest's mc2.exe row; (None, why) when unknown."""
    try:
        c = exe_commit(game_executable(install))
    except ValueError:
        return None, "missing or ambiguous payload executable"
    if c:
        return c, "exe fingerprint"
    c = read_manifest(install)[1]
    return (c, MANIFEST) if c else (None, f"no fingerprint in {install}/mc2.exe and no mc2.exe row in {MANIFEST}")


def git_ok(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True,
                          **console_child.child_kwargs(redirected=True)).returncode == 0


def commit_order_problem(older, newer, what_older, what_newer):
    """None when `older` is an ancestor of (or equal to) `newer`; else the exact mismatch."""
    if not older or not newer:
        return f"{what_older if not older else what_newer}: commit unknown"
    for c, w in ((older, what_older), (newer, what_newer)):
        if not git_ok("cat-file", "-e", c + "^{commit}"):
            return f"{w} commit {c[:12]} is not in this repository"
    if not git_ok("merge-base", "--is-ancestor", older, newer):
        return f"{what_older} {older[:12]} is not an ancestor of {what_newer} {newer[:12]}"
    return None


SHADER_BUILD_DIRS = ("shaders/vulkan/", "shaders/include/", "shaders/spv/")


def _data_mismatch_entries(ma, mb):
    """[(relpath, line)] for every data file that differs between manifests A and B, ignoring the build under test: mc2.exe,
    everything under shaders/vulkan/ (the .spv and the sources they compile from ship together with the exe) and the build
    outputs that never change a capture's pixels (mc2.pdb pairs with mc2.exe; mc2_asset_viewer.exe is not launched by a
    capture). W135: the gate's capture-before (published exe) vs capture-after (candidate) differed only in mc2.pdb/asset
    viewer and failed; W136: a window that edits shaders/vulkan sources (.frag, include/*.glsl) failed the same way.
    W137: shaders/include (*.hglsl) and shaders/spv (prebuilt packages) ship with the exe the same way. Only these three
    deployed shader dirs are exempt; any other file under shaders/ (data, fixtures) still refuses."""
    build_outputs = EXECUTABLES | {"mc2.pdb", "mc2_asset_viewer.exe", "mc2_asset_viewer"}
    skip = lambda p: p in build_outputs or p.startswith(SHADER_BUILD_DIRS)
    ma = {k: v for k, v in ma.items() if not skip(k)}
    mb = {k: v for k, v in mb.items() if not skip(k)}
    entries = [(k, f"only in A: {k}") for k in sorted(ma.keys() - mb.keys())]
    entries += [(k, f"only in B: {k}") for k in sorted(mb.keys() - ma.keys())]
    entries += [(k, f"differs: {k} (A {ma[k][:12]} vs B {mb[k][:12]})") for k in sorted(ma.keys() & mb.keys()) if ma[k] != mb[k]]
    return entries


def manifest_mismatches(ma, mb):
    """The human-readable lines of _data_mismatch_entries (one per differing data file)."""
    return [line for _, line in _data_mismatch_entries(ma, mb)]


def candidate_mismatches(ma, mb, commit, install_b=None, baseline=None):
    """Candidate-only, exact-hash declarations; auxiliary binaries require baseline or declared bytes.

    An absent auxiliary row in B remains a mismatch. No blanket executable/DLL exclusion.
    """
    declared = asset_parity.intentional_changes(commit, ROOT)
    mb = dict(mb)
    if install_b is not None:
        for rel, sha in declared.items():
            if asset_parity._sha(Path(install_b) / rel) != sha:
                raise ValueError(f"intentional asset hash mismatch: {rel}")
        mb.update(declared)
    accepted = {rel for rel, sha in declared.items() if mb.get(rel) == sha}
    baseline = Path(baseline) if baseline else asset_parity.default_ref()
    for rel in asset_parity.AUXILIARY:
        if (baseline / rel).is_file() and mb.get(rel) == asset_parity._sha(baseline / rel):
            if install_b is None or asset_parity._sha(Path(install_b) / rel) == mb[rel]:
                accepted.add(rel)
    remaining = []
    for rel, line in _data_mismatch_entries(ma, mb):
        if rel in accepted:
            print(f"[ab] ACCEPTED candidate asset {rel} sha256={mb[rel]} "
                  f"source={'intentional-asset-changes' if rel in declared else 'install baseline'}", flush=True)
        else:
            remaining.append((rel, line))
    return remaining


def manifest_diff_files(ma, mb):
    """The set of data relpaths that differ between manifests A and B (same scope as manifest_mismatches)."""
    return {path for path, _ in _data_mismatch_entries(ma, mb)}


def git_diff_files(older, newer):
    """Relpaths changed between commits older and newer (`git diff --name-only --no-renames older newer`); None when the diff
    cannot be computed (unknown commit, git failure) so callers fail closed instead of accepting an unverified payload.
    --no-renames: a moved file must list its old path too (W138g: icon-sharp moved hud/mcui_high7/* to hud/*, and rename
    detection hid the removed paths, so the window's own payload was refused)."""
    try:
        out = subprocess.run(["git", "-C", str(ROOT), "diff", "--name-only", "--no-renames", older, newer],
                             capture_output=True, text=True,
                             **console_child.child_kwargs(redirected=True))
    except OSError:
        return None
    if out.returncode != 0:
        return None
    return {line for line in out.stdout.splitlines() if line}


# Deploy-cooked payload: untracked files deploy_payload generates and pins by sha256 in a tracked pin file. When the window
# changed the pin file, the pinned outputs are the window's payload too, but only at the pinned bytes (W138h: stock-tree-upscale
# cooks data/tgl/128/<stem>.ktx2 from data/kit/tree-upscale.json; gitignored, so `git diff` never lists them).
COOKED_PAYLOAD_PINS = {e["pin_file"]: (lambda pins, e=e: cooked_outputs.expand_pins(e, pins)) for e in cooked_outputs.load()["pinned"]}


# Unpinned deploy-cooked outputs: gitignored, no pin file, so they are the window's payload when the window changed the cook
# script that produces them (W147: cook_grass_turf.py cooks data/terrain_layers/grass_turf_normal.ktx2). Both tables come from
# tools/cooked_outputs.json, the one list of PC-cooked outputs.
COOKED_UNPINNED_OUTPUTS = cooked_outputs.unpinned_by_cook()


def cooked_unpinned(diff_files):
    """Install relpaths of the unpinned cooked outputs whose cook script changed in the window."""
    return {path for script, outs in COOKED_UNPINNED_OUTPUTS.items() if script in diff_files for path in outs}


def cooked_unpinned_accepted(diff_files, commit, install_manifest):
    """The unpinned cooked outputs accepted as window payload: cook script changed, and, for a row that pins `sha256` in
    tools/cooked_outputs.json (read at `commit`), the capture's file must be exactly those bytes (fail closed: an unreadable manifest
    or a different hash accepts nothing for that row). Rows without a pin keep the cook-script rule."""
    changed = cooked_unpinned(diff_files)
    if not changed:
        return set()
    try:
        r = subprocess.run(["git", "-C", str(ROOT), "show", f"{commit}:tools/cooked_outputs.json"], capture_output=True, text=True,
                           **console_child.child_kwargs(redirected=True))
        rows = json.loads(r.stdout)["unpinned"] if r.returncode == 0 else None
    except (OSError, ValueError, KeyError, TypeError):
        rows = None
    if rows is None:
        return {p for p in changed if p not in cooked_outputs.pinned_unpinned_paths()}
    pins = {row["out"]: row.get("sha256") for row in rows}
    return {p for p in changed if not pins.get(p) or install_manifest.get(p) == pins[p]}


def cooked_payload(diff_files, commit):
    """{install relpath: pinned sha256} of the deploy-cooked outputs whose pin file changed in the window, read at `commit`.
    An unreadable or malformed pin file yields nothing for it (fail closed: its outputs keep refusing)."""
    out = {}
    for pin, expand in COOKED_PAYLOAD_PINS.items():
        if pin not in diff_files:
            continue
        try:
            r = subprocess.run(["git", "-C", str(ROOT), "show", f"{commit}:{pin}"], capture_output=True, text=True,
                               **console_child.child_kwargs(redirected=True))
            if r.returncode == 0:
                out.update(expand(json.loads(r.stdout)))
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            continue
    return out


EXAMPLE_MODS_SRC = "examples/mods"  # scripts/deploy_payload.py EXAMPLE_MODS_SRC: ships to <install>/mods/<id>/


def repo_path_to_install(path):
    """A repo-relative git path -> the install-relative manifest path deploy_payload ships it as. Payload paths are the repo
    paths themselves except example mods (examples/mods/<id>/... -> mods/<id>/...); the rest is unchanged."""
    if path.startswith(EXAMPLE_MODS_SRC + "/"):
        return "mods/" + path[len(EXAMPLE_MODS_SRC) + 1:]
    return path


LANE_BASE_BRANCH = "claude/nifty-mendeleev"


def git_merge_base(a, b):
    """merge-base of commits a and b, or None when it cannot be computed (unknown commit, git failure)."""
    try:
        out = subprocess.run(["git", "-C", str(ROOT), "merge-base", a, b],
                             capture_output=True, text=True,
                             **console_child.child_kwargs(redirected=True))
    except OSError:
        return None
    if out.returncode != 0:
        return None
    return out.stdout.strip() or None


def lane_base_ref():
    """The tip of the lane base branch (claude/nifty-mendeleev), or None when it cannot be resolved."""
    try:
        out = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "refs/heads/" + LANE_BASE_BRANCH],
                             capture_output=True, text=True,
                             **console_child.child_kwargs(redirected=True))
    except OSError:
        return None
    if out.returncode != 0:
        return None
    return out.stdout.strip() or None


def exe_a_base_check(commit_a, commit_b):
    """(warning line or None, {exe_a, lane_base} or None) when the A exe is not built from the lane base (the merge-base of
    B and the lane base branch). A clean A/B has the A exe built exactly at the lane base; anything else may be cross-base."""
    base_ref = lane_base_ref()
    lane_base = git_merge_base(commit_b, base_ref) if (commit_b and base_ref) else None
    if commit_a and lane_base and commit_a != lane_base:
        return (f"exe A is {commit_a}, lane base is {lane_base}: differences may be cross-base",
                {"exe_a": commit_a, "lane_base": lane_base})
    return None, None


def base_problems(install_a=None, install_b=None, exe_a=None, exe_b=None, install=None):
    """Reasons the two sides are not a same-base A/B. Install mode: A's commit must be an ancestor of B's and the data/shader
    manifests must match. Exe-swap mode: each exe's commit must descend from the install's deployed commit."""
    problems = []
    if install_a and install_b:
        ca, how_a = install_commit(install_a)
        cb, how_b = install_commit(install_b)
        p = commit_order_problem(ca, cb, f"A ({install_a}; {how_a})", f"B ({install_b}; {how_b})")
        if p:
            problems.append(p)
        (ma, _), (mb, _) = read_manifest(install_a), read_manifest(install_b)
        if not ma or not mb:
            problems.append(f"{MANIFEST} missing in " + " and ".join(d for d, m in ((install_a, ma), (install_b, mb)) if not m))
        else:
            diffs = [line for _, line in candidate_mismatches(ma, mb, cb, install_b, install)]
            if diffs:
                problems.append(f"{len(diffs)} data/shader manifest mismatch(es) besides mc2.exe and shaders/vulkan/*.spv: "
                                + "; ".join(diffs[:20]) + (" ..." if len(diffs) > 20 else ""))
    elif exe_a and exe_b:
        base, how = install_commit(install)
        for name, exe in (("A", exe_a), ("B", exe_b)):
            p = commit_order_problem(base, exe_commit(exe), f"install {install} ({how})", f"exe {name} {exe}")
            if p:
                problems.append(p)
    return problems


def capture_argv(install, bookmarks, out, env_extra, hud=False, allow_asset_drift=False):
    """The capture.py command line. --clean hides the HUD, so it is omitted with hud=True (the default capture is clean)."""
    cmd = [sys.executable, str(HERE / "capture.py"), "--install", install,
           "--bookmarks", bookmarks, "--out", str(out)]
    if not hud:
        cmd.append("--clean")
    if allow_asset_drift:
        cmd.append("--allow-asset-drift")
    for e in env_extra:
        cmd += ["--env", e]
    return cmd


def run_capture(install, bookmarks, out, env_extra, hud=False, allow_asset_drift=False):
    cmd = capture_argv(install, bookmarks, out, env_extra, hud, allow_asset_drift)
    print("[ab] " + " ".join(cmd), flush=True)
    # background-launch: allow streams capture.py progress to the caller's console; inherits it (no new window when the parent has one)
    proc = subprocess.Popen(cmd, env=dict(os.environ, MC2_CURSOR_NO_PARK="1"))  # background-launch: allow same console stream as the subprocess.call it replaces
    try:
        rc = proc.wait()
    except BaseException:
        # Interrupted: end only the capture.py this call launched (and its tree), never another lane's game.
        if sys.platform == "win32":
            subprocess.call(["taskkill", "/F", "/T", "/PID", str(proc.pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)  # background-launch: allow console tool, output discarded
        else:
            proc.kill()
        raise
    if rc != 0:
        sys.exit(f"[ab] capture failed rc={rc}: {out}")


def find_png(d: Path, bm: str):
    p = d / bm / f"{bm}.png"
    if p.exists():
        return p
    c = [x for x in sorted((d / bm).glob("*.png")) if not x.stem.endswith("_dup")]
    return c[0] if c else None


def load(p):
    return Image.open(p).convert("RGB")


def side(a: Image.Image, b: Image.Image, maxw: int) -> Image.Image:
    w = a.width + b.width
    if w > maxw:
        s = maxw / w
        a = a.resize((max(1, int(a.width * s)), max(1, int(a.height * s))), Image.LANCZOS)
        b = b.resize((max(1, int(b.width * s)), max(1, int(b.height * s))), Image.LANCZOS)
    m = Image.new("RGB", (a.width + b.width, max(a.height, b.height)))
    m.paste(a, (0, 0))
    m.paste(b, (a.width, 0))
    return m


def heat(d: np.ndarray) -> Image.Image:
    v = np.clip(d.max(axis=2) * 4.0, 0, 255).astype(np.uint8)  # 4x gain
    r = v
    g = np.clip(v.astype(int) * 2 - 255, 0, 255).astype(np.uint8)
    b = np.zeros_like(v)
    return Image.fromarray(np.stack([r, g, b], -1))


def flip_map(a: Image.Image, b: Image.Image):
    """NVIDIA FLIP (LDR) error map, HxW float in [0,1]; None when flip-evaluator is not installed (pip install flip-evaluator==1.7)."""
    try:
        import flip_evaluator
    except ImportError:
        print("[ab] flip-evaluator not installed: FLIP columns are n/a", flush=True)
        return None
    f32 = lambda im: np.asarray(im, dtype=np.float32) / 255.0
    return flip_evaluator.evaluate(f32(a), f32(b), "LDR", inputsRGB=True, applyMagma=False)[0][:, :, 0]


def flip_image(fm: np.ndarray) -> Image.Image:
    """Magma-like ramp (black, purple, orange, pale yellow) of the FLIP map; FLIP 1.0 = full scale, no gain."""
    v = np.clip(fm, 0, 1)
    r = np.clip(v * 3.0, 0, 1)
    g = np.clip(v * 3.0 - 1.2, 0, 1)
    bl = np.clip(np.where(v < 0.5, v * 2.0, 1.2 - v * 2.0), 0, 1) * 0.8 + np.clip(v * 3.0 - 2.2, 0, 1) * 0.6
    return Image.fromarray((np.stack([r, g, np.clip(bl, 0, 1)], -1) * 255).astype(np.uint8))


def crop_box(img, c):
    w, h = img.size
    return (int(c[0] * w), int(c[1] * h), max(int(c[2] * w), int(c[0] * w) + 1),
            max(int(c[3] * h), int(c[1] * h) + 1))


def capture_gpus(cap_dir: Path) -> list:
    """The GPU device names a capture dir's manifests logged (run_visual_capture records them), sorted."""
    names = set()
    for mf in Path(cap_dir).glob("*_capture_manifest.json"):
        try:
            m = json.loads(mf.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        rec = m.get("gpu") or (m.get("extra") or {}).get("gpu") or {}
        names.update(rec.get("logged") or [])
    return sorted(names)


def usable_png(p) -> bool:
    """True for an existing, non-empty PNG that PIL can decode (a 0-byte or truncated capture is not evidence)."""
    try:
        if not p or Path(p).stat().st_size == 0:
            return False
        with Image.open(p) as im:
            im.load()
        return True
    except (OSError, ValueError):
        return False


def evidence_problems(a_dir: Path, b_dir: Path, bookmarks) -> list:
    """One line per bookmark whose A or B image is missing, empty or undecodable. An empty bookmark list is itself a
    problem: a comparison of nothing must never exit 0."""
    if not bookmarks:
        return ["no bookmarks were compared"]
    return [f"{bm}: side {name} has no usable png" for bm in bookmarks
            for name, d in (("A", a_dir), ("B", b_dir)) if not usable_png(find_png(d, bm))]


def discover_bookmarks(a_dir: Path, b_dir: Path) -> list:
    """Union of the capture folders on either side, so a bookmark one side lacks is reported instead of silently skipped."""
    names = set()
    for d in (a_dir, b_dir):
        if d.is_dir():
            names |= {p.name for p in d.iterdir() if p.is_dir()}
    return sorted(names)


def stamp_cross_base(img: Image.Image, text: str = "CROSS-BASE") -> Image.Image:
    """Burn a red banner (CROSS-BASE and/or CROSS-DATA) into the top-left of a montage (not a same-base/same-data A/B)."""
    from PIL import ImageDraw, ImageFont
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.load_default(size=max(16, img.width // 40))
    except TypeError:
        font = ImageFont.load_default()
    box = d.textbbox((8, 8), text, font=font)
    d.rectangle((box[0] - 4, box[1] - 4, box[2] + 4, box[3] + 4), fill=(200, 0, 0))
    d.text((8, 8), text, fill=(255, 255, 255), font=font)
    return img


def compare(gl_dir: Path, vk_dir: Path, out: Path, crops, bookmarks=None, cross_base=False, data_delta_accepted=None,
            exe_a_base_mismatch=None, exe_a_swap=None, hud=False, data_parity=None):
    """Returns the compared rows; compare.problems lists missing/empty evidence (callers exit nonzero on it)."""
    if not bookmarks:
        bookmarks = discover_bookmarks(gl_dir, vk_dir)
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    gpu_a, gpu_b = capture_gpus(gl_dir), capture_gpus(vk_dir)
    ph_cfg = pixel_hist.load_thresholds()
    for bm in bookmarks:
        pg, pv = find_png(gl_dir, bm), find_png(vk_dir, bm)
        if not usable_png(pg) or not usable_png(pv):
            print(f"[ab] {bm}: missing or empty png (a={pg} b={pv})")
            continue
        a, b = load(pg), load(pv)
        size_mismatch = None
        if a.size != b.size:
            size_mismatch = {}
            print(f"[ab] WARNING {bm}: image size mismatch A={a.size} B={b.size}; resizing B (metrics unreliable)", flush=True)
            b = b.resize(a.size, Image.LANCZOS)
        A, B = np.asarray(a).astype(np.int16), np.asarray(b).astype(np.int16)
        D = np.abs(A - B)
        fm = flip_map(a, b)
        # pixel-count verdict next to MAD (reported only, never a gate input yet)
        ph_res = pixel_hist.evaluate(np.asarray(a), np.asarray(b), pixel_hist.resolve_thresholds(ph_cfg, bm, gpu_b))
        row = {"bookmark": bm, "size": list(a.size), "pixel_hist": ph_res,
               "flip_mean": float(fm.mean()) if fm is not None else None,
               "flip_p99": float(np.percentile(fm, 99)) if fm is not None else None,
               "mad": float(D.mean()),
               "pct_diff_gt24": float((D.max(axis=2) > THRESH).mean() * 100),
               "size_mismatch": size_mismatch,
               "crops": []}
        bd = out / bm
        bd.mkdir(exist_ok=True)
        m = side(a, b, 1800)
        if fm is not None:  # FLIP error map under the A|B pair, scaled to the montage width
            fi = flip_image(fm)
            fi = fi.resize((m.width, max(1, int(fi.height * m.width / fi.width))), Image.BILINEAR)
            mm = Image.new("RGB", (m.width, m.height + fi.height))
            mm.paste(m, (0, 0))
            mm.paste(fi, (0, m.height))
            m = mm
            flip_image(fm).save(bd / "flip.jpg", quality=85)
        cross_data = bool(data_parity and data_parity.get("cross_data"))
        if cross_base or cross_data:
            m = stamp_cross_base(m, " + ".join(t for t, on in (("CROSS-BASE", cross_base), ("CROSS-DATA", cross_data)) if on))
        m.save(bd / "montage.jpg", quality=85)
        h = heat(D.astype(np.uint8))
        if h.width > 1200:
            h = h.resize((1200, int(h.height * 1200 / h.width)), Image.BILINEAR)
        h.save(bd / "diff.jpg", quality=85)
        for i, c in enumerate(crops):
            box = crop_box(a, c)
            ca, cb = a.crop(box), b.crop(box)
            m = float(D[box[1]:box[3], box[0]:box[2]].mean())
            row["crops"].append({"crop": list(c), "mad": m,
                                 "flip_mean": float(fm[box[1]:box[3], box[0]:box[2]].mean()) if fm is not None else None})
            side(ca, cb, 1600).save(bd / f"crop{i}.jpg", quality=85)
        rows.append(row)
    # FLIP leads (perceptual: 0 identical, 1 worst); MAD and %>24 stay for continuity. No pass threshold here.
    hdr = "bookmark | FLIP mean | FLIP p99 | MAD | %>24" + "".join(f" | crop{i} (MAD/FLIP)" for i in range(len(crops)))
    print(hdr)
    for r in rows:
        fmean = "n/a" if r["flip_mean"] is None else f"{r['flip_mean']:.4f}"
        fp99 = "n/a" if r["flip_p99"] is None else f"{r['flip_p99']:.4f}"
        print(f"{r['bookmark']} | {fmean} | {fp99} | {r['mad']:.4f} | {r['pct_diff_gt24']:.2f}"
              + "".join(f" | {c['mad']:.2f}/" + ("n/a" if c["flip_mean"] is None else f"{c['flip_mean']:.4f}") for c in r["crops"]))
    for r in rows:  # separate lines (no '|'): the gate parses the table above by columns
        print(f"[pixel_hist] {r['bookmark']}: {pixel_hist.summary_line(r['pixel_hist'])}")
    if gpu_a and gpu_b and gpu_a != gpu_b:
        print(f"[ab] WARNING: the two sides ran on different GPUs ({gpu_a} vs {gpu_b}); the diff is not a clean A/B")
    problems = evidence_problems(gl_dir, vk_dir, bookmarks)
    if problems:
        print("[ab] FAIL: " + "; ".join(problems))
    (out / "summary.json").write_text(json.dumps(
        {"gl": str(gl_dir), "vk": str(vk_dir), "threshold": THRESH, "cross_base": cross_base, "hud": hud,
         "data_delta_accepted": list(data_delta_accepted or []), "exe_a_base_mismatch": exe_a_base_mismatch,
         "exe_a_swap": exe_a_swap, "data_parity": data_parity,
         "cross_data": bool(data_parity and data_parity.get("cross_data")),
         "rows": rows, "problems": problems,
         "gpu": {"a": gpu_a, "b": gpu_b, "same": gpu_a == gpu_b}}, indent=2))
    print(f"[ab] wrote {out}")
    compare.problems = problems
    return rows




# Files the game WRITES at run time (cwd = the install dir): a hardlinked stage would write through to the source install and to
# the other side, so they are real copies. Found from the game's write sites (rg fopen/ofstream/CreateFile in GameOS/ and code/):
#   options.cfg / prefs*.cfg / system.cfg  code/prefs_io.cpp:50-59 (options.cfg.tmp, chmod, rename), code/mechcmd2.cpp:320
#   imgui.ini                              3rdparty/imgui/imgui.cpp:1444,4257 (SaveIniSettingsToDisk, in place)
#   *.log / *.txt / *.csv / *.json(l)      engine logs and diag dumps written in cwd
#   saved_bookmarks/                       the capture harness reads and rewrites the bookmark json
#   cache/vulkan/                          GameOS/gameos/vk_render_infra.cpp:206-209 (pipeline cache .bin, rewritten on exit)
#   debug_state/, crashes/                 GameOS/gameos/diagnostic_trace.cpp:176, gos_crashpad.cpp:65 (output only: staged empty)
# Saves live under the user data dir (code/mechcmd2.cpp:445), not the install. cache/content-model (5 GB) is content-addressed and
# only written on a miss, so it stays hardlinked.
GAME_WRITTEN_SUFFIXES = {".cfg", ".ini", ".txt", ".log", ".csv", ".json", ".jsonl", ".old", ".tmp", ".bak"}
# Top-level binaries/archives are memory-mapped by any running game; Windows refuses to delete ANY hardlink of a mapped file
# (WinError 5), so a hardlinked stage could not be removed while another lane's game ran (11-10: avcodec-61.dll, textures.fst).
# Copied instead (~400 MB per side).
MAPPED_SUFFIXES = {".dll", ".exe", ".fst"}
GAME_WRITTEN_COPY_PREFIXES = ("saved_bookmarks/", "cache/vulkan/")
GAME_WRITTEN_EMPTY_DIRS = ("debug_state", "crashes")
# Data caches the game truncates in place via File::create (a hardlink writes through into the source install):
#   data/textures/*.lst, *.txm   mclib/terrtxm.cpp:1496 (.lst) and :1673 (.txm) when quickLoad is false (empty .lst / missing .txm, :133-155)
#   data/tgl/*.agl               mclib/msl_anim.cpp:568 SaveBinaryCopy (File::create) on an .agl version mismatch or when the .ase exists
# Other File::create sites under data/ are not reachable by a staged run: editor-only (terrtxm2.cpp:374 saveWaterDetail via
# editor/EditorData_save.cpp:573; terrtxm2_bake.cpp:139,198 height-map regen), a never-called msl_load.cpp:437 SaveBinaryCopy, and
# texture_assets.cpp:331 save (caller-supplied path).
GAME_REWRITTEN_DATA = (("data/textures/", {".lst", ".txm"}), ("data/tgl/", {".agl"}))
DISK_GUARD_FREE_GB = 30
GB = 1 << 30


def stage_kind(rel: str) -> str:
    """'skip' (output-only dir, staged empty), 'copy' (game-written: a real copy) or 'link' (shared data) for install relpath `rel`."""
    rel = rel.replace("\\", "/")
    if rel.split("/", 1)[0] in GAME_WRITTEN_EMPTY_DIRS:
        return "skip"
    if rel.startswith(GAME_WRITTEN_COPY_PREFIXES) or ("/" not in rel and Path(rel).suffix.lower() in GAME_WRITTEN_SUFFIXES | MAPPED_SUFFIXES):
        return "copy"
    low = rel.lower()
    if any(low.startswith(prefix) and Path(low).suffix in suffixes for prefix, suffixes in GAME_REWRITTEN_DATA):
        return "copy"
    return "link"


def _walk_files(install: Path):
    """(relpath with /, absolute path) for every file under install that a stage carries (output-only dirs excluded)."""
    for root, _dirs, files in os.walk(install):
        rel_root = os.path.relpath(root, install).replace("\\", "/")
        for name in files:
            rel = name if rel_root == "." else f"{rel_root}/{name}"
            if stage_kind(rel) != "skip":
                yield rel, os.path.join(root, name)


def _same_volume(a: Path, b: Path) -> bool:
    try:
        return os.stat(a).st_dev == os.stat(b).st_dev
    except OSError:
        return False


def _existing_ancestor(path: Path) -> Path:
    path = Path(path)
    while not path.exists() and path.parent != path:
        path = path.parent
    return path


def estimate_stage_bytes(install: Path, stage_parent: Path) -> int:
    """Bytes one stage of `install` under `stage_parent` really writes: the game-written copies on one volume, everything across."""
    same = _same_volume(install, _existing_ancestor(stage_parent))
    return sum(os.stat(p).st_size for rel, p in _walk_files(install) if not same or stage_kind(rel) == "copy")


def disk_guard(target: Path, need_bytes: int, min_free_gb: float | None = None) -> None:
    """DISK_GUARD: refuse when the target volume would drop below min_free_gb free after `need_bytes` of staging."""
    if min_free_gb is None:
        # The 30 GB reserve is the PC drive rule. The 100 GB vast.ai box runs under its own 75% ceiling and sets
        # MC2_AB_DISK_RESERVE_GB (SLOTS.md); a 30 GB reserve there refused every A/B capture (hangar-cull-1 11-15).
        env = os.environ.get("MC2_AB_DISK_RESERVE_GB", "").strip()
        min_free_gb = float(env) if env else DISK_GUARD_FREE_GB
    probe = _existing_ancestor(target)
    free = shutil.disk_usage(probe).free
    need = min_free_gb * GB + need_bytes
    line = (f"DISK_GUARD {probe}: free {free / GB:.1f} GB, need {min_free_gb:.0f} GB reserve + "
            f"{need_bytes / GB:.2f} GB stage = {need / GB:.1f} GB")
    if free < need:
        sys.exit(f"ab.py: {line} -> refused; free space or pass --stage-root on a volume with room")
    print(f"[ab] {line} -> ok", flush=True)


def data_tree_files(install) -> dict:
    """rel -> (size, mtime seconds) for the shared data a run renders: every staged 'link' file except the exe/pdb, the
    manifest and shaders/ (those are the exe side of an A/B and are compared elsewhere)."""
    out = {}
    for rel, path in _walk_files(Path(install)):
        low = rel.lower()
        # Top-level binaries are the exe side of an A/B: Windows .exe/.pdb, and on the box the extensionless Linux `mc2` plus
        # its .so/.debug files (hangar-cull-1 11-15: Linux `mc2` was fingerprinted as data and refused every box A/B).
        top_binary = "/" not in low and (low.endswith((".exe", ".pdb", ".so", ".debug")) or Path(low).suffix == "")
        if stage_kind(rel) != "link" or low.startswith("shaders/") or rel == MANIFEST or top_binary:
            continue
        st = os.stat(path)
        out[rel] = (st.st_size, int(st.st_mtime))
    return out


def data_fingerprint(files: dict) -> str:
    h = hashlib.sha256()
    for rel in sorted(files):
        h.update(f"{rel}\t{files[rel][0]}\t{files[rel][1]}\n".encode("utf-8"))
    return h.hexdigest()


def data_parity(install_a, install_b, full_install_a=False) -> dict:
    """Compare the data trees the two sides will render -> {sha_a, sha_b, files_a, files_b, same, differing, cross_data, ...}."""
    fa, fb = data_tree_files(install_a), data_tree_files(install_b)
    differing = sorted(r for r in set(fa) | set(fb) if fa.get(r) != fb.get(r))
    _, commit = read_manifest(install_b)
    declared = asset_parity.intentional_changes(commit, ROOT)
    accepted = {}
    for rel in differing:
        path = Path(install_b) / rel
        if rel in declared and path.is_file() and asset_parity._sha(path) == declared[rel]:
            accepted[rel] = declared[rel]
        elif rel in asset_parity.AUXILIARY:
            baseline = asset_parity.default_ref() / rel
            if path.is_file() and baseline.is_file() and asset_parity._sha(path) == asset_parity._sha(baseline):
                accepted[rel] = asset_parity._sha(path)
    sa, sb = spv_digest(Path(install_a) / "shaders"), spv_digest(Path(install_b) / "shaders")
    sdiff = sorted(r for r in set(sa) | set(sb) if sa.get(r) != sb.get(r))
    return {"shaders_same": not sdiff, "shaders_differing_count": len(sdiff), "shaders_differing": sdiff[:200],
            "sha_a": data_fingerprint(fa), "sha_b": data_fingerprint(fb), "files_a": len(fa), "files_b": len(fb),
            "same": not differing, "differing": differing[:200], "differing_count": len(differing),
            "full_install_a": bool(full_install_a), "cross_data": bool(differing),
            "accepted_asset_changes": accepted, "unlisted_differing_count": len(set(differing) - accepted.keys())}


def data_parity_lines(dp: dict) -> list:
    sh = ("[ab] SHADERS: same .spv set" if dp.get("shaders_same", True) else
          f"[ab] SHADERS: differ ({dp['shaders_differing_count']} .spv; side A runs its own exe's shaders, e.g. v0.4's; first: "
          + ", ".join(dp["shaders_differing"][:5]) + ") -- shader-driven looks (vegetation, water) will differ even with the same exe")
    if dp["same"]:
        return [f"[ab] DATA: same ({dp['files_a']} files, fingerprint {dp['sha_a'][:12]})", sh]
    lines = [sh, f"[ab] DATA: differs ({dp['differing_count']} files); first {min(10, len(dp['differing']))}:"]
    lines += ["[ab]   " + r for r in dp["differing"][:10]]
    if dp["full_install_a"]:
        lines.append("[ab] !!! CROSS-DATA: side A renders its own install's data (--full-install-a); the diff mixes exe AND data changes !!!")
    return lines


def stage_tree(install: Path, dest: Path) -> dict:
    """Clone `install` into `dest`: shared data hard-linked, game-written files real copies, output-only dirs empty. Falls back
    to a plain copy (one printed line) when the volumes differ or a link fails. Returns counts."""
    install, dest = Path(install), Path(dest)
    dest.mkdir(parents=True)
    same = _same_volume(install, dest)
    stats = {"linked": 0, "copied": 0, "bytes_copied": 0, "fallback": not same}
    if not same:
        print(f"[ab] stage: {install} and {dest} are on different volumes; staging by full copy (no hardlinks)", flush=True)
    for d in GAME_WRITTEN_EMPTY_DIRS:
        (dest / d).mkdir(exist_ok=True)
    for rel, src in _walk_files(install):
        dst = dest / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if same and stage_kind(rel) == "link":
            try:
                os.link(src, dst)
                stats["linked"] += 1
                continue
            except OSError as exc:
                if not stats["fallback"]:
                    stats["fallback"] = True
                    print(f"[ab] stage: hardlink failed ({exc}); staging by copy", flush=True)
        copy_writable(src, dst)
        stats["copied"] += 1
        stats["bytes_copied"] += dst.stat().st_size
    return stats


def copy_writable(src, dst, tries: int = 5, delay: float = 0.5) -> None:
    """shutil.copy2 that retries a locked file (PermissionError while an exe or scanner still holds it) with backoff and
    leaves the copy writable: copy2 keeps the source's read-only bit and the game rewrites its own files in a stage."""
    src, dst = Path(src), Path(dst)
    for attempt in range(1, tries + 1):
        try:
            if dst.exists():
                os.chmod(dst, 0o700)
            shutil.copy2(src, dst)
            os.chmod(dst, 0o700)
            return
        except PermissionError as exc:
            if attempt == tries:
                raise
            print(f"[ab] stage: {dst.name} locked ({exc}); retry {attempt}/{tries - 1}", flush=True)
            time.sleep(delay * attempt)


def cleanup_stage(staged, keep=False) -> None:
    """Delete the staged install dirs this run created (never a user-supplied install) and their now-empty stage root."""
    stuck = []

    def onexc(func, path, _exc):
        try:
            os.chmod(path, 0o700)
            func(path)
        except OSError as exc:
            stuck.append(f"{path}: {exc}")
    for d in map(Path, staged):
        if keep:
            print(f"[ab] --keep-stage: leaving {d}", flush=True)
            continue
        if d.exists():
            shutil.rmtree(d, onexc=onexc)
            print(f"[ab] stage removed: {d}" if not stuck else
                  f"[ab] !!! stage NOT fully removed: {d} ({len(stuck)} files, first: {stuck[0]}); delete it by hand", flush=True)
        for level, parent in enumerate((d.parent, d.parent.parent)):  # the run dir, then the default _ab-stage root, each only when empty
            if level and parent.name != "_ab-stage":
                break
            try:
                parent.rmdir()
            except OSError:
                break


def _swap_spv_rows(rows, dest, shaders_parent, commit, exe_name, exe_path):
    """Manifest rows for a stage whose shaders/ was replaced from `shaders_parent`/shaders: the exe row and every shaders/vulkan
    *.spv row come from the staged bytes (rows for shaders the new set no longer ships are dropped, new ones added) so
    payload_identity verifies exactly what runs."""
    spv = {p.relative_to(dest).as_posix(): p for p in (dest / "shaders" / "vulkan").rglob("*.spv")}
    new_rows = []
    for row in rows:
        if not row:
            continue
        if row[0] == exe_name:
            row[1:4] = [hashlib.sha256(exe_path.read_bytes()).hexdigest(), str(exe_path.stat().st_size), commit]
        elif row[0] in spv:
            src = spv[row[0]]
            row[1:4] = [hashlib.sha256(src.read_bytes()).hexdigest(), str(src.stat().st_size), commit]
        elif row[0].startswith("shaders/") and not (Path(shaders_parent) / row[0]).is_file():
            continue
        new_rows.append(row)
    have = {row[0] for row in new_rows if row}
    for rel, src in sorted(spv.items()):
        if rel not in have:
            new_rows.append([rel, hashlib.sha256(src.read_bytes()).hexdigest(), str(src.stat().st_size), commit])
    return new_rows


def spv_digests(parent) -> dict:
    """{name: sha256} of <parent>/shaders/vulkan/*.spv (empty when the dir has none)."""
    d = Path(parent) / "shaders" / "vulkan"
    return {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(d.glob("*.spv"))} if d.is_dir() else {}


def shader_side_plan(install, exe_a, exe_b, shaders_a=None, shaders_b=None, commit_shaders_differ=None):
    """(shaders dir for side A or None, for B or None, log lines) for an --exe-a/--exe-b A/B. A side with no explicit
    --shaders-a/--shaders-b uses the shaders/ beside its exe when that dir holds .spv files that differ from --install's (an exe
    is built and deployed with its shaders, so they are the right ones); otherwise it loads --install's. Exits when the two sides
    would load ONE shader set although the exes' commits differ in shaders/ (`commit_shaders_differ` True): that A/B would show
    a diff of exactly 0 whatever the shaders do. REFUSED rather than guessed, since a wrong set looks like a pass."""
    inst = spv_digests(install)
    notes, picked = [], []
    for side, exe, explicit in (("A", exe_a, shaders_a), ("B", exe_b, shaders_b)):
        chosen = None
        if explicit:
            chosen = shaders_parent_of(explicit)
            notes.append(f"[ab] shaders {side}: --shaders-{side.lower()} {chosen}")
        else:
            beside = Path(exe).parent
            theirs = spv_digests(beside)
            if theirs and theirs != inst:
                chosen = beside
                notes.append(f"[ab] shaders {side}: the shaders/ beside {exe} differ from {install}'s; using them")
        picked.append(chosen)
    sa, sb = picked
    da, db = (spv_digests(x) if x else inst for x in (sa, sb))
    if da == db and commit_shaders_differ:
        sys.exit("ab.py: both A/B sides would load the SAME shader set, but the two exes' commits differ in shaders/ "
                 "(the diff would be exactly 0 whatever the shaders do). Deploy each exe with its own shaders next to it, or pass "
                 "--shaders-a DIR and --shaders-b DIR (an install dir or its shaders/ dir) so each side loads its build's shaders.")
    if not notes:
        notes.append(f"[ab] shaders: both sides load {install}'s shaders/ (the exes' commits do not differ in shaders/)")
    return sa, sb, notes


def commits_differ_in_shaders(commit_a, commit_b):
    """True/False when `git diff --name-only A B` has/has no file under shaders/; None when it cannot be computed."""
    if not commit_a or not commit_b:
        return None
    files = git_diff_files(commit_a, commit_b)
    if files is None:
        return None
    return any(f.replace(chr(92), "/").startswith("shaders/") for f in files)


def shaders_parent_of(path) -> Path:
    """--shaders-a accepts an install dir (has shaders/) or the shaders dir itself; returns the dir that holds shaders/."""
    path = Path(path)
    if (path / "shaders").is_dir():
        return path
    if path.name == "shaders" and path.is_dir():
        return path.parent
    sys.exit(f"ab.py: --shaders-a {path}: no shaders/ directory there")


def spv_digest(shaders_dir) -> dict:
    """{relative path: sha256} of every .spv under a shaders dir (empty when it does not exist)."""
    root = Path(shaders_dir)
    if not root.is_dir():
        return {}
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(root.rglob("*.spv"))}


def default_shaders_a(install: Path, exe_a: Path):
    """Side A's shaders when --shaders-a is not given: the exe swap stages A on the B install's data, so A's old exe would load
    B's new .spv (e.g. water init fails). If the exe's own install dir carries a shaders/ whose .spv set differs from the data
    install's, A defaults to that (as --shaders-a would); else None. Logs which it chose."""
    own = Path(exe_a).parent
    if (own / "shaders").is_dir() and spv_digest(own / "shaders") != spv_digest(Path(install) / "shaders"):
        print(f"[ab] side A shaders: the data install's .spv set differs from {own}/shaders; A defaults to its own "
              "(an explicit --shaders-a overrides)", flush=True)
        return own
    print("[ab] side A shaders: same .spv set as the data install (or the exe has no shaders/ beside it); A uses the install's",
          flush=True)
    return None


def stage_install(install: Path, exe: Path, dest: Path, shaders_from=None) -> Path:
    """An install clone whose mc2.exe is `exe`: shared data and DLLs are hard-linked from `install` (see stage_tree).
    `shaders_from` (an install dir or its shaders/ dir) also swaps in that build's shaders/, manifest rows rewritten."""
    if dest.exists():
        shutil.rmtree(dest)
    print(f"[ab] stage {dest.name}: {stage_tree(install, dest)}", flush=True)
    target = dest / game_executable(install).name
    if target.exists():
        target.unlink()
    shutil.copy2(exe, target)
    # The staged executable is an intentional replacement, with its own manifest row.
    manifest = dest / MANIFEST
    with manifest.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.reader(stream))
    commit = exe_commit(exe)
    if not commit:
        sys.exit(f"ab.py: unknown executable identity: {exe}")
    if len(commit) != 40:
        commit = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", commit + "^{commit}"], text=True,
                                         **console_child.child_kwargs(redirected=True)).strip()
    if shaders_from:
        sp = shaders_parent_of(shaders_from)
        if (dest / "shaders").exists():
            shutil.rmtree(dest / "shaders")
        shutil.copytree(sp / "shaders", dest / "shaders")
        rows = _swap_spv_rows(rows, dest, sp, commit, target.name, target)
        print(f"[ab] staged {dest.name}: shaders/ swapped from {sp} (manifest rewritten from the swapped bytes)", flush=True)
    else:
        for row in rows[2:]:
            if row and row[0] == target.name:
                row[1:4] = [hashlib.sha256(target.read_bytes()).hexdigest(), str(target.stat().st_size), commit]
    manifest.unlink()  # copied installs may share this inode with their source
    with manifest.open("w", newline="", encoding="utf-8") as stream:
        csv.writer(stream).writerows(rows)
    return dest


def worktree_head():
    """HEAD of the worktree this tool runs from, or None when git cannot say."""
    try:
        out = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True, text=True,
                             **console_child.child_kwargs(redirected=True))
    except OSError:
        return None
    return (out.stdout.strip() or None) if out.returncode == 0 else None


def capture_commit_report(commit_a, commit_b, head):
    """Lines printed after capture: each side's CAPTURE src_commit, and a loud warning when side B (the build under test) is not
    the worktree HEAD (a stale exe makes the whole A/B meaningless)."""
    lines = [f"[ab] CAPTURE src_commit A={commit_a} B={commit_b} (worktree HEAD {head})"]
    if head and commit_b != head:
        lines.append(f"[ab] !!! WARNING: side B src_commit {commit_b} != worktree HEAD {head} -- the B exe is STALE (not built "
                     "and deployed from this HEAD); this A/B does not test your change !!!")
    return lines


def stage_exe_swap(install: Path, exe: Path, dest: Path) -> Path:
    """A hardlink copy of `install` (the B side) whose mc2.exe/pdb and whole shaders/ dir come from `exe`'s install dir: the A
    side runs the other build's executable against B's data, so a diff isolates the executable. The manifest is rewritten from
    the swapped bytes so the payload identity check still verifies what actually runs -- an explicit, logged exemption for this
    staged side only, never a global weakening of the check for other callers."""
    install, exe, dest = Path(install), Path(exe), Path(dest)
    exe_dir = exe.parent
    if dest.exists():
        shutil.rmtree(dest)
    print(f"[ab] stage {dest.name}: {stage_tree(install, dest)}", flush=True)
    target = dest / exe.name
    if target.exists():
        target.unlink()
    shutil.copy2(exe, target)
    pdb_src = exe_dir / (exe.stem + ".pdb")
    pdb_dst = dest / (exe.stem + ".pdb")
    if pdb_src.is_file():
        if pdb_dst.exists():
            pdb_dst.unlink()
        shutil.copy2(pdb_src, pdb_dst)
    shaders_src = exe_dir / "shaders"
    shaders_dst = dest / "shaders"
    if shaders_src.is_dir():
        if shaders_dst.exists():
            shutil.rmtree(shaders_dst)
        shutil.copytree(shaders_src, shaders_dst)
    manifest = dest / MANIFEST
    with manifest.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.reader(stream))
    commit = exe_commit(exe)
    if not commit:
        sys.exit(f"ab.py: unknown executable identity: {exe}")
    if len(commit) != 40:
        commit = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", commit + "^{commit}"], text=True,
                                         **console_child.child_kwargs(redirected=True)).strip()
    new_rows = _swap_spv_rows(rows, dest, exe_dir, commit, target.name, target)
    manifest.unlink()  # copied installs may share this inode with their source
    with manifest.open("w", newline="", encoding="utf-8") as stream:
        csv.writer(stream).writerows(new_rows)
    print(f"[ab] staged A side {dest.name}: mc2.exe/pdb + shaders/ swapped from {exe_dir} "
          f"(manifest rewritten; payload identity re-verified on the swapped bytes)", flush=True)
    return dest


def seed_install(install, bookmarks, reference=DEFAULT_INSTALL):
    """A fresh lane install lacks saved_bookmarks/<bm>.json and data/textures/*.burnin.*: copy what is missing from the reference
    install (as the gate's stage step does); exit naming what is still missing so a capture never runs on the wrong data."""
    install, reference = Path(install), Path(reference)
    missing = []
    for bm in [b.strip() for b in bookmarks.split(",") if b.strip()]:
        dst = install / "saved_bookmarks" / (bm + ".json")
        src = reference / "saved_bookmarks" / (bm + ".json")
        if not dst.is_file() and src.is_file():
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
        repo = ROOT / "tests" / "visual" / "bookmarks" / (bm + ".json")
        if not dst.is_file() and repo.is_file():
            # capture.py copies the repo bookmark into its own out dir; an install ab.py does not own is never written to
            print(f"[ab] bookmark {bm}: not in {install}/saved_bookmarks or {reference}; capture.py will use the repo copy {repo}", flush=True)
        elif not dst.is_file():
            missing.append(f"{dst} (not in reference {reference} or {repo.parent} either)")
    textures = install / "data" / "textures"
    for src in (reference / "data" / "textures").glob("*.burnin.*"):
        if not (textures / src.name).exists():
            textures.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, textures / src.name)
    if not any(textures.glob("*.burnin.*")):
        missing.append(f"{textures}/*.burnin.* (reference {reference} has none)")
    if missing:
        sys.exit("ab.py: install is missing required capture data:\n  " + "\n  ".join(missing)
                 + "\n  fix: use --install DIR to name a reference install that has them, or copy them in.")


def ensure_saved_bookmarks(install, reference, staged, bookmarks=""):
    """If the install has no saved_bookmarks/*.json at all, copy the reference's bookmarks (read-only) so a capture never runs on
    an empty bookmark set; fail clearly if the reference has none either. Writes only into an install this run staged (`staged`:
    the stage dirs main recorded); a user-supplied install without bookmarks is refused, never written into."""
    install, reference = Path(install), Path(reference)
    bookmark_dir = install / "saved_bookmarks"
    names = [name.strip() for name in bookmarks.split(",") if name.strip()]
    if names and all((bookmark_dir / (name + ".json")).is_file()
                     or (ROOT / "tests/visual/bookmarks" / (name + ".json")).is_file() for name in names):
        return  # capture.py resolves named repo bookmarks into its own output directory.
    if any(bookmark_dir.glob("*.json")):
        return
    if str(install) not in {str(d) for d in staged}:
        sys.exit(f"ab.py: {install} has no saved_bookmarks/*.json and is not a staged copy, so ab.py will not write into it; "
                 f"copy bookmarks into {bookmark_dir} first")
    sources = sorted((reference / "saved_bookmarks").glob("*.json"))
    if not sources:
        sys.exit(f"ab.py: {install} has no saved_bookmarks/*.json and the reference {reference} has none either; "
                 f"copy bookmarks into {bookmark_dir} before capturing")
    bookmark_dir.mkdir(parents=True, exist_ok=True)
    for src in sources:
        shutil.copy2(src, bookmark_dir / src.name)
    print(f"[ab] {install.name}: no saved_bookmarks; copied {len(sources)} from {reference}", flush=True)


def bookmark_parity_problems(ia, ib, bookmarks: str) -> list:
    """One line per requested bookmark that side A and side B do not carry byte-identically (on one side only, or different
    content): the two sides must replay the same pose files or the diff is not about the exe."""
    problems = []
    for bm in [b.strip() for b in bookmarks.split(",") if b.strip()]:
        shas = {}
        for side, inst in (("A", ia), ("B", ib)):
            f = Path(inst) / "saved_bookmarks" / (bm + ".json")
            shas[side] = hashlib.sha256(f.read_bytes()).hexdigest() if f.is_file() else None
        if shas["A"] is None and shas["B"] is None:
            continue  # on neither side: the capture itself names it
        for side, inst in (("A", ia), ("B", ib)):
            if shas[side] is None:
                problems.append(f"{bm}: side {side} has no {Path(inst) / 'saved_bookmarks' / (bm + '.json')}")
        if shas["A"] and shas["B"] and shas["A"] != shas["B"]:
            problems.append(f"{bm}: side A and side B bookmark files differ")
    return problems


def _stage_root(a, out: Path, install: Path, count: int) -> Path:
    """Where the staged installs go: --stage-root, else <install>/../_ab-stage/<run> on the install's own volume (hardlinks need
    one volume). Runs the DISK_GUARD for `count` stages before anything is written."""
    root = Path(a.stage_root) / out.name if a.stage_root else Path(install).resolve().parent / "_ab-stage" / out.name
    if not a.stage_root:
        root = shorten_stage_root(root, Path(install))
    disk_guard(root, count * estimate_stage_bytes(Path(install), root))
    return root


MAX_STAGE_PATH = 240  # headroom under Windows MAX_PATH (260) for the files the game writes in the stage


def longest_install_rel(install: Path) -> int:
    return max((len(f"install_a/{rel}") for rel, _ in _walk_files(install)), default=0)


def shorten_stage_root(root: Path, install: Path) -> Path:
    """The default stage root, or <drive>/_ab-stage/<short> when the deepest staged file would pass MAX_PATH (same volume, so
    hardlinks still work). A long default made hzb-subpixel-cull pass only with --stage-root."""
    if len(str(root)) + 1 + longest_install_rel(install) <= MAX_STAGE_PATH:
        return root
    anchor = Path(install).resolve().anchor or str(root.parent)
    short = Path(anchor) / "_ab-stage" / hashlib.sha1(str(root).encode()).hexdigest()[:6]
    print(f"[ab] stage root {root} would pass MAX_PATH; using {short}", flush=True)
    return short


def _staged(a, dest) -> str:
    """Record a staged install dir (main deletes these when the run ends) and return it as the path string callers use."""
    getattr(a, "staged", []).append(str(dest))
    return str(dest)


def resolve_installs(a, out: Path):
    """-> (install_a, install_b) from --install-a/--install-b, --exe-a/--exe-b (+ --install), or --exe-a + --install-b; exits on a bad combination."""
    if a.exe_a and not a.exe_b:
        # --exe-a alone: A is a hardlink copy of the B install (--install-b) with A's mc2.exe/pdb + shaders/ swapped in from the
        # exe's install dir, so a diff isolates the executable.
        if not a.install_b or a.install_a:
            sys.exit("use --exe-a with --install-b (the B install); A is staged as a hardlink copy of B with the exe swapped")
        try:
            payload_identity(a.install_b)
        except (OSError, ValueError) as exc:
            sys.exit(f"ab.py: refusing B install: {exc}")
        exe = Path(a.exe_a)
        if not exe.is_file():
            sys.exit(f"exe not found: {exe}")
        root = _stage_root(a, out, Path(a.install_b), 1)
        return (_staged(a, stage_exe_swap(Path(a.install_b), exe, root / "install_a")), a.install_b)
    if a.install_a or a.install_b:
        if not (a.install_a and a.install_b) or a.exe_a or a.exe_b:
            sys.exit("use --install-a with --install-b, or --exe-a with --exe-b, or --exe-a with --install-b, not a mix")
        for d in (a.install_a, a.install_b):
            seed_install(d, a.bookmarks or "", a.install)
        return a.install_a, a.install_b
    if a.exe_a and a.exe_b:
        try:
            payload_identity(a.install)
        except (OSError, ValueError) as exc:
            sys.exit(f"ab.py: refusing source install: {exc}")
        for e in (a.exe_a, a.exe_b):
            if not Path(e).is_file():
                sys.exit(f"exe not found: {e}")
        sa, sb, notes = shader_side_plan(a.install, a.exe_a, a.exe_b, getattr(a, "shaders_a", None), getattr(a, "shaders_b", None),
                                         commits_differ_in_shaders(exe_commit(a.exe_a), exe_commit(a.exe_b)))
        for line in notes:
            print(line, flush=True)
        root = _stage_root(a, out, Path(a.install), 2)
        if sa is None:   # shader_side_plan compares shaders/vulkan/*.spv; default_shaders_a (ab-data-parity) the whole shaders/ tree
            sa = default_shaders_a(Path(a.install), Path(a.exe_a))
        return (_staged(a, stage_install(Path(a.install), Path(a.exe_a), root / "install_a", sa)),
                _staged(a, stage_install(Path(a.install), Path(a.exe_b), root / "install_b", sb)))
    sys.exit("need --install-a/--install-b, or --exe-a/--exe-b, or --exe-a/--install-b (or --compare-only DIR_A DIR_B)")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--bookmarks")
    ap.add_argument("--install-a", metavar="DIR")
    ap.add_argument("--install-b", metavar="DIR")
    ap.add_argument("--exe-a", metavar="EXE",
                    help="A's mc2.exe; with --exe-b it stages both exes on --install's data, alone with --install-b it stages A "
                         "as a hardlink copy of B with A's mc2.exe/pdb + shaders/ swapped in from the exe's install dir")
    ap.add_argument("--exe-b", metavar="EXE")
    ap.add_argument("--stage-root", metavar="DIR",
                    help="where --exe-a/--exe-b stage their install copies (default <install>/../_ab-stage/<run>, the install's own "
                         "volume so shared files are hardlinked)")
    ap.add_argument("--shaders-a", metavar="DIR",
                    help="with --exe-a/--exe-b: stage side A with the shaders/ of this install (e.g. v0.4) and rewrite A's manifest "
                         "rows from the swapped bytes")
    ap.add_argument("--full-install-a", action="store_true",
                    help="with --install-a/--install-b: side A renders ITS OWN install's data (v0.4 look vs tip look). The sides then "
                         "differ in data as well as exe: CROSS-DATA is printed, stamped on the montage and recorded in summary.json. "
                         "Without it a data difference between --install-a and --install-b is refused; the default exe-swap "
                         "(--exe-a/--exe-b or --exe-a + --install-b) runs both exes on ONE data tree")
    ap.add_argument("--shaders-b", metavar="DIR",
                    help="with --exe-a/--exe-b: stage side B with the shaders/ of this install (default: the shaders/ beside B's exe "
                         "when they differ from --install's, else --install's)")
    ap.add_argument("--keep-stage", action="store_true",
                    help="leave the staged install_a/install_b dirs after the run (default: deleted when it ends, pass or fail)")
    ap.add_argument("--install", default=DEFAULT_INSTALL, help="data/DLL source for --exe-a/--exe-b")
    ap.add_argument("--focus", metavar="mech[:N]", default=None,
                    help="MC2_CAPTURE_FOCUS: move every bookmark pose onto the Nth live "
                         "player-team mech (default 0); the bookmark still supplies rot/proj.")
    ap.add_argument("--focus-zoom", type=float, default=None, metavar="ALT",
                    help="camera altitude with --focus (default 200 = mech at about a fifth of the frame)")
    ap.add_argument("--env", action="append", default=[], metavar="K=V", help="extra env for both captures")
    ap.add_argument("--hud", action="store_true",
                    help="keep the HUD visible in both captures (default: capture.py --clean hides it)")
    ap.add_argument("--crop", action="append", default=[], help="x0,y0,x1,y1 fractions")
    ap.add_argument("--out-root")
    ap.add_argument("--compare-only", nargs=2, metavar=("DIR_A", "DIR_B"))
    ap.add_argument("--allow-base-mismatch", action="store_true",
                    help="deliberate cross-base comparison: print the base/data mismatch, stamp CROSS-BASE on the montage, go on")
    ap.add_argument("--allow-asset-drift", action="store_true",
                    help="capture although an install's data/assets/*.fst differ from v0.4 (default: capture.py refuses)")
    ap.add_argument("--allow-data-delta", action="store_true",
                    help="--compare-only: when capture B's commit descends from A's, accept only the data files the window "
                         "changed (`git diff --name-only A B`) instead of refusing; any other differing data file still refuses; "
                         "the commit order and payload identity checks still apply")
    a = ap.parse_args(argv)
    os.environ["MC2_CURSOR_NO_PARK"] = "1"
    crops = [tuple(float(x) for x in c.split(",")) for c in a.crop]
    for c in crops:
        if len(c) != 4:
            sys.exit("--crop needs 4 comma-separated fractions")
    stamp = time.strftime("%Y%m%d_%H%M%S")
    out = Path(a.out_root) if a.out_root else ROOT / "tests/visual/captures" / f"ab_{stamp}"
    if a.compare_only:
        try:
            ia, ib = [captured_identity(Path(d)) for d in a.compare_only]
        except ValueError as exc:
            sys.exit(f"ab.py: {exc}")
        bad = [p for p in [commit_order_problem(ia["commit"], ib["commit"], "capture A", "capture B")] if p]
        entries = candidate_mismatches(ia["manifest"], ib["manifest"], ib["commit"], baseline=a.install)
        accepted_delta = []
        if a.allow_data_delta and not bad:
            # the merge gate's before (published v0.4) vs after (candidate): B descends from A, so a data difference is the
            # window's own payload (W138: logistics-art shipped 574 icons). P1: accept only the files the window actually
            # changed (`git diff --name-only A B`); any other differing data file still refuses.
            diff_files = git_diff_files(ia["commit"], ib["commit"])
            if diff_files is None:
                bad.append("cannot compute `git diff --name-only` between the two capture commits; refusing --allow-data-delta")
            else:
                accepted = manifest_diff_files(ia["manifest"], ib["manifest"]) & {repo_path_to_install(f) for f in diff_files}
                accepted |= {path for path, sha in cooked_payload(diff_files, ib["commit"]).items()
                             if ib["manifest"].get(path) == sha}
                accepted |= cooked_unpinned_accepted(diff_files, ib["commit"], ib["manifest"])
                accepted_delta = sorted(accepted)
                for path, line in entries:
                    if path in accepted:
                        print("[ab] data delta (window payload): " + line, flush=True)
                    else:
                        bad.append(line)
        else:
            bad += [line for _, line in entries]
        if bad and not a.allow_base_mismatch:
            sys.exit("ab.py: mismatched capture identities: " + "; ".join(bad))
        compare(Path(a.compare_only[0]), Path(a.compare_only[1]), out, crops,
                cross_base=bool(bad), data_delta_accepted=accepted_delta, hud=a.hud)
        sys.exit(1 if getattr(compare, "problems", None) else 0)
    if not a.bookmarks:
        sys.exit("--bookmarks required")
    bad = base_problems(a.install_a, a.install_b, a.exe_a, a.exe_b, a.install)
    if bad and not a.allow_base_mismatch:
        sys.exit("ab.py: refusing an A/B across different bases:\n  " + "\n  ".join(bad)
                 + "\n  fix: deploy side A's base to the install (so both sides run the same base), or pass --allow-base-mismatch"
                 + " for a deliberate cross-base comparison (the montage is then stamped CROSS-BASE)")
    for b in bad:
        print("[ab] CROSS-BASE (allowed): " + b, flush=True)
    out.mkdir(parents=True, exist_ok=True)
    a.staged = []  # install dirs this run staged (never a user-supplied --install-a/--install-b): deleted when the run ends
    try:
        ia, ib = resolve_installs(a, out)
        ensure_saved_bookmarks(ia, a.install, a.staged, bookmarks=a.bookmarks)
        ensure_saved_bookmarks(ib, a.install, a.staged, bookmarks=a.bookmarks)
        parity = bookmark_parity_problems(ia, ib, a.bookmarks)
        if parity:
            sys.exit("ab.py: bookmarks are not seeded identically on both sides:\n  " + "\n  ".join(parity))
        dpar = data_parity(ia, ib, a.full_install_a)
        for line in data_parity_lines(dpar):
            print(line, flush=True)
        if dpar.get("unlisted_differing_count", dpar["differing_count"]) and not a.full_install_a:
            sys.exit("ab.py: the two sides render different data trees; an exe A/B must use one data tree. Use --exe-a/--exe-b or "
                     "--exe-a with --install-b (both run the B data), or pass --full-install-a for a deliberate v0.4-vs-tip look sheet")
        exe_a_swap = None
        if a.exe_a and not a.exe_b:
            exe_a_swap = {"exe": a.exe_a, "exe_dir": str(Path(a.exe_a).parent), "b_install": a.install_b}
        try:
            identities = [payload_identity(d) for d in (ia, ib)]
        except (OSError, ValueError) as exc:
            sys.exit(f"ab.py: refusing capture: {exc}")
        # Warn (never refuse) when the A exe was not built from the lane base: the A/B then mixes the lane's changes with
        # cross-base ones, so a nonzero diff is not a clean signal of the window.
        base_line, exe_a_base_mismatch = exe_a_base_check(identities[0]["commit"], identities[1]["commit"])
        if base_line:
            print("[ab] WARNING: " + base_line, flush=True)
        env = ["MC2_RENDER_BACKEND=vulkan", "MC2_VK_SHARED_GBUFFER=1"] + pin_stock_overlay_concrete(a.env) + env_pin.focus_declarations(a.focus, a.focus_zoom)
        run_capture(ia, a.bookmarks, out / "a", env, hud=a.hud, allow_asset_drift=a.allow_asset_drift)
        stamp_identity(out / "a", identities[0])
        run_capture(ib, a.bookmarks, out / "b", env, hud=a.hud, allow_asset_drift=a.allow_asset_drift)
        stamp_identity(out / "b", identities[1])
        for line in capture_commit_report(identities[0]["commit"], identities[1]["commit"], worktree_head()):
            print(line, flush=True)
        compare(out / "a", out / "b", out / "cmp", crops, [s.strip() for s in a.bookmarks.split(",") if s.strip()],
                cross_base=bool(bad), exe_a_base_mismatch=exe_a_base_mismatch, exe_a_swap=exe_a_swap, hud=a.hud, data_parity=dpar)
        rc = 1 if getattr(compare, "problems", None) else 0
    finally:
        cleanup_stage(a.staged, keep=a.keep_stage)  # a/, b/ and cmp/ stay; the 13 GB stage goes, pass or fail
    sys.exit(rc)


if __name__ == "__main__":
    main()
