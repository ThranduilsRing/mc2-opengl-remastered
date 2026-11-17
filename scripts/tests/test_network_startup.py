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


def test_lobby_proof_enters_via_widgets_and_keeps_cli_default(tmp_path, monkeypatch):
    from contextlib import ExitStack
    import run_mp_loopback_proof as proof
    captured = {}
    monkeypatch.setattr(proof.nofocus, "spawn", lambda argv, **kw: captured.update(argv=argv, **kw))
    for lobby in (False, True):
        with ExitStack() as logs:
            proof.peer_start(str(tmp_path), str(tmp_path), 0, 23460, 64, 120, True, logs, lobby=lobby)
        assert ("--host" in captured["argv"]) != lobby
        assert ("MC2_INPUT_SCRIPT" in captured["env"]) == lobby
    assert captured["env"]["MC2_BOOT_TO_SCREEN"] == "mainmenu"
    assert captured["env"]["MC2_DIAG_TAGS"] == "COOP_UI"
    assert (tmp_path / "lobby.script").read_text() == "wait 180\n"


def test_lobby_multiplayer_hit_region_uses_defs_display_dimensions():
    # The retained 800x600 Environment mode is not the 1920x1080 defs rectangle's space.
    source = (ROOT / "code/mainmenu.cpp").read_text()
    region = source[source.index('getButton(MM_MSG_MULTIPLAYER);', source.index('void MainMenu::render()')):]
    region = region[:region.index('\n\t}')]
    dimensions = re.search(r'GuiRuntime::GetDisplaySize\((\w+), (\w+)\)', region)
    assert dimensions, "physical defs rectangles need actual display dimensions"
    assert re.search(r'coopProofWidget\("Multiplayer",[^;]*, ' + dimensions[1] + ', ' + dimensions[2] + r'\);', region)


def test_lobby_driver_waits_for_listening_and_enabled_controls(tmp_path):
    from run_mp_loopback_proof import LobbyDriver
    peers = [tmp_path / "host", tmp_path / "client"]
    for root in peers:
        root.mkdir()
        (root / "lobby.script").write_text("wait 180\n")
        (root / "stderr.log").write_text("[COOP_UI] Multiplayer 400 300\n[COOP_UI] Host 260 250\n[COOP_UI] Join 310 250\n")
    driver = LobbyDriver(peers)
    driver.pump()
    driver.pump()
    assert driver.steps == [2, 1]  # Join waits until the host finishes identity work.
    driver.pump()
    assert driver.steps == [2, 1]
    with (peers[0] / "stderr.log").open("a") as log:
        log.write("[COOP_UI] Listening 0 0\n[COOP_UI] Ready 260 300\n")
    driver.pump()
    assert driver.steps == [3, 2]
    driver.pump()
    assert driver.steps == [3, 2]  # Ready and Start have not been exposed yet.
    with (peers[1] / "stderr.log").open("a") as log:
        log.write("[COOP_UI] Ready 260 300\n")
    with (peers[0] / "stderr.log").open("a") as log:
        log.write("[COOP_UI] Start 260 330\n")
    driver.pump()
    driver.pump()
    assert driver.steps == [4, 3]
    assert [row["control"] for row in driver.actions if row["slot"] == 0] == ["Multiplayer", "Host", "Ready", "Start"]
    assert [row["control"] for row in driver.actions if row["slot"] == 1] == ["Multiplayer", "Join", "Ready"]
    assert "click 310.0 250.0" in (peers[1] / "lobby.script").read_text()
    assert "shot " in (peers[0] / "lobby.script").read_text()


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
    args = arguments(base + ["--role", "join", "--endpoint", "192.168.1.2:23460", "--compare-only"])
    assert (args.role, args.address, args.port, args.compare_only) == ("join", "192.168.1.2", 23460, True)
    for bad in (["--address", "0.0.0.0"], ["--address", "localhost"],
                ["--address", "192.168.1.2"], ["--loss", "100"], ["--loss", "nan"],
                ["--latency", "nan"], ["--jitter", "20"], ["--stall-ms", "-1"],
                ["--port", "65535"], ["--kill-peer", "host", "--kill-tick", "18000"],
                ["--stall-ms", "3000", "--stall-tick", "18000"], ["--timeout", "0"]):
        with pytest.raises(SystemExit):
            arguments(base + bad)


