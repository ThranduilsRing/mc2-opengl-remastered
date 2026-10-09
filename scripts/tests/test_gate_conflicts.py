"""Idiom-class and conflict-detection tests for scripts/gate_idioms.py and
tools/gate_conflicts.py, on synthetic C++ so they cannot drift with the tree.

Each reader idiom is written out as the code that spells it and pinned to the
eight-probe outcome vector it must produce (U, E, Z, F, O, X, X10, X25). If the
interpreter ever reads one of these differently the migration's conflict list
is wrong, so the vectors are literals here, not derived.
"""
import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import gate_idioms as gi  # noqa: E402
import gate_conflicts as gc  # noqa: E402

ON, OFF = "on", "off"


def classify(code, helpers="", gate="MC2_X"):
    r = gi.classify_snippet(code, gate, helpers)
    d = gi.describe(r)
    return r, d


def vec(s):
    """'01..' -> outcome list, 1 = on, 0 = off."""
    return [ON if c == "1" else OFF for c in s]


# (id, code, expected class, expected vector as 8 bits U E Z F O X X10 X25, inverted)
BOOL_IDIOMS = [
    ("presence_file_scope", 'static const bool s = (getenv("MC2_X") != nullptr);',
     "Presence", "01111111", False),
    ("presence_if", 'void f(){ if (getenv("MC2_X")) go(); }',
     "Presence", "01111111", False),
    ("lead0_helper_shape",
     'bool f(){ const char* v = getenv("MC2_X"); return v && v[0] && v[0] != \'0\'; }',
     "Lead0", "00011111", False),
    ("lead0_kill_lambda",
     'static const bool s = []{ const char* v = getenv("MC2_X"); return !(v && v[0]==\'0\'); }();',
     "Lead0Kill", "11011111", False),
    ("lead0_kill_ternary",
     'bool f(){ const char* v = getenv("MC2_X"); return (v && v[0]==\'0\') ? false : true; }',
     "Lead0Kill", "11011111", False),
    ("lead0_present_not_zero",
     'bool f(){ const char* v = getenv("MC2_X"); return v && v[0] != \'0\'; }',
     "Lead0P", "01011111", False),
    ("def_on_lead0",
     'bool f(){ const char* v = getenv("MC2_X"); return !v || (v[0] && v[0] != \'0\'); }',
     "DefOnLead0", "10011111", False),
    ("lead1", 'bool f(){ const char* v = getenv("MC2_X"); return v && v[0]==\'1\'; }',
     "Lead1", "00001010", False),
    ("exact1_strcmp",
     'bool f(){ const char* v = getenv("MC2_X"); return v && strcmp(v,"1")==0; }',
     "Exact1", "00001000", False),
    ("exact1_two_char_check",
     'bool f(){ const char* v = getenv("MC2_X"); return v && v[0]==\'1\' && v[1]==0; }',
     "Exact1", "00001000", False),
    ("nonempty", 'bool f(){ const char* v = getenv("MC2_X"); return v && *v; }',
     "NonEmpty", "00111111", False),
    ("atoi_nonzero", 'static const bool s = (getenv("MC2_X") && std::atoi(getenv("MC2_X")) != 0);',
     "AtoiNZ", "00001011", False),
    # polarity: the same TEST used as a disable flag is recognised, and marked.
    ("zero_is_disable_flag",
     'bool f(){ const char* v = getenv("MC2_X"); return v && v[0]==\'0\' && v[1]==0; }',
     "Lead0Kill", "11011111", True),
    ("bail_when_set", 'void f(){ if (getenv("MC2_X")) return; go(); }',
     "Presence", "01111111", True),
    ("kill_on_one",
     'bool f(){ const char* v = getenv("MC2_X"); return !(v && v[0]==\'1\'); }',
     "Lead1", "00001010", True),
]


@pytest.mark.parametrize("name,code,cls,bits,inverted", BOOL_IDIOMS, ids=[c[0] for c in BOOL_IDIOMS])
def test_bool_idiom(name, code, cls, bits, inverted):
    r, d = classify(code)
    assert r["status"] == "reader", r
    fam, v, got_cls, inv = d
    assert fam == "bool"
    assert got_cls == cls
    assert v == vec(bits)
    assert inv is inverted


