#!/usr/bin/env python3
"""contracts_cache.py -- row-level input-hash cache for scripts/check-contracts.sh.

A row that PASSED against exactly these inputs is not run again. The cache key of a row is a SHA-256 over:
  * CACHE_VERSION, the python and git versions (tool versions),
  * the row's command line,
  * every input file's identity: the index blob sha for a tracked, unmodified file; a hash of the working-tree
    content for a modified / untracked (and, for `+ignored` rows, ignored) file; a DELETED marker for a tracked
    file missing from the tree. That is "git blob shas of the inputs plus the working-tree diff of those paths".
Inputs = the row's declared scan roots (scripts/contracts-cache-inputs.txt) PLUS the check's own code, found
automatically: repo files named in the command, transitively the local python imports and repo paths mentioned in
those files, the same-stem siblings of each entry script (baselines/allowlists) and the whole directory of an
entry script that lives in a sub-directory.

SAFE DEFAULTS
  * a row with no declaration is never cached;
  * only a PASS is ever stored (the caller stores after rc == 0); FAIL / KNOWN are never cached;
  * any error here degrades to MISS, never to a false HIT.

CLI (used by check-contracts.sh)
  keys   <repo>  < TSV(label \t cmd)   -> TSV(label \t key \t HIT|MISS)   (undeclared rows are omitted)
  store  <repo>  < TSV(label \t key)   -> records PASS for each
  Both honour no flags (tests: scripts/tests/test_contracts_cache.py); `--no-cache` is handled by the shell (it simply never calls this).
"""
from __future__ import annotations

import fnmatch
import hashlib
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import console_child  # noqa: E402

CACHE_VERSION = "1"
DECL_REL = "scripts/contracts-cache-inputs.txt"
TOKEN_RE = re.compile(r"[A-Za-z0-9_\-./]+\.[A-Za-z0-9]{1,12}")
LOCAL_DIRS = ("scripts", "tools")


def _git(repo, *args, nul=False):
    r = subprocess.run(["git", "-C", repo, *args], capture_output=True, check=True,
                       **console_child.child_kwargs(redirected=True))
    out = r.stdout.decode("utf-8", "replace")
    return [x for x in out.split("\0" if nul else "\n") if x]


class Universe:
    def __init__(self, repo):
        self.repo = repo
        # Refresh stat info first: a file a test merely touched (same bytes, new mtime) must not read as modified.
        subprocess.run(["git", "-C", repo, "update-index", "-q", "--really-refresh"], capture_output=True,
                       **console_child.child_kwargs(redirected=True))
        self.tracked = {}       # path -> index blob sha
        for line in _git(repo, "ls-files", "-s", "-z", nul=True):
            meta, path = line.split("\t", 1)
            self.tracked[path] = meta.split()[1]
        self.dirty = set(_git(repo, "ls-files", "-m", "-o", "--exclude-standard", "-z", nul=True))
        self._ignored = None

    @property
    def ignored(self):
        if self._ignored is None:
            self._ignored = set(_git(self.repo, "ls-files", "-o", "-i", "--exclude-standard", "-z", nul=True))
        return self._ignored

    def paths(self, with_ignored):
        s = set(self.tracked) | self.dirty
        if with_ignored:
            s |= self.ignored
        return s

    def ident(self, path):
        full = os.path.join(self.repo, path)
        if path in self.tracked and path not in self.dirty:
            return self.tracked[path]
        try:
            with open(full, "rb") as f:
                data = f.read()
            # git blob id of the working bytes: identical bytes give the identical id whether or not stat info is fresh
            if path not in self.tracked:
                return "w:" + hashlib.sha256(data).hexdigest()
            return hashlib.sha1(b"blob %d" % len(data) + bytes([0]) + data).hexdigest()
        except OSError:
            return "DELETED" if path in self.tracked else "ABSENT"


def load_decls(repo):
    macros, rows = {}, {}
    p = os.path.join(repo, DECL_REL)
    if not os.path.isfile(p):
        return rows
    for raw in open(p, encoding="utf-8"):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("@"):
            name, _, val = line.partition("=")
            macros[name.strip()] = [x for t in val.split() for x in macros.get(t, [t])]
            continue
        label, _, val = line.partition(":")
        specs = []
        for t in val.split():
            specs.extend(macros.get(t, [t]))
        rows[label.strip()] = specs
    return rows


