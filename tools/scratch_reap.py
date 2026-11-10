#!/usr/bin/env python3
"""scratch_reap.py -- delete a lane slot's own scratch, or one legacy E:/mc2-tmp-<name> dir. Dry run unless --apply.

  py -3 tools/scratch_reap.py <slot|slot05|5> [--apply]     # E:/mc2-tmp/<slotNN>/ and G:/mc2-tmp/<slotNN>/
  py -3 tools/scratch_reap.py --legacy <path> [--apply]      # one allow-listed legacy E:/mc2-tmp-<name> dir
  py -3 tools/scratch_reap.py --legacy-list [--dry-run|--apply]   # every allow-listed legacy dir: size, owner lane, verdict

Deletes only: <root>/<slotNN> under the two scratch roots, or a name in LEGACY. Refuses (and prints why): anything outside those,
the archive, owner-test-*, v0.4/v0.4c, A:/Games/mc2-opengl, a worktree, a slot with a live (registry-claimed) lane, and any tree
holding a crashes/ dir or *.dmp/*.mdmp that is not mirrored (same name and size) under E:/mc2-archive-2026-10/crashes/.
Prints the bytes freed (apparent size, hardlinks counted once) and the paths it kept.
"""
import argparse
import os
import re
import shutil
import stat
import subprocess
import sys

sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children; scripts/console_child.py)

ROOTS = ("E:/mc2-tmp", "G:/mc2-tmp")  # drive-path: ok PC slot scratch roots (lane-rules section 5); never run on the laptop gate
ARCHIVE = "E:/mc2-archive-2026-10"  # drive-path: ok PC archive, refused as a delete target
CRASH_MIRROR = ARCHIVE + "/crashes"
MAIN_ROOT = "A:/Games/mc2-main"  # drive-path: ok PC canonical worktree (worktree list, slot paths)
SLOT_COUNT = 12
LEGACY_PARENT = "E:/"  # drive-path: ok PC legacy E:/mc2-tmp-<name> scratch parent
LEGACY = ("clc", "dis", "idr", "isp", "mns", "mpl2", "s02", "s03", "s04", "s05", "s06", "s06b", "s06c", "s07",
          "s11", "s11b", "s11c", "s11d", "s12", "s12b", "wrp", "wrs")
FORBIDDEN_PARTS = ("owner-test-", "v0.4", "mc2-win64-v0.4")
FORBIDDEN_PREFIXES = (ARCHIVE, "A:/Games/mc2-opengl")  # drive-path: ok PC owner mod installs, refused as a delete target
EVIDENCE_SUFFIX = (".dmp", ".mdmp")


def norm(p):
    return os.path.abspath(p).replace("\\", "/").rstrip("/")


def lower(p):
    return norm(p).lower()


def slot_name(arg):
    m = re.fullmatch(r"(?:mc2-)?(?:slot)?0*(\d{1,2})", str(arg).replace("\\", "/").rstrip("/").split("/")[-1], re.I)
    if not m or not 1 <= int(m.group(1)) <= SLOT_COUNT:
        raise ValueError(f"not a slot: {arg}")
    return f"slot{int(m.group(1)):02d}"


def slot_dirs(slot):
    return [f"{r}/{slot_name(slot)}" for r in ROOTS]


def ensure_slot_scratch(slot):
    """Create (idempotent, best effort) the slot's two scratch roots; returns their paths."""
    out = slot_dirs(slot)
    for d in out:
        try:
            os.makedirs(d, exist_ok=True)
        except OSError as e:
            print(f"[scratch_reap] WARNING: cannot create {d}: {e}", file=sys.stderr)
    return out


