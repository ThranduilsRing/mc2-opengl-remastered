#!/usr/bin/env python3
# scripts/make-isolated-deploy.py
"""Build a verified lane install cheaply; detach changed payload on deploy.

WHY THIS EXISTS
---------------
`scripts/check-hardlink-isolation.py` refuses to let a lane deploy into a
directory whose written artifacts are hardlinked to somebody else's install.
A refusal with no affordable remedy is just an obstacle, so this is the
remedy: it makes a private install for the ~0.6 GB that actually gets
written, instead of the full 12 GB.

THE SPLIT
---------
A full MC2 install MEASURES 12.26 GB / 40135 files (mc2-win64-v0.4) and A: had
8-11 GB free while this was written -- one naive copy does not fit, which is
exactly the P0 this session already suffered once. But almost all of that is
data the game only ever READS. So:

    stock + static manifested payload -> os.link(), no byte copy
    runtime/user writes                -> real copy, private inode

Lean installs carry .lane-seed.json. deploy_payload uses this receipt to
retain unchanged shared payload and atomically privatize changed files BEFORE
the existing isolation guard. A normal install without this receipt retains
the original guard behavior. Snapshot mode retains the original class split.

Classification is imported from check-hardlink-isolation.py rather than
restated here, so the two tools cannot drift into disagreeing about what
"mutated" means. That matters: the guard refusing files this tool did not
break out would produce a dir that fails its own verification, and the reverse
would produce a dir the guard calls clean while a deploy still clobbers a peer.

SELF-PROVING
------------
The last thing this script does is run the guard against the directory it just
built and require verdict OK. If the result is not isolated, the run FAILS --
it does not print a hopeful summary and leave a booby-trapped install behind.

FAILURE MODES IT REFUSES TO PAPER OVER
--------------------------------------
* --from and --to on DIFFERENT VOLUMES. os.link() cannot span volumes, so
  every file would silently fall back to a byte copy and the measured 0.64 GB
  install would land as 12.26 GB. Checked BEFORE anything is written, by st_dev,
  and fatal. This is the single most expensive way this tool could fail, and
  it is the one a per-file try/except would hide.
* --to already exists and is non-empty. Refused; the caller picks a new name.
  Merging into an existing install is how you end up unsure what you smoked.
* Not enough free space for the COPIED bytes. Delegated to
  scripts/check-disk-space.py with the measured figure -- and, per DISK-GUARD-3
  (c5d934d5), a guard that is missing or unrunnable is FATAL, not a warning.
* A failed link requires another disk guard for the increased byte cost,
  then a verified private copy. Cross-volume full copies remain refused.

Pure stdlib. Examples:

  # private lane install off the canonical 0.4 dir
  py -3 scripts/make-isolated-deploy.py \
      --from A:/Games/mc2-opengl/mc2-win64-v0.4 \
      --to   A:/Games/mc2-opengl/mc2-win64-mylane

  # see the cost first, write nothing
  py -3 scripts/make-isolated-deploy.py --from ... --to ... --dry-run

  # prove the tool works (temp dirs only)
  py -3 scripts/make-isolated-deploy.py --selftest
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import csv
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import threading
import json
from pathlib import Path
import console_child  # noqa: E402  (stdio-inheriting children keep their output; scripts/console_child.py)

PREFIX = "[ISO_DEPLOY]"
SCRIPTS = Path(__file__).resolve().parent

GIB = 1024 ** 3

# The convention root for private lane installs. `--lane NAME` resolves here so
# that the cheap, correct path is also the shortest one to type; a free-form
# `--to` invites ad-hoc locations and full-byte copies on the SYSTEM drive.
PRIVATE_ROOT = Path("A:/Games/mc2-opengl/private")

# Lane names become a directory under PRIVATE_ROOT, so they are validated
# rather than trusted: `--lane ../../../Windows` must not resolve outside the
# root, and a name with a drive letter or a separator is a caller who meant
# --to. Deliberately narrow -- these are lane names, not free-form paths.
_LANE_OK = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_."


def out(msg: str) -> None:
    print(f"{PREFIX} {msg}", flush=True)


def err(msg: str) -> None:
    print(f"{PREFIX} {msg}", file=sys.stderr)


def fail(msg: str, code: int = 1):
    err(f"FAIL: {msg}")
    sys.exit(code)


def gb(n: float) -> str:
    return f"{n / GIB:.2f} GB"


def _load_guard():
    """Import check-hardlink-isolation.py (hyphenated) as a module.

    Fatal if absent: without it this tool cannot classify files OR verify its
    own output, and guessing at either is how the incident happened.
    """
    path = SCRIPTS / "check-hardlink-isolation.py"
    if not path.is_file():
        fail(f"classifier/verifier not found at {path} -- refusing to build an "
             "install whose isolation cannot be classified or checked. It is a "
             "sibling of this script in the same repo; its absence means a "
             "broken or partial checkout.", 2)
    spec = importlib.util.spec_from_file_location("check_hardlink_isolation",
                                                  path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ---------------------------------------------------------------------------
# Planning
# ---------------------------------------------------------------------------

def plan(src: Path, guard, seed=None) -> tuple[list, list, int, int]:
    """Walk src once. Returns (to_link, to_copy, link_bytes, copy_bytes)."""
    to_link, to_copy = [], []
    link_bytes = copy_bytes = 0
    parents = set()
    for rel in seed or ():
        parents.update(p.as_posix().casefold() for p in Path(rel).parents)
    for dirpath, _dirs, files in os.walk(src):
        if seed is not None:
            # Prune BEFORE descending: runtime caches can dwarf the install.
            base = Path(dirpath).relative_to(src).as_posix()
            prefix = '' if base == '.' else base + '/'
            _dirs[:] = [d for d in _dirs if
                        (prefix + d).casefold() in ('data', 'assets') or
                        (prefix + d).casefold().startswith(('data/', 'assets/')) or
                        (prefix + d).casefold() in parents]
            _dirs[:] = [d for d in _dirs if (prefix + d).casefold() not in ('cache', 'mods')]
        for fn in files:
            p = Path(dirpath) / fn
            rel = p.relative_to(src).as_posix()
            if seed is not None and not seed_member(rel, guard, seed):
                continue
            try:
                size = p.stat().st_size
            except OSError:
                continue          # vanished mid-walk; nothing to reproduce
            if guard.classify(rel) == "mutated":
                to_copy.append((rel, size))
                copy_bytes += size
            else:
                to_link.append((rel, size))
                link_bytes += size
    return to_link, to_copy, link_bytes, copy_bytes


def seed_member(rel, guard, manifest):
    """Only runtime data and manifested payload enter the lean seed projection."""
    low = rel.casefold()
    if low.startswith('mods/') and not low.startswith(('mods/mc2-industrial-kit/', 'mods/mc2-vehicle-kit/')):
        return False
    if low in manifest or low == '.deployed_manifest.csv':
        # PDBs are deploy/debug outputs, never runtime seed inputs.
        return not low.endswith('.pdb')
    if '/' not in low:
        # Startup reads prefs.cfg unconditionally; minprefs.cfg supplies the
        # first-run options/orgprefs defaults. Neither is deployed payload.
        return low.endswith(('.fst', '.pak')) or low in (
            'options.cfg', 'system.cfg', 'prefs.cfg', 'minprefs.cfg', 'orgprefs.cfg')
    if low.startswith(('cache/', 'crashes/', 'debug_state/', 'saved_bookmarks/', 'mods/')):
        return False
    if low.startswith('data/') and low.endswith('.cookstamp'):
        return True  # private mutable receipt paired with the stock cooked texture
    if low.endswith(('.log', '.tmp', '.bak', '.cookstamp', '.dmp')):
        return False
    # Runtime texture caches and authoring tools are writable artifacts; only
    # explicitly deployed files in these roots are admitted above.
    return low.startswith(('data/', 'assets/')) and guard.classify(rel) == 'data'


def io_workers():
    """Bound concurrent filesystem operations by the host's lane CPU budget."""
    try:
        return max(1, min(4, int(Path('A:/Games/mc2-ops/JOBS').read_text().strip())))
    except (OSError, ValueError):
        return 2


