"""Tests for tools/gate_codemod.py on synthetic C++ (no repo files, no git).

Each reader idiom x caching shape is written out as the code that spells it and pinned to the exact
replacement text and verdict.  The fixtures use a one-gate table so the rule the row carries is
whatever the test says it is; the tool is a planner, so nothing here touches a real source file.
"""
import os
import re
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import gate_idioms as gi  # noqa: E402
import gate_codemod as cm  # noqa: E402
import gate_table_gen as gtg  # noqa: E402

G = "MC2_X"


def mk(rule="Presence", kind="Bool", default=False, rng=None, harness=()):
    return {G: cm.Row(G, kind, rule, default, rng=rng, harness=harness)}


def one(code, rows=None, ledger=None):
    res = cm.analyze_text("snippet.cpp", code, rows if rows is not None else mk(), ledger)
    assert len(res) == 1, [(r.where, r.verdict, r.why) for r in res]
    return res[0]


def getx(rule=None, neg=False):
    return cm.get_expr(G, rule, neg)


# ---------------------------------------------------------------------------
# the ported rule table must agree with the interpreter's table
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("default", [False, True])
@pytest.mark.parametrize("rule", ["Std", "Lead0Def"] + [r for r in gi._RULE_BITS])
def test_ported_rule_vectors_match_gate_idioms(rule, default):
    want = gi.rule_vector(rule, default)
    assert want is not None
    got = ["on" if b else "off" for b in cm.rule_vec(rule, default)]
    assert got == want[0]


# ---------------------------------------------------------------------------
# Bool idioms (cond on a `const char* v`) x caching shape
# ---------------------------------------------------------------------------

COND = {
    "Presence": "v != nullptr",
    "NonEmpty": "v && *v",
    "Exact1": "v && v[0] == '1' && v[1] == 0",
    "Lead1": "v && v[0] == '1'",
    "Lead0": "v && v[0] && v[0] != '0'",
    "Lead0Kill": "!(v && v[0] == '0')",
    "Lead0P": "v && v[0] != '0'",
    "DefOnLead0": "!v || (v[0] && v[0] != '0')",
    "AtoiNZ": "v && atoi(v) != 0",
}


def lam(rule):
    return "[] { const char* v = getenv(\"%s\"); return %s; }()" % (G, COND[rule])


@pytest.mark.parametrize("rule", sorted(COND))
def test_lambda_static_at_namespace_scope_keeps_the_static(rule):
    r = one("namespace {\nstatic const bool s_on = %s;\n}\n" % lam(rule), mk(rule))
    assert (r.verdict, r.shape, r.idiom) == ("NO-CHANGE", "lambda-static", rule)
    assert r.replacement == "static const bool s_on = %s;" % getx()


@pytest.mark.parametrize("rule", sorted(COND))
def test_lambda_static_inside_a_function(rule):
    r = one("bool f() {\n  static const bool s = %s;\n  return s;\n}\n" % lam(rule), mk(rule))
    assert (r.verdict, r.shape) == ("NO-CHANGE", "lambda-static")
    assert r.replacement == "static const bool s = %s;" % getx()


def test_namespace_scope_lambda_without_static_is_still_a_static_initializer():
    r = one("namespace {\nconst bool kOn = %s;\n}\n" % lam("Lead1"), mk("Lead1"))
    assert r.shape == "lambda-static"
    assert r.replacement == "const bool kOn = %s;" % getx()


def test_unstatic_lambda_in_a_function_is_per_call():
    r = one("void f() {\n  bool on = %s;\n  use(on);\n}\n" % lam("Lead1"), mk("Lead1"))
    assert r.shape == "per-call"
    assert r.replacement == "bool on = %s;" % getx()


@pytest.mark.parametrize("rule", sorted(COND))
def test_per_call_pointer_return(rule):
    r = one("bool f() {\n  const char* v = getenv(\"%s\");\n  return %s;\n}\n" % (G, COND[rule]), mk(rule))
    assert (r.verdict, r.shape, r.mode) == ("NO-CHANGE", "per-call", "local")
    assert r.replacement == "return %s;" % getx()


