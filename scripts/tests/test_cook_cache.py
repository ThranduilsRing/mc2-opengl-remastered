"""Shared content-addressed cook cache: hit, miss, concurrent miss, corrupt entry (fake cooker)."""
import hashlib
import os
import shutil
import sys
import threading
import time
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import kit_payload as kit


@pytest.fixture
def world(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_COOK_CACHE", str(tmp_path / "cook-cache"))
    monkeypatch.delenv("MC2_KIT_STORE", raising=False)
    store = tmp_path / "store"
    rows = []
    for i in range(3):
        rel = f"data/model_overrides/t/pristine_intact_lod{i}.glb"
        p = store / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(rel.encode())
        rows.append(dict(path=rel, bytes=p.stat().st_size, sha256=kit.sha(p)))
    manifest = dict(schema=1, store=str(store), assets=[dict(id="t", source_bake_tag="r2")], files=rows)
    return tmp_path, store, manifest


def install(world, name):
    tmp, store, _ = world
    target = tmp / name
    shutil.copytree(store, target / kit.KIT_REL)
    return target


def cooker_for(target, calls, delay=0):
    def cook(command, **kwargs):
        if delay:
            threading.Event().wait(delay)
        calls.append(command)
        key = hashlib.sha256(command[1].encode()).hexdigest()[:16]
        cache = target / "cache/content-model"
        cache.mkdir(parents=True, exist_ok=True)
        (cache / f"model-{key}.mci").write_bytes(b"recipe " + key.encode())
        return SimpleNamespace(returncode=0, stdout="", stderr=f"[CONTENT_COOK] key={key} cache=miss")
    return cook


def exe(world, name, content):
    p = world[0] / name
    p.write_bytes(content)
    return p


def test_second_fresh_install_hits_with_a_different_importer_exe(world):
    manifest = world[2]
    calls = []
    a, b = install(world, "a"), install(world, "b")
    kit.finish(a, manifest, exe(world, "a.exe", b"worktree 1"), cooker_for(a, calls), importer_key="src")
    assert len(calls) == 3
    kit.finish(b, manifest, exe(world, "b.exe", b"worktree 2"), cooker_for(b, calls), importer_key="src")
    assert len(calls) == 3  # hit: same sources, different exe bytes, no cook
    names = lambda t: sorted(p.name for p in (t / "cache/content-model").glob("*.mci"))
    assert names(a) == names(b)


def test_source_key_change_misses(world):
    manifest = world[2]
    calls = []
    a, b = install(world, "a"), install(world, "b")
    tool = exe(world, "t.exe", b"x")
    kit.finish(a, manifest, tool, cooker_for(a, calls), importer_key="k1")
    kit.finish(b, manifest, tool, cooker_for(b, calls), importer_key="k2")
    assert len(calls) == 6


def test_corrupt_cache_entry_is_recooked_and_healed(world):
    manifest = world[2]
    calls = []
    a, b, c = (install(world, n) for n in "abc")
    tool = exe(world, "t.exe", b"x")
    kit.finish(a, manifest, tool, cooker_for(a, calls), importer_key="k")
    entry = next(kit.cook_cache_root().glob("models/*/*/model-*.mci"))
    entry.write_bytes(b"corrupted!")  # sha mismatch
    kit.finish(b, manifest, tool, cooker_for(b, calls), importer_key="k")
    assert len(calls) == 4  # only the corrupted model recooked
    kit.finish(c, manifest, tool, cooker_for(c, calls), importer_key="k")
    assert len(calls) == 4  # healed entry hits again


def test_cooking_in_an_install_does_not_mutate_the_cache(world):
    manifest = world[2]
    calls = []
    a, b = install(world, "a"), install(world, "b")
    tool = exe(world, "t.exe", b"x")
    kit.finish(a, manifest, tool, cooker_for(a, calls), importer_key="k")
    kit.finish(b, manifest, tool, cooker_for(b, calls), importer_key="k")
    cached = {p.name: p.read_bytes() for p in kit.cook_cache_root().glob("models/*/*/model-*.mci")}
    # a later cook in b (new key) must detach linked recipes before writing in place
    for p in (b / "cache/content-model").glob("model-*.mci"):
        p.write_bytes(b"installed copy mutated")
    kit.finish(b, manifest, tool, cooker_for(b, calls), importer_key="k2")
    after = {p.name: p.read_bytes() for p in kit.cook_cache_root().glob("models/*/*/model-*.mci")}
    assert cached == after


def test_concurrent_miss_cooks_once(world):
    manifest = world[2]
    calls, errors = [], []
    targets = [install(world, n) for n in "ab"]
    tool = exe(world, "t.exe", b"x")
    gate = threading.Barrier(2)

    def run(t):
        try:
            gate.wait()
            kit.finish(t, manifest, tool, cooker_for(t, calls, delay=0.3), importer_key="k")
        except Exception as exc:  # noqa: BLE001
            errors.append(exc)

    threads = [threading.Thread(target=run, args=(t,)) for t in targets]
    [t.start() for t in threads]
    [t.join() for t in threads]
    assert not errors
    assert len(calls) == 3  # the second deploy waited for the first cook and hit


def test_prune_keeps_recent_and_protected(tmp_path):
    root = tmp_path / "c"
    now = time.time()
    for i in range(12):
        d = root / f"k{i}"
        d.mkdir(parents=True)
        age = 0 if i == 11 else 30 * 86400 + i
        os.utime(d, (now - age, now - age))
    kit.prune_cook_cache(root, "k0")
    left = {d.name for d in root.iterdir()}
    assert "k11" in left and "k0" in left  # protected: fresh / current key
    assert len(left) < 12  # stale keys beyond the retention window are gone
