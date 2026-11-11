"""Offline tag replay negative controls. No engine, archive or real leases."""
import contextlib
import json
from pathlib import Path
import sys
from types import SimpleNamespace

from PIL import Image
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import tag_replay as replay

SHA = "a" * 40
GPU = '[VK_PROFILE v1] verdict=ok device="fixture" api=1.3 instance_api=1.3 floor=1.2 required_missing=- wanted_missing=- items=- enabled=- extensions=-'
LOG = f"[BUILD_FINGERPRINT v1] sha={SHA} dirty=0 branch=test built=now src=fixture\n{GPU}\n"


@pytest.fixture
def bookmarks(tmp_path):
    path = tmp_path / "owner.json"
    replay.dump(path, {"mission": "mc2_01", "bookmarks": [
        {"name": name, "pos": [1, 2, 3], "rot": 4, "alt": 5, "proj": 6} for name in ("owner_a", "owner_b", "owner_c")]})
    return path


def payload(path, sha=SHA, dirty=0):
    path.mkdir()
    exe = path / "mc2.exe"
    exe.write_bytes(f"MC2FP1|{sha}|{dirty}|test|2026-11-11T12:00:00|/src|FP1END".encode())
    shader = path / "shaders/vulkan/a.spv"
    shader.parent.mkdir(parents=True)
    shader.write_bytes(b"spirv")
    (path / ".deployed_manifest.csv").write_text("manifest_version,v1,,,\nrelpath,sha256,bytes,src_commit\n"
        + "\n".join(f"{p.relative_to(path).as_posix()},{replay.bench.sha_file(p)},{p.stat().st_size},{SHA}" for p in (exe, shader)))
    return exe


def test_historical_binary_is_not_harness_head(tmp_path):
    exe = payload(tmp_path / "install")
    identity = replay.identity(exe, SHA)
    assert identity["payload"]["commit"] == SHA
    assert identity["binary"]["embedded_fingerprint"]["sha"] == SHA
    assert identity["shader_sha256"] and identity["payload_sha256"]


@pytest.mark.parametrize("sha,dirty", [("b" * 40, 0), (SHA, 1)])
def test_identity_refuses_wrong_or_dirty_binary(tmp_path, sha, dirty):
    with pytest.raises(ValueError, match="provenance"):
        replay.identity(payload(tmp_path / "install", sha, dirty), SHA)


def test_identity_refuses_missing_stamp_and_shader_substitution(tmp_path):
    exe = payload(tmp_path / "install")
    (exe.parent / "shaders/vulkan/a.spv").write_bytes(b"swapped")
    with pytest.raises(ValueError, match="manifest mismatch"):
        replay.identity(exe, SHA)
    exe.write_bytes(b"no stamp")
    with pytest.raises(ValueError, match="provenance"):
        replay.identity(exe, SHA)


def test_stage_plan_has_all_owner_poses_and_duplicate_corner(bookmarks, tmp_path):
    docs = replay.bookmark_inputs(bookmarks)
    stages = replay.plan(tmp_path / "mc2.exe", SHA, tmp_path / "work", docs)
    assert len(stages) == 14
    assert [s.mission for s in stages if s.kind == "bench"] == list(replay.MISSIONS)
    assert [m["name"] for m in docs["mc2_01"]["bookmarks"]] == ["owner_a", "owner_b", "owner_c", "corner"]
    for mission in replay.MISSIONS[1:]:
        marks = docs[mission]["bookmarks"]
        assert marks[0]["name"] != marks[1]["name"]
        assert {k: v for k, v in marks[0].items() if k != "name"} == {k: v for k, v in marks[1].items() if k != "name"}
    assert all(s.ref_commit == SHA for s in stages)
    assert all("--no-kill" in s.command for s in stages if s.kind == "capture")
    assert "--ref-commit" in next(s.command for s in stages if s.kind == "bench")
    assert "owner_c" in bookmarks.read_text() and "corner" not in bookmarks.read_text()


