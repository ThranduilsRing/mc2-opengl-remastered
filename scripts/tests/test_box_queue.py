"""box_run --queue / --wait / --status and the single-ssh run, with ssh mocked (no box, no network)."""
import base64
import io
import json
from pathlib import Path
import sys
import tarfile
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import box_run


def tasks(**by_id):
    return json.dumps({"tasks": by_id})


def test_rows_travel_as_one_base64_argument():
    assert base64.b64decode(box_run.rows_b64(["smoke-lite", "capture:mc2_01"])).decode() == "smoke-lite\ncapture:mc2_01"


def test_remote_script_truncates_the_append_only_tsvs_and_prints_lock_wait():
    assert 'rm -f "$OUT"/rows.tsv "$OUT"/fingerprints.tsv' in box_run.REMOTE
    assert "lock_wait" in box_run.REMOTE


@pytest.mark.parametrize("status,state,rc", [
    ({"Running": {"start": "x"}}, "Running", None),
    ("Queued", "Queued", None),
    ({"Done": {"result": "Success"}}, "Done", 0),
    ({"Done": {"result": {"Failed": 3}}}, "Done", 3),
])
def test_task_state_reads_pueue_status_json(monkeypatch, status, state, rc):
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: tasks(**{"7": {"status": status, "label": "box_run:b:abcd1234:gpu1"}}))
    assert box_run.task_state(7) == (state, rc, "box_run:b:abcd1234:gpu1")


def test_task_state_missing_task(monkeypatch):
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: tasks())
    assert box_run.task_state(9)[0] == "missing"


def test_status_prints_one_line_and_exit_code_2_while_running(monkeypatch, capsys, tmp_path):
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: tasks(**{"4": {"status": {"Running": {}}, "label": "box_run:b:abcd1234:gpu0"}}))
    assert box_run.task_verdict(4, tmp_path, wait=False) == 2
    assert capsys.readouterr().out.strip().splitlines() == ["[box_run] task=4 state=Running rc=-"]


def test_wait_blocks_fetches_reports_and_prints_one_verdict_line(monkeypatch, capsys, tmp_path):
    calls = []
    monkeypatch.setattr(box_run, "run", lambda cmd, **kw: calls.append(cmd[-1]))
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: tasks(**{"5": {"status": {"Done": {"result": "Success"}}, "label": "box_run:br:abcd1234:gpu2"}}))

    def fake_fetch(out_remote, local_out):
        assert out_remote == "/workspace/lanes/br/out/abcd1234"
        (Path(local_out) / "rows.tsv").write_text("smoke-lite\t0\ncapture\t1\n")
    monkeypatch.setattr(box_run, "fetch_back", fake_fetch)
    assert box_run.task_verdict(5, tmp_path, wait=True) == 1   # a row failed although the script rc was 0: never PASS
    assert any("pueue wait 5" in c for c in calls)
    lines = capsys.readouterr().out.strip().splitlines()
    assert len(lines) == 1 and "state=Done rc=0 rows=1/2" in lines[0]


def test_failed_task_exits_1(monkeypatch, tmp_path):
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: tasks(**{"6": {"status": {"Done": {"result": {"Failed": 1}}}, "label": "x"}}))
    assert box_run.task_verdict(6, tmp_path, wait=False) == 1


def test_pick_gpu_takes_the_least_loaded(monkeypatch):
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: "0 2\n1 0\n2 1\n")
    assert box_run.pick_gpu() == "1"


def test_fetch_back_extracts_what_the_box_tarred(monkeypatch, tmp_path):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tf:
        data = b"smoke-lite\t0\n"
        ti = tarfile.TarInfo("./rows.tsv")
        ti.size = len(data)
        tf.addfile(ti, io.BytesIO(data))
    monkeypatch.setattr(box_run.subprocess, "run", lambda cmd, **kw: SimpleNamespace(stdout=buf.getvalue(), returncode=0))
    box_run.fetch_back("/workspace/x", tmp_path)
    assert (tmp_path / "rows.tsv").read_text() == "smoke-lite\t0\n"
    assert "*.tsv" in box_run.BACK and "*.json" in box_run.BACK and "tail -n 200" in box_run.BACK


