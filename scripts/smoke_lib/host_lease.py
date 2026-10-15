# scripts/smoke_lib/host_lease.py
"""Host-wide engine lease: one lane's harness at a time may have an engine (mc2.exe / EditRel.exe) running on this host.

WHAT: a process that is about to launch the game holds this lease for the rest of its life. Taking it waits until (1) no other live
process holds the lease and (2) no game or editor process is running that this process did not start, whatever install it came from.
The lease file lives at ONE fixed host-global path, outside every worktree and independent of every other harness setting: four lanes
running the harness from four worktree copies contend for the same file.
WHY: the deploy-folder lease only serialises two harnesses on the SAME install. Two lanes on different installs (or a lane next to a
game the owner started) still ran two engines at once, and the second engine's window handling and cursor guard fought the first
(2026-10-10). A captured frame, a timing or a determinism pair taken while another engine renders is not a measurement.
WHY the lease never follows the cursor-guard switch: until 2026-10-12 `MC2_CURSOR_GUARD_OFF=1` (the test-tree isolation switch that the
conftests and check-contracts set, and that lanes export for their smoke shells) also switched this lease off, so three lanes ran three
engines at once with every runner call passing `--wait-timeout`. Test isolation is a private lease file (`MC2_HOST_LEASE_PATH`), never
"no lease".

  * default is to WAIT with a countdown line; `wait=False` / `--no-wait` raises HostLeaseBusy at once, naming the holder.
  * a process that already holds the lease re-enters freely, so a runner that launches several games takes it once.
  * a lease whose holder pid is dead is taken over (`clear-dead` does the same by hand); a live engine with no live lease holder is
    never killed here (only `kill_orphans`, which child_registry restricts to provable orphans of a dead run).
  * off only for the owner: `MC2_HOST_LEASE=off` together with `MC2_OWNER_NO_LEASE=1`; the first without the second is ignored aloud.

CLI: `py -3 -m scripts.smoke_lib.host_lease status | clear-dead` (status exits 0 free, 1 held, 2 held by a dead process);
`hold --tag X [--ttl S]` ... `release --tag X` keeps one lease across a chain of runs started with MC2_HOST_LEASE_BATCH=X.
"""
from __future__ import annotations

import argparse
import atexit
import json
import os
import sys
import time
from pathlib import Path
from typing import Callable, List, Optional, Tuple

try:
    from . import bounded_wait, child_registry
except ImportError:                       # imported as a top-level module from scripts/
    import bounded_wait                   # type: ignore
    import child_registry                 # type: ignore

#: The one lease file every lane on this host contends for. Fixed on purpose: not under a worktree, not derived from the child
#: registry directory or any other overridable setting.
LEASE_PATH = Path("A:/Games/mc2-opengl/.host_game_lease.json")
PATH_ENV = "MC2_HOST_LEASE_PATH"          # tests only: a private lease file (the conftests set one per pytest process)
OFF_ENV = "MC2_HOST_LEASE"                # "off" switches leasing off, owner only (see OWNER_ENV)
OWNER_ENV = "MC2_OWNER_NO_LEASE"          # "1" authorises OFF_ENV; the same owner switch run_smoke --no-lease needs
TIMEOUT_ENV = "MC2_HOST_LEASE_TIMEOUT"    # seconds, default 3600
BATCH_ENV = "MC2_HOST_LEASE_BATCH"        # the tag of a `hold` batch lease this process's launches run under
DEFAULT_BATCH_TTL_S = 7200.0
MAX_BATCH_TTL_S = 6 * 3600.0              # the lane cap: no batch outlives a lane session
DEFAULT_TIMEOUT_S = 3600.0
POLL_S = 2.0
REPORT_EVERY_S = 30.0


class HostLeaseBusy(RuntimeError):
    """Another lane holds the host lease, or a game this process did not start is still running."""


def _log_default(m: str) -> None:
    print(m, file=sys.stderr, flush=True)


def redirected() -> bool:
    """True when a private lease file is in use (a test): the host's real engines are then not this process's business."""
    return bool(os.environ.get(PATH_ENV))


def lease_path() -> Path:
    override = os.environ.get(PATH_ENV)
    if override:
        return Path(override)
    if "pytest" in sys.modules:
        raise RuntimeError(f"a test reached the real host lease {LEASE_PATH}; the conftest.py files set {PATH_ENV} to a private file")
    return LEASE_PATH


_off_logged = False


def disabled(log: Callable[[str], None] = _log_default) -> bool:
    """True only for the owner: MC2_HOST_LEASE=off with MC2_OWNER_NO_LEASE=1. Never follows the cursor-guard test switch."""
    global _off_logged
    if os.environ.get(OFF_ENV, "").lower() != "off":
        return False
    if os.environ.get(OWNER_ENV) == "1":
        return True
    if not _off_logged:
        _off_logged = True
        log(f"[host-lease] {OFF_ENV}=off ignored: it needs {OWNER_ENV}=1 (owner only); leasing as usual")
    return False


