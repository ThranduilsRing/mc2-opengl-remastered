#!/usr/bin/env python3
"""lane_slot.py -- fixed pool of lane worktrees (A:/Games/mc2-slot01 .. slot12) so the sccache base-dir list never changes and
each slot keeps its warm Ninja tree between lanes.

  py -3 tools/lane_slot.py acquire --branch <name> [--base <ref>] [--reset-branch] [--no-warm]
        # pick/create a free slot; new branch = created at --base (default nifty); EXISTING branch = checked out at its own tip,
        # never moved (a differing --base is refused unless --reset-branch); prints HEAD, starts the warm build (pid in <tree>/warm.pid)
  py -3 tools/lane_slot.py release <slot|slot03|path> [--discard]   # refuses a dirty slot (--discard archives the diff to
        # E:/mc2-archive-2026-10/stray-diffs first); drops the slot's registry claim, deletes scratch/install, detaches at nifty
  py -3 tools/lane_slot.py status                                   # shows each claiming lane and its age

A slot is free when it exists, is git-clean, is not claimed in the lane registry, and its branch is merged into
claude/nifty-mendeleev (or has a refs/archive/<branch> ref) or it is detached. Never touches owner-test-*, v0.4c, mods or drive I:.
"""
import argparse
import datetime
import glob
import os
import re
import shutil
import subprocess
import sys

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
PROTECTED =("owner-test-", "v0.4", "_")  # install names never deleted


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


def drop_claims(path, branch=None):
    """Release every registry entry (fresh or stale) whose worktree is this slot, plus the branch's own entry. -> names released."""
    names = {e.get("name") for e in registry_lanes() if norm(e.get("worktree", "")) == norm(path)} - {None}
    if branch:
        names.add(branch)
    done = []
    try:
        import lane_registry  # noqa: PLC0415
        from pathlib import Path  # noqa: PLC0415
        for n in sorted(names):
            if lane_registry.release_lane(Path(MAIN_ROOT), n).get("released"):
                done.append(n)
    except Exception as e:  # noqa: BLE001
        print(f"[lane_slot] WARNING: lane registry release failed: {e}", file=sys.stderr)
    return done


def discard_dirty(path):
    """Write the slot's uncommitted diff (tracked + untracked, binary-safe) to STRAY_DIR, then reset and clean it. -> patch path."""
    os.makedirs(STRAY_DIR, exist_ok=True)
    stem = f"{STRAY_DIR}/{os.path.basename(path)}-{datetime.date.today().isoformat()}"
    patch, n = stem + ".patch", 1
    while os.path.exists(patch):
        n += 1
        patch = f"{stem}-{n}.patch"
    git(path, "add", "-N", ".")  # intent-to-add so untracked files appear in the diff
    rc, _, err = git(path, "diff", "HEAD", "--binary", f"--output={patch}")
    if rc:
        sys.exit("release: could not write the stray diff, nothing discarded: " + err)
    git(path, "reset", "--hard", "-q")
    git(path, "clean", "-fdq")  # no -x: ignored build trees stay
    return patch


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


def branch_plan(branch, base, reset_branch, root=MAIN_ROOT):
    """-> (switch args, start ref). An existing branch is NEVER moved: it is checked out at its own tip; a --base that
    differs from that tip is refused unless --reset-branch (2026-11-07: `switch -C` wiped rt-scene-1). A new branch is created at base (default nifty)."""
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
        lane_registry.register_lane(Path(MAIN_ROOT), branch, worktree=path)
    except Exception as e:  # noqa: BLE001
        print(f"[lane_slot] WARNING: lane registration failed: {e}", file=sys.stderr)
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
    deleted = []
    for d in glob.glob(f"{SCRATCH}/{os.path.basename(path)}-*") + (install_dirs(info["branch"]) if info["branch"] else []):
        if os.path.isdir(d):
            shutil.rmtree(d, ignore_errors=True)
            deleted.append(d)
    rc, _, err = git(path, "switch", "--detach", NIFTY)
    if rc:
        sys.exit("release: detach at nifty failed: " + err)
    dropped = drop_claims(path, info["branch"])
    print(f"released {path} (was {info['branch']}); registry claims dropped: {dropped or 'none'}; deleted: {deleted or 'nothing'}; build tree kept")
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
    args = ap.parse_args(argv)
    if args.cmd == "acquire":
        acquire(args.branch, args.base, args.reset_branch, args.no_warm)
    elif args.cmd == "release":
        release(args.slot, args.discard)
    else:
        status()
    return 0


if __name__ == "__main__":
    sys.exit(main())
