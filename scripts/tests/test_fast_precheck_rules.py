"""Fast-precheck rules: ungated new log tags, link-by-default for compiled inputs, perf_counter-only smoke verdicts (no game, no build)."""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from tools import lane_merge as lm  # noqa: E402
from tools import ungated_log_tags as ult  # noqa: E402
import console_child  # noqa: E402
import merge_gate as gate  # noqa: E402
from smoke_lib.gates import GateConfig, evaluate  # noqa: E402
from test_perf_counters import _ready  # noqa: E402


def _diff(*added, context=()):
    body = "".join(f" {c}\n" for c in context) + "".join(f"+{a}\n" for a in added)
    return ("diff --git a/mclib/x.cpp b/mclib/x.cpp\n--- a/mclib/x.cpp\n+++ b/mclib/x.cpp\n"
            f"@@ -10,3 +10,{len(context) + len(added)} @@\n" + body)


PRINT = '    std::fprintf(stderr, "[NEWTAG v1] event=x\\n");'


def test_ungated_tag_fails():
    bad = ult.check_diff(_diff(PRINT), set())
    assert len(bad) == 1 and "[NEWTAG]" in bad[0] and bad[0].startswith("mclib/x.cpp:10")


_HDR = "diff --git a/mclib/x.cpp b/mclib/x.cpp\n--- a/mclib/x.cpp\n+++ b/mclib/x.cpp\n"
_OLD = '-    std::fprintf(stderr, "[NEWTAG v1] event=x\\n");\n'


def test_rewritten_print_with_same_tag_passes():
    assert ult.check_diff(_HDR + "@@ -10,1 +10,1 @@\n" + _OLD + "+" + PRINT.replace("std::fprintf", "mc2::fmt::print_err") + "\n", set()) == []


def test_rewritten_multiline_print_passes():
    old = '-    fprintf(stderr,\n-        "[NEWTAG v1] event=x\\n");\n'
    assert ult.check_diff(_HDR + "@@ -10,2 +10,1 @@\n" + old + "+" + PRINT.replace("std::fprintf", "mc2::fmt::print_err") + "\n", set()) == []


def test_new_ungated_tag_still_fails_next_to_a_rewrite():
    other = '+    std::fprintf(stderr, "[OTHERTAG v1] event=y\\n");\n'
    bad = ult.check_diff(_HDR + "@@ -10,1 +10,2 @@\n" + _OLD + "+" + PRINT + "\n" + other, set())
    assert len(bad) == 1 and "[OTHERTAG]" in bad[0]


def test_tag_moved_to_a_different_hunk_still_fails():
    diff = _HDR + "@@ -10,1 +10,0 @@\n" + _OLD + "@@ -50,0 +49,1 @@\n+" + PRINT + "\n"
    bad = ult.check_diff(diff, set())
    assert len(bad) == 1 and "[NEWTAG]" in bad[0]


def test_gated_tag_passes_guard_in_window():
    guarded = _diff('    if (mc2_diag::tagRequested("NEWTAG"))', PRINT)
    assert ult.check_diff(guarded, set()) == []
    # guard far above the print (outside the window) does not count
    far = _diff(PRINT, context=['if (mc2_diag::tagRequested("NEWTAG"))'] + ["int pad;"] * 12)
    far = far.replace("\n+    std::fprintf", "\n+    std::fprintf")  # print is the last (added) line
    assert len(ult.check_diff(far, set())) == 1


def test_allowlisted_and_selfgated_and_non_source_pass():
    assert ult.check_diff(_diff(PRINT), {"NEWTAG"}) == []
    assert ult.check_diff(_diff('    MC2_DIAGF("[NEWTAG v1] event=x\\n");'), set()) == []
    assert ult.check_diff(_diff(PRINT).replace("x.cpp", "x.py"), set()) == []
    assert ult.check_diff(_diff('    // see [NEWTAG v1] in docs'), set()) == []


def test_baseline_names_are_the_quiet_check_allowlist():
    names = ult.allowed_names(open(os.path.join(ROOT, ult.BASELINE_PATH), encoding="utf-8").read())
    assert "BUILD_FINGERPRINT" in names