@pytest.mark.parametrize("name", ["../tag", "a/b", "-option", "a..b", "a\\b"])
def test_unsafe_tags_refused(name):
    with pytest.raises(ValueError):
        replay.tag_name(name)


def test_tag_resolution_uses_tag_namespace(monkeypatch):
    def check(cmd, **kw):
        assert cmd[-1] == "refs/tags/nifty-green-148-1fdbb7c6c^{commit}"
        return SHA + "\n"
    monkeypatch.setattr(replay.subprocess, "check_output", check)
    assert replay.resolve_tag("nifty-green-148-1fdbb7c6c") == SHA


@pytest.fixture
def fake_parity(monkeypatch, tmp_path):
    reference = tmp_path / "reference"
    (reference / "data").mkdir(parents=True)
    (reference / "data/concrete").write_bytes(b"new concrete")
    (reference / "stock.fst").write_bytes(b"archive")
    index = {"data/concrete": ("data/concrete", 12), "stock.fst": ("stock.fst", 7)}
    result = SimpleNamespace(ok=True, sampled=2, total=2)
    def compare(install, ref, sample, sample_bytes):
        assert sample == 2 and sample_bytes == 20
        return result
    api = SimpleNamespace(reference_index=lambda _: index, SKIP_SUFFIXES=(".cookstamp",), EXCLUDED=("backup/",),
                          compare=compare, verdict_line=lambda *a: "failed fixture")
    monkeypatch.setattr(replay, "parity_api", lambda: api)
    monkeypatch.setattr(replay, "V04", reference)
    return reference, result


def test_parity_full_hash_and_stable_reference(fake_parity, tmp_path):
    reference, _ = fake_parity
    checked = replay.parity(tmp_path / "install/mc2.exe", reference)
    assert checked["files"] == 2 and "all reference" in checked["method"]
    assert checked["reference_sha256"]
    (reference / "data/concrete").write_bytes(b"old concrete")
    with pytest.raises(ValueError, match="reference identity mismatch"):
        replay.parity(tmp_path / "install/mc2.exe", reference, checked["reference_sha256"])


def test_copied_reference_requires_pc_identity(fake_parity, monkeypatch, tmp_path):
    reference, _ = fake_parity
    checked = replay.parity(tmp_path / "install/mc2.exe", reference)
    monkeypatch.setattr(replay, "V04", tmp_path / "canonical")
    with pytest.raises(ValueError, match="copied v0.4"):
        replay.parity(tmp_path / "install/mc2.exe", reference)
    assert replay.parity(tmp_path / "install/mc2.exe", reference, checked["reference_sha256"])["status"] == "PASS"


def test_parity_never_skips_or_launders_partial_hashes(fake_parity, tmp_path):
    reference, result = fake_parity
    with pytest.raises(ValueError, match="unavailable"):
        replay.parity(tmp_path / "install/mc2.exe", tmp_path / "missing")
    result.ok = False
    with pytest.raises(ValueError, match="parity failed"):
        replay.parity(tmp_path / "install/mc2.exe", reference)
    result.ok, result.sampled = True, 1
    with pytest.raises(ValueError, match="incomplete"):
        replay.parity(tmp_path / "install/mc2.exe", reference)


def test_missing_parity_dependency_is_clear(monkeypatch):
    original = replay.importlib.import_module
    def missing(name):
        if name == "asset_parity":
            raise ImportError("fixture")
        return original(name)
    monkeypatch.setattr(replay.importlib, "import_module", missing)
    with pytest.raises(ValueError, match="install-asset-parity-1"):
        replay.parity_api()


def log_sim(path, count=600, gap=False):
    lines = [f"{replay.sim.PREFIX}{i + (1 if gap and i > 10 else 0)} t=0.000 n=2 graded=aa visual=bb strict=cc "
             "nobj=3 objects=dd nabl=1 abl=ee rng=1/2/3 animrng=4" for i in range(count)]
    path.write_text(LOG + "[DET_RNG] mission=mc2_01 reseed=0xC0FFEE\n" + "\n".join(lines))
    return path