def test_per_call_pointer_assignment_to_a_member():
    r = one("void f() {\n  const char* env = getenv(\"%s\");\n  s_kill = (env == nullptr) || (env[0] != '0');\n}\n" % G,
            mk("Lead0Kill"))
    assert r.verdict == "NO-CHANGE" and r.shape == "per-call"
    assert r.replacement == "s_kill = %s;" % getx()


def test_pointer_reused_later_is_refused():
    r = one("void f() {\n  const char* v = getenv(\"%s\");\n  s = v && v[0]=='1';\n  log(v);\n}\n" % G, mk("Lead1"))
    assert r.verdict == "UNHANDLED" and "used again" in r.reason


def test_pointer_consumed_by_a_branch_is_refused():
    r = one("void f() {\n  const char* v = getenv(\"%s\");\n  if (v) { go(v); }\n}\n" % G, mk("Presence"))
    assert r.verdict == "UNHANDLED"


# ----- direct presence expressions: namespace-static / local-static / per-call ------------------------

def test_presence_namespace_static():
    r = one("namespace {\nstatic const bool s = (getenv(\"%s\") != nullptr);\n}\n" % G)
    assert (r.verdict, r.shape) == ("NO-CHANGE", "namespace-static")
    assert r.replacement == "static const bool s = %s;" % getx()


def test_presence_namespace_static_without_the_static_keyword():
    r = one("namespace {\nconst bool kOn = getenv(\"%s\") != nullptr;\n}\n" % G)
    assert r.shape == "namespace-static"
    assert r.replacement == "const bool kOn = %s;" % getx()


def test_presence_local_static():
    r = one("inline bool on() {\n  static const bool s = (std::getenv(\"%s\") != nullptr);\n  return s;\n}\n" % G)
    assert (r.verdict, r.shape) == ("NO-CHANGE", "local-static")
    assert r.replacement == "static const bool s = %s;" % getx()


def test_presence_per_call_condition():
    r = one("void f() {\n  if (getenv(\"%s\")) { go(); }\n}\n" % G)
    assert (r.verdict, r.shape, r.idiom) == ("NO-CHANGE", "per-call", "Presence")
    assert r.replacement == "if (%s) { go(); }" % getx()


def test_presence_per_call_assignment_and_multiline_declaration():
    r = one("namespace {\nstatic const bool s =\n    (getenv(\"%s\") != nullptr);\n}\n" % G)
    assert r.replacement == "static const bool s = %s;" % getx()
    r = one("void f() { s_on = (getenv(\"%s\") != nullptr); }\n" % G)
    assert (r.shape, r.replacement) == ("per-call", "void f() { s_on = %s; }" % getx())


def test_equals_nullptr_is_a_negated_presence_test():
    r = one("namespace {\nstatic const bool s = (getenv(\"%s\") == nullptr);\n}\n" % G)
    assert r.verdict == "NO-CHANGE"
    assert r.replacement == "static const bool s = %s;" % getx(neg=True)
    assert "inverted" in r.idiom or "nullptr" in r.idiom


def test_bang_getenv_keeps_the_bang_outside():
    r = one("void f() {\n  if (!getenv(\"%s\")) { go(); }\n}\n" % G)
    assert r.replacement == "if (!%s) { go(); }" % getx()


def test_pointer_use_of_the_call_is_refused():
    r = one("void f() {\n  std::string s = getenv(\"%s\");\n}\n" % G)
    assert r.verdict == "UNHANDLED"


# ---------------------------------------------------------------------------
# verdicts
# ---------------------------------------------------------------------------

def test_presence_site_on_a_std_row_is_semantics_with_a_presence_override():
    code = "namespace {\nstatic const bool s = (getenv(\"%s\") != nullptr);\n}\n" % G
    r = one(code, mk("Std", default=True))
    assert r.verdict == "SEMANTICS"
    assert r.override == "Presence"
    assert r.replacement == "static const bool s = %s;" % getx("Presence")
    assert {"U", "Z"} <= set(r.diff)
    # the override spelling is the one the baseline counter looks for
    assert re.search(r'get\s*<\s*"MC2_\w+"\s*,\s*(?:gate::|mc2::gate::)?Rule::', r.replacement)


def test_nonempty_site_on_a_std_row_is_semantics_because_zero_differs():
    r = one("bool f() {\n  const char* v = getenv(\"%s\");\n  return v && *v;\n}\n" % G, mk("Std", default=False))
    assert r.verdict == "SEMANTICS" and r.override == "NonEmpty"
    assert r.replacement == "return %s;" % getx("NonEmpty")
    assert "Z" in r.diff


