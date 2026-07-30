"""DATA-PROVENANCE-CLOSE-1 -- test-suite policy for stock-data verification.

`srcdata.find_tgl_dir` / `find_srcdata` now byte-verify whatever tree they
return against the shipped `tgl.fst` (see tools/mc2data/stock_corpus.py). Most
of this suite builds SYNTHETIC two-file tgl dirs under `tmp_path` to exercise
probe ORDER, asset resolution and CLI plumbing -- those trees are non-stock by
construction, and the resolver is right to refuse them.

So the suite declares the opt-out the resolver provides for exactly this case,
rather than each test being edited to smuggle a flag through. Note what this
does NOT do: it does not disable verification, it makes the refusal
non-fatal and keeps the loud stderr banner. `test_unify.py::
test_synthetic_tree_is_refused_without_the_opt_out` covers the default (no
opt-out) path, so this fixture cannot hide a regression in the gate itself.
"""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools" / "mc2data"))
from stock_corpus import ENV_ALLOW_NONSTOCK  # noqa: E402


@pytest.fixture(autouse=True)
def _allow_synthetic_srcdata(monkeypatch):
    monkeypatch.setenv(ENV_ALLOW_NONSTOCK, "1")
