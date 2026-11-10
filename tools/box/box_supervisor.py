#!/usr/bin/env python3
"""Box supervisor: the one watchdog on the vast box (runs under supervisord as program box-supervisor; deployed as /workspace/bin/box-supervisor).

Every 30 s (one tick; `--once` runs one tick and exits):
  workers   restart a dead filler worker (/workspace/filler/worker.sh 0-2) unless /workspace/filler/STOP exists; >5 restarts in 10 min = backoff
  reap      gpuN.sK.owner / .row (and the legacy gpuN.owner / .row) naming a dead pid (older than 60 s), *.want older than 10 min, a quiet file of a dead pid, filler .run markers
            of dead pids. The gpuN lock files themselves are never removed; a live lock or a live pid is never touched
  gate GPU  the gate GPU (GATE_GPU in cores.env; the one-GPU box's only GPU 0) belongs to gate rows: any process on its bus that does not
            descend from the live gpu<N>.owner pid is an intruder. While a gate holds the lock, intruders get SIGSTOP (recorded in
            /workspace/locks/gpu<N>.stopped) and SIGCONT when the gate ends or this starts. BOX_GPU3_ENFORCE=log only logs. A gate-GPU lock
            held by anything but a box_run lane script is a problem
  repin     Blender / asset scripts (blender-5, scene2.py, micro.py) onto BUILD_CPUS (cores.env), off the game cores
  status    /workspace/STATUS.json (filler/status.py), /workspace/supervisor.heartbeat, /workspace/supervisor.state.json
  autosync  when the mirror's nifty tip is ahead of the filler's current_sha: queue `bash /workspace/filler/sync.sh --mirror` in pueue group build
            (one try per sha, 10 min apart)
Log: /workspace/logs/box-supervisor.log (every restart, reap, stop and resume). Liveness is /proc/<pid> only (never a signal 0).
"""
import json, os, signal, subprocess, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.realpath(__file__)), "filler"))
import status   # noqa: E402  (tools/box/filler/status.py; the release dir keeps this file and filler/ side by side)

W = os.environ.get("MC2_BOX_W", "/workspace")
LOCKS = os.environ.get("MC2_BOX_LOCKS", W + "/locks")
FILLER = os.environ.get("MC2_BOX_FILLER", W + "/filler")
PROC = os.environ.get("MC2_BOX_PROC", "/proc")
LOG = os.environ.get("MC2_BOX_SUPERVISOR_LOG", W + "/logs/box-supervisor.log")
STATE = W + "/supervisor.state.json"
HEARTBEAT = W + "/supervisor.heartbeat"
MIRROR = W + "/toolchains/mirror.git"
PUEUE = W + "/toolchains/pueue/pueue"
NIFTY = "refs/heads/claude/nifty-mendeleev"
INTERVAL = 30
STALE_OWNER_S = 60
STALE_WANT_S = 600
FLAP_N, FLAP_WINDOW_S = 5, 600
SYNC_GAP_S = 600
DU_EVERY_S = 600
REPIN = ("blender-5", "scene2.py", "micro.py")


# --- /proc helpers (PROC is a directory, so tests build a fake one) ------------------------------------------------

def alive(pid):
    return pid is not None and int(pid) > 0 and os.path.isdir(f"{PROC}/{int(pid)}")


def cmdline(pid):
    try:
        with open(f"{PROC}/{int(pid)}/cmdline", "rb") as f:
            return f.read().replace(b"\0", b" ").decode(errors="replace").strip()
    except (OSError, ValueError):
        return ""


def stat_fields(pid):
    """fields after the comm, so [0]=state [1]=ppid ... [19]=starttime"""
    try:
        with open(f"{PROC}/{int(pid)}/stat") as f:
            return f.read().rsplit(")", 1)[1].split()
    except (OSError, ValueError, IndexError):
        return []


def ppid(pid):
    s = stat_fields(pid)
    return int(s[1]) if len(s) > 1 and s[1].isdigit() else 0


def starttime(pid):
    s = stat_fields(pid)
    return s[19] if len(s) > 19 else ""


def descends(pid, root):
    """pid is root or one of its descendants (ppid chain, at most 64 hops)"""
    p = int(pid)
    for _ in range(64):
        if p == int(root):
            return True
        if p <= 1:
            return False
        p = ppid(p)
    return False


def read_pidfile(path):
    try:
        with open(path) as f:
            tok = f.read().split()
        return int(tok[0]) if tok and tok[0].isdigit() else None
    except OSError:
        return None