def test_lead0kill_site_on_a_presence_row_is_semantics_with_the_lead0kill_override():
    r = one("namespace {\nstatic const bool s = %s;\n}\n" % lam("Lead0Kill"), mk("Presence"))
    assert r.verdict == "SEMANTICS" and r.override == "Lead0Kill"
    assert r.replacement == "static const bool s = %s;" % getx("Lead0Kill")


def test_difference_only_on_empty_text_is_edge_and_names_the_exact_alternative():
    r = one("namespace {\nstatic const bool s = (getenv(\"%s\") != nullptr);\n}\n" % G, mk("NonEmpty"))
    assert r.verdict == "EDGE" and r.diff == ["E"]
    assert r.override == ""
    assert r.replacement == "static const bool s = %s;" % getx()
    assert getx("Presence") in r.why


def test_lead1_site_on_an_exact1_row_is_edge_on_the_ten_probe():
    r = one("bool f() {\n  const char* v = getenv(\"%s\");\n  return v && v[0] == '1';\n}\n" % G, mk("Exact1"))
    assert r.verdict == "EDGE" and set(r.diff) == {"X10"}


def test_harness_value_that_hits_a_changed_input_is_reported():
    r = one("namespace {\nstatic const bool s = (getenv(\"%s\") != nullptr);\n}\n" % G,
            mk("Std", default=False, harness=["0", "1"]))
    assert r.verdict == "SEMANTICS" and r.harness_hits == ["0"]
    assert "harness sets '0'" in r.why


def test_inverted_use_is_negated_in_the_replacement():
    r = one("bool f() {\n  const char* v = getenv(\"%s\");\n  return v && v[0] == '0' && v[1] == 0;\n}\n" % G,
            mk("Lead0Kill", default=True))
    assert r.verdict == "NO-CHANGE"
    assert r.replacement == "return %s;" % getx(neg=True)


def test_std_default_mismatch_cannot_be_fixed_by_an_override():
    code = ("bool f() {\n  const char* v = getenv(\"%s\");\n"
            "  return (!v || !*v) ? true : !(!strcmp(v, \"0\") || !_stricmp(v, \"false\") || "
            "!_stricmp(v, \"off\") || !_stricmp(v, \"no\"));\n}\n" % G)
    r = one(code, mk("Std", default=False))
    # reads as Std(def=on); the row is Std(def=off): no per-site override changes a default
    assert r.verdict == "UNHANDLED" and "default" in r.reason


# ---------------------------------------------------------------------------
# refusals
# ---------------------------------------------------------------------------

def test_gate_missing_from_the_table_is_unhandled():
    r = one("void f() { if (getenv(\"MC2_X\")) go(); }", {})
    assert r.verdict == "UNHANDLED" and "not in the gate table" in r.reason


def test_helper_callee_is_left_to_the_helper_slice():
    helpers = [("h.h", "inline bool envFlag(const char* n) { const char* v = getenv(n); return v && v[0]; }")]
    rows = mk("Lead0")
    res = cm.analyze_text("snippet.cpp", "static const bool s = envFlag(\"MC2_X\");", rows, None, helpers)
    assert len(res) == 1 and res[0].verdict == "UNHANDLED" and "helper" in res[0].reason


def test_lambda_with_captures_is_refused():
    r = one("void f(int n) {\n  auto b = [n] { const char* v = getenv(\"%s\"); return v && n; }();\n}\n" % G)
    assert r.verdict == "UNHANDLED"


def test_lambda_that_names_outside_state_is_refused():
    r = one("static const bool s = []{ const char* v = getenv(\"%s\"); return v && g_frame > 3; }();\n" % G)
    assert r.verdict == "UNHANDLED" and "g_frame" in r.reason


def test_row_kind_tri_is_left_to_g4():
    r = one("static const bool s = []{ const char* v = getenv(\"%s\"); return v && v[0]=='1'; }();\n" % G,
            mk("Std", kind="Tri"))
    assert r.verdict == "UNHANDLED" and "Tri" in r.reason


