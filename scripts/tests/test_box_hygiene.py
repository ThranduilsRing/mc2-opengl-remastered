"""Box hygiene: BOX address file, disk guard (BOX_INFRA), lane cleanup, STATUS.json, core map, GPU fallback, row log names, hooks.
No box, no ssh: the remote bash snippets run under local bash with a fake df; ssh callers are mocked."""
import importlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "box" / "filler"))
sys.path.insert(0, str(ROOT / "scripts"))
import box_run
import console_child

BOX = ROOT / "tools" / "box"
BASH = shutil.which("bash") or "bash"   # not a bare "bash": on Windows CreateProcess finds the WSL launcher, which mangles $ in -c text


def bash(script, env=None, **kw):
    return subprocess.run([BASH, "-c", script], capture_output=True, text=True, env={**os.environ, **(env or {})},
                          **console_child.child_kwargs(redirected=True, **kw))


def between(text, a, b):
    i = text.index(a)
    return text[i:text.index(b, i) + len(b)]


# --- BOX address file ---------------------------------------------------------------------------------------------

def test_box_file_parsed_and_comments_skipped(tmp_path):
    f = tmp_path / "BOX"
    f.write_text("# comment\nhost=root@1.2.3.4\nport=999\nkey=C:/k/id\n")
    assert box_run.load_box_config(f) == {"host": "root@1.2.3.4", "port": "999", "key": "C:/k/id"}


def test_missing_box_file_falls_back_to_constants(tmp_path):
    assert box_run.load_box_config(tmp_path / "nope") == {}


def test_precedence_env_then_file_then_constants(tmp_path, monkeypatch):
    f = tmp_path / "BOX"
    f.write_text("host=root@file\nport=111\n")
    monkeypatch.setenv("MC2_BOX_FILE", str(f))
    monkeypatch.delenv("MC2_BOX_HOST", raising=False)
    monkeypatch.delenv("MC2_BOX_PORT", raising=False)
    monkeypatch.delenv("MC2_BOX_KEY", raising=False)
    m = importlib.reload(box_run)
    try:
        assert (m.HOST, m.PORT) == ("root@file", "111")
        assert m.KEY == "C:/SPB_Data/.ssh/mc2farm_ed25519"      # not in the file: the constant
        monkeypatch.setenv("MC2_BOX_HOST", "root@env")
        assert importlib.reload(box_run).HOST == "root@env"      # env beats the file
    finally:
        monkeypatch.undo()
        importlib.reload(box_run)


def test_real_box_file_has_the_three_keys():
    p = Path("A:/Games/mc2-ops/BOX")
    if not p.exists():
        pytest.skip("ops dir not on this machine")
    assert {"host", "port", "key"} <= set(box_run.load_box_config(p))


# --- disk guard ---------------------------------------------------------------------------------------------------

def guard():
    return between(box_run.REMOTE, "#@@DISK_GUARD@@", "#@@END_DISK_GUARD@@")


@pytest.mark.parametrize("avail,floor,rc", [(5, None, 77), (14, None, 77), (15, None, 0), (40, None, 0), (20, "30", 77), (20, "10", 0)])
def test_disk_guard_floor(avail, floor, rc, tmp_path):
    (tmp_path / "df").write_text(f'#!/bin/sh\necho Avail\necho "{avail}G"\n')   # a fake df first on PATH
    env = {"PATH": f"{tmp_path.as_posix()}{os.pathsep}{os.environ['PATH']}"}
    r = bash('unset MC2_BOX_MIN_FREE_GB\n' + (f"export MC2_BOX_MIN_FREE_GB={floor}\n" if floor else "") + guard() + "\necho PASSED", env=env)
    assert r.returncode == rc, r.stdout + r.stderr
    assert ("BOX_INFRA" in r.stdout) == (rc == 77)
    assert ("PASSED" in r.stdout) == (rc == 0)


def test_guard_runs_before_source_and_build():
    assert box_run.REMOTE.index("#@@DISK_GUARD@@") < box_run.REMOTE.index("git -C $MIRROR worktree add") < box_run.REMOTE.index("ninja -C build64-ninja")


