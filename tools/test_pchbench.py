"""pchbench: the .ninja_log summing and the gain rule (no build)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pchbench as pb  # noqa: E402

LOG = "\n".join([
    "# ninja log v6",
    "0\t1000\t0\tCMakeFiles/mc2.dir/code/a.cpp.obj\tabc",
    "0\t3000\t0\tCMakeFiles/mc2.dir/code/b.cpp.obj\tabc",
    "0\t4000\t0\tout/mclib/CMakeFiles/mclib.dir/x.cpp.obj\tabc",
    "0\t500\t0\tCMakeFiles/mc2.dir/cmake_pch.cxx.obj\tabc",
    "0\t700\t0\tout/other/CMakeFiles/other.dir/y.cpp.obj\tabc",
    "garbage line",
])


def test_ninja_log_sums_groups_by_target_and_separates_the_pch(tmp_path):
    log = tmp_path / ".ninja_log"
    log.write_text(LOG, encoding="utf-8")
    cpu, count, pch = pb.ninja_log_sums(str(log))
    assert cpu["mc2"] == 4000 and count["mc2"] == 2 and cpu["mclib"] == 4000 and "other" not in cpu
    assert pch["mc2"] == 500


def test_gain_counts_the_pch_build_against_the_saving():
    assert abs(pb.gain(1000, 700) - 0.3) < 1e-9
    assert abs(pb.gain(1000, 700, 100) - 0.2) < 1e-9
    assert pb.gain(0, 5) == 0.0


def test_report_rows(tmp_path):
    for name, text in (("off", LOG.replace("cmake_pch.cxx.obj", "z.cpp.obj")), ("on", LOG)):
        d = tmp_path / name
        d.mkdir()
        (d / ".ninja_log").write_text(text, encoding="utf-8")
    rows = {r[0]: r for r in pb.report(str(tmp_path / "off"), str(tmp_path / "on"))}
    assert rows["mclib"][6] == 0.0 and rows["mc2"][5] == 0.5
