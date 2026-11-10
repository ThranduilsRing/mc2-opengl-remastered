"""box_run --queue / --wait / --status and the single-ssh run, with ssh mocked (no box, no network)."""
import base64
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import box_run
import console_child  # noqa: E402  (box_run put scripts/ on sys.path)


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
    ({"Done": "Success"}, "Done", 0),
    ({"Done": {"Failed": 1}}, "Done", 1),
    ({"Done": "Killed"}, "Done", 77),
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
    monkeypatch.setattr(box_run, "run", lambda cmd, **kw: (calls.append(cmd[-1]), SimpleNamespace(returncode=0))[1])
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: tasks(**{"5": {"status": {"Done": {"result": "Success"}}, "label": "box_run:br:abcd1234:gpu2"}}))

    def fake_fetch(out_remote, local_out):
        assert out_remote == "/workspace/lanes/br/out/abcd1234"
        (Path(local_out) / "rows.tsv").write_text("smoke-lite\t0\ncapture\t1\n")
    monkeypatch.setattr(box_run, "fetch_back", fake_fetch)
    assert box_run.task_verdict(5, tmp_path, wait=True) == 1   # a row failed although the script rc was 0: never PASS
    assert any("pueue wait 5" in c for c in calls)
    lines = capsys.readouterr().out.strip().splitlines()
    assert len(lines) == 1 and "state=Done rc=0 rows=1/2" in lines[0]


def test_unlabelled_task_is_infra(monkeypatch, tmp_path):
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: tasks(**{"6": {"status": {"Done": {"result": {"Failed": 1}}}, "label": "x"}}))
    assert box_run.task_verdict(6, tmp_path, wait=False) == 77


def test_pick_gpu_takes_the_least_loaded(monkeypatch):
    monkeypatch.setattr(box_run, "gpu_ids", lambda: ["0", "1", "2"])
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: "0 2\n1 0\n2 1\n")
    assert box_run.pick_gpu() == "1"


def test_one_gpu_busy_submissions_stay_on_gpu_zero(monkeypatch):
    seen = []

    def fake_ssh(cmd):
        seen.append(cmd)
        return "GPU 0: NVIDIA GeForce RTX 4060 (UUID: GPU-fake)\n" if cmd == "nvidia-smi -L" else "0 1\n"

    monkeypatch.setattr(box_run, "ssh_out", fake_ssh)
    assert box_run.pick_gpu() == "0"
    assert box_run.pick_gpu() == "0"  # the second task queues in the same gpu0 group
    assert all("for g in 0;" in cmd for cmd in seen if cmd != "nvidia-smi -L")


def test_no_discovered_gpu_fails_closed(monkeypatch):
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: "")
    with pytest.raises(RuntimeError, match="no GPUs discovered"):
        box_run.pick_gpu()
@pytest.mark.parametrize("result,tsv,expected,word", [
    ("Success", "sh:true\t0\nsh:exit 0\t0\n", 0, "PASS"),
    ({"Failed": 1}, "sh:true\t0\nsh:false\t1\n", 1, "sh:false"),
    ({"Failed": 2}, "sh:true\t0\n", 77, "BOX_INFRA"),
    ({"Failed": 1}, "determinism:mc2_01\t78\n", 4, "NO_BASELINE"),
    ("Success", "", 77, "BOX_INFRA"),
    ({"Failed": 76}, "", 76, "LOCK_TIMEOUT"),
    ({"Failed": 77}, "", 77, "BOX_INFRA"),
])
def test_wait_real_pueue_shapes_and_row_table(monkeypatch, tmp_path, capsys, result, tsv, expected, word):
    monkeypatch.setattr(box_run, "run", lambda *a, **kw: SimpleNamespace(returncode=0))
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: tasks(**{"5": {"status": {"Done": result}, "label": "box_run:br:abcd1234:build"}}))
    monkeypatch.setattr(box_run, "local_reports", lambda *a: tmp_path)
    monkeypatch.setattr(box_run, "fetch_back", lambda remote, local: (local / "rows.tsv").write_text(tsv))
    assert box_run.task_verdict(5, tmp_path, wait=True) == expected
    lines = capsys.readouterr().out.strip().splitlines()
    assert len(lines) == 1 and word in lines[0] and "table=" in lines[0]
    assert json.loads(lines[0].split("table=", 1)[1]) == [ln.split("\t") for ln in tsv.splitlines()]


