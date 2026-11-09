"""Pinned Remastered building payload; binary releases live outside git."""
import hashlib
import csv
import importlib.util
import json
import os
import re
from pathlib import Path, PureWindowsPath
import subprocess
import sys
import shutil
from concurrent.futures import ThreadPoolExecutor

import console_child

KIT_REL = Path("mods/mc2-industrial-kit")
MANIFEST = Path("data/kit/industrial-kit.json")
IMPORT_ARGS = ["--no-ground", "--static-prop"]
REF_DEFAULT = "E:/mc2-ref"


def ref_path(path):
    """A manifest path under E:/mc2-ref, re-rooted at MC2_REF_ROOT when set (the laptop gate keeps its reference copy elsewhere)."""
    s = str(path).replace("\\", "/")
    root = os.environ.get("MC2_REF_ROOT")
    if root and s.casefold().startswith(REF_DEFAULT.casefold() + "/"):
        s = root.rstrip("/\\").replace("\\", "/") + s[len(REF_DEFAULT):]
    return s


def seed_install(source, target, hash_cache=None):
    """Refresh an owner-test projection from one deployed tree, without a cook.

    Read-only data can link on the same volume; cross-volume members copy.
    Written artifacts always get private inodes. Only pinned model recipes carry over.
    """
    if hash_cache is None:
        lane = hashlib.sha256(str(Path(__file__).resolve().parents[1]).encode()).hexdigest()[:16]
        hash_cache = Path('G:/mc2-tmp') / f'shared-data-sha256-{lane}.json'
    source, raw_target = Path(source).resolve(), Path(target).absolute()
    target = raw_target.resolve()
    if target != raw_target:
        raise ValueError('seed target must not be a junction/symlink')
    if source == target or source.is_relative_to(target) or target.is_relative_to(source):
        raise ValueError('seed source and target overlap')
    spec = importlib.util.spec_from_file_location('isolated_seed', Path(__file__).with_name('make-isolated-deploy.py'))
    iso = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(iso)
    guard = iso._load_guard()
    try:
        hashes = json.loads(hash_cache.read_text())
    except (OSError, ValueError):
        hashes = {}

    def digest(path, force=False):
        stat = path.stat()
        key = str(path.resolve())
        stamp = [stat.st_size, stat.st_mtime_ns]
        old = hashes.get(key)
        # Executable bytes are always verified, including a warm seed.
        if not force and path.suffix.lower() != '.exe' and old and old[:2] == stamp:
            return old[2]
        value = sha(path)
        hashes[key] = [*stamp, value]
        return value

    with (source / '.deployed_manifest.csv').open(encoding='utf-8-sig', newline='') as stream:
        if next(csv.reader(stream), [])[:2] != ['manifest_version', 'v1']:
            raise ValueError('unsupported seed manifest')
        manifest = {row['relpath'].casefold(): row for row in csv.DictReader(stream)
                    if not row['relpath'].lower().endswith('.pdb')}
    if 'mc2.exe' not in manifest:
        raise ValueError('seed manifest lacks mc2.exe')
    members = set()
    for row in manifest.values():
        path = contained_path(source, row['relpath'])
        if path.stat().st_size != int(row['bytes']) or digest(path, force=True) != row['sha256']:
            raise ValueError(f'seed manifest mismatch: {path}')
        members.add(row['relpath'])
    links, copies, _, _ = iso.plan(source, guard, manifest)
    members.update(rel for rel, _ in links + copies if rel != '.deployed_manifest.csv')
    # Reuse only verified content-addressed recipes, not arbitrary writable caches.
    for name in ('industrial-cook.json', 'vehicle-cook.json'):
        receipt = source / 'cache/content-model' / name
        if receipt.is_file():
            rows = json.loads(receipt.read_text())['files']
            checked_files(receipt.parent, rows)
            members.add(receipt.relative_to(source).as_posix())
            members.update((receipt.parent / row['path']).relative_to(source).as_posix() for row in rows)
    target.mkdir(parents=True, exist_ok=True)
    prior = contained_path(target, '.deployed_manifest.csv')
    if prior.is_file():
        with prior.open(encoding='utf-8-sig', newline='') as stream:
            next(stream)
            retired = list(csv.DictReader(stream))
        for row in retired:
            if row['relpath'] not in members and not row['relpath'].lower().endswith('.pdb'):
                path = contained_path(target, row['relpath'])
                if path.is_file():
                    if digest(path, force=True) != row['sha256']:
                        raise ValueError(f'modified retired seed member: {path}')
                    path.unlink()
    for rel in sorted(members):
        src, dst = contained_path(source, rel), contained_path(target, rel)
        force = rel.casefold() in manifest or guard.classify(rel) != 'data' or rel == '.deployed_manifest.csv'
        expected = digest(src, force=force)
        if (dst.is_file() and digest(dst, force=force) == expected
                and (guard.classify(rel) == 'data' or dst.stat().st_nlink == 1)):
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        temp = dst.with_name(dst.name + f'.seed-{os.getpid()}')
        try:
            if guard.classify(rel) == 'data' and iso.same_volume(src, dst):
                os.link(src, temp)
            else:
                shutil.copy2(src, temp)
            if digest(temp, force=True) != expected:
                raise ValueError(f'seed copy hash mismatch: {rel}')
            os.replace(temp, dst)
        finally:
            temp.unlink(missing_ok=True)
    # Replace the manifest inode too: an older owner-test may have linked it.
    manifest_path = contained_path(target, '.deployed_manifest.csv')
    pending_manifest = target / f'.seed-manifest-{os.getpid()}.csv'
    fields = ['relpath', 'sha256', 'bytes', 'src_commit', 'timestamp', 'mtime_ns']
    with pending_manifest.open('w', encoding='utf-8', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['manifest_version', 'v1'])
        writer.writerow(fields)
        for row in manifest.values():
            writer.writerow([row.get(key, '') for key in fields])
    os.replace(pending_manifest, manifest_path)
    hash_cache.parent.mkdir(parents=True, exist_ok=True)
    pending = hash_cache.with_suffix(f'.{os.getpid()}.tmp')
    pending.write_text(json.dumps(hashes), encoding='utf-8')
    pending.replace(hash_cache)
    print(f'[deploy_payload] seed verified: {len(members)} files; exe sha={digest(target / "mc2.exe")}; no cook')