def _git(*args):
    r = subprocess.run(["git", "-C", MAIN_ROOT, *args], capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    return r.stdout.strip() if r.returncode == 0 else ""


def worktree_paths():
    return {lower(l[9:]) for l in _git("worktree", "list", "--porcelain").splitlines() if l.startswith("worktree ")}


def live_slots():
    """Slot names whose worktree is claimed by a fresh lane-registry entry; None when the registry cannot be read (fail closed)."""
    try:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "repo_intel"))
        import lane_registry  # noqa: PLC0415
        from pathlib import Path  # noqa: PLC0415
        lanes = lane_registry.list_lanes(Path(MAIN_ROOT))["lanes"]
    except Exception:  # noqa: BLE001
        return None
    out = set()
    for e in lanes:
        m = re.search(r"mc2-slot(\d+)$", (e.get("worktree") or "").replace("\\", "/").rstrip("/"), re.I)
        if m and not e.get("stale"):
            out.add(f"slot{int(m.group(1)):02d}")
    return out


def legacy_owner(path):
    """(slot name or None, description) for a legacy dir: sNN(x) maps to slotNN; the worktree's branch is shown."""
    m = re.fullmatch(r"mc2-tmp-s(\d+)[a-z]?", os.path.basename(norm(path)))
    if not m:
        return None, "unknown (not a slot-named dir)"
    slot = f"slot{int(m.group(1)):02d}"
    wt = f"{MAIN_ROOT.rsplit('/', 1)[0]}/mc2-{slot}"
    out = subprocess.run(["git", "-C", wt, "branch", "--show-current"], capture_output=True, text=True,
                         **console_child.child_kwargs(redirected=True)).stdout.strip()
    branch = out or "(detached/none)"
    return slot, f"{slot} now on {branch}"


def is_legacy(path):
    p = lower(path)
    return any(p == lower(f"{LEGACY_PARENT}mc2-tmp-{n}") for n in LEGACY)


def in_scratch_root(path):
    p = lower(path)
    return any(re.fullmatch(re.escape(lower(r)) + r"/slot\d\d", p) for r in ROOTS)


def tree_files(path):
    """Yield (full path, is_dir, is_link) without following links."""
    for parent, dirs, files in os.walk(path, followlinks=False):
        for n in dirs:
            fp = os.path.join(parent, n)
            yield fp, True, os.path.islink(fp) or getattr(os.path, "isjunction", lambda _: False)(fp)
        for n in files:
            yield os.path.join(parent, n), False, os.path.islink(os.path.join(parent, n))


def tree_size(path):
    seen, total = set(), 0
    for fp, is_dir, _ in tree_files(path):
        if is_dir:
            continue
        try:
            st = os.lstat(fp)
        except OSError:
            continue
        key = (st.st_dev, st.st_ino)
        if st.st_ino and key in seen:
            continue
        seen.add(key)
        total += st.st_size
    return total


def evidence_files(path):
    """Crash evidence: files below a crashes/ dir, plus *.dmp / *.mdmp anywhere."""
    out = []
    for fp, is_dir, _ in tree_files(path):
        if is_dir:
            continue
        rel = os.path.relpath(fp, path).replace("\\", "/").lower().split("/")
        if "crashes" in rel[:-1] or fp.lower().endswith(EVIDENCE_SUFFIX):
            out.append(fp)
    return out


def mirror_index():
    idx = set()
    for parent, _, files in os.walk(CRASH_MIRROR):
        for n in files:
            try:
                idx.add((n.lower(), os.path.getsize(os.path.join(parent, n))))
            except OSError:
                pass
    return idx


