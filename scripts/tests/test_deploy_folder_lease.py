"""deploy_payload holds the smoke folder lease while it writes into an install, and waits for a live holder instead of overwriting the install."""
import json
import os
import sys
import time

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from scripts import deploy_payload  # noqa: E402

# deploy_payload loads the lease module by whichever import path works for it; the test must redirect THAT module object, never the shared registry.
deploy_lease = deploy_payload._deploy_lease_module()


@pytest.fixture()
def registry(tmp_path, monkeypatch):
    monkeypatch.setattr(deploy_lease, "LEASE_REGISTRY_PATH", tmp_path / "leases.json")
    return tmp_path / "leases.json"


def _foreign_lease(folder):
    deploy_lease.acquire_lease(str(folder), "someone else's smoke")


def test_registry_lock_never_steals_an_old_live_holder(registry, monkeypatch):
    with deploy_lease._registry_lock():
        path = deploy_lease._lock_path()
        os.utime(path, (0, 0))
        monkeypatch.setattr(deploy_lease, "_LOCK_WAIT_SECS", 0)
        with pytest.raises(TimeoutError):
            with deploy_lease._registry_lock():
                pytest.fail("a live lock was stolen")
    with deploy_lease._registry_lock():
        pass


def test_registry_lock_supports_package_import(tmp_path, monkeypatch):
    from pathlib import Path
    from scripts.smoke_lib import deploy_lease as packaged
    monkeypatch.setattr(packaged, "LEASE_REGISTRY_PATH", tmp_path / "packaged.json")
    scripts_dir = Path(__file__).resolve().parents[1]
    monkeypatch.setattr(sys, "path", [p for p in sys.path if Path(p).resolve() != scripts_dir])
    monkeypatch.delitem(sys.modules, "build_tokens", raising=False)
    with packaged._registry_lock():
        pass


def test_a_free_install_is_leased_for_the_deploy_and_released_after(registry, tmp_path):
    install = tmp_path / "install"
    install.mkdir()
    assert deploy_payload.take_folder_lease(str(install), 0)
    assert len(json.loads(registry.read_text(encoding="utf-8"))) == 1
    deploy_payload.release_folder_lease(str(install))
    assert json.loads(registry.read_text(encoding="utf-8")) == {}


def test_a_live_lease_of_another_session_refuses_a_deploy_with_no_wait(registry, tmp_path):
    install = tmp_path / "install"
    install.mkdir()
    _foreign_lease(install)
    with pytest.raises(SystemExit):
        deploy_payload.take_folder_lease(str(install), 0)
    assert len(json.loads(registry.read_text(encoding="utf-8"))) == 1     # the holder's lease is untouched


def test_the_deploy_waits_for_the_holder_and_then_goes_ahead(registry, tmp_path, monkeypatch):
    install = tmp_path / "install"
    install.mkdir()
    _foreign_lease(install)
    bounded_wait = sys.modules[deploy_lease.__name__.rsplit('.', 1)[0] + '.bounded_wait']
    real_wait = bounded_wait.wait_until
    ticks = []

    def wait_and_release(probe, what, wait_s, *a, **k):
        ticks.append(probe())                                  # still busy now
        deploy_lease.release_lease(str(install))               # the holder finishes
        return real_wait(probe, what, wait_s, *a, **k)

    monkeypatch.setattr(bounded_wait, "wait_until", wait_and_release)
    assert deploy_payload.take_folder_lease(str(install), 30)
    assert ticks and ticks[0] != ""
    deploy_payload.release_folder_lease(str(install))


def test_a_dead_owner_on_this_host_is_taken_over_at_once(registry, tmp_path, monkeypatch):
    install = tmp_path / "install"
    install.mkdir()
    _foreign_lease(install)
    monkeypatch.setattr(deploy_lease, "_pid_alive", lambda pid: False)   # the owner (this test process) is "dead"
    assert deploy_payload.take_folder_lease(str(install), 0)
