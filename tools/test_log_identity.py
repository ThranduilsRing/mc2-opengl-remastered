"""log_identity: line shapes with numbers masked, and the one-sided report (no game)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import log_identity as li  # noqa: E402


def _dir(tmp_path, name, text):
    d = tmp_path / name
    d.mkdir()
    (d / "m.log").write_text(text, encoding="utf-8")
    return str(d)


def test_numbers_and_addresses_are_masked():
    assert li.shape("[PERF v1] avg_fps=61.3 p99_ms=17.25 ptr=0x7FF612345678 id=-4") == "[PERF v1] avg_fps=# p99_ms=# ptr=# id=#"


def test_equal_runs_with_different_numbers_compare_equal(tmp_path):
    a = _dir(tmp_path, "a", "[TIMING v1] event=x elapsed_ms=12\nplain 5\n")
    b = _dir(tmp_path, "b", "[TIMING v1] event=x elapsed_ms=987\nplain 6\nplain 7\n")
    assert li.compare(a, b) == ([], [], True)


def test_a_changed_spelling_or_key_order_or_padding_is_reported(tmp_path):
    a = _dir(tmp_path, "a", "[X v1] name=abc   n=1\n[Y] a=1 b=2\n")
    b = _dir(tmp_path, "b", "[X v1] name=abc n=1\n[Y] b=2 a=1\n")
    only_b, only_a, order_ok = li.compare(a, b)
    assert len(only_b) == 2 and len(only_a) == 2 and order_ok


def test_a_lost_tag_changes_the_tag_set(tmp_path):
    a = _dir(tmp_path, "a", "[A v1] x=1\n[B v1] y=1\n[A v1] x=2\n")
    b = _dir(tmp_path, "b", "[A v1] x=1\n[A v1] x=2\n")
    assert li.compare(a, b)[2] is False


def test_known_run_to_run_noise_is_ignored(tmp_path):
    a = _dir(tmp_path, "a", "[BUILD_FINGERPRINT v1] sha=abc123456789 built=1\nsame 1\n")
    b = _dir(tmp_path, "b", "[BUILD_FINGERPRINT v1] sha=def123456789 built=2\nsame 2\n")
    assert li.compare(a, b) == ([], [], True)


def test_the_frame_2_frustum_plane_dump_is_no_longer_noise(tmp_path):
    a = _dir(tmp_path, "a", "[TerrainLOD v1] DIAG plane[0]=left (nan) PASS\n")
    b = _dir(tmp_path, "b", "[TerrainLOD v1] DIAG plane[0]=left (0.0) FAIL\n")
    only_b, only_a, _ = li.compare(a, b)
    assert only_b and only_a


def test_interleaved_tag_order_is_not_a_difference_but_a_missing_tag_is(tmp_path):
    a = _dir(tmp_path, "a", "[A v1] x=1\n[B v1] y=1\n[A v1] x=2\n")
    b = _dir(tmp_path, "b", "[B v1] y=1\n[A v1] x=1\n[A v1] x=2\n")
    assert li.compare(a, b)[2] is True


def test_number_padding_is_not_a_difference_but_text_padding_is(tmp_path):
    a = _dir(tmp_path, "a", "[TIME] t=  5s phase=x\nname=abc   n=1\n")
    b = _dir(tmp_path, "b", "[TIME] t= 12s phase=x\nname=abc n=1\n")
    only_b, only_a, _ = li.compare(a, b)
    assert len(only_b) == 1 and "name=abc" in only_b[0] and len(only_a) == 1
