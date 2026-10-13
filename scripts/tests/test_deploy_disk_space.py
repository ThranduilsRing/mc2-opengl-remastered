"""Disk failures must stop deployment before any payload is copied."""
from pathlib import Path
from types import SimpleNamespace
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts import deploy_payload as dp


@pytest.mark.parametrize("free,expected", [(9, 3), (10, 1), (15, 0)])
def test_destination_volume_and_exact_threshold(monkeypatch, free, expected):
    probes = []
    def usage(path):
        probes.append(Path(path))
        return SimpleNamespace(free=free)
    monkeypatch.setattr(dp.shutil, "disk_usage", usage)
    rc, _facts = dp.disk_space(ROOT / "disk-space-test-missing" / "target", 10, 5)
    assert rc == expected
    assert probes == [ROOT]


def test_deploy_refuses_low_space_and_probe_failure(monkeypatch):
    monkeypatch.delenv("MC2_DEPLOY_SKIP_DISK_GUARD", raising=False)
    monkeypatch.setattr(dp.shutil, "disk_usage", lambda path: SimpleNamespace(free=0))
    with pytest.raises(SystemExit):
        dp.check_free_space_for_payload([], str(ROOT))
    def inaccessible(path):
        raise OSError("unreadable volume")
    monkeypatch.setattr(dp.shutil, "disk_usage", inaccessible)
    with pytest.raises(SystemExit):
        dp.check_free_space_for_payload([], str(ROOT))


def test_payload_headroom_and_floor(monkeypatch):
    monkeypatch.delenv("MC2_DEPLOY_SKIP_DISK_GUARD", raising=False)
    required = []
    monkeypatch.setattr(dp, "disk_space", lambda path, need, **kwargs: (required.append(need) or 0, {}))
    dp.check_free_space_for_payload([], str(ROOT))
    monkeypatch.setattr(dp.os.path, "getsize", lambda path: 4 * 1024 ** 3)
    dp.check_free_space_for_payload([("payload", "payload", "file")], str(ROOT))
    assert required == [2 * 1024 ** 3, (4 * 1024 ** 3 * 115 + 99) // 100]


@pytest.mark.parametrize("flags", [["--need-bytes", "-1"], ["--need-gb", "nan"],
    ["--need-gb", "inf"], ["--need-bytes", "0", "--warn-gb", "-1"]])
def test_invalid_threshold_is_an_error(flags):
    assert dp.disk_space_main(flags) == 2
