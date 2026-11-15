"""Single-player startup must not expose a network listener."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_no_owned_wildcard_binds():
    # Multiplayer is compiled out in this tree. Any future wildcard bind needs
    # an explicit multiplayer-start contract before it can be allowed here.
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


def test_mp_loopback_requires_both_desync_reports_at_the_first_bad_tick(tmp_path):
    import pytest
    from run_mp_loopback_proof import compare_traces
    host, client = tmp_path / "host", tmp_path / "client"
    for index, directory in enumerate((host, client)):
        directory.mkdir()
        (directory / "stderr.log").write_text("[MP_DESYNC] tick=2 trace=session.trace\n")
        (directory / "session.trace").write_text(f"C 1 00\nH 1 a b c d e\nH 2 {index} b c d e\n")
    assert compare_traces(host, client, 2, forced=2)["first_divergent_tick"] == 2
    with pytest.raises(ValueError, match="first divergent"):
        compare_traces(host, client, 2)
    (host / "stderr.log").write_text("")
    with pytest.raises(ValueError, match="missing desync"):
        compare_traces(host, client, 2, forced=2)