def stock_fnv(paths):
    """Compute legacy FNV in native code, batching Windows files in one child."""
    if os.name != 'nt':
        import ctypes
        import ctypes.util
        native = ctypes.CDLL(ctypes.util.find_library('stdc++'))
        fnv = native._ZSt15_Fnv_hash_bytesPKvmm
        fnv.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_size_t]
        fnv.restype = ctypes.c_size_t
        values = []
        for path in paths:
            data = path.read_bytes()
            values.append(f'{fnv(data, len(data), 14695981039346656037):016x}')
        return values
    command = r"""
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition 'using System; using System.IO;
public static class StockHash {
  public static string Hash(string path) {
    ulong hash = 14695981039346656037UL;
    using (var input = File.OpenRead(path)) {
      var buffer = new byte[65536]; int n;
      while ((n = input.Read(buffer, 0, buffer.Length)) > 0)
        for (int i = 0; i < n; ++i) hash = unchecked((hash ^ buffer[i]) * 1099511628211UL);
    }
    return hash.ToString("x16");
  }
}'
foreach ($path in (ConvertFrom-Json ([Console]::In.ReadToEnd()))) { [StockHash]::Hash($path) }
"""
    result = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', command],
                            input=json.dumps([str(p) for p in paths]), capture_output=True,
                            text=True, timeout=300, check=True,
                            **console_child.child_kwargs(redirected=True))
    values = result.stdout.splitlines()
    if len(values) != len(paths) or any(not re.fullmatch('[0-9a-f]{16}', v) for v in values):
        raise ValueError('native stock FNV returned an incomplete inventory')
    return values