def test_compiled_inputs_changed_takes_an_end_ref(tmp_path):
    def git(*a):
        subprocess.run(["git", "-C", str(tmp_path), "-c", "user.name=t", "-c", "user.email=t@t", *a], check=True, capture_output=True, **console_child.child_kwargs(redirected=True))
    git("init", "-q", "-b", "main")
    (tmp_path / "a.md").write_text("a")
    git("add", "."); git("commit", "-q", "-m", "base")
    git("checkout", "-q", "-b", "doc"); (tmp_path / "b.md").write_text("b"); git("add", "."); git("commit", "-q", "-m", "doc")
    git("checkout", "-q", "main"); git("checkout", "-q", "-b", "code"); (tmp_path / "c.cpp").write_text("int x;"); git("add", "."); git("commit", "-q", "-m", "code")
    assert not lm.compiled_inputs_changed(str(tmp_path), "main", "doc")
    assert lm.compiled_inputs_changed(str(tmp_path), "main", "code")


def test_precheck_link_defaults_and_flags():
    ap = lm.build_parser()
    a = ap.parse_args(["--precheck", "x"])
    assert not a.with_link and not a.no_link  # neither flag => auto (None) in main()
    assert ap.parse_args(["--precheck", "x", "--no-link"]).no_link


def test_precheck_auto_link_follows_compiled_inputs(monkeypatch):
    seen = {}
    monkeypatch.setattr(lm, "git_out", lambda root, *a: "a" * 40)
    monkeypatch.setattr(lm, "virtual_merge", lambda *a, **k: ("tree", []))
    monkeypatch.setattr(lm, "virtual_head", lambda *a, **k: "vcommit")
    monkeypatch.setattr(lm, "compiled_inputs_changed", lambda root, pre, post="HEAD": seen.setdefault("changed", True))

    class Stop2(Exception):
        pass

    def tree_lock(*a, **k):
        seen["linked"] = True
        raise lm.Stop(9, "stop here")
    monkeypatch.setattr(lm, "TreeLock", tree_lock)
    monkeypatch.setattr(lm, "stage", lambda *a, **k: None, raising=False)
    monkeypatch.setattr(lm, "stale_inventories", lambda *a, **k: [])
    monkeypatch.setattr(lm, "run", lambda *a, **k: type("R", (), {"returncode": 0, "stdout": ""})())
    assert lm.precheck(ROOT, "tip", None) == 1  # forced stop inside the linked branch
    assert seen.get("linked")
    seen.clear()
    monkeypatch.setattr(lm, "compiled_inputs_changed", lambda root, pre, post="HEAD": False)
    monkeypatch.setattr(lm, "precheck_scratch_base", lambda root: (_ for _ in ()).throw(lm.Stop(9, "stop here")))
    assert lm.precheck(ROOT, "tip", None) == 1
    assert "linked" not in seen
    # explicit --no-link beats compiled-input changes
    monkeypatch.setattr(lm, "compiled_inputs_changed", lambda root, pre, post="HEAD": True)
    assert lm.precheck(ROOT, "tip", False) == 1 and "linked" not in seen


def _verdict(perf_over, **mutate):
    cfg = GateConfig(duration_s=30, stem="mc2_24", vegetation_expected=False,
                     perf_ceilings={"mc2_24": {"draws.props": 10.0}})
    s = _ready()
    s.presence_counts = {"perf_counters_missing": 1}
    s.perf_counters = {"draws.props": 12.5 if perf_over else 9.0}
    for k, v in mutate.items():
        setattr(s, k, v)
    return evaluate(s, cfg, exit_code=0, walltime_s=31)


def test_perf_counter_only_is_pass_with_warning():
    v = _verdict(True)
    assert v.passed and v.buckets == [] and v.warnings and "draws.props=12.5>10" in v.warnings[0]
    assert _verdict(False).warnings == []


def test_perf_counter_with_another_bucket_stays_red():
    v = _verdict(True, shader_errors=1)
    assert not v.passed and "shader_error" in v.buckets and "perf_counter" in v.buckets


def test_gate_status_records_perf_warn_numbers():
    out = "x\n[runner] [PERF_COUNTER_WARN] mc2_01: perf counters over ceiling: draws.props=12.5>10\n"
    st = gate.perf_warn_status(out)
    assert st.startswith("PASS (perf_counter-only WARN: mc2_01: ") and "draws.props=12.5>10" in st
    assert gate.stage_passed(st) and gate.perf_warn_status("clean") == "PASS"
    assert not gate.stage_passed("FAIL (1)")
    assert lm.failure_line("[runner] [PERF_COUNTER_WARN] mc2_01: x")  # kept in the child's output tail


