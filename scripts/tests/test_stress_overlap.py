"""stress_overlap judges each run against the serial reference over the common frame prefix."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import stress_overlap as so


def lines(n, salt=""):
    return [f"[SIMID v1] simframe={i} graded=aa{salt} rng=1/{i}/3" for i in range(n)]


def row(tag, ln, violation=""):
    return {"mission": "m", "tag": tag, "rc": 0, "frames": len(ln), "violation": violation, "aborted": False, "_lines": ln}


def test_audit_levels_keep_fence_and_jitter_without_retired_gates():
    for jitter, level in ((0, "2"), (6, "8"), (50, "52")):
        env = so.stress_env(jitter)
        assert env["MC2_SIM_OVERLAP_AUDIT"] == level
        assert "MC2_OVERLAP_DEBUG" not in env and "MC2_SIM_OVERLAP_JITTER" not in env
    assert so.serial_env()["MC2_SIM_OVERLAP_AUDIT"] == "1"


def test_cosmetic_rng_counts_do_not_split_hashes():
    a = so.simid_lines("\n".join(f"[SIMID v1] simframe=1 rng=5/{k}/{k}" for k in (1,)))
    b = so.simid_lines("[SIMID v1] simframe=1 rng=5/9/9")
    assert a == b


def test_interleaved_console_lines_keep_every_record():
    # Another thread's line glued after a record, and a record glued after another line.
    text = ("[SIMID v1] simframe=1 rng=5/1/1 animrng=2[SHADOW_BAKE] re-recorded\n"
            "samples=601[SIMID v1] simframe=2 rng=6/1/1 animrng=2s\n")
    assert so.simid_lines(text) == ["[SIMID v1] simframe=1 rng=5 animrng=2", "[SIMID v1] simframe=2 rng=6 animrng=2"]


def test_green_on_matching_prefix_red_on_mismatch_or_violation():
    ok = so.judge([row("serial", lines(400)), row("jitter1", lines(350))])
    assert ok["green"] and ok["frames_compared"] == 350
    bad = so.judge([row("serial", lines(400)), row("jitter1", lines(400, "x"))])
    assert not bad["green"] and "simid_mismatch" in bad["runs"][1]["red"]
    v = so.judge([row("serial", lines(400)), row("jitter1", lines(400), so.VIOLATION + " kind site=x")])
    assert "violation" in v["runs"][1]["red"]
    short = so.judge([row("serial", lines(400)), row("jitter1", lines(10))])
    assert not short["green"]
    failed = row("jitter1", lines(400))
    failed["rc"] = 1
    assert "rc=1" in so.judge([row("serial", lines(400)), failed])["runs"][1]["red"]
