#!/usr/bin/env python3
"""lane_slot.py -- fixed pool of lane worktrees (A:/Games/mc2-slot01 .. slot12) so the sccache base-dir list never changes and
each slot keeps its warm Ninja tree between lanes.

  py -3 tools/lane_slot.py acquire --branch <name> [--base <ref>] [--reset-branch] [--no-warm]
        # pick/create a free slot; new branch = created at --base (default nifty); EXISTING branch = checked out at its own tip,
        # never moved (a differing --base is refused unless --reset-branch); prints HEAD, starts the warm build (pid in <tree>/warm.pid)
  py -3 tools/lane_slot.py release <slot|slot03|path> [--discard]   # refuses a dirty slot (--discard archives the diff to
        # E:/mc2-archive-2026-10/stray-diffs first); drops the claim and detaches; retains install/scratch
  py -3 tools/lane_slot.py gc [--hours 72]                         # unclaimed merged/aged installs and slot scratch; archives crashes first
  py -3 tools/lane_slot.py status                                   # shows each claiming lane and its age

A slot is free when it exists, is git-clean, is not claimed in the lane registry, and its branch is merged into
claude/nifty-mendeleev (or has a refs/archive/<branch> ref) or it is detached. Never touches owner-test-*, v0.4c, mods or drive I:.
"""
import argparse
import datetime
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "scripts"))
sys.path.insert(0, os.path.join(HERE, "repo_intel"))
import configure_ninja  # noqa: E402
import console_child  # noqa: E402  (BELOW_NORMAL + CREATE_NO_WINDOW for every child)

NIFTY = "claude/nifty-mendeleev"
MAX_SLOTS = configure_ninja.SLOT_COUNT
SLOT_PATHS = [f"A:/Games/mc2-slot{n:02d}" for n in range(1, MAX_SLOTS + 1)]
MAIN_ROOT = "A:/Games/mc2-main"
INSTALLS = "E:/mc2-installs"
SCRATCH = "G:/mc2-tmp"
STRAY_DIR = "E:/mc2-archive-2026-10/stray-diffs"
PROTECTED =("owner-test-", "v0.4", "_", "merge-acceptance", "mc2-win64-v0.4", "mods")
CRASH_ARCHIVE = "E:/mc2-archive-2026-10/proofs"


def git(root, *args):
    r = subprocess.run(["git", "-C", root, *args], capture_output=True, text=True, stdin=subprocess.DEVNULL,
                       **console_child.child_kwargs(redirected=True))
    return r.returncode, r.stdout.strip(), r.stderr.strip()


def norm(p):
    return p.replace("\\", "/").rstrip("/").lower()


def registry_lanes():
    """All lane registry entries (stale ones included, flagged); [] with a warning when the registry is unreadable."""
    try:
        import lane_registry  # noqa: PLC0415
        from pathlib import Path  # noqa: PLC0415
        return lane_registry.list_lanes(Path(MAIN_ROOT))["lanes"]
    except Exception:  # noqa: BLE001 -- registry unreadable: treat nothing as claimed but say so
        print("[lane_slot] WARNING: lane registry unreadable; claims not checked", file=sys.stderr)
        return []


def claims():
    """{normalised worktree: (lane name, age_secs)} for fresh (non-stale) registry lanes."""
    return {norm(e.get("worktree", "")): (e.get("name") or e.get("lane") or "?", e.get("age_secs"))
            for e in registry_lanes() if not e.get("stale")}


def claimed_worktrees():
    """Normalised worktree paths of fresh (non-stale) lanes in the lane registry."""
    return set(claims())


def fmt_age(secs):
    if secs is None:
        return "?"
    return f"{secs // 3600}h{secs % 3600 // 60:02d}m" if secs >= 3600 else f"{secs // 60}m"