def test_wait_rejects_missing_requested_row(monkeypatch, tmp_path, capsys):
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: tasks(**{"5": {"status": {"Done": "Success"}, "label": "box_run:br:abcd1234:build"}}))
    monkeypatch.setattr(box_run, "local_reports", lambda *a: tmp_path)
    def fetch(remote, local):
        (local / "rows.tsv").write_text("sh:true\t0\n")
        (local / "requested.tsv").write_text("sh:true\nsh:false\n")
    monkeypatch.setattr(box_run, "fetch_back", fetch)
    assert box_run.task_verdict(5, tmp_path, wait=False) == 77
    assert "missing requested rows" in capsys.readouterr().out


def test_copy_back_failure_cannot_reuse_stale_green_rows(monkeypatch, tmp_path, capsys):
    (tmp_path / "rows.tsv").write_text("sh:true\t0\n")
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: tasks(**{"5": {"status": {"Done": "Success"}, "label": "box_run:br:abcd1234:build"}}))
    monkeypatch.setattr(box_run, "local_reports", lambda *a: tmp_path)
    monkeypatch.setattr(box_run.subprocess, "run", lambda *a, **kw: SimpleNamespace(returncode=255, stdout=b""))
    assert box_run.task_verdict(5, tmp_path, wait=False) == 77
    assert not (tmp_path / "rows.tsv").exists()
    assert "copy-back failed" in capsys.readouterr().out


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


def queued_main(monkeypatch, tmp_path, argv, stdout="12\n", picked="2"):
    sent = []

    def fake_run(cmd, **kw):
        sent.append((cmd[-1], kw.get("input")))
        return SimpleNamespace(returncode=0, stdout=stdout if "pueue add" in cmd[-1] else "", stderr="")
    monkeypatch.setattr(box_run, "run", fake_run)
    monkeypatch.setattr(box_run, "git", lambda *a, repo=None: ("br" if "--abbrev-ref" in a else "deadbeef" * 5) if a[0] == "rev-parse" else "")
    if picked is not None:
        monkeypatch.setattr(box_run, "pick_gpu", lambda: picked)
        monkeypatch.setattr(box_run, "gpu_ids", lambda: ["0", "1", "2"])
    monkeypatch.setattr(box_run.subprocess, "run", lambda cmd, **kw: SimpleNamespace(stdout="deadbeef" * 5, returncode=0))
    monkeypatch.setattr(sys, "argv", ["box_run.py", "--repo", str(tmp_path), *argv])
    return box_run.main(), sent


def test_one_gpu_queue_uses_existing_busy_group(monkeypatch, tmp_path):
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: "GPU 0: RTX 4060 (UUID: GPU-fake)\n" if cmd == "nvidia-smi -L" else "0 1\n")
    for _ in range(2):
        rc, sent = queued_main(monkeypatch, tmp_path, ["--queue", "gpu:true"], picked=None)
        assert rc == 0
        assert any("-g gpu0 -a 12" in cmd for cmd, _ in sent)
        assert not any("-g gpu1" in cmd or "-g gpu2" in cmd for cmd, _ in sent)


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


def test_slot_dry_run_reuses_path_across_branches(monkeypatch, tmp_path, capsys):
    repo = tmp_path / "mc2-slot02"
    repo.mkdir()
    for branch, sha in (("claude/first", "a" * 40), ("claude/second", "b" * 40)):
        monkeypatch.setattr(box_run, "git", lambda *a, repo=None: (branch if "--abbrev-ref" in a else sha) if a[0] == "rev-parse" else "")
        monkeypatch.setattr(box_run, "run", lambda *a, **kw: pytest.fail("dry-run contacted the box"))
        monkeypatch.setattr(sys, "argv", ["box_run.py", "--repo", str(repo), "--dry-run", "--gpu", "0", "build"])
        assert box_run.main() == 0
        out = capsys.readouterr().out
        assert "lane=/workspace/lanes/box-lane-02 " in out
        assert f"branch={branch} sha={sha}" in out
        assert str(box_run.local_reports(repo, sha[:8])) in out


