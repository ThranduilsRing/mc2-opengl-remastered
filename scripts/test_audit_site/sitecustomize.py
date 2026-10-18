"""Runtime input recorder for the per-module test cache (scripts/module_test_cache.py).

Python imports this file at startup when the directory is on PYTHONPATH. It does nothing unless MC2_AUDIT_OUT (a
directory) and MC2_AUDIT_ROOT (the repo root) are set. Then a sys.addaudithook hook records, for this process:
  * reads   repo files opened (first access wins): rel path -> [sha1 of the content seen | ABSENT, time]. A file whose
            first access truncates / creates it is an output, not an input, and is not recorded. A .pyc under
            __pycache__ is mapped to its .py source.
  * dirs    repo directories listed (os.listdir / os.scandir, hence os.walk, glob, pathlib): rel path -> sha1 of the
            sorted entry names (so a new or deleted file invalidates), ignoring __pycache__ / *.pyc.
  * globs   glob.glob patterns (informational; the listings they trigger are in dirs).
  * opaque  child processes that can read anything. A python child that inherits this environment records itself
            (modelled, not listed here); git, sh, cmake, a python with -I/-E/-S or a custom env are listed as
            {toks, cwd} and classified by the runner (git in a foreign repo, tracked-list, HEAD, compiler tools, ...).
At exit one JSON file <pid>.json is written into MC2_AUDIT_OUT. Standard library only; never raises into the program.
"""
import os
import sys

_OUT = os.environ.get("MC2_AUDIT_OUT")
_ROOT = os.environ.get("MC2_AUDIT_ROOT")

if _OUT and _ROOT:
    import atexit
    import hashlib
    import json
    import threading
    import time

    _BS = chr(92)
    _root = os.path.abspath(_ROOT).replace(_BS, "/").rstrip("/") + "/"
    _rootl = _root.lower()
    _tl = threading.local()
    _reads, _dirs, _globs, _spawns, _written = {}, {}, [], [], set()
    _WR = os.O_WRONLY | os.O_RDWR | os.O_APPEND | os.O_CREAT | os.O_TRUNC
    _SKIP_NAMES = ("__pycache__", ".pytest_cache")
    _PY = ("python", "pythonw", "python3", "py")
    _exe = os.path.normcase(sys.executable)

    def _rel(p):
        if isinstance(p, bytes):
            p = os.fsdecode(p)
        if not isinstance(p, str):
            return None
        p = p.replace(_BS, "/")
        if not (len(p) > 1 and (p[1] == ":" or p.startswith("/"))):
            p = os.getcwd().replace(_BS, "/") + "/" + p
        if "/../" in p or "/./" in p or p.endswith("/.."):
            p = os.path.normpath(p).replace(_BS, "/")
        if p.lower().startswith(_rootl):
            return p[len(_root):]
        if p.lower() + "/" == _rootl:
            return ""
        return None

    def _sha(path):
        h = hashlib.sha1()
        with open(path, "rb") as f:
            while True:
                b = f.read(1 << 20)
                if not b:
                    break
                h.update(b)
        return h.hexdigest()

    def _listing(path):
        names = sorted(n for n in os.listdir(path) if n not in _SKIP_NAMES and not n.endswith(".pyc"))
        return hashlib.sha1(chr(10).join(names).encode("utf-8", "replace")).hexdigest()

    def _on_open(args):
        path = args[0]
        if isinstance(path, int):
            return
        rel = _rel(path)
        if rel is None or rel == ".git" or rel.startswith(".git/"):
            return
        if rel.endswith(".pyc") and "/__pycache__/" in "/" + rel:
            d, _, f = ("/" + rel).rpartition("/__pycache__/")
            rel = (d + "/" + f.split(".")[0] + ".py").lstrip("/")
        if rel in _reads or rel in _written:
            return
        flags = args[2] if len(args) > 2 and isinstance(args[2], int) else 0
        full = _root + rel
        if flags & _WR:
            truncating = bool(flags & os.O_TRUNC) or (bool(flags & os.O_WRONLY) and not flags & os.O_APPEND)
            if truncating or not os.path.isfile(full):
                _written.add(rel)
                return
        if os.path.isdir(full):
            return
        try:
            _reads[rel] = [_sha(full) if os.path.isfile(full) else "ABSENT", time.time()]
        except OSError:
            _reads[rel] = ["UNREADABLE", time.time()]

    def _on_list(args):
        path = args[0] if args else None
        if isinstance(path, int):
            return
        rel = _rel(path if path is not None else ".")
        if rel is None or rel == ".git" or rel.startswith(".git/"):
            return
        rel = rel.rstrip("/")
        if rel in _dirs:
            return
        try:
            _dirs[rel] = _listing(_root + rel)
        except OSError:
            _dirs[rel] = "ABSENT"

    def _modelled(args):
        """(True, None) for a python child that records itself; else (False, {exe, toks, cwd}) for the runner to classify."""
        pad = list(args) + [None] * 4
        argv, cwd, env = pad[1], pad[2], pad[3]
        toks = argv if isinstance(argv, (list, tuple)) else str(argv).split()
        toks = [os.fsdecode(t) if isinstance(t, bytes) else str(t) for t in toks]
        info = {"toks": toks[:14], "cwd": os.fsdecode(cwd) if cwd else os.getcwd()}
        if not toks:
            return False, info
        name = os.path.basename(toks[0]).lower()
        if name.endswith(".exe"):
            name = name[:-4]
        if not (name in _PY or os.path.normcase(toks[0]) == _exe):
            return False, info
        for t in toks[1:]:
            if t in ("-m", "-c") or not t.startswith("-"):
                break
            if not t.startswith("--") and any(c in "IES" for c in t[1:] if c.isalpha()):
                return False, info
        if env is not None:
            try:
                if "MC2_AUDIT_OUT" not in env:
                    return False, info
            except TypeError:
                return False, info
        return True, None

    _EVENTS = ("open", "os.listdir", "os.scandir", "subprocess.Popen", "glob.glob",
               "os.system", "os.exec", "os.spawn", "os.posix_spawn", "os.startfile")

    def _hook(event, args):
        if event not in _EVENTS or getattr(_tl, "busy", False):
            return
        _tl.busy = True
        try:
            if event == "open":
                _on_open(args)
            elif event in ("os.listdir", "os.scandir"):
                _on_list(args)
            elif event == "glob.glob":
                _globs.append(str(args[0]))
            elif event == "subprocess.Popen":
                ok, info = _modelled(args)
                if not ok:
                    _spawns.append(info)
            else:
                _spawns.append({"toks": [event, (str(args[0])[:200] if args else "")], "cwd": os.getcwd()})
        except Exception:
            pass
        finally:
            _tl.busy = False

    def _dump():
        try:
            _tl.busy = True
            os.makedirs(_OUT, exist_ok=True)
            with open(os.path.join(_OUT, "%d.json" % os.getpid()), "w", encoding="utf-8") as f:
                json.dump({"argv": sys.argv[:3], "reads": _reads, "dirs": _dirs, "globs": _globs[:200],
                           "opaque": _spawns[:400]}, f)
        except Exception:
            pass

    atexit.register(_dump)
    sys.addaudithook(_hook)