def all_pids():
    try:
        return [int(d) for d in os.listdir(PROC) if d.isdigit()]
    except OSError:
        return []


# --- log and state -------------------------------------------------------------------------------------------------

def log(msg):
    line = f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} {msg}\n"
    try:
        os.makedirs(os.path.dirname(LOG), exist_ok=True)
        if os.path.exists(LOG) and os.path.getsize(LOG) > 5 * 2**20:
            os.replace(LOG, LOG + ".1")
        with open(LOG, "a") as f:
            f.write(line)
    except OSError:
        pass
    sys.stdout.write(line)
    sys.stdout.flush()


def write_json(path, obj):
    tmp = f"{path}.tmp.{os.getpid()}"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=1)
    os.replace(tmp, path)


def load_state():
    try:
        with open(STATE) as f:
            s = json.load(f)
    except (OSError, ValueError):
        s = {}
    for k, v in (("restarts", {}), ("backoff", {}), ("intruders", {}), ("synced_tried", []), ("problems", []), ("lanes_gb", {})):
        s.setdefault(k, v)
    return s


# --- workers -------------------------------------------------------------------------------------------------------

def worker_alive(g):
    pid = read_pidfile(f"{FILLER}/worker{g}.pid")
    return alive(pid) and "worker.sh" in cmdline(pid)


def start_worker(g):
    os.makedirs(f"{FILLER}/logs", exist_ok=True)
    with open(f"{FILLER}/logs/worker{g}.log", "ab") as lg:
        p = subprocess.Popen(["bash", f"{FILLER}/worker.sh", str(g)], stdin=subprocess.DEVNULL, stdout=lg, stderr=subprocess.STDOUT,  # background-launch: allow runs on the Linux box only (supervisor child: filler worker/pueue/git); never this desktop
                             cwd=FILLER, start_new_session=True, close_fds=True)
    with open(f"{FILLER}/worker{g}.pid", "w") as f:
        f.write(str(p.pid))
    return p.pid


def check_workers(state, now, start=start_worker):
    out = {}
    if os.path.exists(f"{FILLER}/STOP") or not os.path.exists(f"{FILLER}/worker.sh"):
        return {str(g): "stopped" for g in filler_gpus()}
    for g in filler_gpus():
        k = str(g)
        if worker_alive(g):
            out[k] = "alive"
            continue
        hist = [t for t in state["restarts"].get(k, []) if now - t < FLAP_WINDOW_S]
        if len(hist) >= FLAP_N:
            if k not in state["backoff"]:
                log(f"worker{g} BACKOFF: {len(hist)} restarts in {FLAP_WINDOW_S}s; not restarting until the window clears (logs/worker{g}.log)")
            state["backoff"][k] = now
            out[k] = "backoff"
        else:
            state["backoff"].pop(k, None)
            old = read_pidfile(f"{FILLER}/worker{g}.pid")
            pid = start(g)
            hist.append(now)
            log(f"worker{g} dead (was pid {old}): restarted pid {pid} (restart {len(hist)} in {FLAP_WINDOW_S}s)")
            out[k] = "restarted"
        state["restarts"][k] = hist
    return out


# --- reaping -------------------------------------------------------------------------------------------------------

def _reap(path, why, check):
    """unlink path only if check(content) still holds on a fresh read (the holder may have rewritten it meanwhile)"""
    try:
        with open(path) as f:
            body = f.read()
        if not check(body):
            return False
        os.unlink(path)
        log(f"reaped {path} ({why}): {body.strip()[:120]}")
        return True
    except OSError:
        return False


def _dead_pid_body(body):
    tok = body.split()
    return bool(tok) and tok[0].isdigit() and not alive(int(tok[0]))


