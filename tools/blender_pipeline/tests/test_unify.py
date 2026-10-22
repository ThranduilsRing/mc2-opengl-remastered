"""test_unify.py — BLENDER-PIPELINE-UNIFY-1 coverage for the shared
srcdata.py resolver and the mc2asset.py dispatcher."""
import sys
from pathlib import Path
from unittest import mock

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import srcdata  # noqa: E402
import mc2asset  # noqa: E402


def _make_tgl(root):
    tgl = root / "tgl"
    tgl.mkdir(parents=True)
    (tgl / "quonset.ase").write_text("*GEOMOBJECT {\n}\n")
    (tgl / "quonset.ini").write_text('FITini\n[TGLData]\nFITend\n')
    return tgl


# ---------------------------------------------------------------------------
# Probe order
# ---------------------------------------------------------------------------

def test_probe_order_nearer_wins(tmp_path, monkeypatch):
    """A nearer (earlier-listed) probe candidate wins over a farther one when
    both exist."""
    near = tmp_path / "near"
    far = tmp_path / "far"
    _make_tgl(near)
    _make_tgl(far)
    fake_probes = (near, far)
    monkeypatch.setattr(srcdata, "SRCDATA_ROOT_PROBES", fake_probes)
    assert srcdata.find_tgl_dir(None) == near / "tgl"


def test_probe_order_falls_back_when_nearer_missing(tmp_path, monkeypatch):
    near = tmp_path / "near"  # does not exist
    far = tmp_path / "far"
    _make_tgl(far)
    monkeypatch.setattr(srcdata, "SRCDATA_ROOT_PROBES", (near, far))
    assert srcdata.find_tgl_dir(None) == far / "tgl"


def test_env_var_override_wins_over_probes(tmp_path, monkeypatch):
    env_root = tmp_path / "env_root"
    probed_root = tmp_path / "probed_root"
    _make_tgl(env_root)
    _make_tgl(probed_root)
    monkeypatch.setattr(srcdata, "SRCDATA_ROOT_PROBES", (probed_root,))
    monkeypatch.setenv("MC2_SRCDATA", str(env_root))
    assert srcdata.find_tgl_dir(None) == env_root / "tgl"


def test_no_probes_hit_raises_with_probe_list(tmp_path, monkeypatch):
    """DATA-PROVENANCE-CLOSE-1 changed this contract, deliberately.

    "No probe hit" is no longer the end of the road: the resolver can now
    materialise the corpus straight out of a deployed `tgl.fst`, so the tool
    works on a machine that has an install but no source checkout. The failure
    case is "no probe AND no deployed archive", and it must still name what was
    tried rather than guessing.
    """
    import stock_corpus as sc
    missing = tmp_path / "nope"
    monkeypatch.delenv("MC2_SRCDATA", raising=False)
    monkeypatch.setattr(srcdata, "SRCDATA_ROOT_PROBES", (missing,))
    monkeypatch.setattr(sc, "CANDIDATE_FST_ROOTS", ())
    with pytest.raises(RuntimeError, match="Probed"):
        srcdata.find_tgl_dir(None)


def test_synthetic_tree_is_refused_without_the_opt_out(tmp_path, monkeypatch):
    """The gate itself, with conftest's blanket opt-out removed.

    Everything else in this suite runs under MC2_STOCK_DATA_ALLOW_NONSTOCK=1
    because its fixtures are synthetic. That fixture must not be able to hide a
    regression in the verification it opts out of -- so this one test takes the
    opt-out away and asserts the default behaviour: a two-file synthetic tgl
    dir is NOT the stock data the engine ships, and is refused rather than
    silently used.
    """
    import stock_corpus as sc
    monkeypatch.delenv(sc.ENV_ALLOW_NONSTOCK, raising=False)
    monkeypatch.delenv("MC2_SRCDATA", raising=False)
    fake = tmp_path / "fake"
    _make_tgl(fake)
    with pytest.raises(RuntimeError, match="NOT the stock tgl data"):
        srcdata.find_tgl_dir(fake)

    # ... and the documented escape hatch lets it through, still flagged.
    got = srcdata.find_tgl_dir(fake, verify=False)
    assert got == fake / "tgl"


# ---------------------------------------------------------------------------
# Dispatcher
# ---------------------------------------------------------------------------

def test_dispatcher_forwards_to_edit(monkeypatch):
    called = {}

    def fake_main(argv):
        called["argv"] = argv
        return 0

    monkeypatch.setattr(mc2asset, "DISPATCH",
                        {**mc2asset.DISPATCH, "edit": fake_main})
    mc2asset.main(["edit", "status", "quonset"])
    assert called["argv"] == ["status", "quonset"]


def test_dispatcher_forwards_to_tex(monkeypatch):
    called = {}

    def fake_main(argv):
        called["argv"] = argv
        return 1

    monkeypatch.setattr(mc2asset, "DISPATCH",
                        {**mc2asset.DISPATCH, "tex": fake_main})
    rc = mc2asset.main(["tex", "quonset", "--dry-run"])
    assert rc == 1
    assert called["argv"] == ["quonset", "--dry-run"]


def test_dispatcher_list_mode(tmp_path, monkeypatch, capsys):
    root = tmp_path / "srcdata"
    tgl = _make_tgl(root)
    monkeypatch.setattr(srcdata, "SRCDATA_ROOT_PROBES", (root,))
    monkeypatch.delenv("MC2_SRCDATA", raising=False)
    rc = mc2asset.main(["list", "quon"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "quonset" in out


def test_dispatcher_unknown_subcommand_exits_two(capsys):
    rc = mc2asset.main(["bogus"])
    assert rc == 2
    assert "unknown subcommand" in capsys.readouterr().err


def test_dispatcher_no_subcommand_exits_two(capsys):
    rc = mc2asset.main([])
    assert rc == 2
