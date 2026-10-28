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


def pool_jobs(argv):
    """Take build tokens from the host-wide pool (scripts/build_tokens.py) and rewrite -jN to the width obtained."""
    import build_tokens
    idx = next(i for i, a in enumerate(argv) if a.startswith("-j"))
    want = int(argv[idx][2:] or "2")
    tokens = build_tokens.acquire(want, timeout_s=120, on_wait=lambda total: print(
        f"[build-tokens] host pool of {total} jobs is busy; waiting for a free slot", flush=True))
    if tokens.waited_s >= 1:
        print(f"[build-tokens] waited {tokens.waited_s:.0f}s, running -j{max(1, len(tokens))}", flush=True)
    argv = list(argv)
    argv[idx] = "-j" + str(max(1, len(tokens)))
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


def main():
    root =os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace("\\", "/")
    argv = sys.argv[1:]
    tree = os.environ.get("MC2_NINJA_TREE") or DEFAULT_TREE
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
    sys.exit(rc)


if __name__ == "__main__":
    main()