def reap(now, holders=None):
    gone = []
    holders = status.lock_holders() if holders is None else holders
    for name in [f"gpu{g}" for g in range(4)] + status.all_slot_names():   # legacy single-lock names (old lane scripts) and the slot files
        base = f"{LOCKS}/{name}"
        try:
            if os.stat(base).st_ino in holders:   # the slot lock is held: its row still runs, so its owner/row files stay
                continue
        except OSError:
            pass
        own = base + ".owner"
        try:
            age = now - os.stat(own).st_mtime
        except OSError:
            age = None
        if age is not None and age > STALE_OWNER_S and _reap(own, "owner pid dead", _dead_pid_body):
            gone.append(own)
        row = base + ".row"
        try:
            rage = now - os.stat(row).st_mtime
        except OSError:
            rage = None
        if rage is not None and rage > STALE_OWNER_S and not alive(read_pidfile(own)) and _reap(row, "no live owner", lambda b: not alive(read_pidfile(own))):
            gone.append(row)
    try:
        names = os.listdir(LOCKS)
    except OSError:
        names = []
    for n in names:
        p = f"{LOCKS}/{n}"
        if n.endswith(".want"):
            try:
                if now - os.stat(p).st_mtime > STALE_WANT_S and _reap(p, f"want not touched for {STALE_WANT_S}s", lambda b: now - os.stat(p).st_mtime > STALE_WANT_S):
                    gone.append(p)
            except OSError:
                pass
    q = f"{LOCKS}/quiet"
    if os.path.exists(q) and _reap(q, "quiet-slot pid dead", _dead_pid_body):
        gone.append(q)
    sha = ""
    try:
        with open(f"{FILLER}/current_sha") as f:
            sha = f.read().strip()
    except OSError:
        pass
    rd = f"{FILLER}/results/{sha}"
    if sha and os.path.isdir(rd):
        for n in os.listdir(rd):
            p = f"{rd}/{n}"
            if n.endswith(".run"):
                try:
                    if now - os.stat(p).st_mtime > STALE_OWNER_S and _reap(p, "filler job pid dead", _dead_pid_body):
                        gone.append(p)
                except OSError:
                    pass
    return gone


# --- gate GPU guard -------------------------------------------------------------------------------------------------