def test_slot_submission_reuses_checkout_build_and_install(monkeypatch, tmp_path):
    repo = tmp_path / "mc2-slot02"
    repo.mkdir()
    for branch in ("claude/first", "claude/second"):
        rc, calls = harness(monkeypatch, repo, ["--gpu", "0", "build"], have=SHA, abbrev=branch)
        cmd = next(c for c in remote_cmds(calls) if "bash /workspace/lanes/.box_lane_" in c)
        assert ".box_lane_box-lane-02.sh" in cmd
        assert "/workspace/lanes/box-lane-02/out/deadbeef" in cmd
        assert cmd.endswith(branch.replace("/", "_"))
    assert "L=/workspace/lanes/$BR; S=$L/src; I=$L/install" in box_run.REMOTE


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


def test_every_attempt_gets_a_fresh_determinism_out_dir(tmp_path):
    # defect 5 (correctness): sim_determinism refuses an existing --out, so a retried row was VOID.
    # The function text is checked, not executed (no bash spawn): it probes <dir>.a0, .a1, ... and prints the first free name.
    assert 'while [ -e "$d.a$n" ]; do n=$((n+1)); done' in box_run.FRESH_FN and 'printf %s "$d.a$n"' in box_run.FRESH_FN
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
    # The generated script text is checked, not executed (no bash spawn).
    assert """bash -c 'cd "$1" && eval "$2"' _ "$S" "$cmd" """.strip() in box_run.REMOTE   # the row travels as argv, never spliced into the command line
    assert "tr -c 'A-Za-z0-9._-' _" in box_run.REMOTE                                      # the log name is sanitised (spaces, quotes, slashes)
    assert 'sh:*|cpu:*)  cmd="${row#*:}"' in box_run.REMOTE


def test_presubmit_warns_when_the_tip_lacks_the_box_clang_fix(monkeypatch, capsys):
    monkeypatch.setattr(box_run.subprocess, "run", lambda cmd, **kw: SimpleNamespace(returncode=1))
    assert box_run.check_behind_box_fix(".") == 1
    assert "WARNING" in capsys.readouterr().err
    monkeypatch.setattr(box_run.subprocess, "run", lambda cmd, **kw: SimpleNamespace(returncode=0))
    assert box_run.check_behind_box_fix(".") == 0 and capsys.readouterr().err == ""


def test_copy_back_never_tars_the_home_dir_when_the_out_dir_is_missing():
    assert box_run.BACK.startswith("set -euo pipefail; cd {out};")


def test_deploy_never_requires_files_the_lane_tree_did_not_build():
    # a lane without crashpad (e.g. shadow-redirty) has no build64-ninja/crashpad_handler: deploy copies it only when present
    assert "[ -f build64-ninja/$opt ] &&" in box_run.REMOTE
    assert "DEPLOY FAILED: crashpad_handler" not in box_run.REMOTE


def test_unit_row_configures_the_doctest_tier_with_the_box_clang_toolchain():
    # defect: a bare `cmake -S tests/unit` on the box found no compiler ("CMAKE_CXX_COMPILER not set")
    i = box_run.REMOTE.index("    unit)")
    row = box_run.REMOTE[i:box_run.REMOTE.index("    build)", i)]
    assert "-S \\$PWD/tests/unit" in row and "box_exclude.txt" in row and "-DCMAKE_CXX_COMPILER=clang++-20" in row and "-DCMAKE_C_COMPILER=clang-20" in row and "mc2_tests" in row and "-stdlib=libc++" in row


def test_build_and_unit_rows_queue_in_the_build_group_and_hold_no_gpu_slot(monkeypatch, tmp_path):
    rc, sent = queued_main(monkeypatch, tmp_path, ["--queue", "--gpu", "1", "build"])
    pueue = [s for s in sent if "pueue add" in s[0]]
    assert rc == 0 and len(pueue) == 1 and "-g build --print-task-id" in pueue[0][0] and "-g gpu" not in pueue[0][0]


