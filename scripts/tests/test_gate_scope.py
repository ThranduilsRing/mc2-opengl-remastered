"""Diff-scoped daytime gate (scripts/gate_scope.py + merge_gate wiring): scope decisions, the 30 h fallback, nightly,
mark handling for a scope-skipped gate, verdict recording, and 2-wide determinism with mocked runners. Offline."""
import json
import sys
import threading
import time
from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import gate_scope as gs
import merge_gate as gate

AGE_OK = 3 * 3600
AGE_OLD = 31 * 3600


def decide(paths, **kw):
    kw.setdefault("mark_age_s", AGE_OK)
    return gs.decide(paths, gate.change_domains, **kw)


def test_docs_only_diff_skips_mods_and_video():
    d = decide(["docs/merge-queue-v2.md", "README.md"])
    assert all(d[s] and d[s].startswith("scope: ") for s in gs.SCOPED_STAGES)


def test_tooling_shader_and_plain_engine_code_skip_both():
    d = decide(["scripts/tests/test_x.py", "tools/lane_merge.py", "shaders/a.slang", "code/mechlabscreen.cpp", "RenderCore/foo.cpp"])
    assert d["mods"] and d["video-decode"]


def test_data_diff_runs_mods_but_not_video():
    d = decide(["data/missions/mc2_01.fit"])
    assert d["mods"] is None and d["video-decode"]


def test_only_direct_mod_content_paths_run_mods():
    for p in ("data/missions/mc2_01.fit", "mods/x/y.fit", "examples/mods/a/b.txt", "tests/mods/t.py", "tools/mod_smoke.py",
              "tools/mod_install.py", "tools/validate_mod_package.py"):
        assert decide([p])["mods"] is None, p


def test_engine_shader_loader_and_unknown_paths_skip_mods():
    for p in ("shaders/a.slang", "RenderCore/foo.cpp", "mclib/fastfile.cpp", "mclib/file.cpp", "mclib/inifile.cpp",
              "GameOS/gameos/gameos_fileio.cpp", "code/mission2.cpp", "code/logisticsdata_save.cpp", "GuiRuntime/GuiRuntime_assets.cpp",
              "CMakeLists.txt", "weird/thing.bin"):
        assert decide([p])["mods"], p


def test_video_relevant_paths_run_video():
    for p in ("scripts/video_decode_smoke.py", "code/mc2video.cpp", "code/mc2movie.h", "GameOS/gameos/sim_overlap.cpp", "data/movies/a.bik",
              "code/controlgui_chat.cpp", "code/tick_commit.cpp", "code/tick_commit.h", "tests/unit/test_playmovie_deferral.cpp"):
        assert decide([p])["video-decode"] is None, p


def test_unknown_path_runs_everything():
    for p in ("CMakeLists.txt", "weird/thing.bin"):
        d = decide(["docs/a.md", p])
        assert d["mods"] and d["video-decode"] is None, p   # unknown paths still force video-decode, never mods
    for p in ("scripts/merge_gate.py", "scripts/deploy_payload.py"):   # never mods, never video (tests/scripts/tools)
        d = decide(["docs/a.md", p])
        assert d["mods"] and d["video-decode"], p


def test_tests_scripts_tools_do_not_force_video():
    # W123's 16 false hits (diff 01580cf53..HEAD): none of these exercise video.
    paths = ["tests/unit/test_foo.cpp", "tests/unit/baseline.json", "tests/saveload/baselines/x.json",
             "tests/smoke/sim_determinism/a.txt", "scripts/merge_gate.py", "scripts/gate_scope.py",
             "scripts/background-launch-allow.json", "tools/visual_verify/ab.py", "tools/lane_merge.py"]
    for p in paths:
        assert decide([p])["video-decode"], p


def test_nightly_runs_everything_even_for_docs_only():
    assert decide(["docs/a.md"], nightly=True) == {"mods": None, "video-decode": None}