def nvidia_query():
    """({index: bus}, [(bus, pid, name, mem_mb)]) or (None, None) when nvidia-smi fails"""
    try:
        a = subprocess.run(["nvidia-smi", "--query-gpu=index,pci.bus_id", "--format=csv,noheader"], capture_output=True, text=True, timeout=20)
        b = subprocess.run(["nvidia-smi", "--query-compute-apps=gpu_bus_id,pid,process_name,used_memory", "--format=csv,noheader,nounits"],
                           capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return None, None
    if a.returncode or b.returncode:
        return None, None
    buses = {}
    for ln in a.stdout.splitlines():
        p = [x.strip() for x in ln.split(",")]
        if len(p) == 2 and p[0].isdigit():
            buses[int(p[0])] = p[1].upper()
    apps = []
    for ln in b.stdout.splitlines():
        p = [x.strip() for x in ln.split(",")]
        if len(p) >= 4 and p[1].isdigit():
            apps.append((p[0].upper(), int(p[1]), p[2], p[3]))
    return buses, apps


def gate_holder(holders=None):
    """(owner pid if alive, /proc/locks pid, lock held?) for the gate GPU. The /proc/locks pid is the flock(1) child that took the lock (it has
    exited), so the identity check uses the owner pid that box_run's lane script writes ($$ of .box_lane_<key>.sh)."""
    holders = status.lock_holders() if holders is None else holders
    gg = gate_gpu()
    try:
        ino = os.stat(f"{LOCKS}/gpu{gg}").st_ino
    except OSError:
        ino = None
    held = ino is not None and ino in holders
    own = read_pidfile(f"{LOCKS}/gpu{gg}.owner")
    return (own if alive(own) else None), (holders.get(ino) if held else None), held


def classify_gpu3(apps, bus, owner):
    """GPU-3 compute processes that are not the live gate owner or its descendants"""
    return [(pid, name) for b, pid, name, _ in apps if b == bus and not (owner and descends(pid, owner))]


def _stopped_path():
    return f"{LOCKS}/gpu{gate_gpu()}.stopped"


def _read_stopped():
    try:
        with open(_stopped_path()) as f:
            return [ln.split(None, 2) for ln in f if ln.strip()]
    except OSError:
        return []


def resume_stopped(why, kill=os.kill):
    for ent in _read_stopped():
        if len(ent) >= 2 and ent[0].isdigit() and alive(int(ent[0])) and starttime(int(ent[0])) == ent[1]:   # same process (pid not reused)
            try:
                kill(int(ent[0]), signal.SIGCONT)
                log(f"gpu3 guard: SIGCONT {ent[0]} ({why})")
            except OSError:
                pass
    try:
        os.unlink(_stopped_path())
    except OSError:
        pass


def guard_gpu3(state, now, query=nvidia_query, holders=None, kill=os.kill):
    buses, apps = query()
    gg = gate_gpu()
    g = {"nvidia_smi_ok": buses is not None, "gate_gpu": gg}
    problems = []
    if buses is None:
        problems.append(f"nvidia-smi failed: GPU {gg} cannot be guarded (fail closed: box unhealthy)")
        state["gpu3"] = {**g, "problems": problems}
        return problems
    want = gpu_count()
    if want and len(buses) < want:
        problems.append(f"nvidia-smi sees {len(buses)} GPUs (configured {want})")
    bus = buses.get(gg)
    owner, lk, held = gate_holder(holders)
    gate = bool(held and owner and ".box_lane_" in cmdline(owner))
    g.update(bus=bus, gate_active=gate, owner=owner, lock_pid=lk)
    if held and owner and not gate:
        problems.append(f"gpu{gg} lock held by pid {owner} ({cmdline(owner)[:80]}), not a box_run gate script")
    if held and not owner:   # flock and the owner write are a few ms apart; only a lasting orphan is a problem
        state.setdefault("gpu3_orphan_since", now)
        if now - state["gpu3_orphan_since"] > STALE_OWNER_S:
            problems.append(f"gpu{gg} locked for {int(now - state['gpu3_orphan_since'])}s with no live gpu{gg}.owner")
    else:
        state.pop("gpu3_orphan_since", None)
    intr = classify_gpu3(apps, bus, owner) if bus else []
    mode = os.environ.get("BOX_GATE_GPU_ENFORCE") or os.environ.get("BOX_GPU3_ENFORCE", "stop")   # BOX_GPU3_ENFORCE: old name, kept as an alias
    known = state["intruders"]
    cur = {}
    for pid, name in intr:
        k = str(pid)
        e = known.get(k) or {"name": name, "since": now, "stopped": False}
        if k not in known:
            log(f"gpu{gg} guard: intruder pid {pid} {name} (gate={'yes' if gate else 'no'}, mode={mode})")
        if gate and mode == "stop" and not e["stopped"]:
            try:
                kill(pid, signal.SIGSTOP)
                with open(_stopped_path(), "a") as f:
                    f.write(f"{pid} {starttime(pid)} {name}\n")
                e["stopped"] = True
                log(f"gpu{gg} guard: SIGSTOP {pid} {name} while the gate holds GPU {gg}")
            except (OSError, AttributeError) as ex:
                problems.append(f"gpu{gg} intruder {pid} could not be stopped: {ex}")
        cur[k] = e
    if not gate and os.path.exists(_stopped_path()):
        resume_stopped("gate ended", kill)
        for e in cur.values():
            e["stopped"] = False
    if gate and mode != "stop" and cur:
        problems.append(f"gpu{gg}: {len(cur)} non-gate process(es) on the gate GPU during a gate (BOX_GPU3_ENFORCE={mode})")
    state["intruders"] = cur
    g["problems"] = problems
    state["gpu3"] = state["gate_gpu"] = g   # "gpu3": old key, kept as an alias
    return problems


# --- repin, status, autosync, du -----------------------------------------------------------------------------------

def build_cpus():
    try:
        with open(f"{LOCKS}/cores.env") as f:
            for ln in f:
                if ln.startswith("BUILD_CPUS="):
                    return ln.split("=", 1)[1].strip()
    except OSError:
        pass
    return f"8-{(os.cpu_count() or 9) - 1}"


def filler_gpus():
    """The GPUs of the one core map (GPU<n>_CPUS in locks/cores.env); the filler runs on each, the gate GPU's guard stops it during a gate."""
    out = []
    try:
        with open(f"{LOCKS}/cores.env") as f:
            for ln in f:
                key = ln.split("=", 1)[0].strip()
                if key.startswith("GPU") and key.endswith("_CPUS") and key[3:-5].isdigit():
                    out.append(int(key[3:-5]))
    except OSError:
        pass
    return tuple(out) or (0,)


def gpu_count():
    """Number of GPUs in the one core map: the GPU<n>_CPUS lines of locks/cores.env (0 when unreadable)."""
    n = 0
    try:
        with open(f"{LOCKS}/cores.env") as f:
            for ln in f:
                key = ln.split("=", 1)[0].strip()
                if key.startswith("GPU") and key.endswith("_CPUS") and key[3:-5].isdigit():
                    n += 1
    except OSError:
        pass
    return n


def gate_gpu():
    """The gate GPU from the one core map (GATE_GPU in locks/cores.env); 0 when unset or unreadable (the one-GPU box's only GPU)."""
    try:
        with open(f"{LOCKS}/cores.env") as f:
            for ln in f:
                if ln.startswith("GATE_GPU="):
                    v = ln.split("=", 1)[1].strip()
                    return int(v) if v.isdigit() else 0
    except OSError:
        pass
    return 0


def repin():
    bc = build_cpus()
    for pid in all_pids():
        c = cmdline(pid)
        if c and any(k in c for k in REPIN):
            subprocess.run(["taskset", "-apc", bc, str(pid)], capture_output=True, timeout=10)


def git_mirror(*a):
    r = subprocess.run(["git", "--git-dir", MIRROR, *a], capture_output=True, text=True, timeout=30)  # background-launch: allow runs on the Linux box only (supervisor child: filler worker/pueue/git); never this desktop
    return r.returncode, r.stdout.strip()


def autosync(state, now, holders=None):
    try:
        with open(f"{FILLER}/current_sha") as f:
            cur = f.read().strip()
    except OSError:
        return "no current_sha"
    rc, tip = git_mirror("rev-parse", "-q", "--verify", NIFTY + "^{commit}")
    a = state.setdefault("autosync", {})
    a.update(mirror_sha=tip if rc == 0 else None, current_sha=cur)
    if rc or not tip or tip == cur:
        return "up to date" if tip == cur else "no mirror ref"
    if tip in state["synced_tried"] or now - a.get("last_try", 0) < SYNC_GAP_S or os.path.exists(f"{FILLER}/STOP"):
        return "waiting"
    if git_mirror("merge-base", "--is-ancestor", cur, tip)[0] != 0:
        return "mirror tip is not ahead of current_sha"
    holders = status.lock_holders() if holders is None else holders
    try:
        if os.stat(f"{FILLER}/sync.lock").st_ino in holders:
            return "sync running"
    except OSError:
        pass
    r = subprocess.run([PUEUE, "add", "-g", "build", "-l", f"filler_sync:auto:{tip[:8]}", "--", "bash", f"{FILLER}/sync.sh", "--mirror"],  # background-launch: allow runs on the Linux box only (supervisor child: filler worker/pueue/git); never this desktop
                       capture_output=True, text=True, timeout=30)
    state["synced_tried"] = (state["synced_tried"] + [tip])[-20:]
    a["last_try"] = now
    log(f"autosync: filler {cur[:9]} -> mirror nifty {tip[:9]}: pueue add rc={r.returncode} {r.stdout.strip()[-80:]}")
    return "queued"


def lanes_du(state, now):
    if now - state.get("lanes_gb_time", 0) < DU_EVERY_S:
        return
    lanes = f"{W}/lanes"
    try:
        dirs = [f"{lanes}/{d}" for d in os.listdir(lanes) if os.path.isdir(f"{lanes}/{d}") and not d.startswith(".")]
    except OSError:
        dirs = []
    sizes = {}
    if dirs:
        try:
            r = subprocess.run(["nice", "-n", "19", "du", "-sm", *dirs], capture_output=True, text=True, timeout=120)
            for ln in r.stdout.splitlines():
                mb, _, p = ln.partition("\t")
                if mb.isdigit():
                    sizes[os.path.basename(p)] = round(int(mb) / 1024, 1)
        except (OSError, subprocess.TimeoutExpired):
            pass
    state["lanes_gb"] = sizes
    state["lanes_gb_time"] = now


def tick(state, now=None):
    now = time.time() if now is None else now
    problems = []
    state["workers"] = check_workers(state, now)
    holders = status.lock_holders()
    state["reaped_last"] = reap(now, holders)
    problems += guard_gpu3(state, now, holders=holders)
    for k, st in state["workers"].items():
        if st == "backoff":
            problems.append(f"filler worker{k} in restart backoff")
    try:
        repin()
    except (OSError, subprocess.TimeoutExpired):
        pass
    try:
        res = autosync(state, now, holders)
    except (OSError, subprocess.TimeoutExpired) as ex:
        res = f"error {ex}"
    state.setdefault("autosync", {})["result"] = res
    lanes_du(state, now)
    try:
        write_json(status.OUT, status.build(now))
    except OSError:
        pass
    state["problems"] = problems
    state["tick"] = now
    write_json(STATE, state)
    with open(HEARTBEAT, "w") as f:
        f.write(f"{int(now)} {os.getpid()}\n")
    return state


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    state = load_state()
    state["started"] = time.time()
    state["pid"] = os.getpid()
    if "--once" not in argv:
        resume_stopped("supervisor start")   # a restart never leaves an intruder frozen; the next tick stops it again if a gate still runs
        log(f"box-supervisor start pid {os.getpid()}")
    while True:
        try:
            tick(state)
        except Exception as ex:   # the watchdog must outlive one bad tick; supervisord restarts it if it dies anyway
            log(f"tick error: {type(ex).__name__}: {ex}")
        if "--once" in argv:
            return 0
        time.sleep(INTERVAL)


if __name__ == "__main__":
    sys.exit(main())