def test_build_runs_before_the_gpu_lock_and_on_cores_32_63():
    r = box_run.REMOTE
    assert r.index("ninja -C build64-ninja") < r.index("take_lock; LOCKED=1") < r.index("$RUN timeout -k 30")   # build, then lazy lock, then rows
    assert "BCPUS=${BUILD_CPUS:-32-63}" in r and "-j16 mc2" in r
    assert "    build)" in r


def test_filler_push_is_noop_at_the_current_sha_and_otherwise_bundles_and_syncs(monkeypatch, tmp_path):
    import box_filler_push
    sent = []
    monkeypatch.setattr(box_run, "git", lambda *a, repo=None: SHA)
    def fake_run(cmd, **kw):
        sent.append((cmd[-1], kw.get("input")))
        out = SHA if "current_sha" in cmd[-1] and "cat >" not in cmd[-1] else ""
        return SimpleNamespace(returncode=0, stdout=out, stderr="")
    monkeypatch.setattr(box_filler_push, "run", fake_run)
    assert box_filler_push.main(["--repo", str(tmp_path)]) == 0 and len(sent) == 1   # already current: nothing uploaded, no sync
    sent.clear()
    monkeypatch.setattr(box_filler_push, "run", lambda cmd, **kw: (sent.append((cmd[-1], kw.get("input"))), SimpleNamespace(returncode=0, stdout="", stderr=""))[1])
    monkeypatch.setattr(box_filler_push.subprocess, "run", lambda cmd, **kw: (Path(cmd[6]).write_bytes(b"B") if "bundle" in cmd else None, SimpleNamespace(returncode=0, stdout="", stderr=""))[1])
    assert box_filler_push.main(["--repo", str(tmp_path)]) == 0
    assert any("incoming/nifty.bundle" in c for c, _ in sent) and any("-g build" in c and "sync.sh" in c for c, _ in sent)


def test_only_game_rows_take_the_lock_cpu_rows_run_first_under_timeout():
    r = box_run.REMOTE
    assert "case $r in build|unit|unit-san|lint|cpu:*|sh:*|determinism:*|saveload*) CR+=" in r and 'ROWS=("${CR[@]}" "${GR[@]}")' in r   # cpu rows first
    assert "unit-san)" in r and "build64-tests-san" in r and "-fsanitize=address,undefined" in r   # sanitizer row: own tree
    assert "build|unit|unit-san|lint|cpu:*|sh:*) RUN=grc;; determinism:*|saveload*) RUN=grh;; *) RUN=gr; [ -z \"$LOCKED\" ] && { take_lock" in r      # lock lazily, game rows only
    # r7 finding 3: the headless determinism/saveload rows (no window, no device) run on the build cores and never take the GPU lock
    assert 'grh() { nice -n 19 taskset -c "$BCPUS" xvfb-run' in r
    assert "$RUN timeout -k 30 $TMO bash -c" in r and "unit) TMO=1200" in r and "smoke*|capture:*|screen:*) TMO=600" in r and "*) TMO=1800" in r
    assert "sh:*|cpu:*)  cmd=" in r and "gpu:*)" in r
    assert "(SECONDS - LW0)" in r   # the 30 min lock wait is not eaten by the build


def test_exit_trap_clears_the_owner_file_and_lane_lock_serialises_runs():
    r = box_run.REMOTE
    assert "rm -f /workspace/locks/$SB.owner" in r and r.index("trap '") < r.index("take_lock()")
    assert "exec 8>$L/.lane.lock; flock 8" in r and "8>&-" in r
    assert "CCACHE_BASEDIR=/workspace CCACHE_NOHASHDIR=1" in r


def test_ssh_options_fail_fast_and_script_is_uploaded_under_a_temp_name(monkeypatch, tmp_path):
    o = " ".join(box_run.SSH)
    assert "BatchMode=yes" in o and "ConnectTimeout=15" in o and "StrictHostKeyChecking=accept-new" in o
    rc, sent = queued_main(monkeypatch, tmp_path, ["--queue", "--gpu", "1", "determinism"])
    assert all(".new.$$ && mv -f" in c for c, _ in sent if "pueue add" in c)


