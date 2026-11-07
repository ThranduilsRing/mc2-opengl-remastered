"""sim_determinism: the comparator that proves two runs (or two commits) agree on the simulation."""
import os
from io import StringIO
from pathlib import Path
import shutil
import sys
import tempfile
from types import SimpleNamespace
import uuid

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sim_determinism as sd  # noqa: E402


def test_selftest_passes():
    assert sd.selftest() == 0


@pytest.mark.parametrize("name", ["mc2_01_ai.fit", "mc2_01_specials.fit", "mc2_01_specials_eject.fit"])
def test_overlay_pin_refuses_brain_fixtures_in_base_install(tmp_path, monkeypatch, name):
    missions = tmp_path / "data/missions"
    missions.mkdir(parents=True)
    (missions / name).write_text("fixture")
    manifest = tmp_path / "overlay.json"
    manifest.write_text("previous pin")
    monkeypatch.setattr(sd, "OVERLAY_MANIFEST", str(manifest))
    assert sd.main(["write-overlay", "--install", str(tmp_path)]) == 1
    assert manifest.read_text() == "previous pin"


def test_native_baseline_is_separate(monkeypatch):
    monkeypatch.setattr(sd.sys, "platform", "win32")
    windows = sd.baseline_path("mc2_10")
    monkeypatch.setattr(sd.sys, "platform", "linux")
    assert sd.baseline_path("mc2_10") == os.path.join(sd.BASELINE_DIR, "linux", "mc2_10.json")
    assert windows == os.path.join(sd.BASELINE_DIR, "mc2_10.json")


def test_logparse_and_the_tool_agree_on_the_chain():
    sys.path.insert(0, os.path.join(HERE, ".."))
    from scripts.smoke_lib.logparse import parse_log
    lines = [f"{sd.PREFIX}{i} t=0.0 n=1 graded=a visual=b strict=c rng=1/2/3" for i in range(50)]
    s = parse_log("\n".join(["noise"] + lines + ["[HEARTBEAT] frames=1"]) + "\n")
    assert s.simid_frames == 50 and s.simid_chain == sd.full_chain(lines)
    assert s.simid_chain != sd.chain(lines)


def test_a_log_without_simid_has_no_chain():
    sys.path.insert(0, os.path.join(HERE, ".."))
    from scripts.smoke_lib.logparse import parse_log
    s = parse_log("[HEARTBEAT] frames=1\n")
    assert s.simid_frames == 0 and s.simid_chain is None


def test_glued_simid_records_are_not_dropped(monkeypatch):
    from scripts.smoke_lib.logparse import parse_log
    import saveload_roundtrip as rt

    frames = [f"{sd.PREFIX}{i} t=0.0 n=1 graded=a visual=b strict=c animrng=1" for i in range(2)]
    detail = "[SIMID v1 mover] simframe=1 id=7 gesture=1 pstep=0/3"
    clean = frames + [detail]
    glued = "PAUSEtxmmgr: Bad texture handle!" + "".join(clean) + "\n"
    monkeypatch.setattr(sd, "open", lambda *a, **k: StringIO(glued), raising=False)
    assert sd.load_simid("unused.log") == frames
    summary = parse_log(glued)
    assert summary.simid_frames == len(frames) and summary.simid_chain == sd.chain(frames)
    assert rt.parse([glued]) == rt.parse(clean)