def test_presence_test_of_a_string_row_becomes_is_set():
    r = one("static const bool s = (getenv(\"%s\") != nullptr);\n" % G, mk("Std", kind="String"))
    assert r.verdict == "NO-CHANGE"
    assert r.replacement == "static const bool s = %s::isSet<\"%s\">();" % (cm.NS, G)


def test_already_typed_sites_are_skipped():
    res = cm.analyze_text("snippet.cpp", "static const bool s = mc2::gate::get<\"%s\">();\n" % G, mk())
    assert res == []


def test_win32_reader_is_not_rewritten():
    res = cm.analyze_text("snippet.cpp", "void f(){ char b[8]; GetEnvironmentVariableA(\"%s\", b, 8); }" % G, mk())
    assert res and res[0].verdict == "UNHANDLED"


def test_a_level_valued_or_divergent_gate_is_never_migrated_as_a_bool():
    for status in ("divergent", "enum"):
        led = {G: {"status": status, "rule": "levels 0/1/2"}}
        r = one("static const bool s = (getenv(\"%s\") != nullptr);\n" % G, mk(), led)
        assert r.verdict == "UNHANDLED" and status in r.why and "levels" in r.why


# ---------------------------------------------------------------------------
# numeric rows
# ---------------------------------------------------------------------------

INT_LAMBDA = '''static const uint32_t s_dim = [] () -> uint32_t {
    const char* v = std::getenv("MC2_X");
    if (v && v[0]) {
        const long n = std::atol(v);
        if (n >= 2 && n <= 256) return static_cast<uint32_t>(n);
    }
    return 64u;
}();
'''


def test_int_reject_lambda_needs_a_range_row_and_then_matches():
    r = one(INT_LAMBDA, mk("Prefix", "Int", 64))
    assert r.verdict == "NO-CHANGE" and r.shape == "lambda-static"
    assert r.row_edit == "range(2, 256)"
    assert r.replacement == "static const uint32_t s_dim = static_cast<uint32_t>(%s);" % getx()


def test_int_row_that_already_has_the_range_needs_no_edit():
    r = one(INT_LAMBDA, mk("Prefix", "Int", 64, rng={"lo": 2, "hi": 256, "clamp": False}))
    assert r.verdict == "NO-CHANGE" and r.row_edit == ""


def test_int_exclusive_lower_bound_becomes_an_inclusive_integer_bound():
    code = INT_LAMBDA.replace("n >= 2 && n <= 256", "n > 0 && n <= 4096").replace("64u", "0u")
    r = one(code, mk("Prefix", "Int", 0))
    assert r.verdict == "NO-CHANGE"
    assert r.row_edit in ("range(1, 4096)", "range(0, 4096)")


def test_wrong_row_default_is_reported_as_a_row_edit():
    r = one(INT_LAMBDA, mk("Prefix", "Int", 32))
    assert "default 32 -> 64" in r.row_edit


FLOAT_CLAMP = '''static const float s_k = []() {
    const char* v = std::getenv("MC2_X");
    float f = 2.2f;
    if (v && v[0]) f = static_cast<float>(std::atof(v));
    if (f < 0.25f) f = 0.25f;
    if (f > 4.0f)  f = 4.0f;
    return f;
}();
'''


def test_float_clamp_in_code_gets_a_clamp_row_and_reports_only_the_infinity_edge():
    r = one(FLOAT_CLAMP, mk("Prefix", "Float", 0.25, rng={"lo": 0.25, "hi": 4, "clamp": False}))
    assert r.row_edit == "default 0.25 -> 2.2; clampRange(0.25, 4)"
    assert r.verdict == "EDGE" and r.diff == ["'1e999'"]
    assert r.replacement == "static const float s_k = %s;" % getx()


FLOAT_ACCEPT = '''static const float s_r = []() {
    const char* s = std::getenv("MC2_X");
    if (!s || !s[0]) return 0.6f;
    float f = (float)std::atof(s);
    return (f > 0.0f && f <= 1.0f) ? f : 0.6f;
}();
'''


def test_float_exclusive_lower_bound_is_edge_at_the_bound():
    r = one(FLOAT_ACCEPT, mk("Prefix", "Float", 0.6))
    assert r.verdict == "EDGE"
    assert "'0'" in r.diff
    assert r.row_edit == "range(0, 1)"