def drop_claims(path):
    """Release only claims belonging to this slot, in one locked registry operation."""
    try:
        import lane_registry  # noqa: PLC0415
        from pathlib import Path  # noqa: PLC0415
        return lane_registry.release_worktree(Path(MAIN_ROOT), path)["released"]
    except Exception as e:  # noqa: BLE001
        sys.exit(f"release: lane registry release failed: {e}")


def discard_dirty(path):
    """Archive index, worktree and untracked bytes separately before reset/clean."""
    os.makedirs(STRAY_DIR, exist_ok=True)
    stem = f"{STRAY_DIR}/{os.path.basename(path)}-{datetime.date.today().isoformat()}"
    archive, n = stem, 1
    while True:
        try:
            os.mkdir(archive)
            break
        except FileExistsError:
            n += 1
            archive = f"{stem}-{n}"
    try:
        for name, args in (("index.patch", ("--cached",)), ("worktree.patch", ())):
            rc, _, err = git(path, "diff", *args, "--binary", f"--output={archive}/{name}")
            if rc:
                raise OSError(f"{name}: {err}")
        files = subprocess.run(["git", "-C", path, "ls-files", "--others", "--exclude-standard", "-z"],
                               check=True, capture_output=True, stdin=subprocess.DEVNULL,
                               **console_child.child_kwargs(redirected=True)).stdout
        with tarfile.open(f"{archive}/untracked.tar", "w") as tar:
            for raw in files.split(b"\0"):
                if raw:
                    name = os.fsdecode(raw)
                    tar.add(os.path.join(path, name), arcname=name, recursive=False)
    except (OSError, subprocess.CalledProcessError, tarfile.TarError) as e:
        sys.exit(f"release: archive failed, nothing discarded: {e}")
    for args in (("reset", "--hard", "-q"), ("clean", "-fdq")):  # no -x: ignored build trees stay
        rc, _, err = git(path, *args)
        if rc:
            sys.exit(f"release: {args[0]} failed; archive kept at {archive}: {err}")
    return archive


def slot_info(path, claimed):
    info = {"path": path, "exists": os.path.isdir(path), "clean": False, "branch": None, "claimed": norm(path) in claimed, "merged": False}
    if not info["exists"]:
        return info
    rc, status, err = git(path, "status", "--porcelain", "--", ".", f":(exclude){HOOK_SEEDED_FILE}")
    if rc:
        sys.exit("lane_slot: could not inspect slot status: " + err)
    # Only the hook-seeded agent file is ignored. The lane's notes/ count as work: a worktree holding just its state note is
    # a live lane, not a free slot (an exclude in the shared .git/info/exclude hid them in every worktree).
    info["clean"] = status == ""
    rc, branch, _ = git(path, "symbolic-ref", "--short", "-q", "HEAD")
    info["branch"] = branch if rc == 0 and branch else None
    b = info["branch"]
    info["merged"] = (b is None or git(path, "merge-base", "--is-ancestor", b, NIFTY)[0] == 0
                      or git(path, "rev-parse", "--verify", "-q", f"refs/archive/{b}")[0] == 0)
    return info


def is_free(info):
    return info["exists"] and info["clean"] and not info["claimed"] and info["merged"]


def pick_slot(infos):
    """Return (path, create) for the first free slot; create=True when a new slot worktree must be made; None when full."""
    for i in infos:
        if is_free(i):
            return i["path"], False
    for i in infos:
        if not i["exists"]:
            return i["path"], True
    return None


