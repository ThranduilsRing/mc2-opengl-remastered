"""Only --promote moves nifty: direct-merge refusal, promote tag, stage preflight."""
from test_lane_merge import _branch, _load_module, _stage_fixture, _verdict, env, git  # noqa: F401

import pytest, subprocess  # noqa: E401


# ---- only --promote moves nifty ----

def test_batch_on_nifty_is_refused_and_hotfix_is_logged(env, monkeypatch, capsys):
    repo, tools = env
    _branch(repo, "lane_a", "a_lane.txt", "a\n", "lane a")
    lm = _load_module()
    import lane_merge_stage
    monkeypatch.setattr(lane_merge_stage, "NIFTY", "main")      # the fixture's checked-out branch plays nifty
    old = git(repo, "rev-parse", "main")
    rc = lm.main(["--batch", "lane_a", "--root", str(repo), "--tools-dir", str(tools)])
    err = capsys.readouterr().err
    assert rc == 5 and "DIRECT MERGE REFUSED" in err and "--stage W --batch" in err
    assert git(repo, "rev-parse", "main") == old
    lm.main(["--hotfix", "--batch", "lane_a", "--root", str(repo), "--tools-dir", str(tools)])
    assert "HOTFIX direct merge" in capsys.readouterr().out


def test_promote_tags_the_green_candidate(env, monkeypatch):
    lm, repo, old, cand = _stage_fixture(env, monkeypatch)
    _verdict(repo, head=cand)
    assert lm.promote(str(repo), "t1") == 0
    tag = f"nifty-green-t1-{cand[:9]}"
    assert git(repo, "cat-file", "-t", tag) == "tag" and git(repo, "rev-parse", tag + "^{commit}") == cand


def test_stage_refused_by_a_red_preflight_blocks_promote(env, monkeypatch):
    lm, repo, old, cand = _stage_fixture(env, monkeypatch)
    import lane_merge_stage
    real = subprocess.run
    mode = {"rc": 1}

    def fake(cmd, *a, **k):
        if "merge_gate.py" not in " ".join(map(str, cmd)):
            return real(cmd, *a, **k)
        out = "[merge_gate] cheap row FAIL: dup_check failed before the build (rc=1): x" if mode["rc"] else ""
        return subprocess.CompletedProcess(cmd, mode["rc"], out, "")
    monkeypatch.setattr(lane_merge_stage.subprocess, "run", fake)
    reds = lm.stage_preflight(str(repo), "t1")
    assert reds and "dup_check" in reds[0]
    _verdict(repo, head=cand)
    with pytest.raises(lm.Stop) as e:
        lm.promote(str(repo), "t1")
    assert e.value.code == 6 and "dup_check" in str(e.value)
    assert git(repo, "rev-parse", "main") == old
    mode["rc"] = 0
    assert lm.stage_preflight(str(repo), "t1") == []