def test_direct_declaration_initializer_with_atoi():
    code = "static const int s_n = getenv(\"MC2_X\") ? atoi(getenv(\"MC2_X\")) : 3;\n"
    res = cm.analyze_text("snippet.cpp", code, mk("Prefix", "Int", 3))
    # atoi("") is 0 but the row answers the default for an empty string: an EDGE on E only
    assert res and all(r.verdict == "EDGE" and r.diff == ["''"] for r in res)
    assert res[0].replacement == "static const int s_n = %s;" % getx()


def test_atoi_prefix_junk_matches_the_prefix_rule_exactly():
    code = "static const int s_n = []{ const char* v = getenv(\"MC2_X\"); return (v && *v) ? atoi(v) : 7; }();\n"
    r = one(code, mk("Prefix", "Int", 7))
    assert r.verdict == "NO-CHANGE" and r.diff == []


def test_whole_string_reader_differs_from_a_prefix_row_on_junk():
    code = ("static const int s_n = []{ const char* v = getenv(\"MC2_X\"); if (!v || !*v) return 7; "
            "char* e = nullptr; long n = strtol(v, &e, 10); return (*e == 0) ? (int)n : 7; }();\n")
    res = cm.analyze_text("snippet.cpp", code, mk("Prefix", "Int", 7))
    assert res
    assert res[0].verdict in ("EDGE", "UNHANDLED")


def test_numeric_row_cast_is_omitted_for_matching_types():
    code = INT_LAMBDA.replace("uint32_t", "int")
    r = one(code, mk("Prefix", "Int", 64))
    assert r.replacement == "static const int s_dim = %s;" % getx()


# ---------------------------------------------------------------------------
# rendering, CLI, determinism
# ---------------------------------------------------------------------------

def test_tsv_has_seven_columns_and_is_deterministic():
    code = ("namespace {\nstatic const bool a = (getenv(\"MC2_X\") != nullptr);\n"
            "static const bool b = %s;\n}\n" % lam("Lead1"))
    r1 = cm.analyze_text("snippet.cpp", code, mk("Presence"))
    r2 = cm.analyze_text("snippet.cpp", code, mk("Presence"))
    t1, t2 = cm.tsv_text(r1), cm.tsv_text(r2)
    assert t1 == t2
    lines = t1.rstrip("\n").split("\n")
    assert lines[0].count("\t") == 6
    assert all(ln.count("\t") == 6 for ln in lines)


def test_markdown_lists_semantics_and_counts():
    code = "namespace {\nstatic const bool a = (getenv(\"MC2_X\") != nullptr);\n}\n"
    rows = mk("Std", default=True)
    res = cm.analyze_text("snippet.cpp", code, rows)
    md = cm.render_md(res, rows, ["snippet.cpp"], "abc123")
    assert "SEMANTICS rows (1)" in md
    assert "| **SEMANTICS** | 1 |" in md
    assert md == cm.render_md(res, rows, ["snippet.cpp"], "abc123")


def test_apply_refuses_without_the_confirmation_flag(capsys):
    assert cm.main(["--apply"]) == 2
    assert "refused" in capsys.readouterr().err


def test_apply_needs_files(capsys):
    assert cm.main(["--apply", "--i-mean-it"]) == 2
    assert "--files" in capsys.readouterr().err


def test_apply_rewrites_only_no_change_and_edge_sites_and_adds_the_include():
    code = ("#include <cstdlib>\n#include \"x.h\"\n\nnamespace {\n"
            "static const bool a = (getenv(\"MC2_X\") != nullptr);\n"
            "static const bool b = std::string(getenv(\"MC2_X\")).empty();\n}\n")
    results = cm.analyze_text("snippet.cpp", code, mk("Presence"))
    new, applied, skipped = cm.apply_edits(code, results)
    assert len(applied) == 1 and not skipped
    assert 'static const bool a = mc2::gate::get<"MC2_X">();' in new
    assert 'std::string(getenv("MC2_X")).empty()' in new          # the UNHANDLED site is untouched
    assert new.index('#include "Gates/gate.h"') > new.index('#include "x.h"')
    assert cm.apply_edits(new, cm.analyze_text("snippet.cpp", new, mk("Presence")))[0].count("Gates/gate.h") == 1