def reconcile_stock_textures(target_dir, inventory=None, jobs=1):
    """Receipt byte-proven stock sources, appearance definitions and sidecars.

    The pins include current batch_cook output and verified golden legacy cooks. Deployment
    never trusts paired source existence, target art, or MOD overlay contents.
    Unknown recipes and mismatched bytes retain user priority.
    """
    target = Path(target_dir).resolve()
    if inventory is None:
        inventory = json.loads((Path(__file__).resolve().parents[1] /
            "data/kit/stock-textures.json").read_text())
    sources = inventory["sha256"]
    cooks = inventory["cooked_sha256"]

    pins = {**sources, **cooks}
    receipts = HashReceipts(target / '.deploy-receipts.json')
    parents = {}
    # Remove only unpinned/invalid receipts; retained stock stamps keep their inode.
    for stamp in (target / "data/tgl").rglob("*.cookstamp"):
        if stamp.relative_to(target).as_posix()[:-10] not in pins:
            stamp.unlink()

    def reconcile(item):
        rel, expected = item
        path = contained_path(target, rel, parents=parents)
        stamp = Path(str(path) + '.cookstamp')
        if not path.is_file():
            stamp.unlink(missing_ok=True)
            return None
        accepted = [expected] if isinstance(expected, str) else expected
        digest = receipts.digest(path)
        if digest not in accepted:
            stamp.unlink(missing_ok=True)
            return None
        return path, stamp, digest

    with ThreadPoolExecutor(max_workers=max(1, min(jobs, int(Path("A:/Games/mc2-ops/JOBS").read_text())))) as pool:
        verified = [row for row in pool.map(reconcile, sorted(pins.items())) if row]
    missing = [row for row in verified if 'fnv:' + row[2] not in receipts.rows]
    if missing:
        for row, value in zip(missing, stock_fnv([row[0] for row in missing])):
            if receipts.digest(row[0], force=True) != row[2]:
                raise ValueError(f'stock texture changed during native hash: {row[0]}')
            receipts.rows['fnv:' + row[2]] = value
    for path, stamp, digest in verified:
        content = f"mc2-stock-v1 {receipts.rows['fnv:' + digest]}\n"
        if not stamp.is_file() or stamp.read_text(encoding='ascii', errors='replace') != content:
            pending = stamp.with_name(stamp.name + f'.{os.getpid()}.tmp')
            pending.write_text(content, encoding='ascii')
            os.replace(pending, stamp)
    receipts.save()
    count = sum(path.suffix.casefold() == '.ktx2' for path, _, _ in verified)
    print(f"[deploy_payload] stock texture receipts verified/backfilled: {count}", flush=True)
    return count


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


