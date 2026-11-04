"""--keep-candidate: the deployed candidate survives the gate with campaign mods staged; --drop-candidate / publish remove it."""
import json
import os
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import merge_gate as gate


def _mods_source(tmp_path):
    src = tmp_path / "mods_src"
    for name in gate.CANDIDATE_MODS:
        (src / name / "data").mkdir(parents=True)
        (src / name / "data" / "x.fit").write_text(name)
    return src


def test_flags_parse_through_argparse(monkeypatch):
    monkeypatch.delenv("MC2_IN_GATE", raising=False)
    dropped, published = [], []
    monkeypatch.setattr(gate, "drop_candidate", lambda *a, **k: dropped.append(1))
    monkeypatch.setattr(gate, "publish", lambda head, dry: published.append(dry) or 0)
    assert gate.main(["--drop-candidate"]) == 0 and dropped == [1]
    assert gate.main(["--keep-candidate", "--publish", "--dry-run"]) == 0 and published == [True]
    with pytest.raises(SystemExit):
        gate.main(["--keep-candidate=yes"])  # store_true takes no value


def _window(monkeypatch, tmp_path, mods_source):
    """Drive execute_window's keep path against a fake seed/candidate; returns the fake seed-stock clone."""
    seed = tmp_path / "stock-clone"
    seed.mkdir()
    (seed / "mc2.exe").write_text("exe")
    from contextlib import contextmanager

    @contextmanager
    def fake_stock_copy(install, parent=None, keep=False):
        assert keep is True
        yield seed
    root = tmp_path / "root"
    (root / "build64-ninja").mkdir(parents=True)
    monkeypatch.setattr(gate, "ROOT", root)
    monkeypatch.setattr(gate, "CANDIDATE", tmp_path / "cand")
    monkeypatch.setattr(gate, "CANDIDATE_MODS_SOURCE", mods_source)
    monkeypatch.setattr(gate, "stock_copy", fake_stock_copy)
    monkeypatch.setattr(gate, "stage_published", lambda install: None)
    monkeypatch.setattr(gate, "execute_parallel", lambda steps, *a, **k: 1)  # a red verdict must survive
    monkeypatch.setattr(gate, "candidate_identity", lambda install, head, root=None: dict(exe_sha256="e" * 64, exe_commit=head[:12]))
    gate.CANDIDATE_ERRORS.clear()
    return seed


def test_execute_window_keeps_candidate_even_on_red(monkeypatch, tmp_path):
    seed = _window(monkeypatch, tmp_path, _mods_source(tmp_path))
    assert gate.execute_window(gate.plan(6), "h" * 40, keep_candidate=True) == 1
    assert (tmp_path / "cand" / "mods" / "mco-compat" / "data" / "x.fit").is_file() and not seed.exists()
    assert not gate.CANDIDATE_ERRORS


def test_staging_failure_cleans_copy_and_never_masks_verdict(monkeypatch, tmp_path):
    seed = _window(monkeypatch, tmp_path, tmp_path / "missing-source")
    assert gate.execute_window(gate.plan(6), "h" * 40, keep_candidate=True) == 1  # verdict preserved, no raise
    assert not seed.exists() and not (tmp_path / "cand").exists()
    assert len(gate.CANDIDATE_ERRORS) == 1 and "MechCommanderOmnitech" in gate.CANDIDATE_ERRORS[0]