def test_thirty_hour_fallback_and_missing_mark_run_everything():
    assert decide(["docs/a.md"], mark_age_s=AGE_OLD) == {"mods": None, "video-decode": None}
    assert decide(["docs/a.md"], mark_age_s=gs.NIGHTLY_MAX_AGE_S - 1)["mods"]
    assert decide(["docs/a.md"], mark_age_s=None) == {"mods": None, "video-decode": None}
    assert decide(None) == {"mods": None, "video-decode": None}


def test_read_mark_and_scoped_diff(tmp_path):
    (tmp_path / "build64-ninja").mkdir()
    (tmp_path / gs.FULL_MARK).write_text(json.dumps(dict(head="abc", finished=1000.0)))
    assert gs.read_mark(tmp_path, now=1500.0) == ("abc", 500.0)
    assert gs.read_mark(tmp_path / "nope") == (None, None)
    calls = []
    def git(*a):
        calls.append(a)
        return "base" if a[0] == "merge-base" else "a.md\0b.md\0"
    assert gs.scoped_diff(git, "abc", "head") == ["a.md", "b.md"]
    assert calls[1][-2:] == ("base", "head")
    assert gs.scoped_diff(lambda *a: (_ for _ in ()).throw(RuntimeError("no")), "abc", "head") is None


def steps_for(*names):
    return [(n, [n], {}) for n in names]


def test_scope_skip_is_recorded_as_skip_scope_never_pass(monkeypatch):
    steps, skipped = gs.apply_scope(steps_for("tier1", "mods", "video-decode"), decide(["docs/a.md"]))
    assert skipped == ["mods", "video-decode"]
    env = {n: e for n, _, e in steps}
    assert "MC2_GATE_SKIP_REASON" not in env["tier1"]
    assert env["mods"]["MC2_GATE_SCOPE_SKIP"] == "1" and env["mods"]["MC2_GATE_SKIP_REASON"].startswith("scope: ")
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    monkeypatch.setattr(gate, "log_lease_holders", lambda: None)
    monkeypatch.setattr(gate, "ACTIVE_STATE", None)
    gate.ROW_RESULTS.clear()
    gate.FAILED_STAGES.clear()
    assert gate.execute(steps, "head", runner=lambda *a, **k: SimpleNamespace(returncode=0, stdout="")) == 0
    assert gate.ROW_RESULTS["mods"]["status"].startswith("SKIP(scope: ")
    verdict, skipped_rows = gate.verdict_of(0, steps)
    assert verdict == "PASS" and skipped_rows == []   # promotable: a scope skip is not PARTIAL
    data = gate.write_verdict([], head="h", branch="b", verdict=verdict, steps=steps, skipped=skipped_rows, artifacts={},
                              timings={}, cleanup_errors=[])
    assert set(data["scope_skipped"]) == {"mods", "video-decode"}
    assert all(v.startswith("SKIP(scope: ") for v in data["scope_skipped"].values())
    assert "mods" not in data["executed"] and data["skipped"] == {}
    gate.ROW_RESULTS.clear()


def test_operator_skip_is_still_partial():
    steps = [("mods", ["mods"], {"MC2_GATE_SKIP_REASON": "operator stage filter; partial gate"})]
    assert gate.verdict_of(0, steps) == ("PARTIAL", ["mods"])


def test_scope_skipped_gate_keeps_the_all_stage_mark(tmp_path):
    (tmp_path / "build64-ninja").mkdir()
    mark = tmp_path / gate.FULL_MARK
    mark.write_text(json.dumps(dict(head="old", finished=1.0)))
    assert gate.mark_full_gate(tmp_path, "new", "PASS", True, False, scope_skipped=True) is False
    assert json.loads(mark.read_text())["head"] == "old"
    assert gate.mark_full_gate(tmp_path, "new", "PASS", True, False) is True
    assert json.loads(mark.read_text())["head"] == "new"


def test_nightly_flag_implies_full_and_is_documented():
    ap_help = Path(gate.__file__).read_text(encoding="utf-8")
    assert "--nightly" in ap_help and "args.full = args.full or args.nightly" in ap_help
    doc = (Path(gate.__file__).resolve().parents[1] / "docs/merge-queue-v2.md").read_text(encoding="utf-8")
    assert "--nightly" in doc and "30 h" in doc