def test_sim_hash_is_first_600_consecutive_observations(monkeypatch, tmp_path):
    monkeypatch.setattr(replay.sim, "data_fingerprint", lambda _: {"digest": "data", "problems": []})
    log = log_sim(tmp_path / "sim.log", 610)
    result = replay.sim_summary(log, tmp_path)
    assert result["count"] == 600 and result["ticks"] == [0, 599]
    assert result["chain_schema"] == 2 and result["seed"] == "0xC0FFEE"
    assert "no determinism" in result["claim"]


@pytest.mark.parametrize("count,gap", [(599, False), (600, True), (0, False)])
def test_short_or_gapped_sim_refused(tmp_path, count, gap):
    with pytest.raises(ValueError, match="SIMID"):
        replay.sim_summary(log_sim(tmp_path / "sim.log", count, gap), tmp_path)


def test_sim_stage_passes_historical_sha_and_exact_single_run(monkeypatch, tmp_path):
    log = log_sim(tmp_path / "sim.log")
    def run(mission, duration, out, **kw):
        assert mission == "mc2_03" and kw["runs"] == 1 and kw["ticks"] == 600
        assert kw["headless"] == "both" and kw["ref_commit"] == SHA
        return [str(log)]
    monkeypatch.setattr(replay.sim, "run_mission", run)
    monkeypatch.setattr(replay.sim, "data_fingerprint", lambda _: {"digest": "data", "problems": []})
    stage = replay.Stage("sim", "mc2_03", [], SHA)
    assert replay.sim_stage(stage, tmp_path / "mc2.exe", SHA, tmp_path / "out")["status"] == "PASS"


def test_host_before_install_and_recheck_after_wait(monkeypatch, tmp_path):
    events = []
    @contextlib.contextmanager
    def host(*a, **kw):
        assert kw["timeout_s"] == 1200
        events.append("host")
        yield
        events.append("host-release")
    monkeypatch.setattr(replay.host_lease, "exclusive_window", host)
    monkeypatch.setattr(replay.deploy_lease, "acquire_lease_waiting", lambda *a: events.append("install"))
    monkeypatch.setattr(replay.deploy_lease, "release_lease", lambda *a: events.append("install-release"))
    with replay.launch_window(tmp_path / "mc2.exe", lambda: events.append("identity")):
        events.append("child")
    assert events == ["host", "install", "identity", "child", "identity", "install-release", "host-release"]


def test_waited_identity_change_releases_install(monkeypatch, tmp_path):
    released = []
    monkeypatch.setattr(replay.host_lease, "exclusive_window", lambda *a, **k: contextlib.nullcontext())
    monkeypatch.setattr(replay.deploy_lease, "acquire_lease_waiting", lambda *a: None)
    monkeypatch.setattr(replay.deploy_lease, "release_lease", lambda *a: released.append(True))
    def changed():
        raise ValueError("changed during lease wait")
    with pytest.raises(ValueError, match="changed"):
        with replay.launch_window(tmp_path / "mc2.exe", changed, headless=True):
            pytest.fail("child must not launch")
    assert released == [True]


def test_headless_child_owns_folder_and_parent_rechecks(monkeypatch, tmp_path):
    held = []
    monkeypatch.setattr(replay.host_lease, "exclusive_window", lambda *a, **k: contextlib.nullcontext())
    monkeypatch.setattr(replay.deploy_lease, "acquire_lease_waiting", lambda *a: held.append(True))
    monkeypatch.setattr(replay.deploy_lease, "release_lease", lambda *a: held.pop())
    with replay.launch_window(tmp_path / "mc2.exe", lambda: None, headless=True):
        assert not held
    assert not held