def _match(path, spec):
    if spec == ".":
        return True
    if spec.startswith("glob:"):
        return fnmatch.fnmatchcase(path, spec[5:].replace("**", "*"))
    spec = spec.rstrip("/")
    return path == spec or path.startswith(spec + "/")


def affected_rows(repo, paths):
    """The existing input table also declares per-merge triggers; undeclared rows are nightly."""
    rows = {label for label, specs in load_decls(repo).items()
            if any(any(_match(p, s) for s in specs if not s.startswith(("!", "+")))
                   and not any(_match(p, s[1:]) for s in specs if s.startswith("!")) for p in paths)}
    if os.environ.get("CONTRACTS_NO_CACHE") == "1":
        return rows
    if tip_passed(repo, "lint"):
        rows -= {"log_tag_census", "script_callers"}
    if tip_passed(repo, "native_unit"):
        rows.discard("audio_music_stream")
    return rows


def clean_tip(repo):
    try:
        return _git(repo, "rev-parse", "HEAD")[0] if not _git(repo, "status", "--porcelain") else None
    except (OSError, subprocess.CalledProcessError, IndexError):
        return None


def tip_passed(repo, label):
    head = clean_tip(repo)
    entry = load_cache(repo).get("exact-tip:" + label, {}) if head else {}
    return bool(head and isinstance(entry, dict) and entry.get("key") == head)


def record_tip_pass(repo, label, head, passed):
    """Reuse the existing PASS cache format; dirty, moved, failed and missing evidence never qualify."""
    if not head:
        return
    cache = load_cache(repo)
    cache.pop("exact-tip:" + label, None)
    if passed and head and clean_tip(repo) == head:
        cache["exact-tip:" + label] = {"key": head}
    p = cache_path(repo)
    tmp = p + f".{os.getpid()}.tmp"
    with open(tmp, "w", encoding="utf-8") as stream:
        json.dump(cache, stream)
    os.replace(tmp, p)