def test_determinism_rows_run_two_wide_when_mods_is_scope_skipped(monkeypatch, tmp_path):
    lock = threading.Lock()
    state = dict(active=0, peak=0, installs=[])

    @contextmanager
    def copy(install):
        yield tmp_path / "clone"

    def child(command, **kw):
        if command[0] == "det":
            with lock:
                state["active"] += 1
                state["peak"] = max(state["peak"], state["active"])
                state["installs"].append(command[command.index("--install") + 1])
            time.sleep(0.15)
            with lock:
                state["active"] -= 1
        return SimpleNamespace(returncode=0, stdout="")
    monkeypatch.delenv("MC2_HOST_LEASE_BATCH", raising=False)
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    monkeypatch.setattr(gate, "log_lease_holders", lambda: None)
    monkeypatch.setattr(gate, "ACTIVE_STATE", None)
    monkeypatch.setattr(gate, "stage_seconds", lambda n: 500.0 if n.startswith("determinism-") else 100.0)
    monkeypatch.setattr(gate, "stock_copy", copy)
    monkeypatch.setattr(gate, "run_child", child)
    install = str(tmp_path / "install")
    steps = [("deploy", ["deploy"], {}), ("tier1", ["tier1"], {})]
    steps += [("determinism-m%d" % i, ["det", "--install", install], {}) for i in range(4)]
    steps += [("saveload-mc2_10", ["python", "tools/saveload_roundtrip.py", "--install", install], {}), ("mods", ["mods"], {})]
    steps, _ = gs.apply_scope(steps, decide(["docs/a.md"]))
    gate.ROW_RESULTS.clear()
    assert gate.execute_parallel(steps, "head") == 0
    assert state["peak"] == 2          # worker A and the idle mods worker, never more than the 2 slots
    assert len(state["installs"]) == 4
    assert any(i.endswith("clone") for i in state["installs"]) and any(i.endswith("install") for i in state["installs"])
    assert gate.ROW_RESULTS["mods"]["status"].startswith("SKIP(scope: ")
    gate.ROW_RESULTS.clear()


import pytest


@pytest.mark.parametrize("failing", ["check-staged-hygiene.py", "dup_check.py", "check-background-launch.py"])
def test_each_cheap_row_stops_preflight_before_the_build(monkeypatch, failing):
    ran = []
    def fake_run(command, **kw):
        ran.append(command[2])
        bad = failing in command[2]
        return SimpleNamespace(returncode=1 if bad else 0, stdout="boom" if bad else "", stderr="")
    monkeypatch.setattr(gate, "git", lambda *a: "base")
    monkeypatch.setattr(gate.subprocess, "run", fake_run)
    errors = gate.cheap_row_errors("a" * 40)
    assert len(ran) == 4 and len(errors) == 1 and "before the build" in errors[0] and "boom" in errors[0]
    monkeypatch.setattr(gate.subprocess, "run", lambda c, **kw: SimpleNamespace(returncode=0, stdout="", stderr=""))
    assert gate.cheap_row_errors("a" * 40) == []


def test_preflight_fails_when_a_cheap_row_fails(monkeypatch, tmp_path, capsys):
    import exe_fingerprint
    monkeypatch.setattr(exe_fingerprint, "extract", lambda p: dict(present=True, sha="a" * 40, dirty=0))
    monkeypatch.setattr(exe_fingerprint, "build_current", lambda *a: True)
    monkeypatch.setattr(gate, "cheap_row_errors", lambda head: ["dup_check failed before the build (rc=1): x"])
    assert gate.preflight("a" * 40, tmp_path / "none", 1, []) == 1
    assert "dup_check failed before the build" in capsys.readouterr().out


def test_memory_size_is_a_warn_only_line(monkeypatch, capsys):
    def fake_run(command, **kw):
        bad = "check-memory-size.py" in command[2]
        return SimpleNamespace(returncode=1 if bad else 0, stdout="too big" if bad else "", stderr="")
    monkeypatch.setattr(gate, "git", lambda *a: "base")
    monkeypatch.setattr(gate.subprocess, "run", fake_run)
    assert gate.cheap_row_errors("a" * 40) == []
    assert "WARN memory-size" in capsys.readouterr().out