def test_apply_keeps_crlf_line_endings():
    code = "#include <cstdlib>\r\nstatic const bool a = (getenv(\"MC2_X\") != nullptr);\r\n"
    new, applied, _ = cm.apply_edits(code, cm.analyze_text("snippet.cpp", code, mk("Presence")))
    assert applied and "\r\n#include \"Gates/gate.h\"\r\n" in new and "\n" not in new.replace("\r\n", "")


def test_summary_counts():
    code = ("namespace {\nstatic const bool a = (getenv(\"MC2_X\") != nullptr);\n"
            "static const bool b = std::string(getenv(\"MC2_X\")).empty();\n}\n")
    s = cm.summarize(cm.analyze_text("snippet.cpp", code, mk("Presence")))
    assert s["total"] == 2
    assert s["verdicts"]["NO-CHANGE"] == 1 and s["verdicts"]["UNHANDLED"] == 1


# ---------------------------------------------------------------------------
# G3 additions: umbrella helpers, paired reads, preprocessor lines, family reports
# ---------------------------------------------------------------------------

def test_umbrella_helper_outside_the_scanned_files_is_reported_not_skipped():
    code = "void f() { int a = mc2vkgate::envTriState(\"MC2_X\"); bool b = mc2vkgate::subGate(\"MC2_X\"); }"
    res = cm.analyze_text("snippet.cpp", code, mk())
    assert [(r.callee, r.verdict) for r in res] == [("envTriState", "UNHANDLED"), ("subGate", "UNHANDLED")]
    assert all("umbrella helper" in r.reason and "G4" in r.reason for r in res)
    assert all(cm.is_umbrella_result(r) and cm.is_helper_result(r) for r in res)


def test_unknown_helper_outside_the_scanned_files_is_a_helper_call_reader():
    res = cm.analyze_text("snippet.cpp", "bool f() { return tglc_envOn(\"MC2_X\"); }", mk())
    assert len(res) == 1 and res[0].verdict == "UNHANDLED" and "helper-call reader tglc_envOn" in res[0].reason
    assert res[0].helper_def == "defined outside the scanned files"
    assert not cm.is_umbrella_result(res[0])


def test_writes_and_log_lines_are_not_readers():
    code = "void f() { _putenv_s(\"MC2_X\", \"1\"); printf(\"%s\", \"MC2_X\"); }"
    assert cm.analyze_text("snippet.cpp", code, mk()) == []


def test_helper_definition_is_recorded_for_the_builder():
    helpers = [("h.h", "inline bool envFlag(const char* n) { const char* v = getenv(n); return v && v[0]; }")]
    res = cm.analyze_text("snippet.cpp", "static const bool s = envFlag(\"MC2_X\");", mk("Lead0"), None, helpers)
    assert res[0].helper_def.startswith("h.h:1 `{ const char* v = getenv(n);")
    groups = cm.helper_groups(res)
    assert list(groups) == ["envFlag"] and groups["envFlag"][0] is res[0]


def test_paired_reads_are_judged_as_one_reader_and_never_get_an_invented_override():
    code = "static const bool s = (getenv(\"MC2_X\") && getenv(\"MC2_X\")[0] == '1');"
    res = cm.analyze_text("snippet.cpp", code, mk("Lead1"))
    assert [r.verdict for r in res] == ["UNHANDLED", "UNHANDLED"]
    assert all(r.paired and r.override == "" for r in res)
    assert "as one reader it is Lead1 vs row Lead1: NO-CHANGE" in res[0].reason
    # against a row with another rule the unit verdict says so, instead of a Presence override
    res = cm.analyze_text("snippet.cpp", code, mk("Presence"))
    assert "as one reader it is Lead1 vs row Presence: SEMANTICS" in res[0].reason
    assert "paired reads" in cm.reason_class(res[0].reason)


def test_statement_after_a_preprocessor_line_does_not_swallow_the_directive():
    code = ("void f() {\n#ifdef FOO\n  const float s = 1.0f;\n#else\n"
            "  static const bool s_on = [] { const char* v = getenv(\"MC2_X\"); return v && v[0] == '1' && v[1] == 0; }();\n"
            "#endif\n}\n")
    r = one(code, mk("Exact1"))
    assert r.verdict == "NO-CHANGE"
    assert r.replacement == "static const bool s_on = %s;" % getx()
    assert "#" not in r.replacement
    assert r.why.endswith("inside #else of #ifdef FOO")


