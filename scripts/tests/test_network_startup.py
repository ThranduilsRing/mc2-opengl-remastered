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