def write_receipt(path, rows):
    """Publish a seed receipt: per-pid temp file, atomic replace, never fatal.

    Concurrent verifiers each replace the receipt; on Windows replace raises PermissionError while another
    process has it open. The receipt is only a hash cache, so retry briefly and then skip (the next run
    re-hashes) instead of failing the lane's install.
    """
    pending = path.with_suffix(f'.{os.getpid()}.{threading.get_ident()}.tmp')
    try:
        pending.write_text(json.dumps(rows), encoding='utf-8')
        for attempt in range(8):
            try:
                pending.replace(path)
                return True
            except PermissionError:
                time.sleep(0.05 * (attempt + 1))
        return False
    except OSError:
        return False
    finally:
        try:
            pending.unlink(missing_ok=True)
        except OSError:
            pass


def verified_seed(src, use_cache=False):
    """Verify the payload manifest before projecting a source into a lean tree."""
    path = src / '.deployed_manifest.csv'
    if not path.is_file():
        fail(f'lean seed requires a deployed manifest: {path}', 2)
    with path.open(encoding='utf-8-sig', newline='') as f:
        rows = csv.reader(f)
        if next(rows, [])[:2] != ['manifest_version', 'v1']:
            fail(f'unsupported seed manifest: {path}', 2)
        manifest = {r['relpath'].casefold(): r for r in csv.DictReader(f)}
    if 'mc2.exe' not in manifest:
        fail('lean seed manifest does not contain mc2.exe', 2)
    cache_path = src / '.lane-seed-verification.json'
    try:
        cached = json.loads(cache_path.read_text(encoding='utf-8')) if use_cache else {}
    except (OSError, ValueError):
        cached = {}
    def verify_member(member):
        rel, row = member
        candidate = (src / row['relpath']).resolve()
        if not candidate.is_relative_to(src.resolve()):
            fail(f'seed manifest path escapes source: {rel}', 2)
        if not candidate.is_file():
            fail(f'seed manifest file missing: {rel}', 2)
        # Another lane hardlinking a seed file bumps its NTFS ChangeTime (link count) mid-check without
        # touching the bytes: re-hash and re-stamp a few times; a real content change still fails the hash.
        for attempt in range(4):
            stamp = content_stamp(candidate)
            old = cached.get(rel, {})
            if old.get('stamp') == stamp and old.get('sha256') == row['sha256']:
                digest = old['sha256']
            else:
                digest = file_sha(candidate)
            if digest != row['sha256']:
                fail(f'seed manifest hash mismatch: {rel}', 2)
            if content_stamp(candidate) == stamp:
                break
        else:
            fail(f'seed changed during verification: {rel}', 2)
        return rel, {'stamp': stamp, 'sha256': digest}

    with ThreadPoolExecutor(max_workers=io_workers()) as pool:
        updated = dict(pool.map(verify_member, ((r, row) for r, row in manifest.items()
                                              if not r.endswith('.pdb'))))
    # Replace, never write through a potentially shared receipt inode.
    if use_cache:
        write_receipt(cache_path, updated)
    # Pinned kits and their verified native recipes enter the seed through kit_seed_members() (W138 merge: it supersedes
    # the W137 recipe rows that were added here; both carried the same pinned sources + cook receipts).
    return manifest