def test_preprocessor_context_names_define_bodies_and_nested_conditions():
    T = "#if A\n#ifndef B\nint x;\n#endif\nint y;\n#endif\n#define M(t) do { \\\n  use(getenv(\"MC2_X\")); \\\n} while (0)\n"
    assert cm.pp_context(T, T.index("int x")) == "inside #if A; #ifndef B"
    assert cm.pp_context(T, T.index("int y")) == "inside #if A"
    assert cm.pp_context(T, T.index("getenv")) == "inside a #define body"
    assert cm.pp_context("int z;\n", 3) == ""


def test_fmt_num_prints_enough_digits_to_tell_float32_defaults_apart():
    third = cm._f32(1.0 / 384.0)
    assert cm._f32(float(cm._fmt_num(third))) == third
    assert cm._fmt_num(third) != cm._fmt_num(0.00260417)
    assert cm._fmt_num(cm._f32(2.2)) == "2.2"
    assert cm._fmt_num(0.25) == "0.25" and cm._fmt_num(3.0) == "3"


def test_ledger_enum_ruling_on_an_int_row_becomes_a_row_edit_with_edge_inputs():
    rows = {G: cm.Row(G, "Int", "Prefix", 1)}
    ledger = {G: {"status": "note", "rule": "Enum {1,2,4}; other values rejected", "evidence": "single reader"}}
    res = cm.analyze_text("snippet.cpp", "int f() { const char* v = getenv(\"MC2_X\"); return v ? atoi(v) : 1; }", rows, ledger)
    edits = cm.ledger_kind_edits(res, rows, ledger)
    assert list(edits) == [G] and edits[G]["tokens"] == ["1", "2", "4"]
    assert {"02", "2abc", "+2", "2.5"} <= set(edits[G]["edge_inputs"]) and "2" not in edits[G]["edge_inputs"]
    md = cm.render_md(res, rows, ["snippet.cpp"], "abc123", "g3", ledger)
    assert "kind Int -> Enum, tokens `1|2|4`" in md
    assert 'mc2::gate::is<"MC2_X", "2">()) return 2;' in md and "return 1;" in md
    assert cm.ledger_kind_edits(res, {G: cm.Row(G, "Enum", "Std", "1")}, ledger) == {}


def _write(tmp_path, name, text):
    p = tmp_path / name
    p.write_text(text, encoding="utf-8")
    return name


def test_umbrella_report_follows_wrappers_and_exported_predicates_across_files(tmp_path):
    a = _write(tmp_path, "island.cpp",
               "bool tier1() {\n  return mc2vkgate::subGate(\"MC2_VK_T\");\n}\n"
               "bool lods() {\n  return tier1() && mc2vkgate::envTriState(\"MC2_VK_L\") == 1;\n}\n"
               "extern \"C\" int mc2_vk_x_enabled() {\n  return lods() ? 1 : 0;\n}\n"
               "int other() {\n  return tier1();\n}\n")
    b = _write(tmp_path, "chunk.cpp", "void draw() {\n  if (mc2_vk_x_enabled()) go();\n  if (mc2_vk_y_enabled()) go();\n}\n")
    rep_ = cm.umbrella_report([a, b], str(tmp_path), None, [])
    got = [(s["file"], s["line"], s["helper"], s["role"], s["gate"], s["func"]) for s in rep_["sites"]]
    assert ("island.cpp", 2, "subGate", "resolver", "MC2_VK_T", "tier1") in got
    assert ("island.cpp", 5, "envTriState", "resolver", "MC2_VK_L", "lods") in got
    assert ("chunk.cpp", 2, "mc2_vk_x_enabled", "consumer", "", "draw") in got
    assert not any(g[2] == "mc2_vk_y_enabled" for g in got)         # not an umbrella-derived predicate
    assert sorted(rep_["dependent"]) == ["draw", "lods", "mc2_vk_x_enabled", "other", "tier1"]
    wr = {w["func"]: w["callers"] for w in rep_["wrappers"]}
    assert wr == {"tier1": 2, "lods": 1}


