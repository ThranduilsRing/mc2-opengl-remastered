#!/usr/bin/env python3
"""spawn_lane.py -- fast agent-lane worktree standup (DEV-VELOCITY-LANES-1).

Stands up a ready-to-build engine lane (worktree + 3rdparty + warm build64)
in ~20-30 s by CLONING a donor worktree's prebuilt state, instead of the
~2.2 min fresh path (checkout + unzip + configure + cold build).

Measured on the reference machine (16 cores, NVMe, 2026-07-01):

  fresh path:  worktree add 6 s + unzip 3rdparty 1 s + configure 10 s
               + cold build (mc2, 596 TUs) 113 s            = ~130 s
  clone path:  worktree add --no-checkout 0.1 s + robocopy 1.3 GB donor 3 s
               + build64 path-rewrite 1 s + git reset/checkout 6 s
               + sanity no-op build 15 s                    = ~25 s

Clone correctness was proven empirically: after the path rewrite the cloned
build64 does a true no-op build (0 TUs), and touching one .cpp recompiles
exactly that TU + relinks (14 s). See docs/dev-velocity.md.

HOW THE CLONE WORKS
  1. git worktree add --no-checkout  (worktree registered, no files written)
  2. robocopy the ENTIRE donor worktree (sources + 3rdparty + build64) with
     mtimes preserved (/COPY:DAT). Preserving source mtimes is load-bearing:
     a normal checkout writes fresh mtimes, which makes every source newer
     than the copied .obj files -> MSBuild would rebuild the world.
  3. Rewrite the donor's absolute path inside build64 text files (vcxproj,
     sln, CMakeCache, CMakeFiles, .tlog). MSBuild tlogs are UTF-16-LE with
     UPPERCASE paths; CMake writes a lowercase drive letter in the cache
     header -- all spellings are handled (see path_variants()).
  4. git reset <base> + git checkout -- .  restores the tracked tree to the
     base ref. Files that differ from what the donor last compiled get fresh
     mtimes -> exactly those TUs recompile on the first build. Honest.
  5. Optional sanity build (default on): cmake --build --target mc2.

KNOWN LIMITS (documented, accepted):
  - Clone from a QUIESCENT donor (not mid-build). A donor rebuilt/deleted
    later can invalidate cloned .obj->compiler-PDB references (/Zi embeds
    absolute PDB paths in .obj); if a later link fails with PDB errors,
    rebuild with --clean-first (~2 min).
  - build64/out generated headers may still embed the donor BRANCH NAME
    (e.g. assimp revision.h GitBranch) -- a label, not a path; refreshed on
    the next reconfigure.
  - CLAUDE.md bans symlinks/junctions for build dirs; this tool only ever
    COPIES (robocopy), never links.

Usage:
  py -3 tools/spawn_lane.py <lane-name> [--base <ref>] [--donor <path>]
        [--dest-root A:/Games] [--mode auto|clone|fresh] [--branch NAME]
        [--no-build] [--suggest-deploy] [--dry-run]

Examples:
  py -3 tools/spawn_lane.py shadow-cache-v2
  py -3 tools/spawn_lane.py decal-fix --base e9b02d7e --suggest-deploy
  py -3 tools/spawn_lane.py demo --dry-run

This tool creates a new worktree + branch and writes ONLY inside it.
It never mutates the donor. Sibling of scripts/new_lane.py (which seeds
lane NOTES under .claude/worktrees/); spawn_lane.py owns the BUILD standup
for A:/Games/mc2-<name> style engine lanes.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

# --- constants ---------------------------------------------------------------

DEFAULT_DEST_ROOT = "A:/Games"
LANE_DIR_PREFIX = "mc2-"

CMAKE_EXE_CANDIDATES = [
    "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/"
    "IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe",
    "cmake",  # PATH fallback
]

# Canonical configure flags (mirrors the donor caches in active lanes).
CONFIGURE_FLAGS = [
    "-G", "Visual Studio 17 2022",
    "-DCMAKE_LIBRARY_ARCHITECTURE=x64",
    "-DMC2_IMGUI=ON",
    "-DMC2_EDITOR=ON",
    "-DMC2_BUILD_TESTS=ON",
]

# Directories never worth cloning (transient run junk; may hold lock files).
CLONE_EXCLUDE_DIRS = ["tests\\smoke\\artifacts", "debug_state", "_harness_out"]

KEBAB_RE = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*$")

# Binary payloads inside build64 that must never be text-rewritten.
BINARY_EXT = {
    ".obj", ".pdb", ".exe", ".dll", ".lib", ".exp", ".ilk", ".pch",
    ".res", ".idb", ".bin", ".pyc", ".zip", ".tga", ".png", ".ipdb",
    ".iobj", ".cache", ".suo", ".db",
}
MAX_REWRITE_SIZE = 64 * 1024 * 1024


# --- small helpers -----------------------------------------------------------

def log(msg: str) -> None:
    print(f"[spawn_lane] {msg}")


def die(msg: str, rc: int = 2) -> None:
    sys.stderr.write(f"[spawn_lane] ERROR: {msg}\n")
    sys.exit(rc)


def run(cmd: list[str], cwd: str | None = None, check: bool = True,
        capture: bool = True) -> subprocess.CompletedProcess:
    proc = subprocess.run(cmd, cwd=cwd, capture_output=capture, text=True)
    if check and proc.returncode != 0:
        die("command failed (rc={0}): {1}\n{2}".format(
            proc.returncode, " ".join(cmd),
            (proc.stderr or proc.stdout or "").strip()[-2000:]))
    return proc


def git(args: list[str], cwd: str, check: bool = True) -> subprocess.CompletedProcess:
    return run(["git", "-C", cwd] + args, check=check)


class Timer:
    """Phase timer that accumulates a printable ledger."""

    def __init__(self) -> None:
        self.ledger: list[tuple[str, float]] = []

    def phase(self, name: str, fn):
        t0 = time.monotonic()
        result = fn()
        dt = time.monotonic() - t0
        self.ledger.append((name, dt))
        log(f"{name}: {dt:.1f} s")
        return result

    def report(self) -> None:
        total = sum(dt for _, dt in self.ledger)
        log("---- phase ledger ----")
        for name, dt in self.ledger:
            log(f"  {name:<28s} {dt:7.1f} s")
        log(f"  {'TOTAL':<28s} {total:7.1f} s")


# --- path rewriting (clone mode) ----------------------------------------------

def path_variants(old: str, new: str) -> list[tuple[bytes, bytes]]:
    """(old, new) byte-pattern pairs covering every path spelling observed in
    a VS/CMake build tree: forward/back/double-backslash, UPPERCASE (MSBuild
    .tlog), lowercase drive letter (CMakeCache header), each in UTF-8 and
    UTF-16-LE (tlogs are UTF-16-LE)."""
    old = old.rstrip("/\\")
    new = new.rstrip("/\\")
    spellings = [
        (old.replace("\\", "/"), new.replace("\\", "/")),
        (old.replace("/", "\\"), new.replace("/", "\\")),
        (old.replace("/", "\\\\"), new.replace("/", "\\\\")),
        (old.replace("/", "\\").upper(), new.replace("/", "\\").upper()),
        (old.replace("\\", "/").upper(), new.replace("\\", "/").upper()),
        (old[0].lower() + old[1:].replace("\\", "/"),
         new[0].lower() + new[1:].replace("\\", "/")),
        (old[0].lower() + old[1:].replace("/", "\\"),
         new[0].lower() + new[1:].replace("/", "\\")),
    ]
    out: list[tuple[bytes, bytes]] = []
    seen: set[bytes] = set()
    for o, n in spellings:
        for enc in ("utf-8", "utf-16-le"):
            ob, nb = o.encode(enc), n.encode(enc)
            if ob not in seen:
                seen.add(ob)
                out.append((ob, nb))
    return out


def rewrite_tree(root: Path, old: str, new: str) -> tuple[int, int]:
    """Rewrite donor paths under `root`. Returns (scanned, changed)."""
    pairs = path_variants(old, new)
    scanned = changed = 0
    for p in root.rglob("*"):
        if not p.is_file() or p.suffix.lower() in BINARY_EXT:
            continue
        try:
            if p.stat().st_size > MAX_REWRITE_SIZE:
                continue
            data = p.read_bytes()
        except OSError:
            continue
        scanned += 1
        new_data = data
        for ob, nb in pairs:
            if ob in new_data:
                new_data = new_data.replace(ob, nb)
        if new_data != data:
            p.write_bytes(new_data)
            changed += 1
    return scanned, changed


# --- copy (clone mode) ---------------------------------------------------------

def clone_copy(donor: Path, dest: Path) -> None:
    """Copy donor worktree -> dest preserving mtimes (load-bearing for MSBuild
    incrementality). robocopy on Windows; shutil fallback elsewhere (tests)."""
    if shutil.which("robocopy"):
        cmd = [
            "robocopy", str(donor), str(dest),
            "/E", "/COPY:DAT", "/DCOPY:DAT", "/MT:16",
            "/XF", ".git",          # donor worktree pointer file
            "/XD", ".git",          # donor main-checkout .git dir
            "/NFL", "/NDL", "/NJH", "/NJS", "/NP", "/R:1", "/W:1",
        ]
        for d in CLONE_EXCLUDE_DIRS:
            cmd += ["/XD", d]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode >= 8:  # robocopy: <8 means success
            die(f"robocopy failed (rc={proc.returncode}):\n"
                f"{(proc.stderr or proc.stdout).strip()[-2000:]}")
    else:
        def ignore(dirpath, names):
            drop = {".git"}
            rel = os.path.relpath(dirpath, donor).replace("/", "\\")
            for d in CLONE_EXCLUDE_DIRS:
                head, _, tail = d.rpartition("\\")
                if rel == (head or ".") and tail in names:
                    drop.add(tail)
            return drop
        shutil.copytree(donor, dest, ignore=ignore, dirs_exist_ok=True)


# --- git plumbing ---------------------------------------------------------------

def branch_exists(repo: str, branch: str) -> bool:
    return git(["rev-parse", "--verify", "--quiet", "refs/heads/" + branch],
               repo, check=False).returncode == 0


def derive_branch(repo: str, lane: str) -> str:
    n = 1
    while True:
        cand = f"claude/{lane}-{n}"
        if not branch_exists(repo, cand):
            return cand
        n += 1


def resolve_ref(repo: str, ref: str) -> str:
    proc = git(["rev-parse", "--verify", "--quiet", ref + "^{commit}"],
               repo, check=False)
    if proc.returncode != 0:
        die(f"base ref '{ref}' does not resolve to a commit in {repo}")
    return proc.stdout.strip()


def find_cmake() -> str | None:
    for cand in CMAKE_EXE_CANDIDATES:
        if cand == "cmake":
            if shutil.which("cmake"):
                return "cmake"
        elif Path(cand).is_file():
            return cand
    return None


# --- deploy suggestion -----------------------------------------------------------

def suggest_deploy(donor: Path, branch: str) -> None:
    """Advisory: ask check-deploy-target.py for a free deploy dir so lanes
    don't contend for smoke folders. Never fatal."""
    script = donor / "scripts" / "check-deploy-target.py"
    if not script.is_file():
        log("deploy suggestion skipped (scripts/check-deploy-target.py not found)")
        return
    proc = run([sys.executable, str(script), "--suggest-free", "--branch", branch],
               cwd=str(donor), check=False)
    out = (proc.stdout or proc.stderr or "").strip()
    log("deploy suggestion (check-deploy-target.py --suggest-free):")
    for line in out.splitlines()[-8:]:
        log(f"  {line}")


