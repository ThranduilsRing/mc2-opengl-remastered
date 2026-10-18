#!/usr/bin/env python3
"""module_test_cache.py -- per-test-module result cache whose inputs are RECORDED at run time, not declared.

Used by scripts/run_tools_tests.py and scripts/run_scripts_tests.py (rows tools_tests / scripts_tests).

HOW IT WORKS
  Each test module runs in its own python process with scripts/test_audit_site/sitecustomize.py on PYTHONPATH. That
  file installs a sys.addaudithook recorder (see its docstring): every repo file the module (and any python child it
  spawns) opens, with the sha1 of the bytes it saw, every repo directory it lists (sha1 of the sorted entry names, so
  a new or deleted file invalidates), plus a list of OPAQUE child processes.
  After a FULL pass (exit 0, nothing skipped / xfailed) the record is stored. A later run is a HIT when the command
  line and tool versions match and every recorded file and listing still has the recorded identity. The module's own
  source, conftests, pytest.ini and everything it imports are in the record because importing opens them.
  A module that spawned a process we cannot model (git, sh, cmake, python -I/-E/-S, custom env) can read anything, so
  it is keyed on the whole tree instead (contracts_cache identity of "."), exactly like the old row-level key.

NEVER CACHED: a failing / erroring / skipped / xfailed module, a module whose record is missing (worker died).
OFF SWITCH: --no-cache on the runner, or CONTRACTS_NO_CACHE=1.

KNOWN BLIND SPOTS (stated, not hidden): file existence / size / mtime checks that never open the file (os.stat,
Path.exists, os.path.getmtime) are not audit events; environment variables; third-party package versions (only the
python and git versions are keyed); an opaque child process is covered only by the whole-tree key.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import console_child  # noqa: E402
import contracts_cache as cc  # noqa: E402

AUDIT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_audit_site")
SKIP_NAMES = ("__pycache__", ".pytest_cache")
# Repo-relative directories the tests use as scratch output (git-ignored): not inputs.
OUTPUT_DIRS = ("scratch", "run")
# What a C++ compiler child can read in the repo: the source trees (same set as @CPP in contracts-cache-inputs.txt).
CXX_SCOPE = ("code", "mclib", "gui", "editor", "GameAdapters", "GameOS", "Viewer", "EditorBridge", "RenderCore",
             "RenderWorld", "Visibility", "MeshRenderer", "MaterialSystem", "DebugRenderer", "RenderDeviceGL",
             "GuiRuntime", "HostServices", "Gates", "cmake", "CMakeLists.txt", "3rdparty")


def _sha_file(path: str) -> str:
    h = hashlib.sha1()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def _cache_file(root: str, tier: str) -> str:
    r = subprocess.run(["git", "-C", root, "rev-parse", "--git-path", f"mc2_testcache_{tier}.json"],
                       capture_output=True, text=True, check=True, **console_child.child_kwargs(redirected=True))
    p = r.stdout.strip()
    return os.path.normpath(p if os.path.isabs(p) else os.path.join(root, p))


class ModuleTestCache:
    def __init__(self, root: str, tier: str, enabled: bool = True):
        self.root = root
        self.tier = tier
        self.enabled = enabled and os.environ.get("CONTRACTS_NO_CACHE", "0") != "1"
        self.path = _cache_file(root, tier) if self.enabled else ""
        self.data: dict = {}
        if self.enabled:
            try:
                with open(self.path, encoding="utf-8") as f:
                    self.data = json.load(f)
            except (OSError, ValueError):
                self.data = {}
        self.entries: dict = self.data.setdefault("modules", {})
        self.memo: dict = self.data.setdefault("memo", {})      # rel -> [size, mtime_ns, sha1]
        self._extras = None
        self.miss: dict[str, str] = {}
        self.meta = f"{cc.CACHE_VERSION}|{cc.versions()}"
        self.stats = {"hit": 0, "ran": 0, "uncacheable": 0}

    # -- identities now ------------------------------------------------------------------------------------------
    def file_ident(self, rel: str) -> str:
        full = os.path.join(self.root, rel)
        try:
            st = os.stat(full)
        except OSError:
            return "ABSENT"
        if not os.path.isfile(full):
            return "ABSENT"
        m = self.memo.get(rel)
        if m and m[0] == st.st_size and m[1] == st.st_mtime_ns:
            return m[2]
        try:
            sha = _sha_file(full)
        except OSError:
            return "UNREADABLE"
        self.memo[rel] = [st.st_size, st.st_mtime_ns, sha]
        return sha

    def dir_ident(self, rel: str) -> str:
        try:
            names = sorted(n for n in os.listdir(os.path.join(self.root, rel) if rel else self.root)
                           if n not in SKIP_NAMES and not n.endswith(".pyc"))
        except OSError:
            return "ABSENT"
        return hashlib.sha1("\n".join(names).encode("utf-8", "replace")).hexdigest()

    def _git_out(self, *args) -> bytes:
        r = subprocess.run(["git", "-C", self.root, *args], capture_output=True,
                           **console_child.child_kwargs(redirected=True))
        return r.stdout if r.returncode == 0 else b"ERR"

    def extras_now(self) -> dict:
        """Start-of-run values of every coarse key a modelled child process can depend on (computed once)."""
        if self._extras is None:
            uni = cc.Universe(self.root)
            self._extras = {
                "tree": cc.compute_key(self.root, uni, "module_test_cache:tree", ["."]),
                "scope": cc.compute_key(self.root, uni, "module_test_cache:shaders",
                                        ["shaders", "tools/shader_offline_build"]),
                "cc": cc.compute_key(self.root, uni, "module_test_cache:cxx", list(CXX_SCOPE)),
                "head": self._git_out("rev-parse", "HEAD").decode().strip(),
                "list": hashlib.sha1(self._git_out("ls-files", "-z")).hexdigest(),
            }
        return self._extras

    def tree_key(self) -> str:
        return self.extras_now()["tree"]

    # -- classification of child processes we cannot audit ---------------------------------------------------------
    def classify(self, sp: dict) -> tuple[str, str]:
        """(kind, detail). kind: foreign | harmless | head | list | tool | tree."""
        toks, cwd = sp.get("toks") or [], sp.get("cwd") or self.root
        desc = " ".join(toks[:6])
        if not toks:
            return "tree", desc
        exe = os.path.basename(toks[0]).lower().replace(".exe", "")
        bs = chr(92)
        root_l = self.root.replace(bs, "/").rstrip("/").lower() + "/"

        def inside(path):
            p = os.path.normpath(path if os.path.isabs(path) else os.path.join(cwd, path)).replace(bs, "/")
            return (p.lower() + "/").startswith(root_l)

        if exe == "git":
            target, i, sub = cwd, 1, None
            while i < len(toks):
                t = toks[i]
                if t == "-C" and i + 1 < len(toks):
                    target = toks[i + 1] if os.path.isabs(toks[i + 1]) else os.path.join(target, toks[i + 1])
                    i += 2
                elif t == "-c" and i + 1 < len(toks):
                    i += 2
                elif t.startswith(("--git-dir", "--work-tree")):
                    return "tree", desc
                elif t.startswith("-"):
                    i += 1
                else:
                    sub = t
                    break
            if not inside(target):
                return "foreign", desc
            rest = toks[i + 1:]
            if sub is None or sub == "version":
                return "harmless", desc
            if sub in ("rev-parse", "show", "cat-file", "ls-tree", "log", "rev-list", "merge-base", "cat-file"):
                refs = [x for x in rest if not x.startswith("-") and not x.startswith("mc2_")]
                if sub == "rev-parse" and all(x.startswith(("--show-toplevel", "--git-path", "--git-dir",
                                                            "--is-inside", "--show-prefix")) or x.startswith("mc2_")
                                              for x in rest):
                    return "harmless", desc
                if any("HEAD" in x for x in refs) or (sub == "rev-parse" and "--short" in " ".join(rest))                         or sub == "cat-file" and not refs:
                    return "head", desc
                # an explicit commit / object id names an immutable object: nothing in the working tree matters
                if refs and all(re.match(r"^[0-9a-f]{7,40}([^:]*)(:.*)?$", x) for x in refs):
                    return "harmless", desc
                return "tree", desc
            if sub in ("init", "clone") and [a for a in rest if not a.startswith("-")]                     and all(not inside(a) for a in rest if not a.startswith("-")):
                return "foreign", desc
            if sub == "hash-object" and "--stdin" in rest:
                return "harmless", desc
            if sub == "ls-files" and all(a in ("-z", "--cached", "-c", "--full-name", "--") or not a.startswith("-")
                                         for a in rest):
                return "list", desc
            return "tree", desc
        if exe == "cmd" and [t.lower() for t in toks[1:]] == ["/c", "ver"]:
            return "harmless", desc
        tmp = os.path.normpath(tempfile.gettempdir()).replace(bs, "/").lower() + "/"
        if os.path.isabs(toks[0]) and os.path.normpath(toks[0]).replace(bs, "/").lower().startswith(tmp):
            return "foreign", desc          # an executable the test itself built into the temp dir
        if exe in ("glslc", "glslangvalidator", "glslang", "spirv-cross", "spirv-val", "spirv-opt", "spirv-dis",
                   "slangc"):
            return "tool", toks[0]
        if exe in ("clang", "clang++", "clang-cl", "cl", "g++", "gcc", "c++"):
            return "cc", toks[0]
        return "tree", desc

    # -- lookup / store -------------------------------------------------------------------------------------------
    def lookup(self, label: str, cmd: str) -> bool:
        why = self._lookup(label, cmd)
        if why:
            self.miss[label] = why
            return False
        self.stats["hit"] += 1
        return True

    def _lookup(self, label: str, cmd: str) -> str:
        """"" for a HIT, else the reason for the miss (reported with --report)."""
        if not self.enabled:
            return "cache off"
        e = self.entries.get(label)
        if not e:
            return "no entry"
        if e.get("meta") != self.meta or e.get("cmd") != cmd:
            return "tool versions / command changed"
        for rel, ident in e["reads"].items():
            if self.file_ident(rel) != ident:
                return "file changed: " + rel
        for rel, ident in e["dirs"].items():
            if self.dir_ident(rel) != ident:
                return "directory listing changed: " + rel
        ex = e.get("extras") or {}
        if ex:
            now = self.extras_now()
            for k in ("tree", "head", "list", "scope", "cc"):
                if k in ex and ex[k] != now[k]:
                    return "coarse key changed: " + k
            for exe, st in (ex.get("tools") or {}).items():
                try:
                    stt = os.stat(exe)
                    if [stt.st_size, stt.st_mtime_ns] != st:
                        return "tool binary changed: " + exe
                except OSError:
                    return "tool binary missing: " + exe
        return ""

    def new_audit(self) -> tuple[dict, str]:
        """(env for the child process, audit out dir)."""
        out = tempfile.mkdtemp(prefix="mc2-audit-")
        env = dict(os.environ)
        env["MC2_AUDIT_OUT"] = out
        env["MC2_AUDIT_ROOT"] = self.root
        env["PYTHONPATH"] = AUDIT_DIR + (os.pathsep + env["PYTHONPATH"] if env.get("PYTHONPATH") else "")
        return env, out

    def finish(self, label: str, cmd: str, out: str, full_pass: bool, secs: float, tree_at_start: str | None):
        """Merge the child records; store when the module fully passed. Always removes `out`."""
        try:
            recs = []
            for n in os.listdir(out):
                try:
                    with open(os.path.join(out, n), encoding="utf-8") as f:
                        recs.append(json.load(f))
                except (OSError, ValueError):
                    pass
        finally:
            shutil.rmtree(out, ignore_errors=True)
        reads, dirs, opaque, globs = {}, {}, [], []
        def noise(rel):
            return any(rel == n or rel.startswith(n + "/") for n in OUTPUT_DIRS)
        for r in recs:
            for rel, (ident, t) in r.get("reads", {}).items():
                if rel not in reads or t < reads[rel][1]:
                    reads[rel] = (ident, t)
            for rel, ident in r.get("dirs", {}).items():
                dirs.setdefault(rel, ident)
            opaque += r.get("opaque", [])
            globs += r.get("globs", [])
        reads = {k: v for k, v in reads.items() if not noise(k)}
        dirs = {k: v for k, v in dirs.items() if not noise(k)}
        if not recs:
            self.stats["uncacheable"] += 1
            return {"opaque": ["no audit record"], "secs": secs}
        kinds: dict[str, list[str]] = {}
        for sp in opaque:
            k, d = self.classify(sp)
            kinds.setdefault(k, []).append(d)
        need = {k for k in kinds if k not in ("foreign", "harmless")}
        info = {"opaque": [f"{k}: {v[0]}" for k, v in sorted(kinds.items()) if k in need],
                "tree": "tree" in kinds, "kinds": {k: len(v) for k, v in kinds.items()},
                "secs": secs, "reads": len(reads), "dirs": len(dirs), "globs": len(globs)}
        if full_pass and self.enabled:
            entry = {"meta": self.meta, "cmd": cmd, "reads": {k: v[0] for k, v in reads.items()},
                     "dirs": dirs, "secs": secs, "kinds": info["kinds"], "opaque": info["opaque"][:6]}
            if need:
                now = self.extras_now()
                ex = {k: now[k] for k in ("head", "list") if k in need}
                if "tree" in need:
                    ex["tree"] = now["tree"]
                if "cc" in need:
                    ex["cc"] = now["cc"]
                    ex["tools"] = ex.get("tools", {})
                    for d in set(kinds["cc"]):
                        try:
                            st = os.stat(d)
                            ex["tools"][d] = [st.st_size, st.st_mtime_ns]
                        except OSError:
                            pass       # resolved through PATH: the version is not keyed (stated risk)
                if "tool" in need:
                    ex["scope"] = now["scope"]
                    ex["tools"] = ex.get("tools", {})
                    for d in set(kinds["tool"]):
                        try:
                            st = os.stat(d)
                            ex["tools"][d] = [st.st_size, st.st_mtime_ns]
                        except OSError:
                            ex["tree"] = now["tree"]
                entry["extras"] = ex
            self.entries[label] = entry
        else:
            self.entries.pop(label, None)
        self.stats["ran"] += 1
        return info

    def save(self):
        if not self.enabled:
            return
        try:
            tmp = self.path + f".{os.getpid()}.tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(self.data, f, separators=(",", ":"))
            os.replace(tmp, self.path)
        except OSError:
            pass


def full_pass(rc: int, tail: str) -> bool:
    """Exit 0 and nothing skipped / xfailed / xpassed in the pytest summary line."""
    if rc != 0:
        return False
    last = [ln for ln in tail.splitlines() if ln.strip()][-1:] or [""]
    return not any(w in last[0] for w in ("skipped", "xfailed", "xpassed", "deselected"))


def run_with_cache(cache: ModuleTestCache, label: str, cmd: str, run):
    """run(env) -> (rc, tail_text, result). Returns (result, hit, info)."""
    if cache.lookup(label, cmd):
        return None, True, None
    tree = cache.tree_key() if cache.enabled else None
    env, out = cache.new_audit()
    t0 = time.time()
    rc, tail, result = run(env)
    info = cache.finish(label, cmd, out, full_pass(rc, tail), time.time() - t0, tree)
    return result, False, info