def _read(repo, rel):
    try:
        with open(os.path.join(repo, rel), encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def code_closure(repo, uni, cmd):
    """The check's own code and the repo files it names (see module doc)."""
    known = uni.paths(False)
    out, todo = set(), []

    def add(p):
        p = p.replace("\\", "/")
        if p.startswith("./"):
            p = p[2:]
        if p in known and p not in out:
            out.add(p)
            todo.append(p)

    for tok in TOKEN_RE.findall(cmd):
        add(tok)
    entries = list(todo)
    for e in entries:
        d, base = os.path.split(e)
        stem = base.split(".")[0]
        if d and d not in LOCAL_DIRS and e.endswith((".py", ".sh")):
            for k in known:
                if k.startswith(d + "/"):
                    add(k)
        for k in known:
            if os.path.dirname(k) == d and os.path.basename(k).startswith(stem + ".") or \
               os.path.dirname(k) == d and os.path.basename(k).startswith(stem + "_"):
                add(k)
    while todo:
        cur = todo.pop()
        if not cur.endswith((".py", ".sh", ".cmake", ".txt", ".json")) or cur.endswith(".json"):
            continue
        text = _read(repo, cur)
        d = os.path.dirname(cur)
        if cur.endswith(".py"):
            for m in re.finditer(r"^\s*(?:from\s+([A-Za-z0-9_.]+)\s+import|import\s+([A-Za-z0-9_., ]+))", text, re.M):
                mods = [m.group(1)] if m.group(1) else [x.strip().split(" as ")[0] for x in m.group(2).split(",")]
                for mod in mods:
                    rel = mod.replace(".", "/")
                    for base in (d, "scripts", "tools", ""):
                        for cand in (f"{base}/{rel}.py".lstrip("/"), f"{base}/{rel}/__init__.py".lstrip("/")):
                            if cand in known:
                                add(cand)
                                if cand.endswith("__init__.py"):
                                    pk = os.path.dirname(cand) + "/"
                                    for k in known:
                                        if k.startswith(pk):
                                            add(k)
        for tok in TOKEN_RE.findall(text):
            add(tok)
            if d:
                add(f"{d}/{tok}")
    return out


def row_inputs(repo, uni, cmd, specs):
    with_ign = "+ignored" in specs
    pos = [s for s in specs if not s.startswith(("!", "+"))]
    neg = [s[1:] for s in specs if s.startswith("!")]
    files = {p for p in uni.paths(with_ign) if any(_match(p, s) for s in pos) and not any(_match(p, n) for n in neg)}
    files |= code_closure(repo, uni, cmd)
    return sorted(files)


_versions = None


def versions():
    global _versions
    if _versions is None:
        g = subprocess.run(["git", "--version"], capture_output=True, text=True,
                           **console_child.child_kwargs(redirected=True)).stdout.strip()
        tree = os.environ.get("MC2_NINJA_TREE") or "build64-ninja"
        _versions = f"{CACHE_VERSION}|py{sys.version.split()[0]}|{g}|ninja_tree={tree}"
    return _versions


def compute_key(repo, uni, cmd, specs):
    nul, nl = bytes([0]), bytes([10])
    h = hashlib.sha256()
    h.update(versions().encode() + nul + cmd.encode() + nul + repr(sorted(specs)).encode() + nul)
    items = [(p, uni.ident(p)) for p in row_inputs(repo, uni, cmd, specs)]
    for p, ident in items:
        h.update(p.encode() + nul + ident.encode() + nl)
    dbg = os.environ.get("CONTRACTS_CACHE_DEBUG")
    if dbg:      # dump what was keyed, to diff two runs: CONTRACTS_CACHE_DEBUG=<dir>
        with open(os.path.join(dbg, hashlib.md5(cmd.encode()).hexdigest() + ".txt"), "w") as f:
            f.write(cmd + chr(10) + chr(10).join(p + " " + i for p, i in items) + chr(10))
    return h.hexdigest()


def _shared_dir():
    """Cache shared by every worktree and throwaway clone (a per-gitdir file always missed in `clone --shared`
    prechecks). Entries are content keyed, so sharing is safe."""
    d = os.environ.get("MC2_CONTRACTS_CACHE_DIR")
    if d:
        return d
    for cand in ("G:/mc2-tmp/precheck-cache", "/workspace/cache/precheck-cache"):
        if os.path.isdir(os.path.dirname(cand)):
            return cand
    return None


def cache_path(repo):
    shared = _shared_dir()
    if shared:
        os.makedirs(shared, exist_ok=True)
        return os.path.normpath(os.path.join(shared, "mc2_contracts_cache.json"))
    r = subprocess.run(["git", "-C", repo, "rev-parse", "--git-path", "mc2_contracts_cache.json"],
                       capture_output=True, text=True, check=True, **console_child.child_kwargs(redirected=True))
    p = r.stdout.strip()
    return os.path.normpath(p if os.path.isabs(p) else os.path.join(repo, p))


def load_cache(repo):
    try:
        with open(cache_path(repo), encoding="utf-8") as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def main(argv):
    if len(argv) < 3 or argv[1] not in ("keys", "store", "affected"):
        print(__doc__)
        return 2
    mode = argv[1]
    # The shell passes an MSYS-style path (/a/Games/...); git resolves it, plain python file reads would not.
    repo = subprocess.run(["git", "-C", argv[2], "rev-parse", "--show-toplevel"], capture_output=True, text=True,
                          check=True, **console_child.child_kwargs(redirected=True)).stdout.strip()
    if mode == "affected":
        paths = _git(repo, "diff", "--name-only", "-z", argv[3], "HEAD", nul=True)
        print("\n".join(sorted(affected_rows(repo, paths))))
        return 0
    rows = [ln.rstrip("\n").split("\t") for ln in sys.stdin if ln.strip()]
    if mode == "keys":
        decls, cache = load_decls(repo), load_cache(repo)
        uni = Universe(repo)
        for label, cmd in rows:
            if label not in decls or label in {"vk_shaders_compile", "audio_music_stream", "delivery_installs"}:
                continue
            try:
                key = compute_key(repo, uni, cmd, decls[label])
            except Exception:       # any doubt is a MISS (and, with no key, an uncached run)
                continue
            print(f"{label}\t{key}\t{'HIT' if cache.get(label, {}).get('key') == key else 'MISS'}")
        return 0
    cache = load_cache(repo)
    for label, key in rows:
        cache[label] = {"key": key}
    p = cache_path(repo)
    tmp = p + f".{os.getpid()}.tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cache, f, indent=1, sort_keys=True)
    os.replace(tmp, p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
