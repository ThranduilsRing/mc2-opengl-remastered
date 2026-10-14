"""One lane model: deploy_lease, check-deploy-target and repo_intel agree on the lane names."""
import importlib.util
from contextlib import nullcontext
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tools", "repo_intel"))

import lane_model  # noqa: E402
from smoke_lib import deploy_lease  # noqa: E402
import deploy_payload  # noqa: E402


def _cdt():
    spec = importlib.util.spec_from_file_location("check_deploy_target", os.path.join(ROOT, "scripts", "check-deploy-target.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_lane2_is_a_known_deploy_name_but_not_part_of_the_unattended_walk():
    assert deploy_lease._NAME_TO_PATH["lane2"] == lane_model.LANES["lane2"]["install"]
    assert "lane2" in [n for n, _ in deploy_lease.DEPLOY_FOLDERS]
    assert deploy_lease._WALK_LANES == ("0.4",)   # the golden install is never picked unattended


def test_good_dirs_follow_the_model_and_leave_out_the_golden():
    names = [n for n, _ in _cdt().GOOD_DIRS]
    assert names == [n for n in lane_model.LANES if n != "0.4c"]
    assert "lane2" in names


def test_lane3_resolves_and_leases_by_name(monkeypatch):
    install = "A:/Games/mc2-opengl/mc2-win64-lane3"
    assert lane_model.lane_for("lane3") == ("lane3", lane_model.LANES["lane3"])
    assert lane_model.LANES["lane3"] == {
        "install": install, "worktree": None, "branch": None, "build_dir": None,
    }
    assert lane_model.lane_for(install)[0] == "lane3"
    assert os.path.basename(install) in deploy_payload.DEPLOY_ALLOWLIST
    acquired = []
    monkeypatch.setattr(deploy_lease, "_folder_has_exe", lambda folder: folder == install)
    monkeypatch.setattr(deploy_lease, "_acquire_waiting", lambda folder, *args: acquired.append(folder))
    assert deploy_lease.auto_acquire(deploy_name="lane3") == install
    assert acquired == [install]


def test_automatic_walk_considers_only_04(monkeypatch):
    considered, acquired = [], []
    monkeypatch.setattr(deploy_lease, "_registry_lock", nullcontext)
    monkeypatch.setattr(deploy_lease, "_read_registry", lambda: {})
    monkeypatch.setattr(deploy_lease, "_folder_has_exe", lambda folder: considered.append(folder) or True)
    monkeypatch.setattr(deploy_lease, "_acquire_locked", lambda folder, *args: acquired.append(folder))
    install = lane_model.LANES["0.4"]["install"]
    assert deploy_lease.auto_acquire() == install
    assert considered == acquired == [install]
