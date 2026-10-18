"""check-glm-convention.py: the column-convention glm helpers are reported (negative control) and the tree is clean."""
import importlib.util
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(os.path.dirname(HERE), "check-glm-convention.py")
spec = importlib.util.spec_from_file_location("check_glm_convention", SCRIPT)
gc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gc)


def test_selftest_probes_fire():
    assert gc.selftest() == 0


def test_each_forbidden_shape_is_reported():
    for probe in gc.PROBES_BAD:
        assert gc.scan_text(probe), probe


def test_comments_and_adapters_pass():
    assert not gc.scan_text("/* glm::translate(m, p) */ auto q = mc2::math::quatXYZW(0, 0, 0, 1);")


def test_tree_is_clean():
    assert gc.scan_tree() == []
