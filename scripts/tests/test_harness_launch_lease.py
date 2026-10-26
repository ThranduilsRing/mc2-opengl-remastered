"""mechbay_preview_harness and campaign_save_roundtrip hold the host game lease (named holder) across spawn; spawn is mocked."""
import contextlib
import os
import subprocess
import sys

import pytest

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from scripts.smoke_lib import host_lease  # noqa: E402
from smoke_lib import host_lease as host_lease_alias, nofocus  # noqa: E402
import mechbay_preview_harness as mbh  # noqa: E402
import campaign_save_roundtrip as csr  # noqa: E402


class _Proc:
    returncode = 0

    def poll(self):
        return self.returncode

    def wait(self, timeout=None):
        return 0

    def kill(self):
        pass


@pytest.fixture()
def seen(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_HOST_LEASE_PATH", str(tmp_path / "lease.json"))
    monkeypatch.setenv("MC2_HOST_GAME_SLOTS", "2")
    for mod in (host_lease, host_lease_alias):
        monkeypatch.setattr(mod, "_registered", False)
        monkeypatch.setattr(mod, "foreign_engines", lambda: [])
    record = {}

    def fake_spawn(argv, **kw):
        record["env"] = kw.get("env", {})
        recs = [r for _, r in host_lease_alias._records() if r]
        record["holds"] = host_lease_alias.holds()
        record["labels"] = [r.get("label") for r in recs]
        record["exclusive"] = [bool(r.get("exclusive")) for r in recs]
        return _Proc()

    monkeypatch.setattr(nofocus, "spawn", fake_spawn)
    yield record
    host_lease_alias.release()
    host_lease.release()


def _deploy(tmp_path):
    d = tmp_path / "deploy"
    d.mkdir()
    (d / "mc2.exe").write_bytes(b"")
    return d


def test_mechbay_harness_holds_named_slot(seen, tmp_path, monkeypatch):
    d = _deploy(tmp_path)
    monkeypatch.setattr(sys, "argv", ["mechbay_preview_harness.py", "bay", "--deploy", str(d)])
    with pytest.raises(SystemExit):
        mbh.main()
    assert seen["holds"] and seen["labels"] and all(l == "mechbay_preview_harness bay capture" for l in seen["labels"])
    assert not any(seen["exclusive"])
    assert "MC2_LOG_PREVIEW" not in seen["env"]


def test_mechbay_preview_log_only_on_flag(seen, tmp_path, monkeypatch):
    d = _deploy(tmp_path)
    monkeypatch.setenv("MC2_LOG_PREVIEW", "1")
    monkeypatch.setattr(sys, "argv", ["mechbay_preview_harness.py", "bay", "--deploy", str(d)])
    with pytest.raises(SystemExit):
        mbh.main()
    assert "MC2_LOG_PREVIEW" not in seen["env"]
    monkeypatch.setattr(sys, "argv", ["mechbay_preview_harness.py", "bay", "--deploy", str(d), "--preview-log"])
    with pytest.raises(SystemExit):
        mbh.main()
    assert seen["env"]["MC2_LOG_PREVIEW"] == "1"


def test_mainmenu_capture_stays_in_front_end(seen, tmp_path, monkeypatch):
    d = _deploy(tmp_path)
    monkeypatch.setattr(sys, "argv", ["mechbay_preview_harness.py", "mainmenu", "--deploy", str(d)])
    with pytest.raises(SystemExit):
        mbh.main()
    assert "MC2_BOOT_TO_BAY" not in seen["env"]
    assert "MC2_SOAK_AUTOWIN" not in seen["env"]


def test_mechbay_busy_lease_fails_without_spawning(seen, tmp_path, monkeypatch):
    d = _deploy(tmp_path)
    monkeypatch.setattr(sys, "argv", ["mechbay_preview_harness.py", "bay", "--deploy", str(d)])
    def busy(*a, **k):
        raise host_lease_alias.HostLeaseBusy("held")
    monkeypatch.setattr(mbh.host_lease, "acquire", busy)
    with pytest.raises(SystemExit) as e:
        mbh.main()
    assert e.value.code == 2 and "holds" not in seen


def test_campaign_roundtrip_holds_named_lease(seen, tmp_path, monkeypatch):
    d = _deploy(tmp_path)
    monkeypatch.setattr(csr.savegame_backup, "default_dir", lambda: str(tmp_path / "saves"))
    monkeypatch.setattr(csr.savegame_backup, "guarded", lambda sdir: contextlib.nullcontext())
    (tmp_path / "saves").mkdir()
    rc = csr.main(["capture", "--out", str(tmp_path / "out"), "--deploy", str(d), "--keep-going"])
    assert rc == 0
    assert seen["holds"] and all(l == csr.LEASE_LABEL for l in seen["labels"])
