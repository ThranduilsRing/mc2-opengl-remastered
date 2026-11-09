#!/usr/bin/env python3
"""ninja_build.py -- build the Ninja dev tree (build64-ninja/) under the MSVC env.

WHY: ninja needs cl/link on PATH (vcvars64) which a plain shell lacks; this wraps
that so the dev build is one short command. Extra args are passed to ninja
(e.g. -n for a dry run, -j8); non-flag args are targets.

USAGE:
  py -3 scripts/ninja_build.py                       # mc2
  py -3 scripts/ninja_build.py mc2                   # one target
  py -3 scripts/ninja_build.py -n mc2                # dry run: what would rebuild
  py -3 scripts/ninja_build.py --warm --base <ref> -j3   # lane prep: detach HEAD at <ref>, then build this tree in the background
Configure first (once): py -3 scripts/configure_ninja.py (--warm configures a missing tree itself)

One build per tree: a build takes <tree>/.build.lock and a second one waits (MC2_BUILD_LOCK_WAIT seconds, default 3600; 0 = exit
at once with the holder named). lane_slot.py acquire records the warm build's pid in <tree>/warm.pid; --no-warm skips that build.

--warm returns at once; the build runs detached at BelowNormal priority and logs to <tree>/warm.log, whose last line is
"[warm] done rc=<n> elapsed=<s>s". --base needs a clean tracked tree (it runs `git switch --detach <ref>`). No daemon: one build, then it exits.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import console_child  # noqa: E402
from configure_ninja import (DEFAULT_TARGETS, DEFAULT_TREE, NINJA, compile_cache_env, configure, ensure_cache_server, tree_uses_cache,
                             under_vcvars)  # noqa: E402


# PDBs retain obsolete type records across links. Compact before gate copies
# and per-run freshness hashes dominate; the fresh link retains full symbols.
# A fresh link of mc2.pdb is about 240 MB (2026-10-27, /Z7 objects); 1 GB let incremental links drift to 800 MB+ in the installs.
PDB_MAX_BYTES = 512 * 1024 ** 2


def drop_oversize_pdb(root, tree, limit=PDB_MAX_BYTES):
    pdb = os.path.join(root, tree, "mc2.pdb")
    try:
        size = os.path.getsize(pdb)
        if size > limit:
            os.remove(pdb)
            exe = os.path.join(root, tree, "mc2.exe")
            if os.path.isfile(exe):
                os.remove(exe)  # PDB is a linker side effect; force Ninja to regenerate both.
            print(f"[ninja_build] deleted {tree}/mc2.pdb ({size / 1024 ** 2:.0f} MB > {limit / 1024 ** 2:.0f} MB; growth guard)", flush=True)
            return True
    except FileNotFoundError:
        pass
    except OSError as exc:   # an open pdb (a link or the debugger is using it) cannot be deleted: say so, the LNK1318 guard did nothing
        print(f"[ninja_build] WARNING: could not delete oversize {tree}/mc2.pdb ({exc}); the next link/copy retains its excess debug records.",
              file=sys.stderr, flush=True)
    return False


def warm(root, argv, tree):
    """Start `ninja_build.py <argv> --warm-run` detached (log in the tree) and return."""
    base = None
    if "--base" in argv:
        i = argv.index("--base")
        base = argv[i + 1]
        del argv[i:i + 2]
    lock = None
    if base:   # take the tree lock BEFORE the checkout moves: a build running in this tree must not have its sources switched under it
        lock = TreeLock(root, tree)
        if not lock.acquire(float(os.environ.get("MC2_BUILD_LOCK_WAIT", "3600"))):
            sys.exit(f"warm: {tree} is being built by another process ({lock.holder()}); not switching the base under it")
    try:
        _warm_locked(root, argv, tree, base)
    finally:
        if lock is not None:
            lock.close()


def _warm_locked(root, argv, tree, base):
    if base:
        dirty = subprocess.run(["git", "-C", root, "status", "--porcelain", "--untracked-files=no"], capture_output=True, text=True,
                               **console_child.child_kwargs(redirected=True)).stdout.strip()
        if dirty:
            sys.exit("warm: tracked changes present, refusing to switch the base:\n" + dirty)
        switched = subprocess.run(["git", "-C", root, "switch", "--detach", base], capture_output=True, text=True,
                                  **console_child.child_kwargs(redirected=True))
        if switched.returncode:
            sys.exit("warm: git switch --detach failed: " + switched.stderr.strip())
    if not os.path.exists(f"{root}/{tree}/build.ninja"):
        configure(root, tree)
    log = f"{root}/{tree}/warm.log"
    env = dict(os.environ, _MSPDBSRV_ENDPOINT_=f"warm-{os.getpid()}")
    flags = 0x00000008 | 0x00000200 | 0x00004000          # DETACHED_PROCESS | NEW_PROCESS_GROUP | BELOW_NORMAL_PRIORITY_CLASS
    with open(log, "w") as out:
        # background-launch: allow detached console build (ninja), not the game; DETACHED_PROCESS is in `flags`
        child = subprocess.Popen([sys.executable, os.path.abspath(__file__), *argv, "--warm-run"], cwd=root, env=env, stdin=subprocess.DEVNULL,
                                 stdout=out, stderr=subprocess.STDOUT, creationflags=flags if os.name == "nt" else 0)
    print(f"warm: pid {child.pid} building {tree} at {base or 'HEAD'}; log {log}")


class TreeLock:
    """One build per tree: an OS byte-range lock on <tree>/.build.lock (released when the holder exits, even if killed).
    The holder's pid/command is written at offset 16 so a waiter can name it. Two ninjas in one tree race sccache/LNK1104."""

    def __init__(self, root, tree):
        self.path = f"{root}/{tree}/.build.lock"
        self.fh = None

    def _try(self):
        if os.name == "nt":
            import msvcrt
            self.fh.seek(0)
            msvcrt.locking(self.fh.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(self.fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)

    def holder(self):
        try:
            with open(self.path, "rb") as f:
                f.seek(16)
                return f.read(200).split(b"\0")[0].decode("utf-8", "replace").strip() or "unknown"
        except OSError:
            return "unknown"

    def acquire(self, wait_s):
        """True when held. Waits up to wait_s (0 = fail at once), saying who holds it every 60 s."""
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        self.fh = open(self.path, "a+b")
        deadline = time.monotonic() + wait_s
        last = 0.0
        while True:
            try:
                self._try()
                break
            except OSError:
                now = time.monotonic()
                if now >= deadline:
                    self.fh.close()
                    self.fh = None
                    return False
                if now - last >= 60 or last == 0.0:
                    print(f"[ninja_build] another build holds this tree ({self.holder()}); waiting (set MC2_BUILD_LOCK_WAIT=0 to fail instead)",
                          flush=True)
                    last = now
                time.sleep(2)
        self.fh.seek(16)
        self.fh.truncate(16)
        self.fh.write(f"pid {os.getpid()} {' '.join(sys.argv[1:])[:120]}".encode())
        self.fh.flush()
        return True

    def close(self):
        if self.fh:
            try:
                if os.name == "nt":
                    import msvcrt
                    self.fh.seek(0)
                    msvcrt.locking(self.fh.fileno(), msvcrt.LK_UNLCK, 1)
            except OSError:
                pass
            self.fh.close()
            self.fh = None


def pool_jobs(argv):
    """Take build tokens from the host-wide pool (scripts/build_tokens.py) and rewrite -jN to the width obtained."""
    import build_tokens
    idx = next(i for i, a in enumerate(argv) if a.startswith("-j"))
    want = int(argv[idx][2:] or "2")
    # Keep one ticket for the entire wait; retrying timed-out acquisitions would lose FIFO position.
    tokens = build_tokens.acquire(want, on_wait=lambda total: print(
        f"[build-tokens] host pool of {total} jobs is busy; queued FIFO (scripts/build_tokens.py status)", flush=True))
    width = max(1, min(want, len(tokens)))  # a small build reserves the floor without adding workers
    if tokens.waited_s >= 1:
        print(f"[build-tokens] waited {tokens.waited_s:.0f}s, running -j{width}", flush=True)
    argv = list(argv)
    argv[idx] = "-j" + str(width)
    return argv, tokens


def build_warning(root, tree):
    """One line, or "": uncommitted tracked changes (the exe will fingerprint dirty and deploy --require-clean refuses it) and/or
    HEAD newer than the sha embedded in the last-built exe. Never raises: a warning must not block a build."""
    try:
        def git(*a):
            return subprocess.run(["git", "-C", root, *a], capture_output=True, text=True, timeout=30,
                                  **console_child.child_kwargs(redirected=True)).stdout.strip()
        notes = []
        dirty = [x for x in git("status", "--porcelain", "--untracked-files=no").splitlines() if x]
        if dirty:
            notes.append(f"{len(dirty)} uncommitted tracked change(s): this exe will fingerprint dirty and "
                         "deploy_payload --require-clean will refuse it (commit before building the exe you deploy)")
        exe = f"{root}/{tree}/mc2.exe"
        head = git("rev-parse", "HEAD")
        if head and os.path.exists(exe):
            from exe_fingerprint import extract, sha_agrees
            fp = extract(exe)
            if fp.get("present") and sha_agrees(fp.get("sha"), head) is not True:
                notes.append(f"HEAD {head[:9]} is newer than the last build ({str(fp.get('sha'))[:9]}); rebuilding")
        return "[ninja_build] WARNING: " + "; ".join(notes) if notes else ""
    except Exception:
        return ""


def pin_compile_cores(root):
    """Pin this process (and every compiler it spawns) to the CPU_SPLIT compile mask; no-op off the PC or under MC2_NO_CPU_PIN."""
    sys.path.insert(0, os.path.join(root, "tools"))
    try:
        from lint_slots import pin_cpus, pin_sccache_servers
        pin_sccache_servers("compile")   # a cache server started outside a pinned build would run cl.exe on game cores
        return pin_cpus("compile")
    except Exception as exc:   # a pin failure must never fail a build
        print(f"[ninja_build] compile-core pin skipped: {exc}", flush=True)
        return 0


def main():
    root =os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace("\\", "/")
    argv = sys.argv[1:]
    tree = os.environ.get("MC2_NINJA_TREE") or DEFAULT_TREE
    pin_compile_cores(root)   # cl/link children inherit the affinity: a build never steals a game core (A:/Games/mc2-ops/CPU_SPLIT)
    if "--warm" in argv:
        argv.remove("--warm")
        return warm(root, argv, tree)
    started = None
    if "--warm-run" in argv:
        argv.remove("--warm-run")
        started = time.monotonic()
    if not any(not a.startswith("-") for a in argv):
        argv += DEFAULT_TARGETS
    # The host is shared by several lanes and the owner; an uncapped ninja pegs every core. MC2_BUILD_JOBS (owner default 2,
    # 2026-10-15) caps a build that names no -j of its own.
    if not any(a.startswith("-j") for a in argv):
        argv.insert(0, "-j" + os.environ.get("MC2_BUILD_JOBS", "2"))
    # MC2_NINJA_TREE names another tree under the repo root (merge verification deliberately uses build64-ninja/ so deploy reuses the verified
    # build); it is configured on first use.
    if tree != DEFAULT_TREE and not os.path.exists(f"{root}/{tree}/build.ninja"):
        configure(root, tree)
    dry_run = "-n" in argv
    lock = None
    if not dry_run:   # one build per tree: a lane's own build waits for the slot's warm build instead of racing it (sccache / LNK1104)
        lock = TreeLock(root, tree)
        if not lock.acquire(float(os.environ.get("MC2_BUILD_LOCK_WAIT", "3600"))):
            sys.exit(f"[ninja_build] {tree} is being built by another process ({lock.holder()}); not starting a second build in the same tree")
    if not dry_run:
        warning = build_warning(root, tree)
        if warning:
            print(warning, flush=True)
        drop_oversize_pdb(root, tree)
    env = compile_cache_env(root) if tree_uses_cache(root, tree) else None  # sccache tree (the default; configure_ninja.py --no-compile-cache opts out)
    if env:
        ensure_cache_server(root, env)
    tokens = None
    if not dry_run and os.environ.get("MC2_BUILD_TOKENS", "1") != "0":
        argv, tokens = pool_jobs(argv)
    command = subprocess.list2cmdline([NINJA, "-C", f"{root}/{tree}"] + argv)
    rc = under_vcvars(command, env)
    if rc == 0 and not dry_run and drop_oversize_pdb(root, tree):
        rc = under_vcvars(command, env)  # Fresh full symbols from the same objects; no second compile.
    if started is not None:
        print(f"[warm] done rc={rc} elapsed={time.monotonic() - started:.0f}s", flush=True)
    if tokens is not None:
        tokens.close()
    if lock is not None:
        lock.close()
    sys.exit(rc)


if __name__ == "__main__":
    main()