def test_cap0_is_dropped_once_head_contains_the_headless_present_fix(monkeypatch, tmp_path):
    monkeypatch.setattr(box_run, "has_vk_headless", lambda repo: True)
    rc, sent = queued_main(monkeypatch, tmp_path, ["--queue", "--gpu", "1", "smoke-lite"])
    cmd = [c for c, _ in sent if "pueue add" in c][-1]
    assert " 1 " + base64.b64encode(b"smoke-lite").decode() not in cmd and " 0 " + base64.b64encode(b"smoke-lite").decode() in cmd


def test_lint_row_is_cpu_only_and_skips_the_build():
    assert "lint" in box_run.NO_GPU_ROWS
    assert "python3 tools/lane_merge.py --lint HEAD" in box_run.REMOTE
    assert 'case $row in build|unit|unit-san|lint|cpu:*|sh:*) RUN=grc' in box_run.REMOTE   # taskset 32-63, never the GPU lock
    assert 'ONLY_LINT=1' in box_run.REMOTE and 'if [ -z "$ONLY_LINT" ]; then' in box_run.REMOTE
    assert 'grep -a "LINT FAIL"' in box_run.REMOTE


def test_gpu_lock_is_a_counting_semaphore_of_slot_files():
    r = box_run.REMOTE
    assert "KS=${MC2_BOX_GPU_SLOTS:-${GPU_SLOTS:-3}}" in r                       # default 3, cores.env GPU_SLOTS, env override
    assert "exec 9>>/workspace/locks/gpu$c.s$k; flock -n 9" in r and "SB=gpu$c.s$k" in r
    assert 'echo "$$ $BR $SHA $(date -Is)" > /workspace/locks/$SB.owner' in r    # owner/row files are per slot
    assert 'echo "$row" > /workspace/locks/$SB.row' in r
    assert "gpu=$GPU slot=$SLOT locked lock_wait_secs=" in r                    # the old grep key survives
    assert "timeout 1 tail -f /dev/null" in r and "timeout 3 tail" not in r      # 1 s poll
    assert "SB=gpu${GATE_GPU:-0}" in r                                            # the gate GPU (GATE_GPU in cores.env) keeps its one exclusive legacy lock
    ce = (Path(box_run.__file__).parent / "box" / "cores.env").read_text()
    assert "GPU_SLOTS=3" in ce and "GATE_GPU=0" in ce                             # the one-GPU box's gate GPU is its only GPU


def _slot_harness(tmp_path, k, name):
    """the real try_slot() from REMOTE against a temp lock dir: prints 'gpu slot' of the slot it got, or 'none'"""
    r = box_run.REMOTE
    fn = r[r.index("try_slot() {"):r.index("try_slot\nif [ -z")]
    script = (f'cand=0; KS={k}; GOT=""; {fn}\n'.replace("/workspace/locks", tmp_path.as_posix())
              + 'if try_slot; then echo "$GOT $SLOT"; else echo none; fi; [ -n "$HOLD" ] && read -t 5 _x <&0\n')
    return script


def test_try_slot_hands_out_k_slots_then_refuses(tmp_path):
    import shutil
    bash = shutil.which("bash")
    if not bash or not shutil.which("flock"):
        pytest.skip("bash/flock not available (the live box run covers this)")
    script = tmp_path / "t.sh"
    script.write_text(_slot_harness(tmp_path, 2, "x"))
    procs = []
    try:
        got = []
        for _ in range(3):
            p = subprocess.Popen([bash, str(script)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env={**os.environ, "HOLD": "1"},
                                 **console_child.child_kwargs(redirected=True))
            got.append(p.stdout.readline().strip()); procs.append(p)   # each holder keeps its flock until stdin closes
        assert got[:2] == ["0 0", "0 1"] and got[2] == "none"
    finally:
        for p in procs:
            p.stdin.close(); p.wait(timeout=10)
