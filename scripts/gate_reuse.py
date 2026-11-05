"""Merge-gate reuse: skip work whose real inputs did not change (lane gate-reuse, 2026-11-05).

Three pieces, all fail-safe (any doubt, error or corruption is a MISS that reruns the stage):
  * mods stage result cache keyed on exe sha + base-install payload + every mod layer + engine payload + the
    mod_smoke import closure + policy data + normalized command/env + HEAD. Hit/miss is logged with the key.
  * persistent mod stage dir (`<stage>/_cache`) sealed with a sha manifest after each PASS and verified before the
    next run; a mismatch wipes it (rebuilt, so slower but never wrong).
  * deployed-payload seed: a pristine deployed install kept per payload key so the stock copy and the deploy are
    skipped when the payload already matches.
File shas come from a stat-keyed cache (relative path + size + mtime_ns), like git's index; unchanged files are
not rehashed, changed or new files always are.
"""
from __future__ import annotations

import ast
import hashlib
import json
import os
import shutil
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

CACHE_ROOT = Path(os.environ.get("MC2_GATE_REUSE_ROOT", "E:/mc2-installs/_gate-reuse"))
SCHEMA = 1
SKIP_TOP = ("debug_state", "crashes", "logs")
SKIP_SUFFIX = (".log", ".dmp", ".tmp", ".pdb")
HASH_THREADS = 4


def log(msg):
    print(f"[gate-reuse] {msg}", flush=True)


def _digest(obj):
    return hashlib.sha256(json.dumps(obj, sort_keys=True).encode()).hexdigest()


