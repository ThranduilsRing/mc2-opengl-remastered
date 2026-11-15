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


def test_mp_shim_loss_delay_jitter_and_stall():
    from run_mp_loopback_proof import ClientShim
    shim = ClientShim(2, 50, 20)
    for i in range(1000):
        shim.enqueue(bytes(str(i), "ascii"), ("192.168.1.2", 23460), 1)
    assert 0 < shim.dropped < 100
    assert not list(shim.ready(1.029))
    assert 30 <= shim.minimum <= shim.maximum <= 70
    shim.stall_until = 4
    assert not list(shim.ready(3.999))
    packets = list(shim.ready(4))
    assert len(packets) == 1000 - shim.dropped == shim.forwarded
    assert len({packet for packet, _ in packets}) == len(packets)
    assert not shim.pending


def test_mp_shim_zero_faults_passes_every_packet():
    from run_mp_loopback_proof import ClientShim
    shim = ClientShim(0, 0, 0)
    shim.enqueue(b"one", ("192.168.1.2", 23460), 1)
    shim.enqueue(b"two", ("192.168.1.2", 23460), 1)
    assert [packet for packet, _ in shim.ready(1)] == [b"one", b"two"]
    assert shim.receipt()["dropped"] == 0


def test_mp_lan_arguments(tmp_path):
    import pytest
    from run_mp_loopback_proof import arguments
    base = ["--install", str(tmp_path), "--out", str(tmp_path / "proof")]
    lan = ["--address", "192.168.1.2", "--client-install", str(tmp_path / "client")]
    assert arguments(base + lan + ["--loss", "2", "--latency", "50", "--jitter", "20"]).loss == 2
    assert arguments(base + ["--kill-peer", "both"]).kill_peer == "both"
    for bad in (["--address", "0.0.0.0"], ["--address", "localhost"],
                ["--address", "192.168.1.2"], ["--loss", "100"], ["--loss", "nan"],
                ["--latency", "nan"], ["--jitter", "20"], ["--stall-ms", "-1"],
                ["--port", "65535"], ["--kill-peer", "host", "--kill-tick", "18000"],
                ["--stall-ms", "3000", "--stall-tick", "18000"], ["--timeout", "0"]):
        with pytest.raises(SystemExit):
            arguments(base + bad)


def test_mp_combat_routes_both_lances_and_rejects_unchanged_rng(tmp_path):
    import pytest
    from run_mp_loopback_proof import compare_traces, fixture_orders
    for slot, movers in ((0, {0}), (1, {1, 2})):
        moves = [row.split() for row in fixture_orders(slot, 18000, 0, combat=True) if " move " in row]
        assert {int(row[1]) for row in moves} == movers
        assert all(float(row[4]) == 1900 for row in moves)
    peers = [tmp_path / "host", tmp_path / "client"]
    for peer in peers:
        peer.mkdir()
        (peer / "session.trace").write_text("C 1 00\nH 1 a b c d e\nH 2 a b c d e\n")
    with pytest.raises(ValueError, match="RNG hash"):
        compare_traces(*peers, 2, combat=True)
    for peer in peers:
        (peer / "session.trace").write_text("C 1 00\nH 1 a b c d e\nH 2 a b c d f\n")
    assert compare_traces(*peers, 2, combat=True)["rng_states"] == 2