def refusal(path, released_slot=None):
    """Reason the path may not be deleted, or None."""
    p = lower(path)
    if any(p == lower(f) or p.startswith(lower(f) + "/") for f in FORBIDDEN_PREFIXES):
        return "protected tree (archive / mc2-opengl)"
    if any(part.startswith(FORBIDDEN_PARTS) for part in p.split("/")):
        return "owner-test / v0.4 / v0.4c path"
    if not (in_scratch_root(path) or is_legacy(path)):
        return "outside the scratch roots and not an allow-listed legacy dir"
    wts = worktree_paths()
    if any(p == w or p.startswith(w + "/") or w.startswith(p + "/") for w in wts):
        return "is or contains a worktree"
    if os.path.lexists(path) and (os.path.islink(path) or getattr(os.path, "isjunction", lambda _: False)(path)):
        return "path is a link"
    slot = None
    m = re.search(r"/(slot\d\d)$", p)
    if m:
        slot = m.group(1)
    else:
        slot = legacy_owner(path)[0]
    if slot and slot != released_slot:
        live = live_slots()
        if live is None:
            return "lane registry unreadable (fail closed)"
        if slot in live:
            return f"live lane holds {slot}"
    if os.path.isdir(path):
        if any(link for _, _, link in tree_files(path)):
            return "tree contains a link or junction"
        ev = evidence_files(path)
        if ev:
            idx = mirror_index()
            miss = [f for f in ev if (os.path.basename(f).lower(), os.path.getsize(f)) not in idx]
            if miss:
                return f"{len(miss)} crash file(s) not mirrored under {CRASH_MIRROR} (first: {miss[0]})"
    return None


def _rm_ro(func, path, _exc):
    os.chmod(path, stat.S_IWRITE)
    func(path)


def reap_path(path, apply=False, released_slot=None, size=True):
    """Returns (bytes, kept_reason). Deletes only when apply and no refusal."""
    path = norm(path)
    if not os.path.exists(path):
        return 0, "absent"
    why = refusal(path, released_slot)
    if why:
        print(f"[scratch_reap] KEPT {path}: {why}")
        return 0, why
    n = tree_size(path) if size else 0
    if apply:
        shutil.rmtree(path, onerror=_rm_ro)
        print(f"[scratch_reap] DELETED {path} ({n / 2**30:.2f} GiB)")
    else:
        print(f"[scratch_reap] DRY-RUN would delete {path} ({n / 2**30:.2f} GiB)")
    return n, None


def reap_slot(slot, apply=False, released=False, size=True):
    s = slot_name(slot)
    total = 0
    for d in slot_dirs(s):
        n, _ = reap_path(d, apply, s if released else None, size)
        total += n
    print(f"[scratch_reap] {s}: {'freed' if apply else 'would free'} {total / 2**30:.2f} GiB")
    return total


def legacy_list(apply=False, size=True):
    total, kept = 0, []
    for name in LEGACY:
        path = f"{LEGACY_PARENT}mc2-tmp-{name}"
        if not os.path.isdir(path):
            continue
        owner = legacy_owner(path)[1]
        why = refusal(path)
        n = tree_size(path) if size else 0
        print(f"{path:<24} {n / 2**30:8.2f} GiB  owner: {owner:<46} {'KEEP: ' + why if why else 'reapable'}")
        if why:
            kept.append((path, why))
        elif apply:
            reap_path(path, True, size=False)
            total += n
        else:
            total += n
    print(f"[scratch_reap] legacy: {'freed' if apply else 'would free'} {total / 2**30:.2f} GiB; kept {len(kept)}: "
          + ("; ".join(p for p, _ in kept) or "none"))
    return total, kept


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("slot", nargs="?")
    ap.add_argument("--legacy", metavar="PATH")
    ap.add_argument("--legacy-list", action="store_true")
    ap.add_argument("--apply", action="store_true", help="delete (default: dry run)")
    ap.add_argument("--dry-run", action="store_true", help="explicit no-op: dry run is the default")
    ap.add_argument("--no-size", action="store_true", help="skip the size walk (fast)")
    a = ap.parse_args(argv)
    if a.apply and a.dry_run:
        ap.error("--apply and --dry-run conflict")
    size = not a.no_size
    if a.legacy_list:
        legacy_list(a.apply, size)
    elif a.legacy:
        if not is_legacy(a.legacy):
            print(f"[scratch_reap] REFUSED {a.legacy}: not an allow-listed legacy dir")
            return 2
        reap_path(a.legacy, a.apply, size=size)
    elif a.slot:
        reap_slot(a.slot, a.apply, size=size)
    else:
        ap.error("give a slot, --legacy PATH or --legacy-list")
    return 0


if __name__ == "__main__":
    sys.exit(main())