HELPERS = '''
bool envFlagDefaultOn(const char* name) { const char* v = std::getenv(name); return !v || (v[0] && v[0] != '0'); }
bool envFlag(const char* name) { const char* v = std::getenv(name); return v && v[0] && v[0] != '0'; }
static bool ParseEnvBool(const char* name, bool def = false) {
    const char* v = getenv(name);
    if (!v || !*v) return def;
    if (v[0] == '0' && !v[1]) return false;
    if (!_stricmp(v, "false") || !_stricmp(v, "off") || !_stricmp(v, "no")) return false;
    return true;
}
inline int envTriState(const char* name) {
    const char* v = std::getenv(name);
    if (!v || v[0] == '\\0' || v[1] != '\\0') return -1;
    if (v[0] == '1') return 1;
    if (v[0] == '0') return 0;
    return -1;
}
inline bool umbrellaOn() { return false; }
inline bool subGate(const char* name) {
    const int t = envTriState(name);
    if (t == 1) return true;
    if (t == 0) return false;
    return umbrellaOn();
}
'''


def test_std_helper_default_argument_decides_unset_and_empty():
    _, d_on = classify('static bool s = ParseEnvBool("MC2_X", true);', HELPERS)
    _, d_off = classify('static bool s = ParseEnvBool("MC2_X");', HELPERS)
    assert d_on[1] == vec("11001111") and d_on[2] == "Std(def=on)"
    assert d_off[1] == vec("00001111") and d_off[2] == "Std(def=off)"


def test_helper_lead0_and_default_on_are_resolved_from_their_bodies():
    _, d = classify('static bool s = envFlag("MC2_X");', HELPERS)
    assert d[2] == "Lead0"
    _, d = classify('static bool s = envFlagDefaultOn("MC2_X");', HELPERS)
    assert d[2] == "DefOnLead0"


def test_tri_state_equals_one_is_exact_one():
    _, d = classify('static bool s = (envTriState("MC2_X") == 1);', HELPERS)
    assert d[2] == "Exact1"


def test_umbrella_inheriting_sub_gate_is_distinct_from_any_concrete_answer():
    r, d = classify('static bool s = subGate("MC2_X");', HELPERS)
    fam, v, cls, inv = d
    assert cls == "Tri(inherit)"
    assert v[gi.PROBE_LABELS.index("O")] == ON and v[gi.PROBE_LABELS.index("Z")] == OFF
    assert v[0] == gi.INH and v[1] == gi.INH     # unset and empty inherit


def test_envtristate_not_minus_one_is_the_deprecation_idiom():
    _, d = classify('static const bool s = [] { return envTriState("MC2_X") != -1; }();', HELPERS)
    assert d[2] == "Is01"


def test_other_state_in_a_conjunction_is_the_identity():
    _, d = classify('void f(){ if (getenv("MC2_X") && (tick() % 64u) == 0u) log(); }')
    assert d[2] == "Presence"
    _, d = classify('static bool s = other_flag || (getenv("MC2_X") != nullptr);')
    assert d[2] == "Presence"


def test_a_helper_that_never_looks_at_the_probe_is_unknown_not_false():
    helpers = "bool tagEnabled(const char* tag) { return false; }"
    _, d = classify('void f(){ if (getenv("MC2_X") && tagEnabled("T")) go(); }', helpers)
    assert d[2] == "Presence"


# ---- numeric readers --------------------------------------------------------

def test_atoi_with_default_keeps_the_default_for_unset_only():
    r, d = classify('int g(){ const char* v = getenv("MC2_X"); int n = v ? atoi(v) : 64; return n; }')
    fam, v, cls, inv = d
    assert fam == "num" and cls == "Num(atoi,raw)"
    assert v == ["64", "0", "0", "0", "1", "0", "10", "2"]


def test_guarded_atof_differs_from_raw_on_empty():
    _, d_raw = classify('float g(){ const char* v = getenv("MC2_X"); return v ? (float)atof(v) : 1.0f; }')
    _, d_grd = classify('float g(){ const char* v = getenv("MC2_X"); return (v && v[0]) ? (float)atof(v) : 1.0f; }')
    assert d_raw[1][1] == "0"            # "" parses to 0
    assert d_grd[1][1] == "1"            # "" keeps the default
    assert d_raw[1][7] == "2.5" and d_grd[1][7] == "2.5"


def test_atoi_and_atof_disagree_on_a_decimal():
    _, a = classify('int g(){ const char* v = getenv("MC2_X"); return v ? atoi(v) : 0; }')
    _, b = classify('float g(){ const char* v = getenv("MC2_X"); return v ? (float)atof(v) : 0; }')
    assert a[1][7] == "2" and b[1][7] == "2.5"


def test_constants_resolve():
    code = ("constexpr bool kDefault = true;\n"
            "void f(){ const char* v = getenv(\"MC2_X\"); bool on;"
            " if (v && v[0]) { on = (v[0] == '1'); } else { on = kDefault; } use(on); }")
    r, d = classify(code)
    assert d[1][0] == ON and d[1][1] == ON       # unset/empty take the constant
    assert d[1][2] == OFF and d[1][4] == ON