@pytest.fixture
def mocked_run(monkeypatch, tmp_path, bookmarks):
    args = SimpleNamespace(tag="fixture-tag", out_root=tmp_path / "archive", scratch_root=tmp_path / "scratch",
        bookmarks=bookmarks, exe=tmp_path / "install/mc2.exe", asset_reference=tmp_path / "reference",
        reference_sha256=None, dry_run=False, pc_sweep=tmp_path / "no-pc-sweep")
    monkeypatch.setattr(replay, "resolve_tag", lambda _: SHA)
    monkeypatch.setattr(replay, "identity", lambda *a: {"binary": {"sha256": "exe", "embedded_fingerprint":
        {"present": True, "dirty": False, "sha": SHA}}, "payload": {"commit": SHA}})
    monkeypatch.setattr(replay, "parity", lambda *a: {"status": "PASS", "reference_sha256": "a" * 64})
    monkeypatch.setattr(replay, "host_snapshot", lambda: {"hostname": "fixture", "lease_count": "UNKNOWN", "host_jobs": "UNKNOWN"})
    monkeypatch.setattr(replay, "launch_window", lambda *a, **kw: contextlib.nullcontext())
    monkeypatch.setattr(replay.bench, "content_identity", lambda *a: {"sha256": "content"})
    monkeypatch.setattr(replay.bench.render_options_pin, "record", lambda *a: {"values": {"BaseQuality": 2}})
    def capture(stage, exe, sha, directory, doc):
        directory.mkdir(parents=True)
        for mark in doc["bookmarks"]:
            path = directory / f"{stage.mission}_{mark['name']}.png"
            Image.new("RGB", (3, 2), (1, 2, 3)).save(path)
            replay.dump(path.with_suffix(".json"), {"pose": mark})
        return {"status": "PASS", "actual_gpu": ["fixture"]}
    monkeypatch.setattr(replay, "capture_stage", capture)
    monkeypatch.setattr(replay.bench, "stock_run", lambda *a: {"actual_gpu": ["fixture"], "cpu_work": {
        "n": 100, "over_6_06_ms": 3, "over_8_33_ms": 1, "p99_ms": 7, "max_ms": 9}})
    monkeypatch.setattr(replay, "sim_stage", lambda *a: {"status": "PASS", "seed": "0xC0FFEE", "ticks": [0, 599],
        "count": 600, "chain_schema": 2, "chain": "simhash", "data_fingerprint": {"digest": "data"}})
    monkeypatch.setattr(replay, "zones_stage", lambda *a: {"status": "NOT RUN", "reason": "no fixture exporter"})
    monkeypatch.setattr(replay, "tracy_tools", lambda: (None, "no fixture exporter"))
    return args


def test_complete_bundle_publishes_only_into_absent_tag(mocked_run):
    args = mocked_run
    assert replay.run(args) == 0
    target = args.out_root / args.tag
    manifest = replay.validate_bundle(target)
    assert manifest["resolved_sha"] == SHA and manifest["host"]["actual_gpu"] == ["fixture"]
    assert len(list((target / "png").glob("*.png"))) == 7
    assert not list((target / "png").glob("*duplicate*"))
    assert "no determinism" in (target / "simhash.txt").read_text()
    before = replay.artifact_hashes(target)
    with pytest.raises(ValueError, match="immutable"):
        replay.run(args)
    assert replay.artifact_hashes(target) == before


def test_failed_child_retains_diagnostics_without_archive(mocked_run, monkeypatch):
    args = mocked_run
    monkeypatch.setattr(replay, "capture_stage", lambda *a: (_ for _ in ()).throw(RuntimeError("child failed fixture")))
    assert replay.run(args) == 1
    assert not (args.out_root / args.tag).exists()
    progress = next(args.scratch_root.glob("*/incomplete.json"))
    doc = json.loads(progress.read_text())
    assert doc["status"] == "incomplete" and "child failed" in doc["error"]


def test_dry_run_has_no_launch_or_write(mocked_run, monkeypatch, capsys):
    args = mocked_run
    args.dry_run = True
    monkeypatch.setattr(replay, "capture_stage", lambda *a: pytest.fail("dry run launched"))
    assert replay.run(args) == 0
    assert not args.scratch_root.exists() and not args.out_root.exists()
    text = capsys.readouterr().out
    assert "generated_corner_inputs" in text and "owner_c" in text and "ref_commit" in text
    assert "frames.json" in text and SHA in text and "MC2_VSYNC" in text