class HashReceipts:
    """Private, disposable hash cache; metadata drift forces byte verification."""
    def __init__(self, path):
        self.path = Path(path)
        try:
            self.rows = json.loads(self.path.read_text())
            if not isinstance(self.rows, dict):
                self.rows = {}
        except (OSError, ValueError):
            self.rows = {}

    @staticmethod
    def stamp(path):
        info = os.stat(path)
        stamp = [info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino]
        if os.name == 'nt':
            # Windows st_ctime is birth time. NTFS ChangeTime also catches writes
            # whose length and last-write timestamp were deliberately restored.
            import ctypes
            import msvcrt
            basic = (ctypes.c_longlong * 5)()
            with open(path, 'rb') as stream:
                handle = ctypes.c_void_p(msvcrt.get_osfhandle(stream.fileno()))
                if not ctypes.windll.kernel32.GetFileInformationByHandleEx(handle, 0, ctypes.byref(basic), 40):
                    raise ctypes.WinError()
            stamp.append(basic[3])
        return stamp

    def digest(self, path, *, force=False, hasher=None, variant=''):
        stamp = self.stamp(path)
        key = variant + ':' + os.path.abspath(path)
        old = self.rows.get(key)
        if not force and isinstance(old, list) and old[:-1] == stamp:
            return old[-1]
        value = (hasher or sha)(path)
        if self.stamp(path) != stamp:
            raise ValueError(f"file changed during hash verification: {path}")
        self.rows[key] = [*stamp, value]
        return value

    def save(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        pending = self.path.with_name(self.path.name + f'.{os.getpid()}.tmp')
        try:
            pending.write_text(json.dumps(self.rows, separators=(',', ':')))
            os.replace(pending, self.path)
        finally:
            pending.unlink(missing_ok=True)


def contained_path(root, rel, *, parents=None):
    """Require a canonical relative member and a contained resolved destination."""
    win = PureWindowsPath(rel)
    if (not rel or win.drive or win.root or "\\" in rel
            or any(p in ("", ".", "..") for p in rel.split("/"))):
        raise ValueError(f"unsafe kit path: {rel}")
    root = Path(root).resolve() if parents is None else Path(root)
    path = root / rel
    if parents is None:
        path = path.resolve()
    else:
        parent = path.parent
        if parent not in parents:
            parents[parent] = parent.resolve()
        path = parents[parent] / path.name
        if path.is_symlink():
            path = path.resolve()
    if not path.is_relative_to(root):
        raise ValueError(f"unsafe kit path: {rel}")
    return path


def checked_files(root, rows, receipts=None):
    """Reject absent, altered and unsafe package members before copying any bytes."""
    root = Path(root).resolve()
    seen = set()
    folded = set()
    parents = {}
    for row in rows:
        rel = row["path"]
        path = contained_path(root, rel, parents=parents)
        if rel.casefold() in folded:
            raise ValueError(f"unsafe/duplicate kit path: {rel}")
        seen.add(rel)
        folded.add(rel.casefold())
        if (not path.is_file() or path.stat().st_size != row["bytes"]
                or (receipts.digest(path) if receipts else sha(path)) != row["sha256"]):
            raise ValueError(f"missing/mismatched pinned kit file: {path}")
    return seen


def payload(src_root, manifest_path=MANIFEST, kit_rel=KIT_REL, receipts=None):
    manifest = json.loads((Path(src_root) / manifest_path).read_text(encoding="utf-8"))
    if manifest["schema"] != 1 or not manifest["assets"] or not manifest["files"]:
        raise ValueError("invalid pinned kit manifest")
    store = Path(os.environ.get("MC2_KIT_STORE", ref_path(manifest["store"])) if manifest_path == MANIFEST
                 else ref_path(manifest["store"]))
    checked_files(store, manifest["files"], receipts)
    models = {p["path"] for p in manifest["files"] if p["path"].endswith(".glb")}
    expected = {f"data/model_overrides/{a['id']}/pristine_intact_lod{i}.glb"
                for a in manifest["assets"] for i in range(3)}
    if models != expected:
        raise ValueError("pinned kit asset list differs from its GLB payload")
    return manifest, [(str(store / r["path"]), (kit_rel / r["path"]).as_posix(), "kit")
                      for r in manifest["files"]]


def build_importer(src_root):
    """Ninja checks the real source/header closure even when an old exe exists."""
    import configure_ninja as cn
    from bootstrap_worktree_build import find_cmake
    root = Path(src_root).resolve()
    tree = root / "build-cc-tgdump"
    cmake = find_cmake()
    jobs = Path("A:/Games/mc2-ops/JOBS").read_text().strip()
    commands = []
    if not (tree / "build.ninja").is_file():
        # MC2_COMPILE_CACHE=0 is configure_ninja's opt-out (a host without the shared sccache: the laptop gate)
        flags, env = cn.cache_cmake_args(str(root)) if os.environ.get("MC2_COMPILE_CACHE") != "0" else ([], None)
        commands.append(([cmake, "-S", str(root / "tools/tg_import_dump"), "-B", str(tree),
                          "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={cn.NINJA}",
                          "-DCMAKE_BUILD_TYPE=RelWithDebInfo", *flags], env))
    commands.append(([cmake, "--build", str(tree), "--config", "RelWithDebInfo",
                      "--target", "tg_import_dump", "--parallel", jobs], None))
    for command, env in commands:
        if cn.under_vcvars(subprocess.list2cmdline(command), env):
            raise ValueError("pinned kit importer build failed")
    tool = tree / "tg_import_dump.exe"
    if not tool.is_file():
        raise ValueError(f"kit importer build produced no executable: {tool}")
    return tool


def finish(target_dir, manifest, tool, cooker=subprocess.run, *, kit_rel=KIT_REL):
    """Verify installed source, cook native recipes and verify the cache receipt."""
    target = Path(target_dir).resolve()
    kit = target / kit_rel
    hashes = HashReceipts(target / '.deploy-receipts.json')
    paths = checked_files(kit, manifest["files"], hashes)
    extra = {p.relative_to(kit).as_posix() for p in kit.rglob("*") if p.is_file()}
    extra -= paths | {".modindex-cache"}
    if extra:
        files = ", ".join(str(kit / rel) for rel in sorted(extra))
        raise ValueError(f"unpinned kit files in install: {files}; "
                         "safe to delete if not in the kit manifest")
    models = sorted(p for p in paths if p.endswith(".glb"))
    # Identical small-prop LODs share a content-addressed native recipe.
    unique_models = len({r["sha256"] for r in manifest["files"] if r["path"].endswith(".glb")})
    identity = hashlib.sha256((hashes.digest(tool) + json.dumps(manifest, sort_keys=True)
                               + json.dumps(IMPORT_ARGS)).encode()).hexdigest()
    cache = target / "cache/content-model"
    cache.mkdir(parents=True, exist_ok=True)
    receipt = cache / ("industrial-cook.json" if kit_rel == KIT_REL else "vehicle-cook.json")
    shared = target.parent / '.cooked-kit' / identity
    shared_receipt = shared / receipt.name

    def publish(previous):
        # Private copies prevent later in-place lane writes from touching the store.
        shared.mkdir(parents=True, exist_ok=True)
        for row in previous['files']:
            dst = shared / row['path']
            pending = dst.with_name(dst.name + f'.{os.getpid()}.tmp')
            try:
                shutil.copyfile(cache / row['path'], pending)
                if sha(pending) != row['sha256']:
                    raise ValueError(f'shared recipe copy mismatch: {dst}')
                os.replace(pending, dst)
            finally:
                pending.unlink(missing_ok=True)
        pending = shared_receipt.with_name(shared_receipt.name + f'.{os.getpid()}.tmp')
        pending.write_text(json.dumps(previous, indent=2) + '\n')
        os.replace(pending, shared_receipt)

    for candidate in (receipt, shared_receipt):
        try:
            previous = json.loads(candidate.read_text())
            recipes = previous["recipes"]
            rows = previous["files"]
            if not (previous["identity"] == identity and isinstance(recipes, dict) and set(recipes) == set(models)
                    and len(rows) == unique_models
                    and set(recipes.values()) == {r["path"] for r in rows}
                    and all(re.fullmatch(r'model-[0-9a-f]{16}\.mci', row['path']) for row in rows)):
                continue
            checked_files(candidate.parent, rows, hashes)
            if candidate == shared_receipt:
                for row in rows:
                    dst = cache / row['path']
                    pending = dst.with_name(dst.name + f'.{os.getpid()}.tmp')
                    try:
                        os.link(shared / row['path'], pending)
                        os.replace(pending, dst)
                    finally:
                        pending.unlink(missing_ok=True)
                receipt.write_text(json.dumps(previous, indent=2) + '\n')
            elif not shared_receipt.is_file():
                publish(previous)
            hashes.save()
            print(f"[deploy_payload] pinned industrial kit verified: {len(models)} GLBs cached; cook skipped")
            return
        except (OSError, ValueError, KeyError, TypeError):
            pass
    # Cookers may overwrite in place; detach every shared recipe before invoking one.
    for path in cache.glob('model-*.mci'):
        if path.stat().st_nlink > 1:
            pending = path.with_name(path.name + f'.{os.getpid()}.tmp')
            try:
                shutil.copyfile(path, pending)
                os.replace(pending, path)
            finally:
                pending.unlink(missing_ok=True)
    # Record the importer's source-to-recipe mapping with the verified inventory.
    recipes = {}
    recipes_by_sha = {}
    model_shas = {r["path"]: r["sha256"] for r in manifest["files"] if r["path"].endswith(".glb")}
    outputs = set()
    unique = {}
    for rel in models:
        unique.setdefault(model_shas[rel], rel)

    def cook_model(item):
        digest, rel = item
        result = cooker([str(tool), (kit_rel / rel).as_posix(), *IMPORT_ARGS],
                        cwd=target, capture_output=True, text=True, timeout=120, env=dict(os.environ, OMP_NUM_THREADS="1"),
                        **console_child.child_kwargs(redirected=True))
        if result.returncode:
            raise ValueError(f"industrial native cook failed: {rel}: {result.stderr[-300:]}")
        keys = re.findall(r"\[CONTENT_COOK\].*?key=([0-9a-f]{16})", result.stdout + result.stderr)
        if len(keys) != 1:
            raise ValueError(f"industrial importer did not report one native recipe: {rel}")
        return digest, f"model-{keys[0]}.mci"

    jobs = max(1, int(Path("A:/Games/mc2-ops/JOBS").read_text()))
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        recipes_by_sha.update(pool.map(cook_model, unique.items()))
    for rel in models:
        recipes[rel] = recipes_by_sha[model_shas[rel]]
        outputs.add(cache / recipes[rel])
    outputs = sorted(outputs)
    if len(outputs) != unique_models or any(not p.is_file() or p.stat().st_size == 0 for p in outputs):
        raise ValueError(f"industrial cook incomplete: {len(outputs)} recipes for {unique_models} distinct GLBs")
    rows = [dict(path=p.name, bytes=p.stat().st_size, sha256=sha(p)) for p in outputs]
    checked_files(cache, rows)
    checked_files(kit, manifest["files"])
    previous = dict(identity=identity, recipes=recipes, files=rows)
    receipt.write_text(json.dumps(previous, indent=2) + "\n")
    publish(previous)
    hashes.save()
    print(f"[deploy_payload] pinned industrial kit verified: {len(models)} GLBs cooked")
