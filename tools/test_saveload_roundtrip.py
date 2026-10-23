"""saveload_roundtrip: the analysis that compares the simulation after a quickload with the one after the quicksave."""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import saveload_roundtrip as rt  # noqa: E402


def test_selftest_passes():
    assert rt.selftest() == 0


def test_restore_ignores_cosmetic_rng_and_detects_animation_rng_drift():
    pre = [f"[SIMID v1] simframe={i} t={i / 30:.3f} n=1 graded=a visual=b strict=c rng={i}/2/3 animrng={i}"
           for i in range(50)]
    post = [f"[SIMID v1] simframe={50 + i} t={(20 + i) / 30:.3f} n=1 graded=a visual=b strict=c rng={20+i}/200/300 animrng={20+i}"
            for i in range(20)]
    assert rt.analyze(pre + post, frames_needed=10)[0]
    post[0] = post[0].replace("animrng=20", "animrng=21")
    assert not rt.analyze(pre + post, frames_needed=10)[0]


def test_mover_detail_is_reported_for_the_first_divergent_frame():
    pre = [f"[SIMID v1] simframe={i} t={i / 30:.3f} n=1 graded=g{i} visual=v strict=s" for i in range(50)]
    pre.insert(21, "[SIMID v1 mover] simframe=20 id=7 gesture=1 pstep=0/3")  # detail follows its frame line and is keyed by frame index
    post = [f"[SIMID v1] simframe={50 + i} t={(20 + i) / 30:.3f} n=1 graded={'g%d' % (20 + i) if i else 'BAD'} visual=v strict=s" for i in range(20)]
    post.insert(1, "[SIMID v1 mover] simframe=50 id=7 gesture=0 pstep=0/3")
    ok, rep = rt.analyze(pre + post, frames_needed=10)
    text = "\n".join(rep)
    assert not ok and "DIVERGE 0 frame" in text and "id 7" in text and "gesture=0" in text


def test_a_save_that_differs_names_its_packet_and_offset(tmp_path):
    pk = [(0, b"ver\0"), (4, b"a" * 64), (4, b"b" * 64)]
    other = [(0, b"ver\0"), (4, b"a" * 64), (4, b"b" * 10 + b"X" + b"b" * 53)]
    for name, p in (("a.ims", pk), ("b.ims", other)):
        rt.si.scs.write(str(tmp_path / name), p, False)
    ok, rep = rt.resave_verdict(str(tmp_path / "a.ims"), str(tmp_path / "b.ims"))
    assert not ok and "packet 2" in " ".join(rep) and "offset 10" in " ".join(rep)


def test_planted_save_is_judged_against_the_noise_mask_across_builds(tmp_path):
    """resave --plant --mask: a planted save comes from another exe, so run and build ranges both apply; a byte outside them fails."""
    mask = os.path.join(HERE, "save_noise_mask.json")
    size = 19864                                                  # MechWarriorData

    def save(name, offset=None):
        record = bytearray(size)
        if offset is not None:
            record[offset] = 1
        path = str(tmp_path / name)
        rt.si.scs.write(path, [(0, b"ver\0"), (4, bytes(record))], False)
        return path

    base = save("base.ims")
    ok, _ = rt.masked_resave_verdict(base, save("same.ims"), mask)
    assert ok
    ok, _ = rt.masked_resave_verdict(base, save("run.ims", 10100), mask)                  # a MovePath step-list byte: run-to-run noise
    assert ok
    ok, _ = rt.masked_resave_verdict(base, save("build.ims", 2200), mask)                 # inside a TacticalOrder block: cross-build noise
    assert ok
    ok, rep = rt.masked_resave_verdict(base, save("text.ims", 5), mask)                   # a pilot string byte is never masked
    assert not ok and "outside the mask" in " ".join(rep)