def test_comparison_decodes_pixels_and_prints_deltas(mocked_run, capsys):
    args = mocked_run
    assert replay.run(args) == 0
    args.tag = "other-tag"
    assert replay.run(args) == 0
    target = args.out_root / args.tag
    png = target / "png/mc2_01_corner.png"
    # Different container encoding, identical pixels must compare equal.
    image = Image.open(png).copy()
    image.save(png, compress_level=0)
    manifest = json.loads((target / "manifest.json").read_text())
    manifest["artifacts"] = replay.artifact_hashes(target)
    replay.dump(target / "manifest.json", manifest)
    capsys.readouterr()
    assert replay.compare(args.out_root, ["fixture-tag", "other-tag"]) == 0
    result = json.loads(capsys.readouterr().out)
    assert result["metrics"] == "COMPATIBLE" and result["frame_deltas"]["mc2_01"]["n"] == 0
    assert all(p["changed_pixels"] == 0 for p in result["png_pairs"])


@pytest.mark.parametrize("mismatch", ["settings", "content", "host"])
def test_comparison_reports_incompatible_metrics(mocked_run, capsys, mismatch):
    args = mocked_run
    assert replay.run(args) == 0
    args.tag = "other-tag"
    assert replay.run(args) == 0
    path = args.out_root / args.tag / "manifest.json"
    manifest = json.loads(path.read_text())
    manifest[mismatch][next(iter(manifest[mismatch]))] = "changed"
    replay.dump(path, manifest)
    capsys.readouterr()
    assert replay.compare(args.out_root, ["fixture-tag", "other-tag"]) == 1
    result = json.loads(capsys.readouterr().out)
    assert result["metrics"] == "INCOMPATIBLE" and result["frame_deltas"] == {}
    assert result["png_pairs"]


def test_missing_png_and_tampered_raw_log_are_rejected(mocked_run, capsys):
    args = mocked_run
    assert replay.run(args) == 0
    args.tag = "other-tag"
    assert replay.run(args) == 0
    (args.out_root / args.tag / "png/mc2_01_corner.png").unlink()
    capsys.readouterr()
    assert replay.compare(args.out_root, ["fixture-tag", "other-tag"]) == 1
    report = json.loads(capsys.readouterr().out)
    assert any(p["status"] == "MISSING/INVALID" for p in report["png_pairs"])
    with pytest.raises(ValueError, match="hashes"):
        replay.validate_bundle(args.out_root / args.tag)


def test_capture_stage_refuses_missing_duplicate_png(monkeypatch, tmp_path, bookmarks):
    doc = replay.bookmark_inputs(bookmarks)["mc2_10"]
    stage = replay.plan(tmp_path / "mc2.exe", SHA, tmp_path, {"mc2_10": doc})[0]
    directory = tmp_path / "capture"
    def child():
        (directory / "mc2_10_capture.log").write_text(LOG)
        return 0
    monkeypatch.setattr(replay.visual(), "main", child)
    with pytest.raises(ValueError, match="missing/invalid PNG"):
        replay.capture_stage(stage, tmp_path / "mc2.exe", SHA, directory, doc)


def test_unknown_counts_are_not_zero(monkeypatch):
    import psutil
    monkeypatch.setattr(psutil, "process_iter", lambda *a: (_ for _ in ()).throw(OSError("unavailable")))
    monkeypatch.delenv("MC2_REPLAY_JOB_DIR", raising=False)
    monkeypatch.setattr(replay.host_lease, "status", lambda: (_ for _ in ()).throw(OSError("unavailable")))
    row = replay.host_snapshot()
    assert row["host_jobs"] == row["lease_count"] == row["other_game_pids"] == "UNKNOWN"
    assert row["timestamp_unix"] and row["pid_scope"]