def test_box_infra_verdict_is_not_green():
    assert box_run.verdict(77) == ("BOX_INFRA", 3)
    assert box_run.verdict(0, ("0",)) == ("PASS", 0)


def test_ccache_and_diskwatch_paths():
    dw = (BOX / "mc2-diskwatch.sh").read_text()
    assert "/workspace/lanes/*" in dw and "lane_cleanup.sh" in dw and "evict 25" in dw
    assert "ccache -M" not in dw and "ccache -c" in dw   # diskwatch trims to max_size, never shrinks or wipes the cache
    assert "ccache -M 12G" in (BOX / "deploy.sh").read_text()


# --- lane_cleanup.sh ----------------------------------------------------------------------------------------------

@pytest.fixture
def lanes(tmp_path):
    w = tmp_path / "w"
    (w / "lanes" / "zz_t" / "out" / "a1b2c3d4").mkdir(parents=True)
    (w / "lanes" / ".box_lane_zz_t.sh").write_text("x")
    m = tmp_path / "m.git"
    subprocess.run(["git", "init", "-q", "--bare", str(m)], check=True, **console_child.child_kwargs())
    (w / "locks").mkdir()
    q = tmp_path / "queue.txt"      # stand-in for `pueue status --json`: one line of command/path/label per queued or running task
    q.write_text("")
    env = {"W": w.as_posix(), "LANES": (w / "lanes").as_posix(), "MIRROR": m.as_posix(), "NO_PROC_SCAN": "1",
           "PUEUE_ACTIVE_FILE": q.as_posix(), "LOCKS": (w / "locks").as_posix(), "IDLE_MIN": "10"}
    if os.name == "nt":      # MSYS has no `ps -eo` or flock: stand-ins that report an idle machine (real liveness is covered on the box)
        shim = tmp_path / "shim.sh"
        shim.write_text("ps() { :; }\nflock() { return 0; }\n")
        env["BASH_ENV"] = shim.as_posix()
    return w, m, env


def cleanup(env, *args):
    return bash(f'bash "{(BOX / "lane_cleanup.sh").as_posix()}" ' + " ".join(args), env=env)


def test_cleanup_dry_run_changes_nothing(lanes):
    w, m, env = lanes
    r = cleanup(env, "one", "zz_t", "--dry-run")
    assert r.returncode == 0 and r.stdout.count("would:") == 4, r.stdout + r.stderr
    assert (w / "lanes" / "zz_t").is_dir() and (w / "lanes" / ".box_lane_zz_t.sh").exists()


def test_cleanup_removes_dir_script(lanes):
    w, m, env = lanes
    r = cleanup(env, "one", "zz_t")
    assert r.returncode == 0, r.stdout + r.stderr
    assert not (w / "lanes" / "zz_t").exists() and not (w / "lanes" / ".box_lane_zz_t.sh").exists()


@pytest.mark.parametrize("key", ["", "../x", "a/b", ".hidden"])
def test_cleanup_rejects_bad_keys(lanes, key):
    assert cleanup(lanes[2], "one", f"'{key}'").returncode == 2


@pytest.mark.skipif(os.name == "nt", reason="liveness uses the /proc cwd scan (a full scan takes minutes under MSYS); runs on the box")
def test_cleanup_refuses_a_lane_with_a_live_process(lanes):
    w, m, env = lanes
    p = subprocess.Popen(["sleep", "30"], cwd=w / "lanes" / "zz_t")
    try:
        r = cleanup({**env, "NO_PROC_SCAN": "0"}, "one", "zz_t")
        assert r.returncode == 3 and "RUNNING" in r.stdout
        assert (w / "lanes" / "zz_t").is_dir()
    finally:
        p.kill()
        p.wait()


@pytest.mark.skipif(not Path("/proc/self/cwd").exists(), reason="uses find -mmin and /proc (the box)")
def test_outs_drops_only_old_out_dirs(lanes):
    w, m, env = lanes
    old = w / "lanes" / "zz_t" / "out" / "old00000"
    old.mkdir()
    t = time.time() - 3 * 86400
    os.utime(old, (t, t))
    r = cleanup(env, "outs", "1440")
    assert r.returncode == 0, r.stdout + r.stderr
    assert not old.exists() and (w / "lanes" / "zz_t" / "out" / "a1b2c3d4").exists()


