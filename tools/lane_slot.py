#!/usr/bin/env python3
"""lane_slot.py -- fixed pool of lane worktrees (A:/Games/mc2-slot01 .. slot12) so the sccache base-dir list never changes and
each slot keeps its warm Ninja tree between lanes.

  py -3 tools/lane_slot.py acquire --branch <name> [--base <ref>]   # pick/create a free slot, switch -C, warm build, print path
  py -3 tools/lane_slot.py release <slot>                           # clean check, delete the lane's scratch/install, detach at nifty
  py -3 tools/lane_slot.py status

A slot is free when it exists, is git-clean, is not claimed in the lane registry, and its branch is merged into
claude/nifty-mendeleev (or has a refs/archive/<branch> ref) or it is detached. Never touches owner-test-*, v0.4c, mods or drive I:.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "scripts"))
sys.path.insert(0, os.path.join(HERE, "repo_intel"))
import configure_ninja  # noqa: E402

NIFTY = "claude/nifty-mendeleev"
MAX_SLOTS = configure_ninja.SLOT_COUNT
SLOT_PATHS = [f"A:/Games/mc2-slot{n:02d}" for n in range(1, MAX_SLOTS + 1)]
MAIN_ROOT = "A:/Games/mc2-main"
INSTALLS = "E:/mc2-installs"
SCRATCH = "G:/mc2-tmp"
PROTECTED = ("owner-test-", "v0.4", "_")  # install names never deleted


def git(root, *args):
    r = subprocess.run(["git", "-C", root, *args], capture_output=True, text=True, stdin=subprocess.DEVNULL)
    return r.returncode, r.stdout.strip(), r.stderr.strip()


def norm(p):
    return p.replace("\\", "/").rstrip("/").lower()


def claimed_worktrees():
    """Normalised worktree paths of fresh (non-stale) lanes in the lane registry."""
    try:
        import lane_registry  # noqa: PLC0415
        from pathlib import Path  # noqa: PLC0415
        return {norm(e.get("worktree", "")) for e in lane_registry.list_lanes(Path(MAIN_ROOT))["lanes"] if not e.get("stale")}
    except Exception:  # noqa: BLE001 -- registry unreadable: treat nothing as claimed but say so
        print("[lane_slot] WARNING: lane registry unreadable; claims not checked", file=sys.stderr)
        return set()


def slot_info(path, claimed):
    info = {"path": path, "exists": os.path.isdir(path), "clean": False, "branch": None, "claimed": norm(path) in claimed, "merged": False}
    if not info["exists"]:
        return info
    info["clean"] = git(path, "status", "--porcelain")[1] == ""
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


def acquire(branch, base):
    claimed = claimed_worktrees()
    picked = pick_slot([slot_info(p, claimed) for p in SLOT_PATHS])
    if not picked:
        sys.exit(f"acquire: all {MAX_SLOTS} slots are busy (dirty, claimed or unmerged); run `lane_slot.py status`")
    path, create = picked
    if create:
        rc, _, err = git(MAIN_ROOT, "worktree", "add", "--detach", path, base)
        if rc:
            sys.exit("acquire: worktree add failed: " + err)
    rc, _, err = git(path, "switch", "-C", branch, base)
    if rc:
        sys.exit("acquire: git switch -C failed: " + err)
    try:
        from pathlib import Path  # noqa: PLC0415
        import lane_registry  # noqa: PLC0415
        lane_registry.register_lane(Path(MAIN_ROOT), branch, worktree=path)
    except Exception as e:  # noqa: BLE001
        print(f"[lane_slot] WARNING: lane registration failed: {e}", file=sys.stderr)
    # Warm the slot's own tree at the branch tip (no --base: that would detach the branch we just created).
    subprocess.run([sys.executable, f"{path}/scripts/ninja_build.py", "--warm"], cwd=path, stdin=subprocess.DEVNULL)
    print(path)
    return path


def install_dirs(branch):
    """Lane installs owned by this branch: E:/mc2-installs/<branch> and E:/mc2-installs/<last component>."""
    out = []
    for name in dict.fromkeys([branch, branch.rsplit("/", 1)[-1]]):
        if name and not name.startswith(PROTECTED) and ".." not in name:
            out.append(f"{INSTALLS}/{name}")
    return out


def release(path):
    path = path.replace("\\", "/").rstrip("/")
    if norm(path) not in {norm(p) for p in SLOT_PATHS}:
        sys.exit(f"release: {path} is not a slot")
    info = slot_info(path, set())
    if not info["exists"]:
        sys.exit(f"release: {path} does not exist")
    if not info["clean"]:
        sys.exit(f"release: {path} has uncommitted changes; commit or discard them first")
    deleted = []
    for d in glob.glob(f"{SCRATCH}/{os.path.basename(path)}-*") + (install_dirs(info["branch"]) if info["branch"] else []):
        if os.path.isdir(d):
            shutil.rmtree(d, ignore_errors=True)
            deleted.append(d)
    rc, _, err = git(path, "switch", "--detach", NIFTY)
    if rc:
        sys.exit("release: detach at nifty failed: " + err)
    if info["branch"]:
        try:
            from pathlib import Path  # noqa: PLC0415
            import lane_registry  # noqa: PLC0415
            lane_registry.release_lane(Path(MAIN_ROOT), info["branch"])
        except Exception:  # noqa: BLE001
            pass
    print(f"released {path} (was {info['branch']}); deleted: {deleted or 'nothing'}; build tree kept")
    return deleted


def status():
    claimed = claimed_worktrees()
    print(f"{'slot':<8}{'branch':<44}{'clean':<7}{'claimed':<9}{'merged':<7}free")
    for p in SLOT_PATHS:
        i = slot_info(p, claimed)
        if not i["exists"]:
            print(f"{os.path.basename(p)[-6:]:<8}(not created)")
            continue
        print(f"{os.path.basename(p)[-6:]:<8}{(i['branch'] or '(detached)'):<44}{str(i['clean']):<7}{str(i['claimed']):<9}"
              f"{str(i['merged']):<7}{'yes' if is_free(i) else 'no'}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("acquire")
    a.add_argument("--branch", required=True)
    a.add_argument("--base", default=NIFTY)
    r = sub.add_parser("release")
    r.add_argument("slot")
    sub.add_parser("status")
    args = ap.parse_args(argv)
    if args.cmd == "acquire":
        acquire(args.branch, args.base)
    elif args.cmd == "release":
        release(args.slot)
    else:
        status()
    return 0


if __name__ == "__main__":
    sys.exit(main())
