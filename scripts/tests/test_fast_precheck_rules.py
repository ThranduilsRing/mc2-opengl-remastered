"""Fast-precheck rules: ungated new log tags, link-by-default for compiled inputs, perf_counter-only smoke verdicts (no game, no build)."""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from tools import lane_merge as lm  # noqa: E402
from tools import ungated_log_tags as ult  # noqa: E402
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
        subprocess.run(["git", "-C", str(tmp_path), "-c", "user.name=t", "-c", "user.email=t@t", *a], check=True, capture_output=True)
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
    monkeypatch.setattr(lm, "git_out", lambda root, *a: "abc")
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