def test_first_save_only_records_a_masked_baseline_although_the_resave_leaves_the_mask(tmp_path, monkeypatch, capsys):
    """The quickload is lossy, so a re-save never stays inside the mask; --first-save-only keeps the masked first-save digest recordable and checkable."""
    mask = os.path.join(HERE, "save_noise_mask.json")

    def save(name, offset=None):
        record = bytearray(19864)                                     # MechWarriorData
        if offset is not None:
            record[offset] = 1
        path = str(tmp_path / name)
        rt.si.scs.write(path, [(0, b"ver\0"), (4, bytes(record))], False)
        return path

    first, noisy_first, resave = save("a.ims"), save("b.ims", 10100), save("c.ims", 5)    # 10100 is masked run noise; 5 is a pilot string byte, never masked
    monkeypatch.setattr(rt, "BASELINE_DIR", str(tmp_path / "baselines"))
    monkeypatch.setattr(rt, "ROOT", str(tmp_path))                  # the tool prints the baseline path relative to the repo root
    monkeypatch.setattr(rt, "run_fixture", lambda *a, **k: (str(tmp_path), first, resave, {"sha": "x", "dirty": 0}, 0))

    def main(*extra):
        return rt.main(["baseline", "--mission", "m", "--mask", mask, *extra])

    assert main() == 1                                                # the re-save gate: refused
    assert main("--first-save-only") == 0                             # recorded
    assert json.load(open(rt.baseline_path("m")))["judged"] == "first-save-only"   # a weak baseline says so
    monkeypatch.setattr(rt, "run_fixture", lambda *a, **k: (str(tmp_path), noisy_first, resave, {"sha": "x", "dirty": 0}, 0))
    assert rt.main(["check", "--mission", "m", "--mask", mask, "--first-save-only"]) == 0     # run-to-run noise in a masked byte does not fail the check
    assert "WEAK baseline: judged first-save-only" in capsys.readouterr().out                  # and the check reports the weakness
    monkeypatch.setattr(rt, "run_fixture", lambda *a, **k: (str(tmp_path), save("d.ims", 5), resave, {"sha": "x", "dirty": 0}, 0))
    assert rt.main(["check", "--mission", "m", "--mask", mask, "--first-save-only"]) == 1     # a changed pilot string byte does
    capsys.readouterr()


def test_check_refuses_a_run_taken_at_other_frames_than_the_baseline(tmp_path):
    """A check run at the CLI default frames against a baseline recorded at other frames fails loudly instead of comparing two scenario moments."""
    path = str(tmp_path / "a.ims")
    rt.si.scs.write(path, [(0, b"ver\0"), (4, bytes(64))], False)
    fp = {"sha": "x", "dirty": 0}
    ok, lines = rt.check_baseline_record(rt.baseline_record("m", 1300, 1400, path, path, fp), rt.baseline_record("m", 1000, 1100, path, path, fp))
    assert not ok and "--save-at 1300 --load-at 1400" in lines[0]
    assert rt.check_baseline_record(rt.baseline_record("m", 1300, 1400, path, path, fp), rt.baseline_record("m", 1300, 1400, path, path, fp))[0]


def test_a_baseline_without_frames_is_refused_by_name():
    """A baseline that lacks its frames names the missing key and asks for a re-record, instead of quoting 'None' as a frame."""
    args = type("Args", (), {"save_at": None, "load_at": None})()
    refusal = rt.frames_from_baseline(args, {"load_at": 1100})
    assert refusal and "no save_at" in refusal and "--baseline" in refusal


def _save(path, size=64):
    rt.si.scs.write(str(path), [(0, b"ver\0"), (4, bytes(size))], False)
    return str(path)


def test_a_missing_resave_is_a_fail(tmp_path, monkeypatch, capsys):
    """WHY: the quickload crashed and wrote no re-save, so the tool compared nothing and still printed PASS."""
    first = _save(tmp_path / "a.ims")
    monkeypatch.setattr(rt, "run_fixture", lambda *a, **k: (str(tmp_path), first, None, {"sha": "x", "dirty": 0}, 0))
    assert rt.main(["resave", "--mission", "m"]) == 1
    out = capsys.readouterr().out
    assert "resave: FAIL" in out and "no re-save was written" in out


def test_an_empty_resave_is_a_fail(tmp_path, monkeypatch, capsys):
    first = _save(tmp_path / "a.ims")
    empty = str(tmp_path / "empty.ims")
    open(empty, "wb").close()
    monkeypatch.setattr(rt, "run_fixture", lambda *a, **k: (str(tmp_path), first, empty, {"sha": "x", "dirty": 0}, 0))
    assert rt.main(["resave", "--mission", "m"]) == 1
    out = capsys.readouterr().out
    assert "resave: FAIL" in out and "empty" in out