def refresh_link_receipts(src, shared):
    """Re-stamp seed receipts after our own hard links bumped NTFS ChangeTime.

    Linking changes only the link count/ChangeTime, so every later install would
    re-hash the whole seed (about 170 s). Accept the new stamp only when the
    verified entry still matches device, inode, size and mtime; any other drift
    keeps the old stamp and forces a re-hash.
    """
    wanted = {r.casefold(): v['stamp'] for r, v in shared.items()}
    for name in ('.lane-seed-verification.json', '.lane-kit-verification.json'):
        path = src / name
        try:
            cached = json.loads(path.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            continue
        changed = False
        for rel, entry in cached.items():
            new = wanted.get(rel)
            if new and entry.get('stamp', [])[:4] == new[:4] and entry['stamp'] != new:
                entry['stamp'] = new
                changed = True
        if changed:
            write_receipt(path, cached)


def restamp_seed(src):
    """After an install delete (link count down, ChangeTime up) re-stamp the seed."""
    rels = set()
    for name in ('.lane-seed-verification.json', '.lane-kit-verification.json'):
        try:
            rels.update(json.loads((src / name).read_text(encoding='utf-8')))
        except (OSError, ValueError):
            pass
    shared = {}
    for rel in rels:
        try:
            shared[rel] = {'stamp': content_stamp(src / rel)}
        except OSError:
            pass
    refresh_link_receipts(src, shared)
    return len(shared)


def content_stamp(path):
    """Include NTFS ChangeTime: Windows st_ctime is the creation time.

    A same-size edit with restored mtime must invalidate a verified hash.
    Unsupported filesystems/API errors fail closed, never trust mtime alone.
    """
    st = path.stat()
    changed = st.st_ctime_ns
    if os.name == 'nt':
        import ctypes
        import msvcrt
        from ctypes import wintypes

        class BasicInfo(ctypes.Structure):
            _fields_ = [('creation', ctypes.c_longlong), ('access', ctypes.c_longlong),
                        ('write', ctypes.c_longlong), ('change', ctypes.c_longlong),
                        ('attributes', wintypes.DWORD)]

        query = ctypes.windll.kernel32.GetFileInformationByHandleEx
        query.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        query.restype = wintypes.BOOL
        info = BasicInfo()
        with path.open('rb') as stream:
            if not query(msvcrt.get_osfhandle(stream.fileno()), 0, ctypes.byref(info), ctypes.sizeof(info)):
                raise ctypes.WinError()
        changed = info.change
    return [st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns, changed]


def kit_seed_members(src, seed, use_cache=False):
    """Carry pinned kits and verified native recipes, never arbitrary caches.

    A changed nifty kit must be deployed into the seed once before new lanes
    use --skip-kit. This comparison prevents silently seeding an obsolete kit.
    """
    import kit_payload
    extra = {}
    cache_path = src / '.lane-kit-verification.json'
    try:
        cached = json.loads(cache_path.read_text()) if use_cache else {}
    except (OSError, ValueError):
        cached = {}
    if use_cache:
        try:
            # The deployed manifest already covers most pinned kit sources.
            # Reuse that verified hash instead of reading gigabytes twice.
            cached.update(json.loads((src / '.lane-seed-verification.json').read_text()))
        except (OSError, ValueError):
            pass
    updated = {}

    def check(root, rows):
        seen = set()
        for row in rows:
            path = kit_payload.contained_path(root, row['path'])
            rel = path.relative_to(src).as_posix().casefold()
            if rel in seen:
                raise ValueError(f'duplicate kit member: {rel}')
            seen.add(rel)
            stamp = content_stamp(path)
            prior = cached.get(rel, {})
            if prior.get('stamp') != stamp or prior.get('sha256') != row['sha256']:
                kit_payload.checked_files(root, [row])
            if path.stat().st_size != row['bytes'] or content_stamp(path) != stamp:
                raise ValueError(f'kit changed during verification: {rel}')
            updated[rel] = {'stamp': stamp, 'sha256': row['sha256']}
    for name in ('industrial', 'vehicle'):
        rel = f'data/kit/{name}-kit.json'
        installed = src / rel
        pinned = SCRIPTS.parent / rel
        root = src / f'mods/mc2-{name}-kit'
        if not root.is_dir():
            continue
        if installed.is_file() and installed.read_bytes().replace(b'\r\n', b'\n') != pinned.read_bytes().replace(b'\r\n', b'\n'):
            fail(f'stale {name} seed kit; refresh from published nifty using deploy_payload.py --seed-owner-test --seed-source (see docs/plans/lane-install-fast.md)', 2)
        manifest = json.loads(pinned.read_text(encoding='utf-8'))
        check(root, manifest['files'])
        for row in manifest['files']:
            member = f'mods/mc2-{name}-kit/{row["path"]}'
            extra[member] = dict(row, relpath=member)
        receipt = src / f'cache/content-model/{name}-cook.json'
        if not receipt.is_file():
            fail(f'seed missing cooked {name} kit: refresh seed with a normal nifty deploy', 2)
        rows = json.loads(receipt.read_text())['files']
        check(receipt.parent, rows)
        for row in rows:
            member = f'cache/content-model/{row["path"]}'
            extra[member] = dict(row, relpath=member)
        extra[receipt.relative_to(src).as_posix()] = {
            'relpath': receipt.relative_to(src).as_posix(), 'sha256': kit_payload.sha(receipt)}
    if use_cache:
        write_receipt(cache_path, updated)
    return extra


def seed_plan(src, guard, seed, use_cache=True):
    """Inventory stock once per deployed seed revision, never its caches.

    Refreshing a seed from nifty replaces its manifest and invalidates this
    inventory. Delete the inventory after manually changing unmanifested stock.
    """
    cache = src / '.lane-seed-inventory.json'
    identity = file_sha(src / '.deployed_manifest.csv')
    try:
        previous = json.loads(cache.read_text()) if use_cache else {}
        if previous.get('identity') == identity:
            links, copies = previous['links'], previous['copies']
            for rel, size in links + copies:
                if Path(rel).is_absolute() or '..' in Path(rel).parts or size < 0:
                    raise ValueError('unsafe seed inventory')
            # Reclassify, rather than trust a persisted isolation decision.
            rows = links + copies
            links = [(r, n) for r, n in rows if guard.classify(r) == 'data']
            copies = [(r, n) for r, n in rows if guard.classify(r) != 'data']
            out('reusing stock inventory for this seed manifest')
            return links, copies, sum(n for _, n in links), sum(n for _, n in copies)
    except (OSError, ValueError, KeyError, TypeError):
        pass
    result = plan(src, guard, seed)
    if use_cache:
        pending = cache.with_suffix(f'.{os.getpid()}.tmp')
        try:
            pending.write_text(json.dumps(dict(identity=identity, links=result[0], copies=result[1])), encoding='utf-8')
            pending.replace(cache)
        finally:
            pending.unlink(missing_ok=True)
    return result


def same_volume(a: Path, b: Path) -> bool:
    """True iff a and b live on the same filesystem (os.link precondition).

    b may not exist yet; walk up to its nearest existing ancestor, which is on
    the same volume by construction.
    """
    probe = b
    while not probe.exists():
        if probe.parent == probe:
            return False
        probe = probe.parent
    try:
        return os.stat(a).st_dev == os.stat(probe).st_dev
    except OSError:
        return False


def resolve_lane(name: str) -> Path:
    """Map a --lane NAME to PRIVATE_ROOT/NAME, or die explaining why not.

    Validation is not paranoia about a hostile caller; it is about the tool
    silently doing something other than what the flag says. `--lane
    ../../mc2-win64-v0.4` that quietly resolved outside PRIVATE_ROOT would put
    a lane back exactly where this flag exists to stop it going.
    """
    if not name:
        fail("--lane needs a name", 2)
    if os.path.splitdrive(name)[0] or "/" in name or "\\" in name:
        fail(f"--lane takes a bare lane NAME, not a path: {name!r}\n"
             f"  It always resolves under {PRIVATE_ROOT}. If you genuinely "
             "want somewhere else, use --to and say so explicitly.", 2)
    if name.startswith(".") or name.strip() != name:
        fail(f"--lane name may not start with '.' or hold surrounding "
             f"whitespace: {name!r}", 2)
    bad = sorted({c for c in name if c not in _LANE_OK})
    if bad:
        fail(f"--lane name has character(s) not allowed in a directory name: "
             f"{''.join(bad)!r} in {name!r}", 2)
    return PRIVATE_ROOT / name


def system_drive() -> str:
    """The drive the OS is installed on, casefolded with colon (e.g. 'c:').

    Empty string when the platform has no drive-letter concept (POSIX) or the
    environment does not say. Callers decide what to do with "unknown" -- this
    function does not guess, because guessing 'c:' on a machine where it is
    wrong would refuse a perfectly good destination.
    """
    for var in ("SystemDrive", "SystemRoot", "windir"):
        val = os.environ.get(var)
        if val:
            drive = os.path.splitdrive(val)[0]
            if drive:
                return drive.casefold()
    return ""


def drive_of(p: Path) -> str:
    return os.path.splitdrive(str(p))[0].casefold()


def check_not_system_drive(dst: Path, src: Path) -> None:
    """LINK-GUARD-2: refuse to build a private install on the system drive.

    The volume check below is about whether os.link() *can* work. It catches
    `A: -> C:` only incidentally, as a volume mismatch, and says nothing at all
    about `C: -> C:` -- which links fine, verifies clean, and is the single
    case where every one of this tool's own success criteria is satisfied by
    the wrong answer. That is not hypothetical: lane FALSE-CONCLUSION-RETRACT-1
    put a 7.00 GB install at C:\\mc2-priv-fcr1 because A: was full, and nothing
    here objected.

    Fail-closed on an undeterminable system drive, but only on Windows, where
    one always exists -- on POSIX there is no drive letter to compare and
    nothing to enforce.

    LOUD escape hatch: MC2_ISO_ALLOW_SYSTEM_DRIVE=1.
    """
    if os.environ.get("MC2_ISO_ALLOW_SYSTEM_DRIVE") == "1":
        out("WARNING: MC2_ISO_ALLOW_SYSTEM_DRIVE=1 -- system-drive refusal "
            "BYPASSED BY REQUEST. You are about to write an install onto the "
            "drive the OS boots from.")
        return

    sd = system_drive()
    if not sd:
        if os.name == "nt":
            fail("could not determine the system drive (SystemDrive, "
                 "SystemRoot and windir are all unset or driveless) -- "
                 "refusing to write without knowing whether the destination "
                 "is the OS drive. Set MC2_ISO_ALLOW_SYSTEM_DRIVE=1 to bypass "
                 "deliberately.", 2)
        return                      # POSIX: no drive letters, nothing to check

    if drive_of(dst) != sd:
        return

    also_src = "" if drive_of(src) != sd else (
        f"\n  NOTE: --from is ALSO on {sd.upper()} ({src}). Same-drive means "
        "the volume check below passes and the result verifies as properly "
        "isolated -- correct by every measure this tool has except the one "
        "that matters. That is exactly how the 7 GB C: install happened.")

    fail(f"--to is on the SYSTEM DRIVE ({sd.upper()}): {dst}"
         f"{also_src}\n"
         f"  The system drive is not a deploy target -- not as a fallback, "
         f"not 'just this once because A: is full'. If A: is tight, reclaim "
         f"space or ask; do not spill onto {sd.upper()}.\n"
         f"  Use the convention root instead:  --lane <your-lane-name>  "
         f"(-> {PRIVATE_ROOT}\\<your-lane-name>)\n"
         f"  Deliberate override: MC2_ISO_ALLOW_SYSTEM_DRIVE=1.", 2)


def check_space(need_bytes: int, target: Path) -> None:
    """DISK-GUARD-3 convention (c5d934d5): LOW warns, everything else that is
    not OK -- including a guard that will not run -- is fatal."""
    guard = SCRIPTS / "check-disk-space.py"
    if not guard.is_file():
        fail(f"disk guard not found at {guard} -- refusing to write "
             "UNGUARDED. Restore it, or set MC2_ISO_SKIP_DISK_GUARD=1 to "
             "bypass deliberately.", 2)
    try:
        rc = subprocess.run(
            [sys.executable, str(guard), "--path", str(target),
             "--need-bytes", str(need_bytes),
             "--label", "isolated deploy (copied bytes only)"],
            timeout=60, **console_child.child_kwargs()).returncode
    except Exception as e:  # noqa: BLE001
        fail(f"disk guard could not run ({e}) -- refusing to write UNGUARDED. "
             "Set MC2_ISO_SKIP_DISK_GUARD=1 to bypass deliberately.", 2)
    if rc in (2, 3):
        fail("insufficient free space for the COPIED portion -- NOTHING was "
             "written. See the [DISK_GUARD] lines above.", 3)
    if rc == 1:
        out("WARNING: disk guard verdict LOW -- it fits, headroom is thin.")
    elif rc != 0:
        fail(f"disk guard returned unrecognised exit code {rc} -- refusing to "
             "write on an unreadable verdict.", 2)


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

def build(src: Path, dst: Path, to_link, to_copy, expected=None) -> tuple[int, int]:
    made_dirs: set = set()
    for rel, _ in to_link + to_copy:
        d = (dst / rel).parent
        if d not in made_dirs:
            d.mkdir(parents=True, exist_ok=True)
            made_dirs.add(d)
    fallback_lock = threading.Lock()
    fallback_bytes = 0
    private_bytes = sum(n for _, n in to_copy)

    def link_one(member):
        nonlocal fallback_bytes
        rel, size = member
        target = dst / rel
        try:
            os.link(src / rel, target)
        except OSError as e:
            # Check the new cost before a fallback, including all pending
            # private copies. A cross-volume install is still refused up front.
            with fallback_lock:
                fallback_bytes += size
                check_space(int((private_bytes + fallback_bytes) * 1.15), dst)
            out(f'link unavailable for {rel}: {e}; guarded copy fallback')
            shutil.copy2(src / rel, target)
            if file_sha(target) != file_sha(src / rel):
                fail(f'fallback copy hash mismatch: {rel}', 4)
            return False
        return True

    def copy_one(member):
        rel, _ = member
        target = dst / rel
        try:
            shutil.copy2(src / rel, target)
            row = (expected or {}).get(rel.casefold())
            if row is not None and file_sha(target) != row['sha256']:
                fail(f'copied manifest hash mismatch: {rel}', 4)
        except OSError as e:
            fail(f"copy failed for {rel}: {e}\n"
                 f"{dst} is INCOMPLETE -- delete it and re-run.", 4)

    with ThreadPoolExecutor(max_workers=io_workers()) as pool:
        started = time.perf_counter()
        linked = sum(pool.map(link_one, to_link))
        out(f'timing link creation (including fallbacks): {time.perf_counter() - started:.3f}s')
        started = time.perf_counter()
        list(pool.map(copy_one, to_copy))
        out(f'timing private copies + changed-file hashes: {time.perf_counter() - started:.3f}s')
    return linked, len(to_link) - linked + len(to_copy)


def file_sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify(dst: Path, guard) -> None:
    """Run the guard on what we just built and require isolation."""
    verdict, facts, msgs = guard.run_check(str(dst), deep=True,
                                           label="freshly isolated install")
    if verdict != "OK":
        for m in msgs:
            err(m)
        fail(f"post-build verification says {verdict}, not OK. The install at "
             f"{dst} is NOT isolated -- do not deploy into it. This is a bug "
             "in this tool's classification, not something to work around.", 5)
    out(f"verified: {facts['mutated_files_checked']} mutated-class file(s) in "
        f"{dst} are private inodes (deep walk).")


# ---------------------------------------------------------------------------
# Selftest
# ---------------------------------------------------------------------------

def selftest() -> int:
    guard = _load_guard()
    failures: list[str] = []

    def check(name: str, cond: bool, detail: str = "") -> None:
        if cond:
            out(f"  selftest PASS: {name}")
        else:
            failures.append(name)
            err(f"  selftest FAIL: {name} {detail}")

    tmp = Path(tempfile.mkdtemp(prefix="mc2_isodeploy_"))
    try:
        src = tmp / "src"
        (src / "shaders").mkdir(parents=True)
        (src / "data" / "terrain").mkdir(parents=True)
        (src / "data" / "defs" / "ui").mkdir(parents=True)
        (src / "mc2.exe").write_bytes(b"EXE" * 100)
        (src / "options.cfg").write_text("cfg\n")
        (src / "shaders" / "a.frag").write_text("// a\n")
        (src / "data" / "defs" / "ui" / "PACKAGE.fit").write_text("defs\n")
        (src / "data" / "terrain" / "big.pak").write_bytes(b"D" * 100000)

        dst = tmp / "dst"
        to_link, to_copy, lb, cb = plan(src, guard)
        check("data is planned for linking",
              [r for r, _ in to_link] == ["data/terrain/big.pak"],
              str(to_link))
        check("mutated set is planned for copying",
              sorted(r for r, _ in to_copy) ==
              ["data/defs/ui/PACKAGE.fit", "mc2.exe", "options.cfg",
               "shaders/a.frag"], str(to_copy))
        check("copied bytes are the small side", cb < lb, f"{cb} vs {lb}")

        build(src, dst, to_link, to_copy)

        # The property that matters: linked data shares an inode, copied
        # artifacts do not.
        check("data file IS shared (that is the point)",
              os.stat(src / "data/terrain/big.pak").st_ino ==
              os.stat(dst / "data/terrain/big.pak").st_ino)
        check("exe is NOT shared",
              os.stat(src / "mc2.exe").st_ino !=
              os.stat(dst / "mc2.exe").st_ino)
        check("shader is NOT shared",
              os.stat(src / "shaders/a.frag").st_ino !=
              os.stat(dst / "shaders/a.frag").st_ino)

        v, _, _ = guard.run_check(str(dst), deep=True)
        check("guard calls the result isolated", v == "OK", f"got {v}")

        # End-to-end: a deploy-shaped write into dst must NOT touch src.
        newexe = tmp / "new.exe"
        newexe.write_bytes(b"NEWBUILD")
        shutil.copyfile(newexe, dst / "mc2.exe")
        check("deploying into the isolated dir leaves the source exe intact",
              (src / "mc2.exe").read_bytes() == b"EXE" * 100,
              f"src exe is now {(src / 'mc2.exe').read_bytes()[:20]!r}")
        check("...and the isolated dir got the new bytes",
              (dst / "mc2.exe").read_bytes() == b"NEWBUILD")

        # Cross-volume precondition helper agrees with itself.
        check("same_volume() is true within one temp tree",
              same_volume(src, tmp / "not-yet-created"))

        # ------------------------------------------------------------------
        # Default destination + system-drive refusal
        # ------------------------------------------------------------------
        def exit_code(fn, *a, **kw):
            """Run fn; return its sys.exit code, or None if it did not exit."""
            try:
                fn(*a, **kw)
            except SystemExit as e:
                return e.code
            return None

        check("--lane resolves under the convention root",
              resolve_lane("mylane-1") == PRIVATE_ROOT / "mylane-1",
              str(resolve_lane("mylane-1")))
        for bad_name in ("../escape", "..", "sub/dir", "sub\\dir",
                         "C:/elsewhere", ".hidden", "has space", "pipe|char",
                         ""):
            check(f"--lane rejects {bad_name!r}",
                  exit_code(resolve_lane, bad_name) == 2)

        # Synthesise the system drive rather than reading the real one, so the
        # assertion is identical on every machine and nothing ever probes C:.
        saved = {k: os.environ.get(k)
                 for k in ("SystemDrive", "SystemRoot", "windir",
                           "MC2_ISO_ALLOW_SYSTEM_DRIVE")}
        try:
            os.environ.pop("MC2_ISO_ALLOW_SYSTEM_DRIVE", None)
            os.environ["SystemDrive"] = "Q:"
            os.environ.pop("SystemRoot", None)
            os.environ.pop("windir", None)
            check("system_drive() reads SystemDrive", system_drive() == "q:",
                  system_drive())

            if os.name == "nt":
                check("REFUSES a destination on the system drive",
                      exit_code(check_not_system_drive,
                                Path("Q:/mc2-priv-lane"), Path("A:/donor"))
                      == 2)
                check("REFUSES system-drive dst even when src is there too "
                      "(the C:->C: case the volume check cannot see)",
                      exit_code(check_not_system_drive,
                                Path("Q:/mc2-priv-lane"), Path("Q:/donor"))
                      == 2)
                check("ALLOWS a destination off the system drive",
                      exit_code(check_not_system_drive,
                                Path("A:/Games/mc2-opengl/private/lane"),
                                Path("A:/donor")) is None)
                check("the convention root itself is allowed",
                      exit_code(check_not_system_drive,
                                PRIVATE_ROOT / "lane", Path("A:/donor"))
                      is None)

                os.environ["MC2_ISO_ALLOW_SYSTEM_DRIVE"] = "1"
                check("MC2_ISO_ALLOW_SYSTEM_DRIVE=1 bypasses the refusal",
                      exit_code(check_not_system_drive,
                                Path("Q:/mc2-priv-lane"), Path("A:/donor"))
                      is None)
                os.environ.pop("MC2_ISO_ALLOW_SYSTEM_DRIVE", None)

                # Fail CLOSED when the system drive cannot be determined.
                os.environ.pop("SystemDrive", None)
                check("undeterminable system drive is FATAL on Windows",
                      exit_code(check_not_system_drive,
                                Path("A:/somewhere"), Path("A:/donor")) == 2)
            else:
                # POSIX has no drive letters; there is nothing to enforce and
                # the guard must not invent a refusal.
                os.environ.pop("SystemDrive", None)
                check("no drive-letter concept -> no system drive",
                      system_drive() == "")
                check("...and the refusal is a no-op, not a false positive",
                      exit_code(check_not_system_drive,
                                Path("/tmp/lane"), Path("/tmp/donor")) is None)
        finally:
            for k, v in saved.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        err(f"SELFTEST FAILED ({len(failures)}): " + "; ".join(failures))
        return 1
    out("selftest OK -- data shared, written artifacts private, and a deploy "
        "into the copy cannot reach the original.")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Create a hardlink-isolated private lane install: share "
                    "read-only data, copy only what gets written.")
    ap.add_argument("--from", dest="src", help="verified deployed seed tree (.deployed_manifest.csv required)")
    ap.add_argument("--lane", dest="lane",
                    help=f"lane NAME -> {PRIVATE_ROOT}\\NAME (the default "
                         "destination convention; prefer this over --to)")
    ap.add_argument("--to", dest="dst",
                    help="explicit destination dir, when --lane's convention "
                         "root is genuinely not what you want")
    ap.add_argument("--dry-run", action="store_true",
                    help="report the plan and cost; write nothing")
    ap.add_argument('--snapshot', action='store_true',
                    help='merge-gate snapshot: retain warm caches and the complete source tree')
    ap.add_argument("--refresh-receipts", action="store_true",
                    help="re-stamp the --from seed's verification receipts after an install delete")
    ap.add_argument("--selftest", action="store_true",
                    help="prove the tool works; temp dirs only")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if args.refresh_receipts:
        if not args.src:
            fail("--refresh-receipts needs --from <seed>", 2)
        print(f'[ISO_DEPLOY] re-stamped {restamp_seed(Path(args.src))} seed receipts')
        return 0
    if args.lane and args.dst:
        fail("--lane and --to are two ways to say the same thing; pass one.\n"
             f"  --lane {args.lane}  would mean  --to {resolve_lane(args.lane)}",
             2)
    if not args.src:
        fail("--from is required (or --selftest). It names the install to base "
             "this one on, e.g. --from A:/Games/mc2-opengl/mc2-win64-v0.4 -- "
             "there is deliberately no default, because which build you branch "
             "from is a real choice and a wrong guess is a silently wrong "
             "baseline.", 2)
    if not args.lane and not args.dst:
        fail("no destination: pass --lane NAME (recommended) or --to DIR.\n"
             f"  --lane NAME resolves to {PRIVATE_ROOT}\\NAME, which is the "
             "convention root for private lane installs. Use your lane's name.",
             2)

    guard = _load_guard()
    src = Path(args.src).expanduser().resolve()
    dst = (resolve_lane(args.lane) if args.lane
           else Path(args.dst).expanduser().absolute())

    if not src.is_dir():
        fail(f"--from is not a directory: {src}", 2)

    # FIRST of the destination checks, deliberately. Every check
    # below also refuses a bad C: path, but each does so for an incidental
    # reason and says something less useful: an existing C: dir would report
    # "already exists", and `A: -> C:` would report a volume mismatch. Only
    # this one names the actual problem, and for `C: -> C:` it is the only
    # check with anything to say at all. Whichever refusal fires is the one
    # the caller reads, so the teaching one goes first.
    check_not_system_drive(dst, src)

    if dst.exists() and any(dst.iterdir()):
        fail(f"--to already exists and is not empty: {dst}\n"
             "  Refusing to merge into an existing install -- that is how a "
             "lane ends up unsure what it actually smoked. Pick a new name, "
             "or delete that dir yourself first.", 2)
    if str(dst).replace("\\", "/").casefold() == \
       str(src).replace("\\", "/").casefold():
        fail("--to and --from are the same directory", 2)

    if not same_volume(src, dst):
        fail(f"--from and --to are on DIFFERENT VOLUMES.\n"
             f"    from: {src}\n    to:   {dst}\n"
             "  os.link() cannot span volumes, so every one of these files "
             "would fall back to a byte copy: a 12 GB full install instead of "
             "well under one. Choose a --to on the same volume as --from.", 2)

    started = time.perf_counter()
    out(f"planning: {src}  ->  {dst}")
    seed = None if args.snapshot else verified_seed(src, use_cache=not args.dry_run)
    out(f'timing manifest verification: {time.perf_counter() - started:.3f}s')
    kit_started = time.perf_counter()
    extras = {} if seed is None else kit_seed_members(src, seed, use_cache=not args.dry_run)
    out(f'timing kit verification: {time.perf_counter() - kit_started:.3f}s')
    selection = None if seed is None else {**seed, **{r.casefold(): row for r, row in extras.items()}}
    full_link, full_copy, full_lb, full_cb = (plan(src, guard) if seed is None else
                                           seed_plan(src, guard, seed, use_cache=not args.dry_run))
    to_link = [(r, n) for r, n in full_link if seed is None or seed_member(r, guard, seed)]
    to_copy = [(r, n) for r, n in full_copy if seed is None or seed_member(r, guard, seed)]
    if seed is not None:
        # Static deployed bytes are detached by deploy_payload only when they
        # differ. Runtime/user writes still require private inodes immediately.
        static = [(r, n) for r, n in to_copy if r.casefold() in seed
                  and not r.casefold().endswith(('.cfg', '.ini', '.bat', '.cookstamp'))
                  and not r.casefold().startswith(('cache/', 'data/textures/'))
                  and r != '.deployed_manifest.csv']
        to_link.extend(static)
        static_names = {r for r, _ in static}
        to_copy = [(r, n) for r, n in to_copy if r not in static_names]
        known = {r for r, _ in to_link + to_copy}
        # Pinned kit sources are read-only at runtime. --skip-kit excludes
        # them from the deploy write set; a full kit deploy detaches changed
        # shared inodes before the existing exact-payload guard.
        def immutable_kit_member(rel):
            # Native recipes publish by atomic replacement; see the isolation
            # guard's contract. Their JSON receipts remain private.
            return rel.startswith('mods/') or rel.endswith('.mci')

        to_link.extend((r, (src / r).stat().st_size) for r in extras
                       if r not in known and immutable_kit_member(r))
        to_copy.extend((r, (src / r).stat().st_size) for r in extras
                       if r not in known and not immutable_kit_member(r))
    link_bytes = sum(n for _, n in to_link)
    copy_bytes = sum(n for _, n in to_copy)
    out(f'{"warm snapshot" if args.snapshot else "lean verified seed"}: {gb(full_lb + full_cb)} -> {gb(link_bytes + copy_bytes)}; '
        f'private copies {gb(full_cb)} -> {gb(copy_bytes)}')
    out(f"  link (read-only data) : {len(to_link):6d} files, "
        f"{gb(link_bytes)}  [costs nothing]")
    out(f"  copy (written artifacts): {len(to_copy):6d} files, "
        f"{gb(copy_bytes)}  [the real cost]")
    if link_bytes + copy_bytes:
        saved = 100.0 * link_bytes / (link_bytes + copy_bytes)
        out(f"  saved by linking      : {saved:.1f}% of the install")

    if args.dry_run:
        out("dry run -- nothing written.")
        return 0

    if os.environ.get("MC2_ISO_SKIP_DISK_GUARD") == "1":
        out("WARNING: MC2_ISO_SKIP_DISK_GUARD=1 -- free-space preflight "
            "BYPASSED BY REQUEST.")
    else:
        check_space(int(copy_bytes * 1.15), dst)

    dst.mkdir(parents=True, exist_ok=True)
    out(f'timing planning + disk guard: {time.perf_counter() - started:.3f}s')
    copying = time.perf_counter()
    linked, copied = build(src, dst, to_link, to_copy, selection)
    out(f'timing links + private copies + changed-file hashes: {time.perf_counter() - copying:.3f}s')
    # The seed is a projection, so retain only the manifest rows it contains.
    # This makes the resulting tree a verified source for the next lane, too.
    writing = time.perf_counter()
    if seed is not None:
        write_seed_manifest(dst, seed)
        shared = {r: {'stamp': content_stamp(dst / r), 'sha256': selection[r.casefold()]['sha256']}
                  for r, _ in to_link if r.casefold() in selection}
        (dst / '.lane-seed.json').write_text(json.dumps({'shared': shared}), encoding='utf-8')
        refresh_link_receipts(src, shared)
    out(f'timing manifest + lane receipt write: {time.perf_counter() - writing:.3f}s')
    checking = time.perf_counter()
    out(f"built: {linked} linked, {copied} copied")
    verdict, _, _ = guard.run_check(str(dst), payload_rels=[r for r, _ in to_copy] + ['.deployed_manifest.csv', '.lane-seed.json'])
    if verdict != 'OK':
        fail(f'post-build isolation: {verdict}', 5)
    out(f'timing isolation verification: {time.perf_counter() - checking:.3f}s')
    out(f'timing total: {time.perf_counter() - started:.3f}s')

    out("")
    out(f"ISOLATED INSTALL READY: {dst}")
    out("Next: deploy into it. It is not in deploy_payload's DEPLOY_ALLOWLIST "
        "(that list is about install COUNT, a separate concern), so pass "
        "--allow-new-target:")
    out(f"  py -3 scripts/deploy_payload.py \"{dst}\" --allow-new-target --ninja --skip-kit")
    return 0


def write_seed_manifest(dst, seed):
    with (dst / '.deployed_manifest.csv').open('w', encoding='utf-8', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['manifest_version', 'v1'])
        fields = ['relpath', 'sha256', 'bytes', 'src_commit', 'timestamp', 'mtime_ns']
        writer.writerow(fields)
        for row in seed.values():
            if (dst / row['relpath']).is_file() and not row['relpath'].lower().endswith('.pdb'):
                writer.writerow([row.get(k, '') for k in fields])


if __name__ == "__main__":
    sys.exit(main())