def queued_main(monkeypatch, tmp_path, argv, stdout="12\n"):
    sent = []

    def fake_run(cmd, **kw):
        sent.append((cmd[-1], kw.get("input")))
        return SimpleNamespace(returncode=0, stdout=stdout if "pueue add" in cmd[-1] else "", stderr="")
    monkeypatch.setattr(box_run, "run", fake_run)
    monkeypatch.setattr(box_run, "git", lambda *a, repo=None: ("br" if "--abbrev-ref" in a else "deadbeef" * 5) if a[0] == "rev-parse" else "")
    monkeypatch.setattr(box_run, "pick_gpu", lambda: "2")
    monkeypatch.setattr(box_run.subprocess, "run", lambda cmd, **kw: SimpleNamespace(stdout="deadbeef" * 5, returncode=0))
    monkeypatch.setattr(sys, "argv", ["box_run.py", "--repo", str(tmp_path), *argv])
    return box_run.main(), sent


def test_queue_submits_one_ssh_to_the_group_and_returns_the_task_id(monkeypatch, tmp_path, capsys):
    rc, sent = queued_main(monkeypatch, tmp_path, ["--queue", "--gpu", "1", "determinism"])
    assert rc == 0
    pueue = [s for s in sent if "pueue add" in s[0]]
    assert len(pueue) == 2   # build task (build group, no GPU slot) then the GPU task that waits on it
    assert "-g build " in pueue[0][0] and base64.b64encode(b"build").decode() in pueue[0][0]
    cmd, stdin = pueue[1]
    assert "-g gpu1 -a 12 --print-task-id" in cmd and stdin == box_run.REMOTE.encode()
    assert base64.b64encode(b"determinism").decode() in cmd
    assert "queued task=12 group=gpu1" in capsys.readouterr().out


def test_queue_gate_goes_to_the_gate_group(monkeypatch, tmp_path):
    rc, sent = queued_main(monkeypatch, tmp_path, ["--queue", "--gate"])
    assert rc == 0 and any("-g gate -a 12 --print-task-id" in s[0] for s in sent)


def test_blocking_mode_stays_the_default_one_ssh_run(monkeypatch, tmp_path):
    monkeypatch.setattr(box_run, "fetch_back", lambda *a: None)
    rc, sent = queued_main(monkeypatch, tmp_path, ["--gpu", "0", "determinism"])
    assert not any("pueue" in s[0] for s in sent)
    assert sum("bash /workspace/lanes/.box_lane_br.sh" in s[0] for s in sent) == 1


# ---- box-run-fixes lane: defects 1-9 -------------------------------------------------------------------------------------------

SHA = "deadbeef" * 5


def harness(monkeypatch, tmp_path, argv, have="", box_has_rc=0, bundle_rc=0, abbrev="br"):
    """Mocked ssh/git/scp: returns (rc, calls) where calls = [(argv, stdin)] for every box_run.run and subprocess.run."""
    calls = []

    def fake_run(cmd, **kw):
        calls.append((cmd, kw.get("input")))
        last = cmd[-1]
        if cmd[0] == "git" and "bundle" in cmd:
            return SimpleNamespace(returncode=bundle_rc, stdout="", stderr="fatal: Refusing to create empty bundle.")
        rc = box_has_rc if "cat-file -e" in last else 0
        return SimpleNamespace(returncode=rc, stdout="9\n" if "pueue add" in last else "", stderr="")

    def fake_subprocess(cmd, **kw):
        calls.append((cmd, None))
        return SimpleNamespace(stdout=have, returncode=0)
    monkeypatch.setattr(box_run, "run", fake_run)
    monkeypatch.setattr(box_run.subprocess, "run", fake_subprocess)
    monkeypatch.setattr(box_run, "git", lambda *a, repo=None: (abbrev if "--abbrev-ref" in a else SHA) if a[0] == "rev-parse" else "")
    monkeypatch.setattr(box_run, "fetch_back", lambda *a: None)
    monkeypatch.setattr(sys, "argv", ["box_run.py", "--repo", str(tmp_path), *argv])
    return box_run.main(), calls


def remote_cmds(calls):
    return [c[0][-1] for c in calls if c[0][0] == "ssh"]


def test_sha_already_on_the_box_sends_no_bundle(monkeypatch, tmp_path):
    # defect 1: sha is an ancestor of the excluded mirror refs (not a ref tip): the bundle would be empty; the box has the commit, so send nothing
    rc, calls = harness(monkeypatch, tmp_path, ["--gpu", "0", "sh:true"], have="a" * 40, box_has_rc=0)
    assert not any("bundle" in c[0] for c in calls if c[0][0] == "git")
    assert any(f"update-ref refs/lanes/br {SHA}" in c for c in remote_cmds(calls))


def test_failed_bundle_prints_stderr_and_stops_when_the_box_lacks_the_sha(monkeypatch, tmp_path, capsys):
    rc, calls = harness(monkeypatch, tmp_path, ["--gpu", "0", "sh:true"], have="a" * 40, box_has_rc=1, bundle_rc=128)
    assert rc == 1
    assert "Refusing to create empty bundle" in capsys.readouterr().err