# --- main ------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        description="Stand up a new engine lane fast (worktree + 3rdparty + "
                    "warm build64 clone).")
    ap.add_argument("lane", help="kebab-case lane name, e.g. shadow-cache-v2")
    ap.add_argument("--base", default=None,
                    help="base ref to fork from (default: donor HEAD)")
    ap.add_argument("--donor", default=None,
                    help="donor worktree to clone build state from "
                         "(default: the worktree containing the CWD)")
    ap.add_argument("--dest-root", default=DEFAULT_DEST_ROOT,
                    help="parent dir for the lane worktree (default: %(default)s)")
    ap.add_argument("--mode", choices=["auto", "clone", "fresh"], default="auto",
                    help="clone = copy donor's warm build64 (~25 s); fresh = "
                         "checkout + unzip + configure + cold build (~130 s); "
                         "auto = clone when donor has a built mc2.exe")
    ap.add_argument("--branch", default=None,
                    help="branch name (default: claude/<lane>-N, N bumped)")
    ap.add_argument("--no-build", action="store_true",
                    help="skip the build step (clone: sanity no-op build; "
                         "fresh: cold build)")
    ap.add_argument("--suggest-deploy", action="store_true",
                    help="print a free deploy dir suggestion for this lane "
                         "(avoids smoke-folder contention)")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the full plan; create nothing")
    return ap


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if not KEBAB_RE.match(args.lane):
        die(f"lane name '{args.lane}' is not kebab-case "
            "(e.g. 'shadow-cache-v2')")

    # --- resolve donor -----------------------------------------------------
    if args.donor:
        donor = Path(args.donor).resolve()
    else:
        proc = run(["git", "rev-parse", "--show-toplevel"], check=False)
        if proc.returncode != 0:
            die("not inside a git worktree and no --donor given")
        donor = Path(proc.stdout.strip())
    if not (donor / ".git").exists():
        die(f"donor '{donor}' is not a git worktree root")

    base_ref = args.base or "HEAD"
    base_sha = resolve_ref(str(donor), base_ref)

    dest = Path(args.dest_root) / (LANE_DIR_PREFIX + args.lane)
    if dest.exists():
        die(f"destination '{dest}' already exists; refusing to clobber", 4)

    branch = args.branch or derive_branch(str(donor), args.lane)
    if branch_exists(str(donor), branch):
        die(f"branch '{branch}' already exists", 4)

    donor_exe = donor / "build64" / "RelWithDebInfo" / "mc2.exe"
    donor_warm = donor_exe.is_file()
    mode = args.mode
    if mode == "auto":
        mode = "clone" if donor_warm else "fresh"
    if mode == "clone" and not donor_warm:
        die(f"--mode clone but donor has no built mc2.exe at {donor_exe}")

    cmake = find_cmake()
    dirty = git(["status", "--porcelain"], str(donor), check=False).stdout
    n_dirty = len([ln for ln in dirty.splitlines() if ln.strip()])

    # --- plan --------------------------------------------------------------
    log("=== lane standup plan ===")
    log(f"  lane:    {args.lane}")
    log(f"  branch:  {branch}")
    log(f"  base:    {base_ref} ({base_sha[:12]})")
    log(f"  donor:   {donor}  (warm build64: {donor_warm}, "
        f"{n_dirty} dirty files)")
    log(f"  dest:    {dest}")
    log(f"  mode:    {mode}")
    log(f"  build:   {'skipped (--no-build)' if args.no_build else 'yes'}")
    log(f"  cmake:   {cmake or 'NOT FOUND (build/configure steps will fail)'}")
    if mode == "clone":
        log("  steps:   worktree add --no-checkout -> robocopy donor "
            "(mtimes preserved) -> path-rewrite build64 -> git reset+checkout"
            + ("" if args.no_build else " -> sanity build"))
        if n_dirty:
            log(f"  note:    donor is dirty ({n_dirty} files); the tracked tree "
                "will be restored to the base ref (divergent TUs recompile "
                "on first build). Clone from a quiescent donor when possible.")
    else:
        log("  steps:   worktree add -> unzip 3rdparty.zip -> configure"
            + ("" if args.no_build else " -> cold build mc2 (~2 min)"))

    if args.dry_run:
        log("--dry-run: creating NOTHING. Above is what would happen.")
        return 0

    if cmake is None and not args.no_build:
        die("cmake not found (checked VS BuildTools path and PATH); "
            "rerun with --no-build or fix the cmake install")

    timer = Timer()

    # --- create worktree -----------------------------------------------------
    if mode == "clone":
        timer.phase("worktree add --no-checkout", lambda: git(
            ["worktree", "add", "--no-checkout", "-b", branch,
             str(dest), base_sha], str(donor)))
        timer.phase("robocopy donor", lambda: clone_copy(donor, dest))
        scanned_changed = timer.phase("path-rewrite build64", lambda: rewrite_tree(
            dest / "build64", str(donor).replace("\\", "/"),
            str(dest).replace("\\", "/")))
        log(f"  rewrite: scanned={scanned_changed[0]} changed={scanned_changed[1]}")

        def reset_checkout():
            git(["reset", "-q", base_sha], str(dest))
            git(["checkout", "-q", "--", "."], str(dest))
        timer.phase("git reset+checkout", reset_checkout)

        leftovers = git(["status", "--porcelain"], str(dest)).stdout
        untracked = [ln for ln in leftovers.splitlines() if ln.startswith("??")]
        if untracked:
            log(f"  {len(untracked)} untracked donor-scratch files came along "
                "(harmless; `git clean -nd` in the lane to review)")

        if not args.no_build:
            timer.phase("sanity build (mc2)", lambda: run(
                [cmake, "--build", "build64", "--config", "RelWithDebInfo",
                 "--target", "mc2"], cwd=str(dest)))
    else:  # fresh
        timer.phase("worktree add", lambda: git(
            ["worktree", "add", "-b", branch, str(dest), base_sha],
            str(donor)))

        def unzip_3rdparty():
            zp = dest / "3rdparty.zip"
            if not zp.is_file():
                die(f"{zp} missing -- git lfs pull needed?")
            if zp.stat().st_size < 1024 * 1024:
                die(f"{zp} is tiny ({zp.stat().st_size} B) -- LFS pointer, "
                    "not content. Run: git lfs pull")
            with zipfile.ZipFile(zp) as z:
                z.extractall(dest)
        timer.phase("unzip 3rdparty", unzip_3rdparty)

        if cmake:
            timer.phase("configure", lambda: run(
                [cmake, *CONFIGURE_FLAGS,
                 f"-DCMAKE_PREFIX_PATH={str(dest).replace(os.sep, '/')}/3rdparty",
                 "-B", "build64"], cwd=str(dest)))
            if not args.no_build:
                timer.phase("cold build (mc2)", lambda: run(
                    [cmake, "--build", "build64", "--config", "RelWithDebInfo",
                     "--target", "mc2"], cwd=str(dest)))

    timer.report()

    exe = dest / "build64" / "RelWithDebInfo" / "mc2.exe"
    log(f"lane ready: {dest}  (branch {branch})")
    log(f"  mc2.exe: {'present' if exe.is_file() else 'NOT BUILT YET'}")

    if args.suggest_deploy:
        suggest_deploy(donor, branch)

    log("next steps:")
    log(f"  cd {dest}")
    log("  # deploy:  py -3 scripts/deploy_payload.py <your-lane-deploy-dir> "
        "--source-root . --build-dir build64 --exe-name mc2.exe")
    log("  # pick dir: py -3 scripts/check-deploy-target.py --suggest-free "
        f"--branch {branch}")
    log("  # smoke:   run_smoke.py with --exe <your-deploy-dir>/mc2.exe "
        "(leases that folder; never share deploy dirs across lanes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
