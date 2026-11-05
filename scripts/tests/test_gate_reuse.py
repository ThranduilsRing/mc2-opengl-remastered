"""Offline tests of the merge-gate reuse keys (scripts/gate_reuse.py): hit, a miss per input, corrupt cache = miss."""
import json
import sys
import types
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import gate_reuse as gr


@pytest.fixture
def world(tmp_path, monkeypatch):
    monkeypatch.setattr(gr, "CACHE_ROOT", tmp_path / "cache")
    root = tmp_path / "repo"
    (root / "tools").mkdir(parents=True)
    (root / "build64-ninja").mkdir()
    (root / "tools/mod_smoke.py").write_text("import helper\n")
    (root / "tools/helper.py").write_text("X = 1\n")
    (root / "tools/unrelated.py").write_text("Y = 1\n")
    (root / "tools/mod_known_failures.json").write_text("[]")
    (root / "tools/mod_warning_budget.json").write_text("{}")
    (root / "system.cfg").write_text("cfg")
    (root / "build64-ninja/mc2.exe").write_bytes(b"exe")
    (root / "engine.dll").write_bytes(b"dll")
    base = tmp_path / "base"
    base.mkdir()
    (base / "data.fst").write_bytes(b"stock")
    (base / "logs").mkdir()
    (base / "logs/x.log").write_text("noise")
    mods_dir = tmp_path / "mods"
    for name in ("A", "B"):
        (mods_dir / name).mkdir(parents=True)
        (mods_dir / name / "mod.json").write_text(name)
    fake = types.SimpleNamespace(
        BATTERY=(("A", "A", None), ("B", "B", "A")),
        DEFAULT_BASE_INSTALL=base, DEFAULT_INSTALL_ROOT=mods_dir,
        WARNING_BUDGET=root / "tools/mod_warning_budget.json", KNOWN_FAILURES=root / "tools/mod_known_failures.json",
        mod_layers=lambda r, f, p: ([r / p] if p else []) + [r / f],
        engine_payload_items=lambda b, b2: [(root / "engine.dll", "engine.dll")])
    command = ["py", "tools/mod_smoke.py", "--battery", "--window", "120", "--exe", str(root / "build64-ninja/mc2.exe"),
               "--build-dir", str(root / "build64-ninja"), "--install-root", str(mods_dir), "--base-install", str(base),
               "--stage-root", str(tmp_path / "stage")]
    return types.SimpleNamespace(root=root, base=base, mods=mods_dir, fake=fake, command=command, tmp=tmp_path)


def key(w, head="h1", command=None, env=None):
    return gr.mods_key(command or w.command, env or {}, w.root, head, cache=gr.ShaCache(w.tmp / "sha.json"), mods=w.fake)


def test_same_inputs_same_key_and_noise_ignored(world):
    k1, _ = key(world)
    (world.base / "logs/x.log").write_text("other noise")
    (world.base / "debug_state").mkdir()
    (world.base / "debug_state/s.json").write_text("{}")
    (world.root / "tools/unrelated.py").write_text("Y = 2\n")  # not in the mod_smoke import closure
    assert key(world)[0] == k1


@pytest.mark.parametrize("change", ["exe", "base", "layer", "second_layer", "engine", "code", "known", "budget", "cfg",
                                    "window", "env", "head"])
def test_each_input_changes_the_key(world, change):
    k1, _ = key(world)
    head, command, env = "h1", None, None
    if change == "exe":
        (world.root / "build64-ninja/mc2.exe").write_bytes(b"exe2")
    elif change == "base":
        (world.base / "data.fst").write_bytes(b"stock2")
    elif change == "layer":
        (world.mods / "A/mod.json").write_text("changed")
    elif change == "second_layer":
        (world.mods / "B/extra.txt").write_text("new file")
    elif change == "engine":
        (world.root / "engine.dll").write_bytes(b"dll2")
    elif change == "code":
        (world.root / "tools/helper.py").write_text("X = 2\n")
    elif change == "known":
        (world.root / "tools/mod_known_failures.json").write_text('[{"mod": "A"}]')
    elif change == "budget":
        (world.root / "tools/mod_warning_budget.json").write_text('{"a": 1}')
    elif change == "cfg":
        (world.root / "system.cfg").write_text("cfg2")
    elif change == "window":
        command = [("121" if a == "120" else a) for a in world.command]
    elif change == "env":
        env = {"MC2_SOMETHING": "1"}
    elif change == "head":
        head = "h2"
    assert key(world, head, command, env)[0] != k1