def test_fetch_force_updates_and_prunes_the_stale_lane_ref(monkeypatch, tmp_path):
    rc, calls = harness(monkeypatch, tmp_path, ["--gpu", "0", "sh:true"], have="a" * 40, box_has_rc=1, bundle_rc=0)
    fetch = [c for c in remote_cmds(calls) if " fetch " in c]
    assert len(fetch) == 1 and "update-ref -d refs/lanes/br" in fetch[0] and "fetch -q -f" in fetch[0] and "+refs/box-run/br:refs/lanes/br" in fetch[0]


def test_detached_head_gets_its_own_lane_dir():
    assert box_run.lane_key("HEAD", SHA) == "detached-deadbeef"
    assert box_run.lane_key("", SHA) == "detached-deadbeef"
    assert box_run.lane_key("claude/x", SHA) == "claude_x"


def test_detached_run_uses_the_detached_lane_in_every_path(monkeypatch, tmp_path):
    rc, calls = harness(monkeypatch, tmp_path, ["--gpu", "0", "sh:true"], have=SHA, abbrev="HEAD")
    run_cmd = [c for c in remote_cmds(calls) if "bash /workspace/lanes/.box_lane_" in c][0]
    assert ".box_lane_detached-deadbeef.sh" in run_cmd and "/workspace/lanes/detached-deadbeef/out/deadbeef" in run_cmd


def test_only_the_script_piping_calls_may_keep_stdin_open(monkeypatch, tmp_path):
    assert "-n" in box_run.SSH and "-n" not in box_run.SSH_STDIN
    rc, calls = harness(monkeypatch, tmp_path, ["--gpu", "0", "sh:true"], have=SHA)
    for cmd, stdin in calls:
        if cmd[0] == "ssh":
            assert (stdin is not None) == ("-n" not in cmd), cmd[-1]


def bash_or_skip():
    import shutil
    b = shutil.which("bash")
    if not b:
        pytest.skip("bash not available")
    return b


def test_every_attempt_gets_a_fresh_determinism_out_dir(tmp_path):
    # defect 5 (correctness): sim_determinism refuses an existing --out, so a retried row was VOID. Run the real helper from the remote script.
    import subprocess
    bash = bash_or_skip()
    d = (tmp_path / "out" / "determinism_mc2_01").as_posix()
    script = box_run.FRESH_FN + f'\nfor i in 1 2 3; do x=$(fresh "{d}"); mkdir -p "$x"; echo "$x"; done'
    out = subprocess.run([bash, "-c", script], capture_output=True, text=True).stdout.split()
    assert out == [d + ".a0", d + ".a1", d + ".a2"]
    assert box_run.REMOTE.count("$(fresh $OUT/$name)") >= 2 and "--out $OUT/$name" not in box_run.REMOTE
    assert "#@@FRESH_FN@@" not in box_run.REMOTE and box_run.FRESH_FN in box_run.REMOTE


def test_no_baseline_is_its_own_verdict_and_never_green():
    assert "rc=78" in box_run.REMOTE and "baseline FAIL no baseline at" in box_run.REMOTE
    assert box_run.verdict(1, ["0", "78"]) == ("NO_BASELINE", 4)
    assert box_run.verdict(0, ["0", "78"]) == ("NO_BASELINE", 4)     # script rc is never the only witness
    assert box_run.verdict(1, ["78", "1"]) == ("FAIL", 1)            # a real failure beside it wins
    assert box_run.verdict(0, ["0"]) == ("PASS", 0)
    for rc, codes in ((1, ["78"]), (0, ["78"]), (76, [])):
        assert box_run.verdict(rc, codes)[1] != 0


def test_no_baseline_row_in_a_gate_receipt_is_not_pass(tmp_path):
    (tmp_path / "rows.tsv").write_text("determinism:mc2_01\t78\nsaveload\t0\n")
    rec = box_run.write_receipt(tmp_path, SHA, "br", ["determinism:mc2_01", "saveload"], 1)
    assert [r["verdict"] for r in rec["rows"]] == ["NO_BASELINE", "PASS"]
    assert box_run.gate_verdict(rec) == 1


def test_lock_timeout_is_a_distinct_rc_and_verdict(tmp_path, capsys):
    assert box_run.RC_LOCK_TIMEOUT == 76 and "exit 76" in box_run.REMOTE and "exit 75" not in box_run.REMOTE
    assert box_run.verdict(76, []) == ("LOCK_TIMEOUT", 76)
    rec = box_run.write_receipt(tmp_path, SHA, "br", ["determinism:mc2_01"], 76)
    assert rec["rows"][0]["verdict"] == "LOCK_TIMEOUT"
    assert box_run.gate_verdict(rec) == 3   # not green; no product verdict, the gate falls back to the Windows rows