def test_enclosing_function_finds_the_column_zero_head_or_a_namespace_initializer():
    T = "static bool a = [] {\n  return f(\"X\");\n}();\nint g(int x)\n{\n  return h(\"X\");\n}\n}\nint k;\n"
    assert cm.enclosing_function(T, T.index("f(\"X\")"))[0] == "<namespace-scope initializer>"
    assert cm.enclosing_function(T, T.index("h(\"X\")")) == ("g", 4)
    assert cm.enclosing_function(T, T.index("int k"))[0] == "<namespace scope>"


def test_runtime_writes_reads_the_gate_out_of_git_grep_lines(monkeypatch):
    out = (b"HEAD:a/b.cpp:12:    _putenv_s(\"MC2_ACTIVE_MOD\", v);\n"
           b"HEAD:c.cpp:7:  SetEnvironmentVariableA(\"MC2_FPS_CAP\", \"60\");\n"
           b"HEAD:d.cpp:3:  putenv(\"NOT_A_GATE=1\");\n")

    class Done:
        stdout = out

    monkeypatch.setattr(cm.subprocess, "run", lambda *a, **k: Done())
    assert cm.runtime_writes("root", "HEAD") == {"MC2_ACTIVE_MOD": ["a/b.cpp:12"], "MC2_FPS_CAP": ["c.cpp:7"]}


def test_per_call_reader_of_a_gate_written_at_run_time_is_called_out():
    code = "void f() { if (getenv(\"MC2_X\")) go(); }"
    rows = mk()
    res = cm.analyze_text("snippet.cpp", code, rows)
    # the family sections only render when something family-specific exists; a ledger note triggers them
    ledger = {G: {"status": "note", "rule": "n/a", "delta": "-", "evidence": "-"}}
    md = cm.render_md(res, rows, ["snippet.cpp"], "abc", "g3", ledger, None, {G: ["x.cpp:9"]})
    assert "Caching hazard" in md and "`x.cpp:9`" in md
    md = cm.render_md(res, rows, ["snippet.cpp"], "abc", "g3", ledger, None, {"MC2_OTHER": ["x.cpp:9"]})
    assert "Caching hazard" not in md and "Caching check" in md


def test_g3_family_is_registered_with_its_seven_files_and_own_outputs():
    fam = cm.FAMILIES["g3"]
    assert [f.rsplit("/", 1)[-1] for f in fam["files"]] == [
        "gos_terrain_lod_chunk.cpp", "gos_terrain_indirect.cpp", "terrain.cpp", "gos_terrain_water_stream.cpp",
        "vulkan_terrain_island.cpp", "gos_terrain_mask_dispatch.cpp", "gos_terrain_height_tex.cpp"]
    assert (fam["md"], fam["tsv"]) == ("docs/gate-codemod-g3.md", "docs/gate-codemod-g3.tsv")
    assert cm.FAMILIES["g1b"]["md"] == cm.OUT_MD


def test_g3_markdown_groups_helpers_and_lists_umbrella_sites_apart():
    helpers = [("h.h", "inline bool envFlag(const char* n) { const char* v = getenv(n); return v && v[0]; }")]
    code = ("static const bool a = envFlag(\"MC2_X\");\nbool u() { return mc2vkgate::subGate(\"MC2_X\"); }\n")
    rows = mk("Lead0")
    res = cm.analyze_text("snippet.cpp", code, rows, None, helpers)
    umb = {"sites": [{"file": "snippet.cpp", "line": 2, "helper": "subGate", "gate": G, "func": "u", "func_line": 2,
                      "code": "bool u() ...", "role": "resolver"}], "wrappers": [], "vk_family": []}
    md = cm.render_md(res, rows, ["snippet.cpp"], "abc", "g3", None, umb)
    assert "### `envFlag()` -- 1 site" in md and "Helper: h.h:1" in md
    assert "VK umbrella-coupled sites (G4" in md and "| `snippet.cpp:2` | `subGate` | resolver |" in md
    assert "| UNHANDLED, helper-call reader (1 helpers) | 1 |" in md
    assert "| UNHANDLED, VK umbrella resolver (G4) | 1 |" in md
    # the G1b rendering is untouched when nothing family-specific exists
    plain = cm.render_md(cm.analyze_text("snippet.cpp", "static const bool a = (getenv(\"MC2_X\") != nullptr);", mk()), mk(),
                         ["snippet.cpp"], "abc")
    assert "UNHANDLED sites, grouped" not in plain and plain.startswith("# Gate codemod, G1b pilot plan")