def test_missing_layer_is_an_error_not_a_hit(world):
    (world.mods / "B/mod.json").unlink()
    (world.mods / "B").rmdir()
    with pytest.raises(FileNotFoundError):
        key(world)


def test_hit_after_store_and_miss_before(world, capsys):
    k, parts = key(world)
    assert gr.lookup(k, parts) is None
    report = world.tmp / "battery.json"
    report.write_text(json.dumps([{"mod": "A"}, {"mod": "B"}]))
    gr.store(k, parts, report, 2)
    assert gr.lookup(k, parts)["key"] == k
    out = capsys.readouterr().out
    assert "mods MISS" in out and "mods HIT key=" + k[:16] in out
    (world.mods / "A/mod.json").write_text("changed")
    k2, parts2 = key(world)
    assert gr.lookup(k2, parts2) is None


@pytest.mark.parametrize("damage", ["entry_garbage", "report_edited", "report_missing", "entry_wrong_status", "too_few_rows"])
def test_corrupt_cache_is_a_miss(world, damage):
    k, parts = key(world)
    report = world.tmp / "battery.json"
    report.write_text(json.dumps([{"mod": "A"}, {"mod": "B"}]))
    gr.store(k, parts, report, 2)
    folder = gr.CACHE_ROOT / "mods" / k
    if damage == "entry_garbage":
        (folder / "entry.json").write_text("{not json")
    elif damage == "report_edited":
        (folder / "battery.json").write_text(json.dumps([{"mod": "A"}, {"mod": "B"}, {"mod": "C"}]))
    elif damage == "report_missing":
        (folder / "battery.json").unlink()
    elif damage == "entry_wrong_status":
        entry = json.loads((folder / "entry.json").read_text())
        entry["status"] = "FAIL"
        (folder / "entry.json").write_text(json.dumps(entry))
    elif damage == "too_few_rows":
        entry = json.loads((folder / "entry.json").read_text())
        entry["min_rows"] = 99
        (folder / "entry.json").write_text(json.dumps(entry))
    assert gr.lookup(k, parts) is None


def test_store_refuses_short_report(world):
    k, parts = key(world)
    report = world.tmp / "battery.json"
    report.write_text("[]")
    gr.store(k, parts, report, 2)
    assert gr.lookup(k, parts) is None


def test_corrupt_sha_cache_rehashes(world):
    k1, _ = key(world)
    (world.tmp / "sha.json").write_text("garbage")
    assert key(world)[0] == k1


def test_sha_cache_never_serves_a_changed_file(tmp_path):
    cache = gr.ShaCache(tmp_path / "c.json")
    f = tmp_path / "f"
    f.write_bytes(b"aaaa")
    first = cache.sha("l", "f", f)
    f.write_bytes(b"bbbbb")  # size changes: stat mismatch must rehash
    assert cache.sha("l", "f", f) != first


def test_dynamic_import_falls_back_to_hashing_all_code(world):
    (world.root / "tools/mod_smoke.py").write_text("import importlib\nimportlib.import_module('x')\n")
    k1, _ = key(world)
    (world.root / "tools/unrelated.py").write_text("Y = 3\n")
    assert key(world)[0] != k1


def test_stage_seal_verify_and_tamper_wipes(tmp_path):
    stage = tmp_path / "x-stage"
    (stage / "_cache/layers").mkdir(parents=True)
    (stage / "_cache/layers/a.bin").write_bytes(b"one")
    cache = gr.ShaCache(tmp_path / "s.json")
    assert gr.stage_verify(stage, cache) is False  # never sealed: invalid, wiped
    (stage / "_cache/layers").mkdir(parents=True)
    (stage / "_cache/layers/a.bin").write_bytes(b"one")
    gr.stage_seal(stage, cache)
    assert gr.stage_verify(stage, cache) is True
    (stage / "_cache/layers/a.bin").write_bytes(b"onetwo")
    assert gr.stage_verify(stage, cache) is False
    assert not (stage / "_cache").exists()
