"""pak_gate: a pak-default install fails the smoke unless the deploy said OK, the pak mounted with eligible > 0 and fst_opens == 0."""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.smoke_lib import pak_gate  # noqa: E402

GOOD = ("[VFS] mount pak name=paks/stock.mc2pak files=8176 eligible=8000\n"
        "[PAK_IO v1] pak_opens=500 pak_bytes_in=1 pak_bytes_out=2 pak_decode_ms=1.0 pak_crc_ms=1.0 fst_opens=0 fst_bytes_in=0 fst_bytes_out=0 fst_read_ms=0.0\n")


def _install(tmp_path, status):
    if status is not None:
        (tmp_path / pak_gate.STATUS_NAME).write_text(status + "\n")
    return tmp_path


def test_pass_pak_served(tmp_path):
    assert pak_gate.pak_failures(_install(tmp_path, "OK key=k"), GOOD) == []


def test_pre_pak_install_is_not_judged(tmp_path):
    assert pak_gate.pak_failures(_install(tmp_path, None), "") == []
    assert pak_gate.pak_failures(_install(tmp_path, "SKIP no FST set"), "") == []


def test_fail_deploy_fell_back(tmp_path):
    out = pak_gate.pak_failures(_install(tmp_path, "FALLBACK mc2pak pack failed rc=1"), GOOD)
    assert len(out) == 1 and "[PAK_DEPLOY] OK" in out[0]


def test_fail_no_mount_or_zero_eligible(tmp_path):
    ok = _install(tmp_path, "OK key=k")
    assert any("eligible>0" in m for m in pak_gate.pak_failures(ok, ""))
    assert any("eligible>0" in m for m in pak_gate.pak_failures(ok, GOOD.replace("eligible=8000", "eligible=0")))


def test_fail_fst_opens(tmp_path):
    out = pak_gate.pak_failures(_install(tmp_path, "OK key=k"), GOOD.replace("fst_opens=0", "fst_opens=3"))
    assert len(out) == 1 and "fst_opens=3" in out[0]