def _read(path: Path) -> Optional[dict]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def _holder_alive(rec: Optional[dict]) -> bool:
    if not rec:
        return False
    pid = int(rec.get("pid", 0))
    if not child_registry.pid_alive(pid):
        return False
    created = rec.get("created")
    now = child_registry.process_create_time(pid)
    return created is None or now is None or int(created) == int(now)      # a reused pid is not the holder


def _try_take(path: Path, label: str) -> Tuple[bool, Optional[dict]]:
    """-> (we hold it, the other holder's record when we do not)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    for _ in range(3):
        try:
            fd = os.open(str(path), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError:
            rec = _read(path)
            if rec and int(rec.get("pid", 0)) == os.getpid():
                return True, None
            if _batch_member(rec) and _holder_alive(rec):
                return True, None              # a step of a held chain: the batch holder's lease covers it
            if _holder_alive(rec):
                return False, rec
            try:
                path.unlink()                  # holder died without releasing
            except OSError:
                pass
            continue
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            json.dump({"pid": os.getpid(), "created": child_registry.process_create_time(os.getpid()), "label": label,
                       "cwd": os.getcwd(), "argv": sys.argv[:3], "since": time.time()}, fh)
        return True, None
    return False, _read(path)


def release() -> None:
    """Give the lease back if this process holds it."""
    path = lease_path()
    rec = _read(path)
    if rec and int(rec.get("pid", 0)) == os.getpid():
        try:
            path.unlink()
        except OSError:
            pass


_registered = False


def holds() -> bool:
    """True when this process already holds the lease, itself or as a member of the live batch named by MC2_HOST_LEASE_BATCH."""
    if not _registered:
        return False
    rec = _read(lease_path()) or {}
    return rec.get("pid") == os.getpid() or (_batch_member(rec) and _holder_alive(rec))


def _batch_member(rec: Optional[dict]) -> bool:
    """True when `rec` is a batch lease whose tag this process carries in MC2_HOST_LEASE_BATCH."""
    tag = os.environ.get(BATCH_ENV, "")
    return bool(rec and tag and rec.get("batch") == tag)


def _describe(rec: Optional[dict]) -> str:
    if not rec:
        return "unknown holder"
    return (f"{rec.get('label', '?')} (pid {rec.get('pid')}, {' '.join(map(str, rec.get('argv', [])))}"
            f"{', in ' + str(rec['cwd']) if rec.get('cwd') else ''})")


def foreign_engines() -> List[Tuple[int, str]]:
    """Engines running on the host as (pid, image). Empty under a private lease file: a test never looks at the real host."""
    if redirected():
        return []
    return child_registry.engine_processes()


def acquire(label: str, *, wait: bool = True, timeout_s: Optional[float] = None, kill_orphans: bool = False,
            log: Callable[[str], None] = _log_default,
            sleep: Callable[[float], None] = time.sleep, clock: Callable[[], float] = time.monotonic,
            scan: Optional[Callable[..., List[Tuple[int, str]]]] = None) -> None:
    """Hold the host lease, or raise HostLeaseBusy. See the module docstring for the rules."""
    global _registered
    if disabled(log):
        log(f"[host-lease] OFF ({OFF_ENV}=off, owner): {label} launches without the host lease")
        return
    scan = scan or foreign_engines
    if timeout_s is None:
        timeout_s = float(os.environ.get(TIMEOUT_ENV) or DEFAULT_TIMEOUT_S)
    path = lease_path()
    deadline = clock() + timeout_s
    last_report = -REPORT_EVERY_S
    already = (_read(path) or {}).get("pid") == os.getpid()
    while True:
        have, other = _try_take(path, label)
        why = ""
        if have:
            foreign = scan()
            if foreign and kill_orphans:
                for pid, _img in foreign:
                    killed, note = child_registry.kill_if_orphan(pid)
                    log(f"[host-lease] game PID {pid}: {'killed, ' if killed else 'left running, '}{note}")
                foreign = scan()
            if not foreign:
                if not _registered:
                    atexit.register(release)
                    _registered = True
                return
            why = "a game this process did not start is running: " + ", ".join(f"{img} pid {pid}" for pid, img in foreign)
        else:
            why = "the host game lease is held by " + _describe(other)
        if not wait:
            if have and not already:
                release()
            raise HostLeaseBusy(why + "; another lane is using the engine (rerun without --no-wait to queue)")
        if clock() >= deadline:
            if have and not already:
                release()
            raise HostLeaseBusy(f"gave up after {timeout_s:.0f} s: {why}")
        if clock() - last_report >= REPORT_EVERY_S:
            last_report = clock()
            log(bounded_wait.countdown_line("host game lease", why, deadline - clock(), timeout_s))
        sleep(POLL_S)


def ensure_held(label: str) -> None:
    """Hold the lease before an engine launch (re-entrant)."""
    if holds():
        return              # our own earlier launches are running; they are not foreign
    acquire(label)


# ---------------------------------------------------------------- batch lease (hold / release)
# WHY: a proof chain (deploy check, smoke, capture, determinism pair) is several runner processes; between two of them another lane
# could take the lease and interleave its engine with the chain. `hold` parks the lease in a small holder process of its own (an agent's
# shell does not outlive one tool call, so no caller pid would do); runner processes started with MC2_HOST_LEASE_BATCH=<tag> enter it.
# The lease still ends on a dead holder pid (the usual rule), on `release`, and at the TTL, so a forgotten hold cannot wedge the host.

def _write_record(path: Path, rec: dict) -> None:
    tmp = path.with_name(path.name + f".{os.getpid()}.tmp")
    tmp.write_text(json.dumps(rec), encoding="utf-8")
    os.replace(tmp, path)


def holder_loop(tag: str, ttl_s: float, *, log: Callable[[str], None] = _log_default, sleep: Callable[[float], None] = time.sleep,
                clock: Callable[[], float] = time.time) -> int:
    """Body of the holder process: take the lease (waiting as usual), mark it as batch `tag`, keep it until it is released, taken
    away, or the TTL passes. -> 0 on a normal end, 4 when the lease could not be taken."""
    try:
        acquire(f"batch {tag}", log=log)
    except HostLeaseBusy as exc:
        log(f"[host-lease] batch {tag}: {exc}")
        return 4
    path = lease_path()
    rec = dict(_read(path) or {}, batch=tag, expires=clock() + ttl_s, label=f"batch {tag}")
    _write_record(path, rec)
    while True:
        cur = _read(path)
        if not cur or cur.get("pid") != os.getpid() or cur.get("batch") != tag:
            return 0                                   # released (or cleared) from outside
        if clock() >= float(cur.get("expires", 0)):
            log(f"[host-lease] batch {tag}: TTL reached, releasing")
            release()
            return 0
        sleep(POLL_S)


def hold(tag: str, ttl_s: float = DEFAULT_BATCH_TTL_S, *, timeout_s: Optional[float] = None,
         log: Callable[[str], None] = _log_default) -> int:
    """Start the holder process and wait until it holds the lease. -> its pid. Raises HostLeaseBusy when it cannot."""
    import subprocess
    try:
        from scripts import console_child         # type: ignore
    except ImportError:
        sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
        import console_child                       # type: ignore
    if not tag or not all(c.isalnum() or c in "-_." for c in tag):
        raise ValueError(f"batch tag {tag!r}: use letters, digits, '-', '_' or '.'")
    ttl_s = min(max(ttl_s, 1.0), MAX_BATCH_TTL_S)
    flags = 0x00000200 | 0x01000000 if sys.platform == "win32" else 0     # new process group, break away from the caller's job
    argv = [sys.executable, str(Path(__file__).resolve()), "_holder", "--tag", tag, "--ttl", str(ttl_s)]
    quiet = {"stdin": subprocess.DEVNULL, "stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL}
    try:
        proc = subprocess.Popen(argv, **console_child.child_kwargs(redirected=True, creationflags=flags, **quiet))
    except OSError:                                # the caller's job forbids breakaway: the holder then lives as long as that job
        proc = subprocess.Popen(argv, **console_child.child_kwargs(redirected=True, creationflags=flags & ~0x01000000, **quiet))
    deadline = time.monotonic() + (timeout_s if timeout_s is not None else float(os.environ.get(TIMEOUT_ENV) or DEFAULT_TIMEOUT_S))
    last = time.monotonic() - REPORT_EVERY_S + 2.0      # first line only after the holder had time to take a free lease
    while True:
        rec = _read(lease_path())
        if rec and rec.get("pid") == proc.pid and rec.get("batch") == tag:
            return proc.pid
        if proc.poll() is not None:
            raise HostLeaseBusy(f"batch {tag}: the holder exited (rc {proc.returncode}) without the lease")
        if time.monotonic() >= deadline:
            proc.terminate()
            raise HostLeaseBusy(f"batch {tag}: not held after the wait; the lease is held by {_describe(rec)}")
        if time.monotonic() - last >= REPORT_EVERY_S:
            last = time.monotonic()
            log(f"[host-lease] batch {tag}: waiting for the lease (held by {_describe(rec)})")
        time.sleep(0.25)


def release_batch(tag: str) -> Tuple[bool, Optional[dict]]:
    """End batch `tag`: remove its lease (the holder sees that and exits). -> (removed, the record). Another lease is left alone."""
    path = lease_path()
    rec = _read(path)
    if not rec or rec.get("batch") != tag:
        return False, rec
    try:
        path.unlink()
    except OSError:
        return False, rec
    return True, rec


# ---------------------------------------------------------------- status / clear-dead

def status() -> dict:
    """What the lease looks like right now: the file, its holder (and whether that process is alive), the engines running."""
    path = lease_path()
    rec = _read(path)
    engines = [{"pid": pid, "image": img, "path": child_registry.image_path(pid)} for pid, img in foreign_engines()]
    return {"path": str(path), "holder": rec, "holder_alive": _holder_alive(rec) if rec else False,
            "engines": engines, "off": disabled(lambda m: None), "private": redirected()}


def clear_dead() -> Tuple[bool, Optional[dict]]:
    """Remove a lease whose holder process is gone. -> (removed, the record). A live holder is left alone: that lane is running."""
    path = lease_path()
    rec = _read(path)
    if rec is None or _holder_alive(rec):
        return False, rec
    try:
        path.unlink()
    except OSError:
        return False, rec
    return True, rec


def _print_status(st: dict) -> int:
    print(f"host lease file: {st['path']}" + (" (private, test)" if st["private"] else "") + (" [OFF: owner]" if st["off"] else ""))
    rec = st["holder"]
    if rec is None:
        print("holder: none (free)")
        code = 0
    else:
        since = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(float(rec.get("since", 0))))
        print(f"holder: {_describe(rec)} since {since}, process {'ALIVE' if st['holder_alive'] else 'DEAD (clear-dead removes it)'}")
        code = 1 if st["holder_alive"] else 2
    for e in st["engines"]:
        print(f"engine: {e['image']} pid {e['pid']} {e['path'] or ''}")
    if not st["engines"]:
        print("engines running: none")
    return code


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(prog="host_lease", description="the host-wide engine lease")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status", help="holder, whether it is alive, engines running (exit 0 free / 1 held / 2 held by a dead process)")
    sub.add_parser("clear-dead", help="remove the lease when its holder process is dead; a live holder is left alone")
    h = sub.add_parser("hold", help="hold the lease for a chain of runs: start a holder process, print the env line the chain's runs need")
    h.add_argument("--tag", required=True, help="batch name; runs started with MC2_HOST_LEASE_BATCH=<tag> enter the held lease")
    h.add_argument("--ttl", type=float, default=DEFAULT_BATCH_TTL_S, help=f"seconds until the lease lapses (max {MAX_BATCH_TTL_S:.0f})")
    h.add_argument("--wait-timeout", type=float, default=None, help="seconds to wait for the lease (default MC2_HOST_LEASE_TIMEOUT or 3600)")
    r = sub.add_parser("release", help="end a batch lease taken with hold")
    r.add_argument("--tag", required=True)
    hl = sub.add_parser("_holder")                 # the holder process itself; started by hold
    hl.add_argument("--tag", required=True)
    hl.add_argument("--ttl", type=float, required=True)
    argv = ["clear-dead" if a == "--clear-dead" else a for a in (sys.argv[1:] if argv is None else argv)]   # deploy_lease spelling
    args = ap.parse_args(argv)
    if args.cmd == "status":
        return _print_status(status())
    if args.cmd == "_holder":
        return holder_loop(args.tag, args.ttl)
    if args.cmd == "hold":
        try:
            pid = hold(args.tag, args.ttl, timeout_s=args.wait_timeout)
        except (HostLeaseBusy, ValueError) as exc:
            print(f"[host-lease] {exc}", file=sys.stderr)
            return 4
        print(f"[host-lease] batch {args.tag} held by pid {pid} for at most {min(args.ttl, MAX_BATCH_TTL_S):.0f} s; "
              f"run the chain with {BATCH_ENV}={args.tag}, end it with: host_lease release --tag {args.tag}")
        return 0
    if args.cmd == "release":
        removed, rec = release_batch(args.tag)
        print(f"[host-lease] batch {args.tag} released" if removed else
              f"[host-lease] batch {args.tag} not held (lease: {_describe(rec) if rec else 'free'})")
        return 0 if removed else 1
    removed, rec = clear_dead()
    if removed:
        print(f"[host-lease] cleared the lease left by {_describe(rec)}")
    elif rec is None:
        print("[host-lease] no lease to clear")
    else:
        print(f"[host-lease] NOT cleared: {_describe(rec)} is alive; wait for that run")
    return 0 if removed or rec is None else 1


if __name__ == "__main__":
    sys.exit(main())