def test_task_status_line_carries_the_verdict(monkeypatch, capsys, tmp_path):
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: tasks(**{"8": {"status": {"Done": {"result": {"Failed": 76}}}, "label": "box_run:b:abcd1234:gpu0"}}))
    monkeypatch.setattr(box_run, "fetch_back", lambda *a: None)
    assert box_run.task_verdict(8, tmp_path, wait=False) == 76
    assert "verdict=LOCK_TIMEOUT" in capsys.readouterr().out


def test_sh_rows_with_spaces_and_slashes_are_passed_as_argv_not_spliced(tmp_path):
    import subprocess
    bash = bash_or_skip()
    assert 'eval "$2"' in box_run.REMOTE and '_ "$S" "$cmd"' in box_run.REMOTE
    # the real name sanitiser and the real argv/eval hand-off, on a row with spaces, quotes and slashes
    row = "sh:echo 'a b' > out/x.txt; ls /tmp | wc -l"
    script = ("row=$1; name=$(printf %s \"$row\" | tr -c 'A-Za-z0-9._-' _); case $row in sh:*) name=${name:0:50}-$(printf %s \"$row\" | cksum | cut -d' ' -f1);; esac\n"
              "cmd=\"${row#sh:}\"; mkdir -p \"$2/out\"; bash -c 'cd \"$1\" && eval \"$2\"' _ \"$2\" \"$cmd\" >/dev/null; echo \"$name\"; cat \"$2/out/x.txt\"")
    r = subprocess.run([bash, "-c", script, "_", row, tmp_path.as_posix()], capture_output=True, text=True)
    lines = r.stdout.split("\n")
    assert "/" not in lines[-3] and " " not in lines[-3] and lines[-2] == "a b", r.stdout + r.stderr


def test_presubmit_warns_when_the_tip_lacks_the_box_clang_fix(monkeypatch, capsys):
    monkeypatch.setattr(box_run.subprocess, "run", lambda cmd, **kw: SimpleNamespace(returncode=1))
    assert box_run.check_behind_box_fix(".") == 1
    assert "WARNING" in capsys.readouterr().err
    monkeypatch.setattr(box_run.subprocess, "run", lambda cmd, **kw: SimpleNamespace(returncode=0))
    assert box_run.check_behind_box_fix(".") == 0 and capsys.readouterr().err == ""


def test_copy_back_never_tars_the_home_dir_when_the_out_dir_is_missing():
    assert box_run.BACK.startswith("cd {out} || exit 0;")


def test_deploy_never_requires_files_the_lane_tree_did_not_build():
    # a lane without crashpad (e.g. shadow-redirty) has no build64-ninja/crashpad_handler: deploy copies it only when present
    assert "[ -f build64-ninja/$opt ] &&" in box_run.REMOTE
    assert "DEPLOY FAILED: crashpad_handler" not in box_run.REMOTE


def test_unit_row_configures_the_doctest_tier_with_the_box_clang_toolchain():
    # defect: a bare `cmake -S tests/unit` on the box found no compiler ("CMAKE_CXX_COMPILER not set")
    i = box_run.REMOTE.index("    unit)")
    row = box_run.REMOTE[i:box_run.REMOTE.index("    sh:*)", i)]
    assert "-S tests/unit" in row and "-DCMAKE_CXX_COMPILER=clang++-20" in row and "-DCMAKE_C_COMPILER=clang-20" in row and "mc2_tests" in row and "-stdlib=libc++" in row


def test_build_and_unit_rows_queue_in_the_build_group_and_hold_no_gpu_slot(monkeypatch, tmp_path):
    rc, sent = queued_main(monkeypatch, tmp_path, ["--queue", "--gpu", "1", "build"])
    pueue = [s for s in sent if "pueue add" in s[0]]
    assert rc == 0 and len(pueue) == 1 and "-g build --print-task-id" in pueue[0][0] and "-g gpu" not in pueue[0][0]


def test_build_runs_before_the_gpu_lock_and_on_cores_32_63():
    r = box_run.REMOTE
    assert r.index("ninja -C build64-ninja") < r.index("flock -n 9") < r.index("gr bash -c")   # build, then lock, then rows
    assert "BCPUS=32-63" in r and "-j16 mc2" in r
    assert "case $r in build|unit) ;; *) NEEDGPU=1" in r   # build-only / unit-only rows never take the lock
    assert "    build)" in r