def test_tracy_missing_exporter_is_not_run(monkeypatch, tmp_path):
    monkeypatch.setattr(replay, "tracy_tools", lambda: (None, "missing exporter"))
    row = replay.zones_stage(replay.Stage("zones", "mc2_01", [], SHA), tmp_path / "mc2.exe", SHA, tmp_path)
    assert row == {"status": "NOT RUN", "reason": "missing exporter"}


def test_tracy_top_ten_use_same_run_structured_self_time(monkeypatch, tmp_path):
    work = tmp_path / "work"
    stage = next(s for s in replay.plan(tmp_path / "mc2.exe", SHA, work, {}) if s.kind == "zones")
    directory = work / "raw/zones/mc2_01"
    def trace(install, arm, mission, seconds, path, **kw):
        assert arm == "vk" and mission == "mc2_01" and seconds == 20
        assert kw["fixed_timestep"] is False and kw["fps_cap"] == 0
        assert "MC2_VSYNC=0" in kw["extra_env"]
        path.write_bytes(b"fixture trace")
        path.with_suffix(".game.log").write_text(LOG)
        return 0
    monkeypatch.setattr(replay, "tracy_tools", lambda: (SimpleNamespace(capture_trace=trace), None))
    monkeypatch.setattr(replay.bench, "summarize_stock", lambda *a: {"acknowledged_pose": replay.bench.CORNER})
    def export():
        assert sys.argv == stage.post_command[1:]
        replay.dump(directory / "zones.json", {"corner.tracy": {"steady_play": {
            "frames": 100, "start_ns": 1, "end_ns": 1000, "frame_latency": {"mean_ms": 2},
            "zones": [{"name": f"zone{i}", "thread": "1", "src_file": "fixture.cpp", "src_line": i,
                       "self_total_ms": i} for i in range(15)]}}})
    original_import = replay.importlib.import_module
    monkeypatch.setattr(replay.importlib, "import_module", lambda name: SimpleNamespace(main=export)
                        if name == "tracy_table" else original_import(name))
    result = replay.zones_stage(stage, tmp_path / "mc2.exe", SHA, directory)
    assert [z["self_total_ms"] for z in result["top_ten"]] == list(range(14, 4, -1))
    assert result["attribution"]["sha"] == SHA
    assert result["attribution"]["trace_sha256"] == replay.bench.sha_file(directory / "corner.tracy")
    assert result["actual_gpu"] == ["fixture"]


def test_private_lease_does_not_claim_no_other_games(monkeypatch):
    import psutil
    monkeypatch.setattr(replay.host_lease, "status", lambda: {"slots": [], "engines": [], "private": True, "path": "fixture"})
    monkeypatch.setattr(psutil, "process_iter", lambda *a: [SimpleNamespace(info={"pid": 123, "name": "mc2.exe", "exe": "other/mc2.exe"})])
    result = replay.host_snapshot()
    assert result["other_game_pids"][0]["pid"] == 123
    assert result["lease_count"] == 0 and "private=True" in result["lease_scope"]


def test_replay_scrubs_features_but_preserves_gpu_choice(monkeypatch, tmp_path):
    monkeypatch.setenv("MC2_VK_DEVICE", "NVIDIA")
    monkeypatch.setenv("MC2_RENDER_SCALE", "0.25")
    with replay.replay_environment(tmp_path):
        assert replay.os.environ["MC2_VK_DEVICE"] == "NVIDIA"
        assert "MC2_RENDER_SCALE" not in replay.os.environ
    assert replay.os.environ["MC2_RENDER_SCALE"] == "0.25"


def test_bundle_rejects_invalid_threshold_counts(mocked_run):
    assert replay.run(mocked_run) == 0
    target = mocked_run.out_root / mocked_run.tag
    manifest = json.loads((target / "manifest.json").read_text())
    manifest["stages"]["bench/mc2_01"]["cpu_work"]["over_6_06_ms"] = 101
    replay.dump(target / "manifest.json", manifest)
    with pytest.raises(ValueError, match="threshold count"):
        replay.validate_bundle(target)
