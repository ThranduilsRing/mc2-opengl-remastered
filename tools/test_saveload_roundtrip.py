"""saveload_roundtrip: the analysis that compares the simulation after a quickload with the one after the quicksave."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import saveload_roundtrip as rt  # noqa: E402


def test_selftest_passes():
    assert rt.selftest() == 0


def test_mover_detail_is_reported_for_the_first_divergent_frame():
    pre = [f"[SIMID v1] simframe={i} t={i / 30:.3f} n=1 graded=g{i} visual=v strict=s" for i in range(50)]
    pre += ["[SIMID v1 mover] simframe=20 id=7 gesture=1 pstep=0/3"]
    post = [f"[SIMID v1] simframe={50 + i} t={(20 + i) / 30:.3f} n=1 graded={'g%d' % (20 + i) if i else 'BAD'} visual=v strict=s" for i in range(20)]
    post += ["[SIMID v1 mover] simframe=50 id=7 gesture=0 pstep=0/3"]
    ok, rep = rt.analyze(pre + post, frames_needed=10)
    text = "\n".join(rep)
    assert not ok and "DIVERGE 0 frame" in text and "id 7" in text and "gesture=0" in text


def test_a_save_that_differs_names_its_packet_and_offset(tmp_path):
    pk = [(0, b"ver\0"), (4, b"a" * 64), (4, b"b" * 64)]
    other = [(0, b"ver\0"), (4, b"a" * 64), (4, b"b" * 10 + b"X" + b"b" * 53)]
    for name, p in (("a.ims", pk), ("b.ims", other)):
        rt.si.scs.write(str(tmp_path / name), p, False)
    ok, rep = rt.resave_verdict(str(tmp_path / "a.ims"), str(tmp_path / "b.ims"))
    assert not ok and "packet 2" in " ".join(rep) and "offset 10" in " ".join(rep)