def test_script_syntax():
    for s in ("lane_cleanup.sh", "deploy.sh", "mc2-diskwatch.sh", "env.sh", "filler/sync.sh", "filler/worker.sh", "filler/run.sh", "filler/stop.sh"):
        r = subprocess.run([BASH, "-n", (BOX / s).as_posix()], capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
        assert r.returncode == 0, f"{s}: {r.stderr}"


def test_cron_runs_daily_and_mirror_upkeep():
    c = (BOX / "mc2-box-hygiene.cron").read_text()
    assert "lane_cleanup.sh daily" in c and "lane_cleanup.sh mirror" in c
    mirror = (BOX / "lane_cleanup.sh").read_text()
    assert "repack -d -q --geometric=2" in mirror and "worktree prune" in mirror


def test_box_run_cleanup_calls_the_remote_script(monkeypatch):
    seen = []
    monkeypatch.setattr(box_run, "run", lambda cmd, **kw: seen.append(cmd) or subprocess.CompletedProcess(cmd, 0, "ok", ""))
    assert box_run.box_cleanup("claude/some-lane", True) == 0
    assert seen[0][-1] == "bash /workspace/mc2-box/lane_cleanup.sh one claude_some-lane --dry-run"


# --- core map -----------------------------------------------------------------------------------------------------

def test_cores_env_is_the_one_map():
    env = dict(ln.split("=", 1) for ln in (BOX / "cores.env").read_text().splitlines() if re.match(r"^[A-Z0-9_]+=", ln))
    assert env["BUILD_CPUS"] == "32-63" and env["GPU0_CPUS"] == "0-7" and env["GPU3_CPUS"] == "24-31"
    assert ". /workspace/locks/cores.env" in box_run.REMOTE and "${BUILD_CPUS:-32-63}" in box_run.REMOTE
    assert "cores.env" in (BOX / "env.sh").read_text() and "cores.env" in (BOX / "filler" / "sync.sh").read_text()


def test_game_cpus_come_from_cores_env_with_old_formula_fallback(tmp_path):
    line = next(ln for ln in box_run.REMOTE.splitlines() if 'cv=GPU${N}_CPUS' in ln)
    expr = line[line.index("cv="):line.index("   #")]
    assert bash(f"N=2; GPU2_CPUS=16-23; {expr}; echo $CPUS").stdout.strip() == "16-23"
    assert bash(f"N=2; {expr}; echo $CPUS").stdout.strip() == "8-11"


# --- STATUS.json --------------------------------------------------------------------------------------------------

def test_status_build_reads_locks(tmp_path, monkeypatch):
    import status
    (tmp_path / "gpu1.owner").write_text(f"{os.getpid()} claude_x abcdef0123 2026-11-05T00:00:00+00:00")
    (tmp_path / "gpu1.row").write_text("smoke-lite\n")
    (tmp_path / "gpu2.owner").write_text("999999 gone deadbeef00 t")
    monkeypatch.setattr(status, "LOCKS", str(tmp_path))
    monkeypatch.setattr(status, "FILLER", str(tmp_path / "f"))
    monkeypatch.setattr(status, "locked_inodes", lambda: set())
    real_exists = os.path.exists
    monkeypatch.setattr(status.os.path, "exists", lambda p: str(p) == f"/proc/{os.getpid()}" or (not str(p).startswith("/proc/") and real_exists(p)))
    monkeypatch.setattr(status.shutil, "disk_usage", lambda p: type("D", (), {"free": 20 * 2**30, "used": 80 * 2**30, "total": 100 * 2**30})())
    monkeypatch.setattr(status.os, "getloadavg", lambda: (1.0, 2.0, 3.0), raising=False)
    s = status.build()
    assert s["free_gb"] == 20.0 and s["load"] == [1.0, 2.0, 3.0] and sorted(s["gpus"]) == ["0", "1", "2", "3"]
    g1 = s["gpus"]["1"]
    assert (g1["holder"], g1["alive"], g1["row"]) == ("claude_x@abcdef01", True, "smoke-lite")
    assert s["gpus"]["2"]["alive"] is False and s["gpus"]["0"]["holder"] is None


def test_status_no_id_prints_json(monkeypatch, capsys):
    monkeypatch.setattr(box_run, "run", lambda cmd, **kw: subprocess.CompletedProcess(cmd, 0, '{"free_gb": 9}\n', ""))
    monkeypatch.setattr(sys, "argv", ["box_run.py", "--status"])
    assert box_run.main() == 0
    assert '"free_gb": 9' in capsys.readouterr().out


def test_status_loop_and_row_file_wiring():
    sup = (BOX / "box_supervisor.py").read_text()
    assert "import status" in sup and "status.build(" in sup
    assert "gpu$GOT.row" in box_run.REMOTE


# --- GPU fallback, row log names, refspec -------------------------------------------------------------------------

def cand(gpu, cap0=0, strict=0, count=3):
    a = box_run.REMOTE.index('AVAILABLE=$(nvidia-smi -L')
    snip = box_run.REMOTE[a:box_run.REMOTE.index("\nGOT=\"\"\n", a)]
    fake = "\\n".join(f"GPU {i}: fake (UUID: GPU-{i})" for i in range(count))
    return bash(f'nvidia-smi() {{ printf "{fake}\\n"; }}\nGPU={gpu}; CAP0={cap0}; MC2_GPU_STRICT={strict}\n{snip}\necho "$cand"').stdout.strip()


def test_busy_gpu_falls_back_to_the_others_unless_strict():
    assert cand("1") == "1 0 2"          # requested first, then the rest: the first free one wins at once
    assert cand("1", strict=1) == "1"
    assert cand("auto") == "0 1 2" and cand("auto", cap0=1) == "0"
    assert "does not exist" in cand("3")
    assert cand("2", cap0=1) == "2 0"
    assert cand("auto", count=1) == "0" and cand("0", count=1) == "0"
    assert "does not exist" in cand("1", count=1)


def test_gpu_strict_flag_reaches_the_remote_command():
    assert "--gpu-strict" in Path(box_run.__file__).read_text() and "MC2_GPU_STRICT=1" in Path(box_run.__file__).read_text()


def test_long_free_text_row_gets_a_short_log_name():
    line = next(ln for ln in box_run.REMOTE.splitlines() if ln.lstrip().startswith("name=$(printf"))
    expr = line[line.index("name=$("):line.index('; echo "[box_run] row=')]
    row = "sh:" + "x" * 400 + " && echo 'a b/c'"
    out = bash(f'RIDX=3; row="{row}"; {expr}; echo "$name"').stdout.strip()
    assert re.fullmatch(r"row3-\d+", out) and len(out) < 30
    assert bash(f'RIDX=1; row="smoke-lite"; {expr}; echo "$name"').stdout.strip() == "smoke-lite"


def test_lane_fetch_refspec_forces():
    src = Path(box_run.__file__).read_text()
    assert "+{ref}:refs/lanes/{branch}" in src and "fetch -q -f" in src


# --- merge hooks --------------------------------------------------------------------------------------------------

def test_deploy_box_tars_tools_box_and_runs_apply(monkeypatch):
    seen = {}
    monkeypatch.setattr(box_run, "run", lambda cmd, **kw: seen.update(cmd=cmd, input=kw.get("input")) or subprocess.CompletedProcess(cmd, 0))
    assert box_run.deploy_box() == 0
    assert "deploy.sh apply" in seen["cmd"][-1] and len(seen["input"]) > 1000
    assert box_run.deploy_box("") == 0 and "deploy.sh revert" in seen["cmd"][-1]


# --- lane-dir free-space floor + eviction ---------------------------------------------------------------------------

def _touch_lock(w, key, age_days):
    d = w / "lanes" / key
    (d / "out").mkdir(parents=True, exist_ok=True)
    f = d / ".lane.lock"
    f.write_text("")
    t = time.time() - age_days * 86400
    for p in (f, d / "out", d):      # last activity = newest mtime in the dir, so age every level
        os.utime(p, (t, t))


def test_evict_dry_run_lists_oldest_first_and_spares_the_keep_lane(lanes):
    w, m, env = lanes
    _touch_lock(w, "zz_new", 1)
    _touch_lock(w, "zz_old", 9)
    _touch_lock(w, "zz_mid", 5)
    _touch_lock(w, "zz_keep", 30)
    r = cleanup({**env, "FAKE_FREE_GB": "10"}, "evict", "30", "zz_keep", "--dry-run")
    names = re.findall(r"removing lane dir (\S+)", r.stdout)
    assert "zz_keep" not in names and names.index("zz_old") < names.index("zz_mid") < names.index("zz_new"), r.stdout
    assert all((w / "lanes" / n).is_dir() for n in names)      # dry run removes nothing


def test_evict_removes_by_name_and_reports_when_nothing_is_left(lanes):
    w, m, env = lanes
    _touch_lock(w, "zz_old", 9)
    r = cleanup({**env, "FAKE_FREE_GB": "10"}, "evict", "30", "zz_t")      # the constant fake never reaches the floor
    assert r.returncode == 1 and "no evictable lane dir left" in r.stdout, r.stdout + r.stderr
    assert not (w / "lanes" / "zz_old").exists() and (w / "lanes" / "zz_t").is_dir()   # evicted the idle one, kept the keep lane
    assert "zz_t" in r.stdout and "unique=" in r.stdout      # the holders table follows the failure
    assert re.search(r" removed zz_old reason=evict floor=30GB", (w / "lanes" / "EVICTIONS.log").read_text())


def test_evict_is_a_no_op_above_the_floor(lanes):
    w, m, env = lanes
    r = cleanup({**env, "FAKE_FREE_GB": "99"}, "evict", "30")
    assert r.returncode == 0 and "0 lane dir(s) evicted" in r.stdout and (w / "lanes" / "zz_t").is_dir()


def _evict_all(lanes_fixture):
    w, m, env = lanes_fixture
    r = cleanup({**env, "FAKE_FREE_GB": "10"}, "evict", "30")
    return w, r


def test_evict_never_removes_a_lane_with_a_queued_pueue_task(lanes):
    """the w121-combo defect: the lane had no process and no lock between two queued tasks"""
    w, m, env = lanes
    for k in ("zz_combo", "zz_cmd", "zz_idle"):
        _touch_lock(w, k, 9)
    (w / "lanes" / ".box_lane_zz_combo.sh").write_text("x")
    Path(env["PUEUE_ACTIVE_FILE"]).write_text(
        "bash /workspace/lanes/.box_lane_zz_combo.sh /workspace\n/workspace/lanes/zz_cmd/box_tests.sh /workspace/lanes/zz_cmd\n")
    w, r = _evict_all(lanes)
    assert (w / "lanes" / "zz_combo").is_dir() and (w / "lanes" / ".box_lane_zz_combo.sh").exists(), r.stdout
    assert (w / "lanes" / "zz_cmd").is_dir()
    assert not (w / "lanes" / "zz_idle").exists()
    assert "QUEUED" in r.stdout      # the holders table says why they stayed


def test_evict_never_removes_a_lane_with_a_want_file(lanes):
    w, m, env = lanes
    _touch_lock(w, "zz_wait", 9)
    (w / "locks" / "zz_wait.want").write_text("")
    w, r = _evict_all(lanes)
    assert (w / "lanes" / "zz_wait").is_dir() and "WANT" in r.stdout


def test_evict_removes_nothing_when_the_queue_is_unreadable(lanes):
    w, m, env = lanes
    _touch_lock(w, "zz_old", 9)
    env = {k: v for k, v in env.items() if k != "PUEUE_ACTIVE_FILE"}
    r = cleanup({**env, "PUEUE": (w / "no_such_pueue").as_posix(), "FAKE_FREE_GB": "10"}, "evict", "30")
    assert (w / "lanes" / "zz_old").is_dir() and "QUEUE-UNKNOWN" in r.stdout, r.stdout


def test_evict_spares_a_recently_active_lane(lanes):
    w, m, env = lanes
    _touch_lock(w, "zz_fresh", 0)
    w, r = _evict_all(lanes)
    assert (w / "lanes" / "zz_fresh").is_dir() and "RECENT" in r.stdout


def test_one_refuses_a_queued_or_wanted_lane(lanes):
    w, m, env = lanes
    Path(env["PUEUE_ACTIVE_FILE"]).write_text("bash /workspace/lanes/.box_lane_zz_t.sh\n")
    r = cleanup(env, "one", "zz_t")
    assert r.returncode == 3 and "QUEUED" in r.stdout and (w / "lanes" / "zz_t").is_dir()
    Path(env["PUEUE_ACTIVE_FILE"]).write_text("")
    (w / "locks" / "zz_t.want").write_text("")
    r = cleanup(env, "one", "zz_t")
    assert r.returncode == 3 and "WANT" in r.stdout and (w / "lanes" / "zz_t").is_dir()


def test_every_removal_is_logged_with_its_reason(lanes):
    w, m, env = lanes
    assert cleanup(env, "one", "zz_t").returncode == 0
    log = (w / "lanes" / "EVICTIONS.log").read_text()
    assert re.match(r"\d{4}-\d\d-\d\dT[\d:]+Z removed zz_t reason=cleanup", log)
    _touch_lock(w, "zz_dry", 9)
    cleanup(env, "one", "zz_dry", "--dry-run")
    assert "zz_dry" not in (w / "lanes" / "EVICTIONS.log").read_text()      # a dry run logs nothing


def test_holders_reports_unique_size_age_and_last_activity(lanes):
    w, m, env = lanes
    _touch_lock(w, "zz_big", 3)
    (w / "lanes" / "zz_big" / "blob").write_bytes(b"x" * 3 * 1024 * 1024)
    for p in ("blob", ""):      # writing the blob touched the dir: age both again
        os.utime(w / "lanes" / "zz_big" / p, (time.time() - 3 * 86400,) * 2)
    r = cleanup(env, "holders")
    line = next(l for l in r.stdout.splitlines() if "zz_big" in l)
    assert re.search(r"unique=\s*[3-9] MB\s+age=\s*\d+ h\s+last_activity=\s*4[0-9]{3} min ago", line), r.stdout


def test_status_prints_the_eviction_log_and_the_remote_guards_against_eviction(monkeypatch, capsys):
    calls = []

    def fake_run(cmd, **kw):
        calls.append(cmd[-1])
        out = '{"gpus": []}' if "STATUS.json" in cmd[-1] else "2026-11-05T01:02:03Z removed claude_x reason=evict floor=30GB"
        return subprocess.CompletedProcess(cmd, 0, out, "")
    monkeypatch.setattr(box_run, "run", fake_run)
    assert box_run.box_status() == 0
    out = capsys.readouterr().out
    assert any("EVICTIONS.log" in c for c in calls) and "removed claude_x reason=evict" in out
    blk = between(box_run.REMOTE, "#@@LANE_FLOOR@@", "#@@END_LANE_FLOOR@@")
    assert "EVICTIONS.log" in blk      # a lane that was evicted is told why when it next starts
    assert "LWANT=/workspace/locks/$BR.want" in box_run.REMOTE and "touch $WANTF $LWANT" in box_run.REMOTE
    assert 'rm -f "$LWANT"' in box_run.REMOTE      # the want file goes away with the run


def test_remote_checks_the_lane_floor_before_creating_the_lane_dir():
    r = box_run.REMOTE
    blk = between(r, "#@@LANE_FLOOR@@", "#@@END_LANE_FLOOR@@")
    assert "[ ! -d $L ]" in blk and "MC2_BOX_LANE_FLOOR_GB:-30" in blk and "lane_cleanup.sh evict" in blk and "exit 77" in blk
    assert r.index("#@@END_LANE_FLOOR@@") < r.index("mkdir -p /workspace/locks $L $OUT")
    assert "*" not in blk.replace("*/", "")      # no wildcard deletion anywhere in the floor block
