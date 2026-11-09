"""gpu-passes box row: parser, stale-log guard, NOT MEASURED floor, dirty-worktree refusal."""
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))
import box_gpu_passes as gp
import box_run


def line(frame, span, water=None, n=120, dropped=0):
    water = f" VKIsl.Water={water:.3f}(x1.0)" if water is not None else ""
    return (f"[VK_GPU_PASSES v1] frame={frame} n={n} span_ms={span:.3f} sum_ms={span - 0.1:.3f} unattributed_ms=0.100 "
            f"scopes=3 dropped={dropped} overflow=0 | VKIsl.Terrain={span / 2:.3f}(x1.0){water} gap:VKIsl.Terrain=0.010(x1.0)")


def test_parse_skips_warmup_names_and_bad_windows():
    text = "\n".join([line(120, 9.0), line(240, 9.0)] + [line(360 + 120 * i, 4.0 + i, water=1.0) for i in range(3)]
                     + [line(900, 50.0, dropped=1), "unrelated log line", "[VK_GPU_PASSES v1] timestamps armed ns_per_tick=1.0"])
    s = gp.parse_log(text)
    assert s["Frame.span"] == [4.0, 5.0, 6.0]          # 2 warm-up windows dropped, the dropped!=0 window ignored
    assert s["VKIsl.Terrain"] == [2.0, 2.5, 3.0]
    assert s["VKIsl.Water"] == [1.0, 1.0, 1.0]
    assert not any(k.startswith("gap:") for k in s)     # gap rows are not passes
    assert gp.parse_log(text, skip_windows=0)["Frame.span"][0] == 9.0


def test_stats_floor_is_not_measured():
    assert gp.stats([1.0] * 5, floor=6) == {"n": 5, "measured": False}
    st = gp.stats([float(i) for i in range(1, 11)], floor=6)
    assert st["measured"] and st["mean"] == 5.5 and st["p50"] == 5 and st["p95"] == 10 and st["n"] == 10


def test_stale_log_is_never_counted(tmp_path):
    old = tmp_path / "old.log"
    old.write_text(line(120, 99.0))
    os.utime(old, (1000, 1000))                          # written long before this run started
    new = tmp_path / "sub" / "new.log"
    new.parent.mkdir()
    new.write_text(line(120, 1.0))
    text = gp.collect_logs(tmp_path, since=2000)
    assert "span_ms=1.000" in text and "span_ms=99.000" not in text


def test_run_pose_wipes_its_dir_and_ignores_old_logs(tmp_path, monkeypatch):
    stale = tmp_path / "mc2_01" / "20250101" / "mc2_01.log"   # an earlier run's log in the same dir
    stale.parent.mkdir(parents=True)
    stale.write_text("\n".join(line(120 * i, 99.0) for i in range(1, 9)))

    def fake_run(cmd, env, **kw):
        art = Path(env["MC2_SMOKE_ARTIFACT_ROOT"])
        assert art == tmp_path / "mc2_01" and not stale.exists()   # wiped before the game starts
        (art / "stamp").mkdir()
        (art / "stamp" / "mc2_01.log").write_text("\n".join(line(120 * i, 2.0) for i in range(1, 9)))
        assert "--ref-commit" in cmd and cmd[cmd.index("--ref-commit") + 1] == "abc"
        return subprocess.CompletedProcess(cmd, 0, stdout="ok")

    monkeypatch.setattr(gp.subprocess, "run", fake_run)
    rc, scopes = gp.run_pose("inst", tmp_path, "mc2_01", 20, ref_commit="abc")
    assert rc == 0 and set(scopes["Frame.span"]) == {2.0} and len(scopes["Frame.span"]) == 6


def result(samples_by_pose, floor=6):
    return {"floor": floor, "poses": [{"mission": m, "label": m, "rc": 0, "samples": s,
                                       "scopes": {k: gp.stats(v, floor) for k, v in s.items()}} for m, s in samples_by_pose.items()]}


def test_compare_ratios_not_measured_and_identical():
    base = result({"mc2_01": {"VKIsl.Terrain": [2.0] * 8, "VKIsl.Water": [1.0] * 3, "Frame.span": [4.0, 5.0] * 4}})
    tip = result({"mc2_01": {"VKIsl.Terrain": [1.0] * 8, "VKIsl.Water": [1.0] * 8, "Frame.span": [4.0, 5.0] * 4}})
    out = "\n".join(gp.compare(base, tip))
    terr = next(ln for ln in out.splitlines() if "VKIsl.Terrain" in ln)
    assert terr.split()[1] == "0.500" and terr.split()[2] == "0.500" and "IDENTICAL" not in terr
    water = next(ln for ln in out.splitlines() if "VKIsl.Water" in ln)
    assert "NOT MEASURED" in water and "3/8" in water and "1.000" not in water   # under the floor: no number
    assert "IDENTICAL SAMPLE LISTS" in next(ln for ln in out.splitlines() if "Frame.span" in ln)


def test_table_flags_missing_run():
    r = result({"mc2_01": {}})
    assert "NOT MEASURED: no [VK_GPU_PASSES v1]" in gp.table(r)


def test_dirty_worktree_refused_only_for_committed_state_rows():
    assert box_run.refuse_dirty(["gpu-passes"]) and box_run.refuse_dirty(["ab:claude/nifty-mendeleev"])
    assert not box_run.refuse_dirty(["smoke-lite", "unit"])


def test_row_is_wired_in_the_remote_script():
    r = box_run.REMOTE
    assert "gpu-passes)  cmd=" in r and "box_gpu_passes.py run" in r
    assert "box_gpu_passes.py compare" in r and "--ref-commit \"$AB_BEFORE\"" in r   # the before arm is stamped with its own commit
    bash = shutil.which("bash")
    if bash:
        p = subprocess.run([bash, "-n"], input=r.encode(), capture_output=True)
        assert p.returncode == 0, p.stderr.decode()