def test_mp_combat_attacks_and_requires_fire_and_damage_for_both_owners(tmp_path):
    import pytest
    from run_mp_loopback_proof import compare_traces, fixture_orders
    for slot, movers in ((0, {0}), (1, {1, 2})):
        attacks = [row.split() for row in fixture_orders(slot, 18000, 0, combat=True) if " attack " in row]
        assert {int(row[1]) for row in attacks} == movers
        assert all(int(row[3]) == 704 + slot for row in attacks)
    peers = [tmp_path / "host", tmp_path / "client"]
    for peer in peers:
        peer.mkdir()
        (peer / "session.trace").write_text("C 1 00\nH 1 a b c d e\nH 2 a b c d e\n")
    with pytest.raises(ValueError, match="firing and damage"):
        compare_traces(*peers, 2, combat=True)
    for peer in peers:
        (peer / "session.trace").write_text("C 1 00\nH 1 a b c d e\nH 2 a b c d f\n")
    with pytest.raises(ValueError, match="firing and damage"):
        compare_traces(*peers, 2, combat=True)  # Animation RNG alone is insufficient.
    for peer in peers:
        with (peer / "session.trace").open("a") as trace:
            trace.write("B 1 0 fire 10 20 0\nB 1 0 damage 10 20 5\nB 2 1 fire 11 21 0\nB 2 1 damage 11 21 6\n")
    assert compare_traces(*peers, 2, combat=True)["combat"][0]["1:damage"] == 1
    (peers[1] / "session.trace").write_text((peers[1] / "session.trace").read_text().replace("damage 11 21 6", "damage 11 21 0"))
    with pytest.raises(ValueError, match="firing and damage"):
        compare_traces(*peers, 2, combat=True)


def test_mp_rendezvous_deadline_includes_loading_and_is_not_reset_by_pumps():
    runtime = (ROOT / "code/net/runtime.cpp").read_text()
    begin = runtime.split("void beginMission(", 1)[1].split("void setRoster", 1)[0]
    pump = runtime.split("bool prepareTick(", 1)[1]
    assert begin.index("began = lastProgress = std::chrono::steady_clock::now()") < begin.index("session->open(")
    assert "began =" not in pump
    assert pump.index("progressTimeout(false,") < pump.index("session->pump()")


def test_mp_trace_bounds_frame_exit_overshoot(tmp_path):
    import pytest
    from run_mp_loopback_proof import compare_traces
    peers = [tmp_path / "host", tmp_path / "client"]
    for extra, peer in enumerate(peers):
        peer.mkdir()
        (peer / "session.trace").write_text("C 1 00\n" + "".join(f"H {i} a b c d e\n" for i in range(1, 3 + extra)))
    result = compare_traces(*peers, 2)
    assert result["equal_ticks"] == 2 and result["uncompared_tail_ticks"] == [0, 1]
    with pytest.raises(ValueError, match="incomplete"):
        compare_traces(*peers, 2, max_extra=0)
    with pytest.raises(ValueError, match="incomplete"):
        compare_traces(*peers, 4)
    (peers[0] / "session.trace").write_text((peers[1] / "session.trace").read_text().replace("H 3 a", "H 3 z"))
    with pytest.raises(ValueError, match="first divergent"):
        compare_traces(*peers, 2)  # A shared overshoot tick must also be compared.


def test_mp_wall_clock_uses_normal_time_and_bounds_the_cell(tmp_path, monkeypatch):
    import pytest
    from contextlib import ExitStack
    import run_mp_loopback_proof as proof
    captured = {}
    monkeypatch.setattr(proof.nofocus, "spawn", lambda argv, **kw: captured.update(argv=argv, **kw))
    with ExitStack() as logs:
        proof.peer_start(str(tmp_path), str(tmp_path), 0, 23460, 1200, 600, False, logs, wall_clock=True)
    assert "MC2_SMOKE_FIXED_TIMESTEP" not in captured["env"]
    assert captured["argv"][captured["argv"].index("--duration") + 1] == "50"
    base = ["--install", str(tmp_path), "--out", str(tmp_path / "out"), "--wall-clock"]
    assert proof.arguments(base + ["--ticks", "1200"]).wall_clock
    assert proof.arguments(base + ["--combat", "--loss", "2", "--latency", "50", "--jitter", "20"]).combat
    for bad in (["--ticks", "64"], ["--loss", "2"], ["--force-desync", "600"]):
        with pytest.raises(SystemExit):
            proof.arguments(base + bad)
