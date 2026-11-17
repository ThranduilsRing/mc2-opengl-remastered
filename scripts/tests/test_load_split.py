from smoke_lib import cold_cache, logparse, report
from smoke_lib.gates import Verdict


def test_load_split_log_and_summary():
    text = """[LOAD_PHASES v1] total=10.0ms phase1=mission_ready:10.0ms
[LOAD_SCOPES v1] sim.example=8.0(x2)
[LOAD_SPLIT v1] total=10.001 io=2.0 decode=1.0 cook=3.0 upload=0.0 gpu_wait=0.0 sim=2.0 pump=0.0 other=2.001 worker_io=5.0 bytes_io=64 bytes_coverage=partial
[LOAD_TOP v1] sim.example=8.0(x2)
"""
    s = logparse.parse_log(text)
    assert s.load_total_ms == 10.001
    assert s.load_split["worker_io"] == 5.0
    assert s.load_split["bytes_io"] == 64
    row = report.Row("mc2_01", Verdict(True), s)
    result = report.render_json([row], tier="lite", profile="", timestamp="")
    assert result["rows"][0]["load_total_ms"] == 10.001
    assert result["rows"][0]["load_split"] == s.load_split


def test_load_split_absent_has_no_summary_fields():
    s = logparse.parse_log("[MISSION] phase=mission_ready")
    row = report.Row("mc2_01", Verdict(True), s)
    result = report.render_json([row], tier="lite", profile="", timestamp="")
    assert "load_split" not in result["rows"][0]
    assert "load_total_ms" not in result["rows"][0]


def test_cold_cache_linux_fadvise_and_re_eviction(tmp_path, monkeypatch):
    p = tmp_path / "asset.bin"
    p.write_bytes(b"data")
    monkeypatch.setattr(cold_cache.sys, "platform", "linux")
    monkeypatch.setattr(cold_cache, "PROBE_MIN_BYTES", 1)
    reads = iter([(4, .1), (4, .1), (4, 1.)])
    monkeypatch.setattr(cold_cache, "_read_all", lambda p: next(reads))
    calls = []
    monkeypatch.setattr(cold_cache.os, "fsync", lambda fd: calls.append("sync"))
    monkeypatch.setattr(cold_cache.os, "POSIX_FADV_DONTNEED", 4, raising=False)
    monkeypatch.setattr(cold_cache.os, "posix_fadvise", lambda fd, off, size, advice: calls.append((off, size, advice)), raising=False)
    r = cold_cache.make_cold(tmp_path)
    assert r.ok and r.extra["probe_slowed"]
    assert not r.extra["all_inputs_cold_proven"]
    assert calls == ["sync", (0, 0, 4), "sync", (0, 0, 4)]


def test_cold_cache_linux_failure_is_not_claimed(tmp_path, monkeypatch):
    (tmp_path / "asset.bin").write_bytes(b"data")
    monkeypatch.setattr(cold_cache.sys, "platform", "linux")
    monkeypatch.setattr(cold_cache, "PROBE_MIN_BYTES", 1)
    monkeypatch.setattr(cold_cache, "_read_all", lambda p: (4, .1))
    def fail(fd):
        raise OSError("fsync failed")
    monkeypatch.setattr(cold_cache.os, "fsync", fail)
    r = cold_cache.make_cold(tmp_path)
    assert not r.ok
    assert len(r.extra["errors"]) == 2
    assert "fsync failed" in r.extra["errors"][0]