def _file_sha(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


class ShaCache:
    """Persistent {label/rel: [size, mtime_ns, sha]}; a stat mismatch always rehashes."""

    def __init__(self, path=None):
        self.path = Path(path) if path else CACHE_ROOT / "shacache.json"
        self.lock = threading.Lock()
        self.dirty = False
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
            self.data = data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            self.data = {}  # corrupt cache: rehash everything

    def sha(self, label, rel, path):
        st = os.stat(path)
        key = f"{label}/{rel}"
        hit = self.data.get(key)
        if hit and hit[0] == st.st_size and hit[1] == st.st_mtime_ns and len(hit[2]) == 64:
            return hit[2]
        value = _file_sha(path)
        with self.lock:
            self.data[key] = [st.st_size, st.st_mtime_ns, value]
            self.dirty = True
        return value

    def save(self):
        if not self.dirty:
            return
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            pending = self.path.with_suffix(".tmp")
            pending.write_text(json.dumps(self.data), encoding="utf-8")
            pending.replace(self.path)
            self.dirty = False
        except OSError:
            pass


def tree_digest(root, label, cache, skip=None):
    """Digest of every file under root as sorted (relpath, sha). A missing root raises (caller treats as miss)."""
    root = Path(root)
    if not root.is_dir():
        raise FileNotFoundError(f"input tree missing: {root}")
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in filenames:
            path = Path(dirpath) / name
            rel = path.relative_to(root).as_posix()
            if skip and skip(rel):
                continue
            files.append((rel, path))
    with ThreadPoolExecutor(max_workers=HASH_THREADS) as pool:
        shas = list(pool.map(lambda item: cache.sha(label, item[0], item[1]), files))
    return _digest(sorted((rel, sha) for (rel, _), sha in zip(files, shas))), len(files)


def _skip_install(rel):
    return rel.split("/", 1)[0] in SKIP_TOP or rel.lower().endswith(SKIP_SUFFIX)


def import_closure(entry, repo, search=("tools", "scripts")):
    """Repo .py files transitively imported by entry. Raises ValueError on dynamic imports (caller hashes everything)."""
    seen, queue = {}, [Path(entry).resolve()]
    dirs = [Path(repo) / d for d in search] + [Path(repo) / "scripts/smoke_lib", Path(repo) / "tools/repo_intel"]
    while queue:
        path = queue.pop()
        if path in seen:
            continue
        seen[path] = True
        tree = ast.parse(path.read_text(encoding="utf-8", errors="replace"))
        for node in ast.walk(tree):
            names = []
            if isinstance(node, ast.Import):
                names = [a.name for a in node.names]
            elif isinstance(node, ast.ImportFrom) and node.module:
                names = [node.module] + [f"{node.module}.{a.name}" for a in node.names]
            elif isinstance(node, ast.Call):
                func = node.func
                fname = func.id if isinstance(func, ast.Name) else getattr(func, "attr", "")
                if fname in ("__import__", "import_module", "spec_from_file_location", "exec_module"):
                    raise ValueError(f"dynamic import in {path}")
            for name in names:
                parts = name.split(".")
                for base in [path.parent] + dirs:
                    for cand in (base.joinpath(*parts).with_suffix(".py"), base.joinpath(*parts, "__init__.py")):
                        if cand.is_file():
                            queue.append(cand.resolve())
    return sorted(seen)


def mods_key(command, overrides, root, head, cache=None, mods=None):
    """(key, parts) for the mods stage. Raises on any unreadable input (caller treats as a miss)."""
    root = Path(root)
    cache = cache or ShaCache()
    if mods is None:
        sys.path.insert(0, str(root / "tools"))
        import mod_smoke as mods  # noqa: WPS433
    def option(flag, default):
        return Path(command[command.index(flag) + 1] if flag in command else default).resolve()
    base = option("--base-install", mods.DEFAULT_BASE_INSTALL)
    mod_root = option("--install-root", mods.DEFAULT_INSTALL_ROOT)
    build = option("--build-dir", root / "build64-ninja")
    exe = Path(command[command.index("--exe") + 1]) if "--exe" in command else build / "mc2.exe"
    parts = {"schema": SCHEMA, "head": head, "exe": cache.sha("exe", exe.name, exe)}
    parts["base"], parts["base_files"] = tree_digest(base, "base", cache, _skip_install)
    for _, folder, parent in mods.BATTERY:
        for layer in mods.mod_layers(mod_root, folder, parent):
            name = "mod:" + layer.name
            if name not in parts:
                parts[name], _ = tree_digest(layer, name, cache)
    payload = sorted((rel, cache.sha("engine", rel, src)) for src, rel in mods.engine_payload_items(build, build))
    parts["engine_payload"] = _digest(payload)
    try:
        code = import_closure(root / "tools/mod_smoke.py", root)
    except (ValueError, SyntaxError):
        code = [p for d in ("scripts", "tools") for p in (root / d).rglob("*.py")]
    parts["code"] = _digest(sorted((p.relative_to(root).as_posix(), cache.sha("code", p.relative_to(root).as_posix(), p))
                                   for p in code))
    data = [mods.WARNING_BUDGET, root / "system.cfg", mods.KNOWN_FAILURES]
    parts["data"] = _digest([(p.name, cache.sha("data", p.name, p) if p.is_file() else None) for p in data])
    norm = [a.replace(str(base), "{INSTALL}").replace(base.as_posix(), "{INSTALL}") for a in command]
    skip_values = {command[command.index(f) + 1] for f in ("--stage-root", "--exe") if f in command}
    norm = [a for a in norm if a not in skip_values]
    env = {k: v for k, v in list(os.environ.items()) + list(overrides.items())
           if (k.startswith("MC2_") or k == "PYTHONPATH") and k != "MC2_SMOKE_ARTIFACT_ROOT"}
    parts["command"] = _digest([norm, env])
    cache.save()
    return _digest(parts), parts


def _entry_path(key, cache_root):
    return Path(cache_root) / "mods" / key


def lookup(key, parts, cache_root=None):
    """Return the stored entry for key, or None. Missing/corrupt/mismatched entries are misses (logged with reason)."""
    folder = _entry_path(key, cache_root or CACHE_ROOT)
    try:
        entry = json.loads((folder / "entry.json").read_text(encoding="utf-8"))
        if entry.get("schema") != SCHEMA or entry.get("key") != key or entry.get("parts") != parts \
                or entry.get("status") != "PASS":
            raise ValueError("entry does not match key")
        report = folder / "battery.json"
        if _file_sha(report) != entry["report_sha"]:
            raise ValueError("report sha mismatch")
        rows = json.loads(report.read_text(encoding="utf-8"))
        if not isinstance(rows, list) or len(rows) < entry["min_rows"]:
            raise ValueError("report rows missing")
    except FileNotFoundError:
        log(f"mods MISS key={key[:16]} reason=no entry")
        return None
    except (OSError, ValueError, KeyError, TypeError) as exc:
        log(f"mods MISS key={key[:16]} reason=corrupt entry ({exc})")
        return None
    log(f"mods HIT key={key[:16]} stored={entry['created']}")
    return entry


def store(key, parts, report, min_rows, cache_root=None):
    """Record a PASS. report is the battery.json the run wrote. Errors are logged, never raised."""
    import time
    folder = _entry_path(key, cache_root or CACHE_ROOT)
    try:
        rows = json.loads(Path(report).read_text(encoding="utf-8"))
        if not isinstance(rows, list) or len(rows) < min_rows:
            raise ValueError("battery report has too few rows")
        shutil.rmtree(folder, ignore_errors=True)
        folder.mkdir(parents=True)
        shutil.copy2(report, folder / "battery.json")
        entry = dict(schema=SCHEMA, key=key, parts=parts, status="PASS", min_rows=min_rows,
                     report_sha=_file_sha(folder / "battery.json"), created=time.strftime("%Y-%m-%d %H:%M:%S"))
        (folder / "entry.json").write_text(json.dumps(entry, indent=2), encoding="utf-8")
        log(f"mods STORED key={key[:16]}")
    except (OSError, ValueError) as exc:
        log(f"mods store skipped: {exc}")


# ---- persistent mod stage dir ------------------------------------------------------------------------------

SEAL = ".integrity.json"


def _stage_manifest(cache_dir, cache):
    files = {}
    for dirpath, _, names in os.walk(cache_dir):
        for name in names:
            path = Path(dirpath) / name
            rel = path.relative_to(cache_dir).as_posix()
            if rel != SEAL:
                files[rel] = path
    shas = {}
    with ThreadPoolExecutor(max_workers=HASH_THREADS) as pool:
        for rel, sha in zip(files, pool.map(lambda r: cache.sha("stage", r, files[r]), files)):
            shas[rel] = [files[rel].stat().st_size, sha]
    return shas


def stage_seal(stage_root, cache=None):
    cache_dir = Path(stage_root) / "_cache"
    if cache_dir.is_dir():
        cache = cache or ShaCache()
        (cache_dir / SEAL).write_text(json.dumps(_stage_manifest(cache_dir, cache)), encoding="utf-8")
        cache.save()


def stage_verify(stage_root, cache=None):
    """True when the persistent stage cache matches its seal. Otherwise it is wiped (rebuilt by the run) and False."""
    cache_dir = Path(stage_root) / "_cache"
    if not cache_dir.is_dir():
        return False
    try:
        sealed = json.loads((cache_dir / SEAL).read_text(encoding="utf-8"))
        current = _stage_manifest(cache_dir, cache or ShaCache())
        ok = sealed == current
        reason = "" if ok else "manifest differs from seal"
    except (OSError, ValueError) as exc:
        ok, reason = False, str(exc)
    if ok:
        log(f"stage cache OK {cache_dir} ({len(sealed)} files)")
    else:
        log(f"stage cache INVALID ({reason}); wiping {cache_dir}")
        shutil.rmtree(cache_dir, ignore_errors=True)
    return ok


# ---- deployed payload seed ---------------------------------------------------------------------------------

def payload_key(root, head, exe=None):
    root = Path(root)
    exe = Path(exe or root / "build64-ninja/mc2.exe")
    shaders = sorted((p.relative_to(root).as_posix(), _file_sha(p)) for p in (root / "shaders").rglob("*") if p.is_file())
    baked = sorted((p.name, _file_sha(p)) for p in (root / "build64-ninja/shaders/vulkan").glob("*.spv"))
    return _digest([SCHEMA, head, _file_sha(exe), shaders, baked])


def install_current(install, root, head):
    """True when the install's deployed payload already equals the HEAD build (so a deploy would change nothing)."""
    try:
        from deploy_payload import verify_published_identity
        verify_published_identity(Path(install), root, head)
        return True
    except Exception:  # any doubt is a miss
        return False


def deployed_seed(root, head, cache_root=None):
    """Path of a pristine deployed install for this payload, or None."""
    seed = Path(cache_root or CACHE_ROOT) / "deployed"
    try:
        mark = json.loads((seed / ".payload-key").read_text(encoding="utf-8"))
        if mark != payload_key(root, head) or not install_current(seed, root, head):
            raise ValueError("payload differs")
    except (OSError, ValueError) as exc:
        log(f"deployed seed MISS ({exc})")
        return None
    log(f"deployed seed HIT {seed}")
    return seed


def save_deployed(install, root, head, cache_root=None):
    """Snapshot a freshly deployed, not yet mutated install as the pristine seed. Errors are logged, never raised."""
    import subprocess
    seed = Path(cache_root or CACHE_ROOT) / "deployed"
    try:
        if not install_current(install, root, head):
            raise ValueError("install is not the HEAD payload")
        shutil.rmtree(seed, ignore_errors=True)
        subprocess.run([sys.executable, str(Path(root) / "scripts/make-isolated-deploy.py"), "--from", str(install),
                        "--to", str(seed), "--snapshot"], check=True)
        (seed / ".payload-key").write_text(json.dumps(payload_key(root, head)), encoding="utf-8")
        log(f"deployed seed SAVED {seed}")
    except Exception as exc:  # noqa: BLE001  never fail the gate over a cache write
        shutil.rmtree(seed, ignore_errors=True)
        log(f"deployed seed not saved: {exc}")


# ---- merge_gate glue (mods stage) ----------------------------------------------------------------------------

_PENDING = {}


def mods_begin(command, overrides, root, head):
    """Entry on a hit, else None (a miss also verifies the persistent stage dir and remembers the key for mods_end)."""
    _PENDING.pop("mods", None)
    if "--battery" not in command or "--stage-root" not in command:
        log("mods MISS reason=not a full battery command")
        return None
    try:
        key, parts = mods_key(command, overrides, root, head)
    except Exception as exc:  # noqa: BLE001  any unreadable input is a miss
        log(f"mods MISS reason=cannot compute key ({type(exc).__name__}: {exc})")
        return None
    entry = lookup(key, parts)
    if entry:
        return entry
    stage = Path(command[command.index("--stage-root") + 1])
    stage_verify(stage)
    _PENDING["mods"] = (key, parts, stage, set(stage.glob("*/battery.json")))
    return None


def mods_end(passed):
    """After a mods run: on PASS store the result and reseal the stage dir; always forget the pending key."""
    pending = _PENDING.pop("mods", None)
    if not pending or not passed:
        return
    key, parts, stage, before = pending
    fresh = [p for p in stage.glob("*/battery.json") if p not in before]
    if not fresh:
        log("mods store skipped: no new battery.json")
        return
    report = max(fresh, key=lambda p: p.stat().st_mtime_ns)
    store(key, parts, report, len(sys.modules["mod_smoke"].BATTERY) if "mod_smoke" in sys.modules else 6)
    stage_seal(stage)
