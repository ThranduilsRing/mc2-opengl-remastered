"""One lane model: deploy_lease, check-deploy-target and repo_intel agree on the lane names."""
import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tools", "repo_intel"))

import lane_model  # noqa: E402
from smoke_lib import deploy_lease  # noqa: E402


def _cdt():
    spec = importlib.util.spec_from_file_location("check_deploy_target", os.path.join(ROOT, "scripts", "check-deploy-target.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_lane2_is_a_known_deploy_name_but_not_part_of_the_unattended_walk():
    assert deploy_lease._NAME_TO_PATH["lane2"] == lane_model.LANES["lane2"]["install"]
    assert "lane2" in [n for n, _ in deploy_lease.DEPLOY_FOLDERS]
    assert deploy_lease._WALK_LANES == ("0.4", "0.4c")


def test_good_dirs_follow_the_model_and_leave_out_the_golden():
    names = [n for n, _ in _cdt().GOOD_DIRS]
    assert names == [n for n in lane_model.LANES if n != "0.4c"]
    assert "lane2" in names