def test_initializer_before_the_block_is_the_unset_value():
    code = ("void f(){ int on = 0; if (const char* e = getenv(\"MC2_X\")) { on = (e[0] != '0'); } use(on); }")
    r, d = classify(code)
    assert d[1][0] == OFF and d[1][1] == ON and d[1][2] == OFF


# ---- classification boundaries ---------------------------------------------

def test_comparing_against_a_token_no_probe_hits_is_not_comparable():
    r, d = classify('bool f(){ const char* v = getenv("MC2_X"); return v && strcmp(v, "painted") == 0; }')
    assert d[2] == "Token/State"


def test_a_print_is_not_a_reader():
    r = gi.classify_snippet('void f(){ printf("%s", getenv("MC2_X")); }', "MC2_X")
    assert r["status"] == "nonreader"


def test_an_unknown_call_that_swallows_the_value_is_unclassified_not_ignored():
    r = gi.classify_snippet('void f(){ mystery(getenv("MC2_X")); }', "MC2_X")
    assert r["status"].startswith("unclassified")


def test_windows_getenv_is_a_nonempty_presence_read():
    code = ('bool f(){ char buf[8]; return GetEnvironmentVariableA("MC2_X", buf, 8) > 0; }')
    r = gi.classify_snippet(code, "MC2_X")
    d = gi.describe(r)
    assert d[2] == "NonEmpty"


def test_macro_bodies_do_not_leak_into_the_statement_before_them():
    code = ('static const bool s = (getenv("MC2_X") != NULL);\n'
            '#define TRACE(fmt, ...) \\\n    do { if (s) { fprintf(stderr, fmt, ##__VA_ARGS__); } } while (0)\n')
    r, d = classify(code)
    assert d[2] == "Presence"


# ---- conflict detection over a tree ----------------------------------------

def _tree(tmp_path, files):
    for rel, text in files.items():
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
    return gc.Tree(str(tmp_path), None)


def _report(tmp_path, files):
    tree = _tree(tmp_path, files)
    return gc.build_report(tree, scan_dirs=["code", "mclib"], with_config=False)


def test_two_readers_that_disagree_are_a_conflict(tmp_path):
    rep = _report(tmp_path, {
        "code/a.cpp": 'static const bool a = (getenv("MC2_G") != nullptr);\n',
        "mclib/b.cpp": 'bool b(){ const char* v = getenv("MC2_G"); return v && v[0]==\'1\'; }\n',
    })
    assert rep["conflict_gates"] == ["MC2_G"]
    inputs = rep["gates"]["MC2_G"]["disagree_inputs"]
    assert "E" in inputs and "Z" in inputs and "F" in inputs


def test_readers_that_agree_are_not(tmp_path):
    rep = _report(tmp_path, {
        "code/a.cpp": 'static const bool a = (getenv("MC2_G") != nullptr);\n',
        "mclib/b.cpp": 'void f(){ if (getenv("MC2_G")) go(); }\n',
    })
    assert rep["conflict_gates"] == []


def test_a_disable_flag_and_its_complement_are_the_same_test(tmp_path):
    rep = _report(tmp_path, {
        "code/a.cpp": 'bool on(){ return getenv("MC2_G") == nullptr; }\n',
        "mclib/b.cpp": 'bool off(){ return getenv("MC2_G") != nullptr; }\n',
    })
    assert rep["conflict_gates"] == []


def test_different_defaults_for_the_same_number_are_a_conflict(tmp_path):
    rep = _report(tmp_path, {
        "code/a.cpp": 'int a(){ const char* v = getenv("MC2_N"); return v ? atoi(v) : 4; }\n',
        "mclib/b.cpp": 'int b(){ const char* v = getenv("MC2_N"); return v ? atoi(v) : 8; }\n',
    })
    assert rep["conflict_gates"] == ["MC2_N"]
    assert rep["gates"]["MC2_N"]["disagree_inputs"][0] == "U"


def test_a_numeric_knob_and_a_presence_trace_are_different_questions(tmp_path):
    rep = _report(tmp_path, {
        "code/a.cpp": 'float a(){ const char* v = getenv("MC2_K"); return v ? (float)atof(v) : 0.5f; }\n',
        "mclib/b.cpp": 'bool b(){ return getenv("MC2_K") != nullptr; }\n',
    })
    assert rep["conflict_gates"] == []


def test_a_mode_flag_read_through_atoi_joins_the_bool_comparison(tmp_path):
    rep = _report(tmp_path, {
        "code/a.cpp": 'int m(){ const char* v = getenv("MC2_M"); return v ? atoi(v) : 1; }\n',
        "mclib/b.cpp": 'bool b(){ const char* v = getenv("MC2_M"); return (!v || !v[0]) ? true : (v[0] != \'0\'); }\n',
    })
    assert rep["conflict_gates"] == ["MC2_M"]
    assert "F" in rep["gates"]["MC2_M"]["disagree_inputs"]      # "false": atoi 0, char test on