def test_keep_stock_copy_stages_linked_mods_and_records(monkeypatch, tmp_path):
    monkeypatch.setattr(gate, "candidate_identity", lambda install, head, root=None: dict(exe_sha256="e" * 64, exe_commit="a" * 12))
    install = tmp_path / "seed-stock-abc"
    install.mkdir()
    (install / "mc2.exe").write_text("exe")
    src = _mods_source(tmp_path)
    old = gate.CANDIDATE_MODS_SOURCE
    gate.CANDIDATE_MODS_SOURCE = src
    try:
        root = tmp_path / "root"
        (root / "build64-ninja").mkdir(parents=True)
        cand = tmp_path / "merge-gate-candidate"
        cand.mkdir()
        (cand / "stale").write_text("old")
        kept = gate.keep_stock_copy(install, "a" * 40, candidate=cand, root=root)
    finally:
        gate.CANDIDATE_MODS_SOURCE = old
    assert kept == cand and not install.exists() and not (cand / "stale").exists()
    for name in gate.CANDIDATE_MODS:
        staged = cand / "mods" / name / "data" / "x.fit"
        assert staged.read_text() == name and os.path.samefile(staged, src / name / "data" / "x.fit")
    mark = json.loads((root / gate.CANDIDATE_MARK).read_text())
    assert mark["path"] == str(cand) and mark["exe_sha256"] == "e" * 64 and mark["exe_commit"] == "a" * 12
    gate.drop_candidate(cand, root)
    assert not cand.exists() and not (root / gate.CANDIDATE_MARK).exists()


def test_missing_mod_source_is_an_error(tmp_path):
    (tmp_path / "inst").mkdir()
    try:
        gate.stage_candidate_mods(tmp_path / "inst", source=tmp_path / "nope")
    except RuntimeError as exc:
        assert "MechCommanderOmnitech" in str(exc)
    else:
        raise AssertionError("expected RuntimeError")



def _identity_fixture(monkeypatch, tmp_path, install_exe, build_exe, fp):
    root = tmp_path / "root"
    (root / "build64-ninja").mkdir(parents=True)
    (root / "build64-ninja" / "mc2.exe").write_bytes(build_exe)
    install = tmp_path / "install"
    install.mkdir()
    (install / "mc2.exe").write_bytes(install_exe)
    import exe_fingerprint
    monkeypatch.setattr(exe_fingerprint, "extract", lambda exe: fp)
    monkeypatch.setattr(exe_fingerprint, "build_current", lambda sha, head, root, dirty=False: not dirty and sha == "b" * 12)
    return root, install


def test_failed_deploy_never_replaces_the_previous_candidate(monkeypatch, tmp_path):
    """W116 item 2: the copy still holds the published exe (deploy failed) -> old candidate and record survive."""
    root, install = _identity_fixture(monkeypatch, tmp_path, b"published exe", b"gate build",
                                      dict(present=True, sha="b" * 12, dirty=0))
    cand = tmp_path / "cand"
    cand.mkdir()
    (cand / "mc2.exe").write_text("previous good candidate")
    (root / gate.CANDIDATE_MARK).write_text(json.dumps(dict(path=str(cand), head="p" * 40)))
    with pytest.raises(RuntimeError, match="deploy did not land"):
        gate.keep_stock_copy(install, "h" * 40, candidate=cand, root=root)
    assert (cand / "mc2.exe").read_text() == "previous good candidate"
    assert json.loads((root / gate.CANDIDATE_MARK).read_text())["head"] == "p" * 40


@pytest.mark.parametrize("fp", [dict(present=False), dict(present=True, sha="b" * 12, dirty=1),
                                dict(present=True, sha="c" * 12, dirty=0)])
def test_candidate_identity_refuses_unattributable_exe(monkeypatch, tmp_path, fp):
    root, install = _identity_fixture(monkeypatch, tmp_path, b"gate build", b"gate build", fp)
    with pytest.raises(RuntimeError, match="not a clean build"):
        gate.candidate_identity(install, "h" * 40, root)


def test_candidate_identity_records_exe_hash_and_source_commit(monkeypatch, tmp_path):
    import hashlib
    root, install = _identity_fixture(monkeypatch, tmp_path, b"gate build", b"gate build",
                                      dict(present=True, sha="b" * 12, dirty=0))
    assert gate.candidate_identity(install, "h" * 40, root) == dict(
        exe_sha256=hashlib.sha256(b"gate build").hexdigest(), exe_commit="b" * 12)