@pytest.mark.parametrize("command", ["check", "run", "pairs-until-fail"])
@pytest.mark.parametrize("install", [None, "lane3", "A:/Games/mc2-opengl/mc2-win64-lane3", "custom-install"])
def test_cli_selects_install_for_runner_and_fingerprint(monkeypatch, tmp_path, command, install):
    expected = sd.resolve_install(install) if install else sd.INSTALL_DIR
    launched, fingerprints, baselines, deployed = [], [], [], []
    monkeypatch.setattr(sd, "run_mission", lambda *args, **kwargs: launched.append(kwargs["install"]) or ["a", "b"])
    monkeypatch.setattr(sd, "pairs_until_fail", lambda *args, **kwargs: launched.append(kwargs["install"]) or 0)
    monkeypatch.setattr(sd, "build_and_deploy", lambda path: deployed.append(path))
    monkeypatch.setattr(sd, "load_simid", lambda log: [sd.PREFIX + "1 n=1 graded=a visual=b strict=c nobj=1 objects=d nabl=1 abl=e rng=1/2/3"])
    monkeypatch.setattr(sd, "baseline_path", lambda mission: __file__)
    monkeypatch.setattr(sd, "_load_env", lambda path: {})
    monkeypatch.setattr(sd, "_seed", lambda log: "seed")
    monkeypatch.setattr(sd, "data_fingerprint", lambda path: fingerprints.append(path) or {"digest": path})
    monkeypatch.setattr(sd, "check_baseline", lambda a, base, seed, fp: baselines.append(fp) or (True, "match"))
    argv = [command, "--out", str(tmp_path / "out"), "--min-frames", "1"]  # not the cwd: it left unused/result.json in the repo
    if command == "pairs-until-fail":
        argv += ["--mission", "mc2_03", "--max", "1"]
    else:
        argv += ["mc2_03", "--deploy"]
        if command == "run":
            argv += ["--baseline"]
    if install:
        argv += ["--install", install]
    assert sd.main(argv) == 0
    assert launched == [expected]
    assert deployed == ([] if command == "pairs-until-fail" else [expected])
    assert fingerprints == ([] if command == "pairs-until-fail" else [expected])
    assert baselines == ([] if command == "pairs-until-fail" else [{"digest": expected}])


@pytest.mark.parametrize("install,target", [
    (sd.INSTALL_DIR, ["--deploy", "0.4"]),
    ("lane3", ["--deploy", "lane3"]),
    ("A:/Games/mc2-opengl/mc2-win64-lane3", ["--deploy", "lane3"]),
    ("custom-install", ["--exe", os.path.join(sd.resolve_install("custom-install"), "mc2.exe" if os.name == "nt" else "mc2")]),
])
def test_runner_passes_selected_install_to_both_mocked_launches(monkeypatch, install, target):
    commands = []
    monkeypatch.setattr(sd.subprocess, "run", lambda cmd, **kwargs: commands.append(cmd) or
                        SimpleNamespace(returncode=0, stdout=f"[runner] artifacts: {HERE}"))
    copied = []
    monkeypatch.setattr(sd.shutil, "copytree", lambda source, dest: copied.append(source))
    assert len(sd.run_mission("mc2_03", "20", "unused", install=install)) == 2
    assert len(commands) == 2
    assert copied == [HERE, HERE]
    for cmd in commands:
        assert cmd[-4:-2] == target  # the install selector sits before --keep-logs --sim-determinism
        assert cmd[-2:] == ["--keep-logs", "--sim-determinism"]


def test_selected_install_reaches_pair_loop(monkeypatch):
    launched = []
    monkeypatch.setattr(sd, "run_mission", lambda *args, **kwargs: launched.append(kwargs["install"]) or ["a", "b"])
    monkeypatch.setattr(sd, "load_simid", lambda log: [sd.PREFIX + "1 n=1 graded=a visual=b strict=c nobj=1 objects=d nabl=1 abl=e rng=1/2/3"])
    monkeypatch.setattr(sd, "host_load", lambda: {"cpu": 0, "procs": {}})
    out = Path(tempfile.gettempdir()) / f"simdet-pairs-{uuid.uuid4().hex}"
    out.mkdir()
    try:
        assert sd.pairs_until_fail("mc2_03", "20", str(out), 1, 1, install="lane3") == 0
    finally:
        shutil.rmtree(out)
    assert launched == ["lane3"]


def test_build_deploy_targets_selected_install(monkeypatch):
    commands = []
    monkeypatch.setattr(sd.subprocess, "run", lambda cmd, **kwargs: commands.append(cmd) or SimpleNamespace(returncode=0, stdout=""))
    sd.build_and_deploy("lane3")
    assert commands[1][2] == sd.resolve_install("lane3")