def test_a_crashed_child_is_a_fail_even_with_two_saves(tmp_path, monkeypatch, capsys):
    """WHY: a non-zero run_smoke exit means the quickload did not complete; identical files cannot make that a pass."""
    first, second = _save(tmp_path / "a.ims"), _save(tmp_path / "b.ims")
    monkeypatch.setattr(rt, "run_fixture", lambda *a, **k: (str(tmp_path), first, second, {"sha": "x", "dirty": 0}, 3))
    assert rt.main(["resave", "--mission", "m"]) == 1
    out = capsys.readouterr().out
    assert "resave: FAIL" in out and "exited 3" in out


def test_a_positive_round_trip_passes(tmp_path, monkeypatch, capsys):
    first, second = _save(tmp_path / "a.ims"), _save(tmp_path / "b.ims")
    monkeypatch.setattr(rt, "run_fixture", lambda *a, **k: (str(tmp_path), first, second, {"sha": "x", "dirty": 0}, 0))
    assert rt.main(["resave", "--mission", "m"]) == 0
    assert "resave: PASS" in capsys.readouterr().out


def test_check_fails_when_the_run_has_no_resave_even_under_first_save_only(tmp_path, monkeypatch, capsys):
    """WHY: --first-save-only waives the lossy re-save's content, never its existence: with no re-save the check compared nothing."""
    first, resave = _save(tmp_path / "a.ims"), _save(tmp_path / "b.ims")
    monkeypatch.setattr(rt, "BASELINE_DIR", str(tmp_path / "baselines"))
    monkeypatch.setattr(rt, "ROOT", str(tmp_path))
    monkeypatch.setattr(rt, "run_fixture", lambda *a, **k: (str(tmp_path), first, resave, {"sha": "x", "dirty": 0}, 0))
    assert rt.main(["baseline", "--mission", "m", "--first-save-only"]) == 0
    monkeypatch.setattr(rt, "run_fixture", lambda *a, **k: (str(tmp_path), first, None, {"sha": "x", "dirty": 0}, 0))
    assert rt.main(["check", "--mission", "m", "--first-save-only"]) == 1
    out = capsys.readouterr().out
    assert "check: FAIL" in out and "no re-save" in out


def test_a_baseline_without_runs_cannot_pass_a_check(tmp_path):
    """A baseline or run with no packets must not compare equal by zip() over nothing."""
    path = _save(tmp_path / "a.ims")
    fp = {"sha": "x", "dirty": 0}
    rec = rt.baseline_record("m", 10, 20, path, path, fp)
    ok, lines = rt.check_baseline_record(dict(rec, runs=[], overall="x"), rec)
    assert not ok and "no packets" in " ".join(lines)
    ok, lines = rt.check_baseline_record(rec, dict(rec, runs=[], packets=0))
    assert not ok and "no packets" in " ".join(lines)


def test_default_run_has_at_least_300_frames_between_save_and_load():
    assert int(rt.LOAD_AT) - int(rt.SAVE_AT) >= 300


def test_run_retains_failed_smoke_evidence_and_limits_liveness_fixture(tmp_path, monkeypatch):
    import subprocess
    from contextlib import nullcontext
    artifact = tmp_path / "tests/smoke/artifacts/2026-fixture"
    artifact.mkdir(parents=True)
    (artifact / "mc2_01.log").write_text("fixture did not load")
    monkeypatch.setattr(rt, "ROOT", str(tmp_path))
    monkeypatch.setattr(rt.savegame_backup, "guarded", lambda path: nullcontext())
    observed = []
    def smoke(*args, **kwargs):
        observed.append(kwargs["env"])
        return subprocess.CompletedProcess(args, 1, "failed gate")
    monkeypatch.setattr(rt.subprocess, "run", smoke)
    ok, report = rt.run("mc2_01", 300, False, str(tmp_path / "out"), "lane3")
    assert not ok and "failed gate" in report and any("no load found" in line for line in report)
    assert "MC2_FIXTURE_ORDER_LIVENESS" not in observed[0]
    assert (tmp_path / "out/run/mc2_01.log").read_text() == "fixture did not load"
