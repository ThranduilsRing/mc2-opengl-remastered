"""sim_determinism: the comparator that proves two runs (or two commits) agree on the simulation."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sim_determinism as sd  # noqa: E402


def test_selftest_passes():
    assert sd.selftest() == 0


def test_logparse_and_the_tool_agree_on_the_chain():
    sys.path.insert(0, os.path.join(HERE, ".."))
    from scripts.smoke_lib.logparse import parse_log
    lines = [f"{sd.PREFIX}{i} t=0.0 n=1 graded=a visual=b strict=c" for i in range(50)]
    s = parse_log("\n".join(["noise"] + lines + ["[HEARTBEAT] frames=1"]) + "\n")
    assert s.simid_frames == 50 and s.simid_chain == sd.chain(lines)


def test_a_log_without_simid_has_no_chain():
    sys.path.insert(0, os.path.join(HERE, ".."))
    from scripts.smoke_lib.logparse import parse_log
    s = parse_log("[HEARTBEAT] frames=1\n")
    assert s.simid_frames == 0 and s.simid_chain is None
