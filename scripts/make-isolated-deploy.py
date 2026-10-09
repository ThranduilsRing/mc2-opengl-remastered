#!/usr/bin/env python3
# scripts/make-isolated-deploy.py
"""Build a private, hardlink-ISOLATED lane install cheaply.

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

    data class    -> os.link()        free, instant, zero bytes
    mutated class -> real byte copy   private inode, safe to overwrite

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
* A link that fails mid-run for any other reason. Fatal, with the count of
  what had been written, rather than a quiet copy fallback that changes the
  cost model out from under the disk guard's already-approved estimate.

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
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PREFIX = "[ISO_DEPLOY]"
SCRIPTS = Path(__file__).resolve().parent

GIB = 1024 ** 3

# LINK-GUARD-2 (2026-07-30, handoff from DISK-RECLAIM-EXECUTE-1): the
# convention root for private lane installs. Before this existed every lane
# invented its own location -- `C:\mc2-priv-fcr1`, `concrete-deliver-1-private`
# -- because `--to` was a free-form required path with nothing behind it. One
# of those inventions was a 7.00 GB full-byte copy on the user's SYSTEM drive.
# `--lane NAME` resolves here so that the cheap, correct path is also the
# shortest one to type; previously the expensive one was the easier one.
PRIVATE_ROOT = Path("A:/Games/mc2-opengl/private")

# Lane names become a directory under PRIVATE_ROOT, so they are validated
# rather than trusted: `--lane ../../../Windows` must not resolve outside the
# root, and a name with a drive letter or a separator is a caller who meant
# --to. Deliberately narrow -- these are lane names, not free-form paths.
_LANE_OK = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_."


def out(msg: str) -> None:
    print(f"{PREFIX} {msg}")


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

def plan(src: Path, guard) -> tuple[list, list, int, int]:
    """Walk src once. Returns (to_link, to_copy, link_bytes, copy_bytes)."""
    to_link, to_copy = [], []
    link_bytes = copy_bytes = 0
    for dirpath, _dirs, files in os.walk(src):
        for fn in files:
            p = Path(dirpath) / fn
            rel = p.relative_to(src).as_posix()
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
            timeout=60, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).returncode
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

def build(src: Path, dst: Path, to_link, to_copy) -> tuple[int, int]:
    linked = copied = 0
    made_dirs: set = set()

    def ensure(rel: str) -> Path:
        p = dst / rel
        d = p.parent
        if d not in made_dirs:
            d.mkdir(parents=True, exist_ok=True)
            made_dirs.add(d)
        return p

    for rel, _size in to_link:
        target = ensure(rel)
        try:
            os.link(src / rel, target)
        except OSError as e:
            fail(f"hardlink failed for {rel}: {e}\n"
                 f"  {linked} linked / {copied} copied before this point; "
                 f"{dst} is INCOMPLETE -- delete it and re-run.\n"
                 "  Not falling back to a byte copy: that would silently turn "
                 "a sub-GB operation into a 12 GB one, past a disk guard "
                 "that already approved the smaller figure.", 4)
        linked += 1
    for rel, _size in to_copy:
        target = ensure(rel)
        try:
            shutil.copyfile(src / rel, target)
        except OSError as e:
            fail(f"copy failed for {rel}: {e}\n"
                 f"  {linked} linked / {copied} copied before this point; "
                 f"{dst} is INCOMPLETE -- delete it and re-run.", 4)
        copied += 1
    return linked, copied


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
        # LINK-GUARD-2: default destination + system-drive refusal
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
    ap.add_argument("--from", dest="src", help="existing install to base on")
    ap.add_argument("--lane", dest="lane",
                    help=f"lane NAME -> {PRIVATE_ROOT}\\NAME (the default "
                         "destination convention; prefer this over --to)")
    ap.add_argument("--to", dest="dst",
                    help="explicit destination dir, when --lane's convention "
                         "root is genuinely not what you want")
    ap.add_argument("--dry-run", action="store_true",
                    help="report the plan and cost; write nothing")
    ap.add_argument("--selftest", action="store_true",
                    help="prove the tool works; temp dirs only")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
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

    # LINK-GUARD-2: FIRST of the destination checks, deliberately. Every check
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

    out(f"planning: {src}  ->  {dst}")
    to_link, to_copy, link_bytes, copy_bytes = plan(src, guard)
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
    linked, copied = build(src, dst, to_link, to_copy)
    out(f"built: {linked} linked, {copied} copied")
    verify(dst, guard)

    out("")
    out(f"ISOLATED INSTALL READY: {dst}")
    out("Next: deploy into it. It is not in deploy_payload's DEPLOY_ALLOWLIST "
        "(that list is about install COUNT, a separate concern), so pass "
        "--allow-new-target:")
    out(f"  py -3 scripts/deploy_payload.py \"{dst}\" --allow-new-target")
    return 0


if __name__ == "__main__":
    sys.exit(main())