def test_gate_execute_passes_a_perf_warn_row():
    class R:
        returncode, stdout = 0, "[PERF_COUNTER_WARN] mc2_01: draws.props=12.5>10\n"
    head = gate.git("rev-parse", "HEAD")
    gate.FAILED_STAGES.clear()
    rc = gate.execute([("smoke", ["x"], {})], head, runner=lambda c, cwd, env: R())
    assert rc == 0 and not gate.FAILED_STAGES


def test_farm_key_ignores_the_resume_out_swap(tmp_path):
    original = ["py", "tools/sim_determinism.py", "check", "mc2_10", "--out", "E:/x/determinism-mc2_10", "--seed", "1"]
    envs = {gate.farm_key(original): {"K": "V"}}
    swapped = list(original)
    swapped[swapped.index("--out") + 1] = str(tmp_path / "determinism-mc2_10-abc123")   # what execute() does when the previous --out exists
    assert envs[gate.farm_key(swapped)] == {"K": "V"}
    assert gate.farm_key(["a", "b"]) == ("a", "b")


def test_gate_execute_resume_with_farm_runner_does_not_keyerror(tmp_path):
    out = tmp_path / "determinism-mc2_10"
    out.mkdir()   # previous output exists => execute() rewrites --out
    command = ["py", "tools/sim_determinism.py", "check", "mc2_10", "--out", str(out)]
    remote = {gate.farm_key(command): {}}
    calls = []

    def farm_runner(c, cwd, env):
        calls.append(remote[gate.farm_key(c)])
        return type("R", (), {"returncode": 0, "stdout": ""})()
    head = gate.git("rev-parse", "HEAD")
    gate.FAILED_STAGES.clear()
    assert gate.execute([("determinism-mc2_10", command, {})], head, runner=farm_runner) == 0 and calls


def test_print_err_is_a_print_site():
    assert len(ult.check_diff(_diff('    mc2::fmt::print_err("[NEWTAG v1] event=x\n");'), set())) == 1


def test_bad_git_range_fails_instead_of_passing(capsys):
    assert ult.main(["--base", "no-such-ref-xyz", "--head", "HEAD"]) == 1
    assert "FAIL" in capsys.readouterr().out


def test_warm_tree_lock_covers_only_the_build(monkeypatch, tmp_path):
    import threading
    held = threading.local()   # the lock is per thread: the build runs concurrently with the check chains
    seen = []

    class Lock:
        def __init__(self, *a, **k):
            pass

        def __enter__(self):
            held.now = True

        def __exit__(self, *e):
            held.now = False
    monkeypatch.setattr(lm, "TreeLock", Lock)
    monkeypatch.setattr(lm, "git_out", lambda root, *a: "a" * 40)
    monkeypatch.setattr(lm, "virtual_merge", lambda *a, **k: ("tree", []))
    monkeypatch.setattr(lm, "virtual_head", lambda *a, **k: "vcommit")
    monkeypatch.setattr(lm, "ensure_precheck_tree", lambda *a, **k: None)
    monkeypatch.setattr(lm, "stale_inventories", lambda *a, **k: [])
    monkeypatch.setattr(lm, "precheck_scratch_base", lambda root: str(tmp_path))
    # "tip" is not a real ref: the sim-row classifier (added after this test) would run git merge-base on it and fail.
    monkeypatch.setattr(lm, "tip_changed_paths", lambda root, tip: [])

    def fake_run(root, cmd, **k):
        text = " ".join(map(str, cmd))
        seen.append((text, getattr(held, "now", False)))
        return type("R", (), {"returncode": 0, "stdout": ""})()
    monkeypatch.setattr(lm, "run", fake_run)
    assert lm.precheck(ROOT, "tip", True) == 0
    build = [h for t, h in seen if "configure_ninja" in t or "ninja_build" in t]
    other = [h for t, h in seen if "configure_ninja" not in t and "ninja_build" not in t]
    assert len(build) == 3 and all(build)          # configure + two links run under the lock
    assert len(other) >= 8 and not any(other)      # ratchets, callers, pytest suites never do