def test_inheriting_reader_conflicts_with_a_non_inheriting_one(tmp_path):
    helpers = HELPERS
    rep = _report(tmp_path, {
        "code/h.h": helpers,
        "code/a.cpp": 'static bool a = subGate("MC2_V");\n',
        "mclib/b.cpp": 'bool b(){ const char* v = getenv("MC2_V"); return v && v[0]==\'1\'; }\n',
    })
    assert rep["conflict_gates"] == ["MC2_V"]
    assert rep["gates"]["MC2_V"]["proposed_rule"].startswith("Tri row")


def test_proposed_rule_follows_the_registry_default(tmp_path):
    reg = ('{ "MC2_R", "MC2_R", EnvVarKind::Feature, true, "d" },\n')
    rep = gc.build_report(_tree(tmp_path, {
        "RenderCore/RendererFeatureRegistry.h": reg,
        "code/a.cpp": 'static const bool a = (getenv("MC2_R") != nullptr);\n',
        "mclib/b.cpp": 'bool b(){ const char* v = getenv("MC2_R"); return v && v[0]==\'1\'; }\n',
    }), scan_dirs=["code", "mclib"], with_config=False)
    e = rep["gates"]["MC2_R"]
    assert e["registry"] == {"default_on": True, "kind": "Feature"}
    assert e["proposed_rule"].startswith("Std(unset=on)")
    changed = {c["class"]: c["changes"] for c in e["behaviour_delta"]}
    assert changed["Presence"]["U"] == "off->on"


# ---- ratchet ---------------------------------------------------------------

def _fake_report(conflicts, unclassified=0):
    return {"conflict_gates": list(conflicts), "totals": {"unclassified_sites": unclassified}}


def test_ratchet_passes_when_equal_or_lower():
    base = {"conflict_gates": ["A", "B"], "unclassified_sites": 3}
    assert gc.check(_fake_report(["A", "B"], 3), base) == []
    assert gc.check(_fake_report(["A"], 2), base) == []


def test_ratchet_fails_on_a_new_conflict_even_if_the_count_holds():
    base = {"conflict_gates": ["A", "B"], "unclassified_sites": 3}
    fails = gc.check(_fake_report(["A", "C"], 3), base)
    assert any("not in conflict are now" in f and "C" in f for f in fails)


def test_ratchet_fails_when_the_count_rises():
    base = {"conflict_gates": ["A"], "unclassified_sites": 3}
    assert any("rose" in f for f in gc.check(_fake_report(["A", "B"], 3), base))


def test_ratchet_fails_when_unclassified_sites_rise():
    base = {"conflict_gates": [], "unclassified_sites": 3}
    assert any("unclassified" in f for f in gc.check(_fake_report([], 4), base))


def test_missing_baseline_is_a_failure_not_a_pass():
    assert gc.check(_fake_report([]), None)


def test_committed_baseline_matches_the_tool_format():
    base = gc.load_baseline(ROOT)
    assert base is not None
    assert isinstance(base["conflict_gates"], list)
    assert base["conflict_gates"] == sorted(base["conflict_gates"])
    assert isinstance(base["unclassified_sites"], int)


# ---- G0 ledger -------------------------------------------------------------

def test_ledger_accepts_enum_and_divergent_but_not_resolved_conflicts():
    rep = _fake_report(["A", "B", "C"], 0)
    rep["totals"]["unresolved_conflicts"] = 1
    rep["regressed_gates"] = ["C"]
    base = {"conflict_gates": ["A", "B", "C"], "unresolved_conflicts": 1, "unclassified_sites": 0}
    fails = gc.check(rep, base)
    assert any("C was resolved by G0" in f for f in fails)


def test_unresolved_count_may_not_rise():
    rep = _fake_report(["A"], 0)
    rep["totals"]["unresolved_conflicts"] = 2
    base = {"conflict_gates": ["A"], "unresolved_conflicts": 1, "unclassified_sites": 0}
    assert any("unresolved conflicts rose" in f for f in gc.check(rep, base))


def test_committed_ledger_covers_every_baseline_conflict():
    import json
    base = gc.load_baseline(ROOT)
    ledger = json.load(open(os.path.join(ROOT, "tools", "gate_g0_ledger.json"), encoding="utf-8"))["gates"]
    for g in base["conflict_gates"]:
        assert ledger.get(g, {}).get("status") in gc.LEDGER_ACCEPTED, g
    for g, e in ledger.items():
        assert e["status"] in ("resolved", "enum", "divergent", "note"), g
