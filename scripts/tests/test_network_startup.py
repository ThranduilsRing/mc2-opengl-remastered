"""Single-player startup must not expose a network listener."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_no_owned_wildcard_binds():
    # Co-op binds only its explicitly selected numeric address after an opt-in.
    # Single-player startup must retain its listener-free contract.
    offenders = []
    for folder in ("GameOS", "code", "mclib", "RenderCore"):
        for path in (ROOT / folder).rglob("*"):
            if path.suffix not in {".cpp", ".h", ".c", ".hpp"}:
                continue
            if re.search(r"\b(INADDR_ANY|in6addr_any|AI_PASSIVE)\b", path.read_text(encoding="utf-8", errors="replace")):
                offenders.append(str(path.relative_to(ROOT)))
    assert not offenders, offenders


def test_tracy_is_opt_in_and_loopback_only():
    cmake = (ROOT / "GameOS/gameos/CMakeLists.txt").read_text()
    assert re.search(r'set_source_files_properties\([^)]*tracy/TracyClient.cpp\s+PROPERTIES COMPILE_DEFINITIONS "TRACY_ONLY_LOCALHOST;TRACY_NO_BROADCAST"\)', cmake)
    src = (ROOT / "3rdparty/tracy/client/TracyProfiler.cpp").read_text()
    assert 'GetEnvVar( "TRACY_PORT" )' in src
    assert "if( m_userPort != 0 && !dataPortSearch )" in src
    assert "else if( m_userPort != 0 )" in src
    shell = (ROOT / "GameOS/gameos/gos_dev_shell.cpp").read_text()
    assert "htonl(INADDR_LOOPBACK)" in shell
    assert "if (!shellEnabled()) return false;" in shell


# Trigger paths: scripts/run_mp_loopback_proof.py, code/net/, tactical_order_queue.*, mission_update.cpp.
def test_mp_loopback_rejects_missing_hashes_and_unequal_commands(tmp_path):
    import pytest
    from run_mp_loopback_proof import compare_traces
    host, client = tmp_path / "host", tmp_path / "client"
    for directory in (host, client):
        directory.mkdir()
        (directory / "stderr.log").write_text("")
        (directory / "session.trace").write_text("C 1 00\nH 1 a b c d e\n")
    assert compare_traces(host, client, 1)["ticks"] == 1
    with pytest.raises(ValueError, match="incomplete"):
        compare_traces(host, client, 2)
    (client / "session.trace").write_text("C 1 01\nH 1 a b c d e\n")
    with pytest.raises(ValueError, match="command traces"):
        compare_traces(host, client, 1)
    (client / "session.trace").write_text("C 1 00\nH 2 a b c d e\n")
    with pytest.raises(ValueError, match="malformed hash"):
        compare_traces(host, client, 1)


def test_mp_loopback_uses_the_runner_environment_boundary(tmp_path, monkeypatch):
    from contextlib import ExitStack
    import run_mp_loopback_proof as proof
    monkeypatch.setenv("MC2_HOST_CORES_PER_SLOT", "2")
    captured = {}
    monkeypatch.setattr(proof.nofocus, "spawn", lambda argv, **kw: captured.update(kw))
    with ExitStack() as logs:
        proof.peer_start(str(tmp_path), str(tmp_path), 0, 23460, 64, 120, False, logs)
    assert "MC2_HOST_CORES_PER_SLOT" not in captured["env"]
    assert captured["env"]["MC2_CONFIG_STRICT"] == "1"


def test_mp_loopback_requires_both_desync_reports_at_the_first_bad_tick(tmp_path):
    import pytest
    from run_mp_loopback_proof import compare_traces
    host, client = tmp_path / "host", tmp_path / "client"
    for index, directory in enumerate((host, client)):
        directory.mkdir()
        (directory / "stderr.log").write_text("[MP_DESYNC] tick=2 trace=session.trace\n")
        (directory / "session.trace").write_text(f"C 1 00\nH 1 a b c d e\nH 2 {index} b c d e\n[MP_DESYNC] tick=2 trace=session.trace\n")
    assert compare_traces(host, client, 2, forced=2)["first_divergent_tick"] == 2
    original = (host / "session.trace").read_text()
    (host / "session.trace").write_text(original + "R 1 100 7 Permission(2)\n")
    with pytest.raises(ValueError, match="command traces differ"):
        compare_traces(host, client, 2, forced=2)
    (host / "session.trace").write_text(original)
    with pytest.raises(ValueError, match="first divergent"):
        compare_traces(host, client, 2)
    (host / "session.trace").write_text("C 1 00\nH 1 a b c d e\nH 2 0 b c d e\n")
    with pytest.raises(ValueError, match="missing desync"):
        compare_traces(host, client, 2, forced=2)


def test_mp_fixture_keeps_both_lances_south_of_airfield():
    from run_mp_loopback_proof import fixture_orders
    for slot, movers in ((0, {0}), (1, {1, 2})):
        rows = [line.split() for line in fixture_orders(slot, 18064, 18064)]
        moves = [row for row in rows if row[2] == "move"]
        assert {int(row[1]) for row in moves} == movers
        assert int(moves[-1][0]) == 18000
        # Stock mc2_01 objective 0: center (3136,-789.333), radius 256.
        assert all(2400 <= float(row[3]) <= 2550 and -2200 <= float(row[4]) <= -2000 for row in moves)
        assert all((float(row[3]) - 3136)**2 + (float(row[4]) + 789.333)**2 > 256**2 for row in moves)
        assert [row for row in rows if row[2] == "desync"] == ([["18064", "0", "desync"]] if slot else [])
