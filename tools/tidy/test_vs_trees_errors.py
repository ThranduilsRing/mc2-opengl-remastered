"""What: tests for the failure text of the msbuild gate (vs_trees.extract_errors / save_log, proof_ext.failure_reason).
Why: txmmgr.h and vfx.h were rejected by the gate with no error text, so nobody could tell a real break from a flaky tree; a failure must carry the compiler or linker lines."""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vs_trees
import proof_ext

CL_LOG = """Microsoft (R) Build Engine
  widget.cpp
C:\\src\\widget.h(12,5): error C2065: 'gizmo': undeclared identifier [C:\\build\\widget.vcxproj]
C:\\src\\widget.h(12,5): error C2065: 'gizmo': undeclared identifier [C:\\build\\widget.vcxproj]
  1>C:\\src\\other.cpp(3): fatal error C1083: Cannot open include file: 'nothing.h': No such file
LINK : fatal error LNK1181: cannot open input file 'x.lib'
MSBUILD : error MSB1009: Project file does not exist.
    0 Error(s)
Build FAILED.
"""


def test_error_lines_are_kept_once_in_order_and_noise_is_dropped():
    out = vs_trees.extract_errors(CL_LOG)
    lines = out.splitlines()
    assert lines[0].startswith("C:\\src\\widget.h(12,5): error C2065")
    assert len([l for l in lines if "C2065" in l]) == 1                    # the repeated line is listed once
    assert any("C1083" in l and not l.startswith("1>") for l in lines) and any("LNK1181" in l for l in lines) and any("MSB1009" in l for l in lines)
    assert not any("Error(s)" in l or l.strip() == "Build FAILED." for l in lines)


def test_a_log_without_an_error_line_falls_back_to_its_tail_instead_of_nothing():
    log = "\n".join("step %d ok" % i for i in range(30)) + "\nsomething crashed\n"
    out = vs_trees.extract_errors(log, max_lines=3)
    assert "no error line" in out and "something crashed" in out and "step 0 ok" not in out
    assert vs_trees.extract_errors("") == "(the build printed nothing)"


def test_a_long_list_is_capped_and_says_how_many_were_left_out():
    log = "\n".join("f%d.cpp(1): error C2001: bad %d" % (i, i) for i in range(20))
    out = vs_trees.extract_errors(log, max_lines=5, max_chars=5000)
    assert len(out.splitlines()) == 6 and "+15 more error line(s)" in out


def test_the_full_log_is_saved_and_its_path_returned(tmp_path):
    p = vs_trees.save_log("tree-x", "mc2_tests.dir", "full text", directory=str(tmp_path / "logs"))
    assert p.endswith("tree-x__mc2_tests.dir.log")
    assert (tmp_path / "logs" / "tree-x__mc2_tests.dir.log").read_text(encoding="utf-8") == "full text"


def test_the_ledger_reason_of_a_rejected_header_carries_the_build_text():
    reason = proof_ext.failure_reason(["t::a.obj", "t::b.obj"], {"t::a.obj": "does not build: x.h(1): error C2059: syntax error", "t::b.obj": ""})
    assert reason.startswith("harness/test objects differ or fail to build: t::a.obj, t::b.obj")
    assert "C2059" in reason and "t::b.obj:" not in reason
    assert proof_ext.failure_reason(["t::a.obj"], {}) == "harness/test objects differ or fail to build: t::a.obj"