def branch_plan(branch, base, reset_branch, root=None):
    """-> (switch args, start ref). An existing branch is NEVER moved: it is checked out at its own tip; a --base that
    differs from that tip is refused unless --reset-branch (2026-11-07: `switch -C` wiped rt-scene-1). A new branch is created at base (default nifty)."""
    root = root or MAIN_ROOT
    rc, tip, _ = git(root, "rev-parse", "--verify", "-q", f"refs/heads/{branch}")
    if rc:
        return ["switch", "-c", branch, base or NIFTY], base or NIFTY
    if base:
        rc, want, err = git(root, "rev-parse", "--verify", "-q", f"{base}^{{commit}}")
        if rc:
            sys.exit(f"acquire: --base {base} does not resolve: {err}")
        if want != tip:
            if not reset_branch:
                sys.exit(f"acquire: branch {branch} exists at {tip[:9]} but --base {base} is {want[:9]}; refusing to move it "
                         "(drop --base to check it out at its own tip, or pass --reset-branch to discard its commits from the branch name)")
            return ["switch", "-C", branch, base], base
    return ["switch", branch], branch


HOOK_SEEDED_FILE = ".claude/agents/mc2-lane.md"   # seeded into every worktree by a hook: not the lane's work, never "dirty"


def acquire(branch, base=None, reset_branch=False, no_warm=False):
    claimed = claimed_worktrees()
    picked = pick_slot([slot_info(p, claimed) for p in SLOT_PATHS])
    if not picked:
        sys.exit(f"acquire: all {MAX_SLOTS} slots are busy (dirty, claimed or unmerged); run `lane_slot.py status`")
    path, create = picked
    switch_args, start = branch_plan(branch, base, reset_branch)
    if create:
        rc, _, err = git(MAIN_ROOT, "worktree", "add", "--detach", path, start)
        if rc:
            sys.exit("acquire: worktree add failed: " + err)
    rc, _, err = git(path, *switch_args)
    if rc:
        sys.exit(f"acquire: git {' '.join(switch_args)} failed: " + err)
    print(f"[lane_slot] {branch} HEAD {git(path, 'rev-parse', 'HEAD')[1]}")
    try:
        from pathlib import Path  # noqa: PLC0415
        import lane_registry  # noqa: PLC0415
        lane_registry.register_lane(Path(MAIN_ROOT), branch, worktree=path, deploy_lane=slot_install(path))
    except Exception as e:  # noqa: BLE001
        sys.exit(f"acquire: lane registration failed: {e}; the slot is unclaimed, so another acquire could take it. "
                 f"Fix the registry and re-run `acquire --branch {branch}`")
    print(f"[lane_slot] install {slot_install(path)} (deploy with --allow-new-target)")
    # Warm the slot's own tree at the branch tip (no --base: that would detach the branch we just created). The warm build holds
    # the tree's build lock (scripts/ninja_build.py), so the lane's own build waits for it; its PID is recorded in <tree>/warm.pid.
    if no_warm:
        print("[lane_slot] --no-warm: no background build started")
    else:
        r = subprocess.run([sys.executable, f"{path}/scripts/ninja_build.py", "--warm"], cwd=path, stdin=subprocess.DEVNULL,
                           capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
        sys.stdout.write(r.stdout)
        m = re.search(r"warm: pid (\d+) building (\S+)", r.stdout)
        if m:
            with open(f"{path}/{m.group(2)}/warm.pid", "w") as f:
                f.write(m.group(1) + "\n")
        else:
            print("[lane_slot] WARNING: warm build did not report a pid: " + (r.stderr.strip() or r.stdout.strip()), file=sys.stderr)
    print(path)
    return path


def slot_install(path):
    """The PC install belongs to the fixed slot, across branch changes."""
    number = next(n for n, p in enumerate(SLOT_PATHS, 1) if norm(p) == norm(path))
    return f"{INSTALLS}/pc-lane-{number:02d}"


def install_dirs(branch):
    """Lane installs owned by this branch: E:/mc2-installs/<branch> and E:/mc2-installs/<last component>."""
    out = []
    for name in dict.fromkeys([branch, branch.rsplit("/", 1)[-1]]):
        if name and not name.startswith(PROTECTED) and ".." not in name:
            out.append(f"{INSTALLS}/{name}")
    return out


def slot_path(arg):
    """A slot short name (slot03, mc2-slot03, 3) or a path -> the full slot path; anything else is returned normalised."""
    arg = arg.replace("\\", "/").rstrip("/")
    m = re.fullmatch(r"(?:mc2-)?(?:slot)?0*(\d{1,2})", arg, re.I)
    if m and 1 <= int(m.group(1)) <= MAX_SLOTS:
        return SLOT_PATHS[int(m.group(1)) - 1]
    return arg


def release(path, discard=False):
    path = slot_path(path)
    if norm(path) not in {norm(p) for p in SLOT_PATHS}:
        sys.exit(f"release: {path} is not a slot")
    info = slot_info(path, set())
    if not info["exists"]:
        sys.exit(f"release: {path} does not exist")
    if not info["clean"]:
        if not discard:
            sys.exit(f"release: {path} has uncommitted changes; commit them, or pass --discard to archive the diff under {STRAY_DIR} and drop them")
        print(f"[lane_slot] discarded uncommitted changes, diff saved to {discard_dirty(path)}")
    rc, _, err = git(path, "switch", "--detach", NIFTY)
    if rc:
        sys.exit("release: detach at nifty failed: " + err)
    dropped = drop_claims(path)
    print(f"released {path} (was {info['branch']}); registry claims dropped: {dropped or 'none'}; install, scratch and build tree kept")
    return []


def gc(hours=72):
    """Collect only unclaimed branch installs / slot scratch; registry failures abort before deletion."""
    if hours < 0:
        sys.exit("gc: --hours must be non-negative")
    import lane_registry  # noqa: PLC0415
    store = lane_registry.store_path(Path(MAIN_ROOT))
    with lane_registry._lock(store):
        # list_lanes treats unreadable/corrupt data as empty, which is unsafe for GC.
        data = json.loads(store.read_text(encoding="utf-8"))
        if not isinstance(data.get("lanes"), dict):
            sys.exit("gc: invalid registry; nothing deleted")
        return _gc(list(data["lanes"].values()), hours)


def _gc(entries, hours):
    held = {norm(e.get("worktree", "")) for e in entries}
    names = {NIFTY} | {e.get("name", "") for e in entries} | {e.get("deploy_lane") or "" for e in entries}
    for p in held:
        slot = re.fullmatch(r"mc2-slot(\d+)", os.path.basename(p))
        if slot:
            number = int(slot.group(1))
            names.update((f"slot{number:02d}", f"mc2-slot{number:02d}", f"lane{number}", f"lane{number:02d}"))
        if os.path.isdir(p):
            names.add(git(p, "symbolic-ref", "--short", "-q", "HEAD")[1])
    protected_paths = {norm(d) for name in names for d in install_dirs(name)}
    protected_paths.update(norm(slot_install(p)) for p in SLOT_PATHS if norm(p) in held)
    rc, refs, err = git(MAIN_ROOT, "for-each-ref", "--format=%(refname:short)", "refs/heads/claude/")
    if rc:
        sys.exit("gc: cannot enumerate branches: " + err)
    candidates = {}
    for branch in refs.splitlines():
        if branch == NIFTY or branch in names:
            continue
        merged = git(MAIN_ROOT, "merge-base", "--is-ancestor", branch, NIFTY)[0] == 0
        for d in install_dirs(branch):
            candidates[d] = (branch, merged, INSTALLS)
    # Orphaned branch installs still age out after the branch ref has been removed.
    if os.path.isdir(INSTALLS):
        for path in Path(INSTALLS).iterdir():
            paths = path.iterdir() if path.name == "claude" and path.is_dir() else [path]
            for install in paths:
                branch = install.relative_to(INSTALLS).as_posix()
                candidates.setdefault(str(install), (branch, False, INSTALLS))
    for slot in SLOT_PATHS:
        if norm(slot) in held:
            continue
        info = slot_info(slot, held)
        branch = info["branch"] or os.path.basename(slot)
        if branch in names:
            continue
        for d in glob.glob(f"{SCRATCH}/{os.path.basename(slot)}-*"):
            # A detached slot no longer identifies the scratch's branch: age/disk policy only.
            candidates[d] = (branch, bool(info["branch"]) and info["merged"], SCRATCH)
    deleted = []
    for d, (branch, merged, root) in candidates.items():
        path = Path(d)
        if not path.is_dir() or norm(d) in protected_paths:
            continue
        relative = path.relative_to(Path(root))
        resolved_relative = path.resolve().relative_to(Path(root).resolve())
        if any(part.lower().startswith(PROTECTED) for part in relative.parts + resolved_relative.parts):
            continue
        # Validate the absolute target and all descendants before moving/deleting anything.
        tree = [path]
        for parent, dirs, files in os.walk(path, followlinks=False):
            tree.extend(Path(parent) / n for n in dirs + files)
        if any(p.is_symlink() or getattr(os.path, "isjunction", lambda _: False)(p) for p in tree):
            print(f"[lane_slot] gc skipped linked tree: {path}")
            continue
        newest = max(p.stat().st_mtime for p in tree)
        low_disk = shutil.disk_usage(root).free < 30 * 2**30
        if re.fullmatch(r"pc-lane-\d{2}", path.name) and not low_disk:
            continue  # retained slots are reclaimed only under disk pressure
        if not (merged or time.time() - newest >= hours * 3600 or low_disk):
            continue
        evidence = [p for p in tree if p.is_file() and
                    (any("crash" in part.lower() for part in p.relative_to(path).parts)
                     or p.suffix.lower() in (".dmp", ".mdmp", ".wer", ".pdb"))]
        if evidence:
            archive = Path(CRASH_ARCHIVE) / branch / "crashes" / (path.name + "-" + str(time.time_ns()))
            for p in evidence:
                dst = archive / p.relative_to(path)
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(str(p), str(dst))  # failure aborts; never delete unarchived evidence
        shutil.rmtree(path)
        deleted.append(d)
        print(f"[lane_slot] gc deleted {d}; crash evidence archived first")
    return deleted


def status():
    held = claims()
    print(f"{'slot':<8}{'branch':<44}{'clean':<7}{'claimed':<9}{'merged':<7}{'free':<6}claim")
    for p in SLOT_PATHS:
        i = slot_info(p, set(held))
        if not i["exists"]:
            print(f"{os.path.basename(p)[-6:]:<8}(not created)")
            continue
        who = held.get(norm(p))
        claim = f"{who[0]} ({fmt_age(who[1])})" if who else ""
        print(f"{os.path.basename(p)[-6:]:<8}{(i['branch'] or '(detached)'):<44}{str(i['clean']):<7}{str(i['claimed']):<9}"
              f"{str(i['merged']):<7}{'yes' if is_free(i) else 'no':<6}{claim}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("acquire")
    a.add_argument("--branch", required=True)
    a.add_argument("--base", default=None, help="start ref for a NEW branch (default nifty); for an existing branch it must equal its tip")
    a.add_argument("--reset-branch", action="store_true", help="allow --base to move an existing branch (discards its commits from the name)")
    a.add_argument("--no-warm", action="store_true", help="do not start the background warm build")
    r = sub.add_parser("release")
    r.add_argument("slot")
    r.add_argument("--discard", action="store_true", help="drop uncommitted changes after saving their diff to " + STRAY_DIR)
    sub.add_parser("status")
    g = sub.add_parser("gc", help="delete unclaimed merged/aged installs and slot scratch, or reclaim below 30 GiB free")
    g.add_argument("--hours", type=float, default=72)
    args = ap.parse_args(argv)
    if args.cmd == "acquire":
        acquire(args.branch, args.base, args.reset_branch, args.no_warm)
    elif args.cmd == "release":
        release(args.slot, args.discard)
    elif args.cmd == "gc":
        gc(args.hours)
    else:
        status()
    return 0


if __name__ == "__main__":
    sys.exit(main())
