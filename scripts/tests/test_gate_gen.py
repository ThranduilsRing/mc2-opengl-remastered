"""Tests for tools/gate_gen.py on synthetic trees, so each seed field's derivation is
pinned independently of what the real tree looks like on a given day.

Every test builds an `Inputs` in memory (C++ sources, a registry header, script
configs, the two retirement plans) and derives records from it; the one test that
touches the real tree only checks the committed seed's shape.
"""
import json
import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import gate_gen as gg  # noqa: E402
import gate_idioms as gi  # noqa: E402

REGISTRY = '''
namespace R {
static constexpr EnvVarDesc kFeatureTable[] = {
    { "MC2_FEATURE_REG_ON", "MC2_REG_ON", EnvVarKind::Feature, true,
      "Default-ON; =0 disables. Second sentence has the history. Third one too." },
    { "MC2_FEATURE_NOENV", nullptr, EnvVarKind::Feature, true, "Always on." },
};
static constexpr EnvVarDesc kAuxEnvVars[] = {
    { "MC2_REG_OFF", "MC2_REG_OFF", EnvVarKind::Trace, false, "One sentence only." },
    { "MC2_REG_DISAGREE", "MC2_REG_DISAGREE", EnvVarKind::Feature, true, "Says default on." },
    { "MC2_REG_ONLY", "MC2_REG_ONLY", EnvVarKind::Feature, false, "Nobody reads this e.g. Foo bar. Next." },
};
}
'''


def derive(cpp=None, registry=REGISTRY, config=None, plan_a="", plan_b=""):
    recs, side = gg.derive(gg.Inputs(cpp or {}, registry, config or {}, plan_a, plan_b))
    return {r["name"]: r for r in recs}, side


# ---------------------------------------------------------------------------
# Kind, rule, default
# ---------------------------------------------------------------------------

def test_presence_read_is_a_bool_presence_default_off():
    r, _ = derive({"code/a.cpp": 'static const bool s = (getenv("MC2_G") != nullptr);\n'})
    g = r["MC2_G"]
    assert (g["kind"], g["rule"], g["default"]) == ("Bool", "Presence", False)
    assert g["seedDefaultSource"] == "reader-idiom"
    assert g["readers"] == [{"at": "code/a.cpp:1", "idiom": "Presence"}]


def test_lead0kill_reader_is_default_on():
    r, _ = derive({"code/a.cpp": 'static const bool s = []{ const char* v = getenv("MC2_G"); '
                                  'return !(v && v[0]==\'0\'); }();\n'})
    g = r["MC2_G"]
    assert (g["rule"], g["default"], g["seedDefaultSource"]) == ("Lead0Kill", True, "reader-idiom")


def test_dominant_idiom_wins_the_rule():
    r, _ = derive({
        "code/a.cpp": 'bool a(){ const char* v = getenv("MC2_G"); return v && v[0]==\'1\'; }\n'
                      'bool b(){ const char* v = getenv("MC2_G"); return v && v[0]==\'1\'; }\n',
        "mclib/b.cpp": 'static const bool s = (getenv("MC2_G") != nullptr);\n'})
    assert r["MC2_G"]["rule"] == "Lead1"


def test_atof_reader_is_a_float_with_the_unset_value_as_default():
    r, _ = derive({"code/a.cpp": 'float f(){ const char* v = getenv("MC2_F"); return v ? (float)atof(v) : 0.25f; }\n'})
    g = r["MC2_F"]
    assert (g["kind"], g["rule"], g["default"], g["seedDefaultSource"]) == ("Float", "Prefix", 0.25, "reader-idiom")


def test_atoi_reader_is_an_int():
    r, _ = derive({"code/a.cpp": 'int f(){ const char* v = getenv("MC2_N"); return v ? atoi(v) : 64; }\n'})
    g = r["MC2_N"]
    assert (g["kind"], g["default"]) == ("Int", 64)
    assert isinstance(g["default"], int)


def test_a_number_beats_a_presence_trace_of_the_same_gate():
    r, _ = derive({
        "code/a.cpp": 'int f(){ const char* v = getenv("MC2_N"); return v ? atoi(v) : 3; }\n',
        "mclib/b.cpp": 'static const bool s = (getenv("MC2_N") != nullptr);\n'})
    assert r["MC2_N"]["kind"] == "Int"


HELPERS = '''
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


def test_umbrella_inheriting_reader_makes_a_tri_gate():
    r, _ = derive({"code/h.h": HELPERS, "code/a.cpp": 'static bool s = subGate("MC2_T");\n'})
    g = r["MC2_T"]
    assert (g["kind"], g["rule"], g["default"]) == ("Tri", "Tri", "inherit")


def test_string_compare_tokens_make_an_enum_with_its_tokens():
    r, _ = derive({"code/a.cpp": 'bool f(){ const char* v = getenv("MC2_E"); '
                                  'return v && (!strcmp(v, "alpha") || !strcmp(v, "beta")); }\n'})
    g = r["MC2_E"]
    assert g["kind"] == "Enum"
    assert g["numericRange"] == {"enum": ["alpha", "beta"]}
    assert (g["default"], g["seedDefaultSource"]) == ("", "reader-idiom")


def test_comparing_against_zero_alone_is_not_an_enum():
    r, _ = derive({"code/a.cpp": 'bool f(){ const char* v = getenv("MC2_E"); return v && strcmp(v, "0") != 0; }\n'})
    assert r["MC2_E"]["kind"] == "Bool"


def test_a_literal_that_is_only_written_is_a_bool_with_unknown_default():
    r, _ = derive({"tools/h.cpp": 'void f(){ _putenv_s("MC2_W", "1"); }\n'})
    g = r["MC2_W"]
    assert (g["kind"], g["default"], g["seedDefaultSource"], g["readers"]) == ("Bool", None, "unknown", [])
    assert g["harnessValues"] == ["1"]
    assert g["owner"] == "unclassified"


# ---------------------------------------------------------------------------
# Registry: registered, registryDefault, doc, defaults agreement
# ---------------------------------------------------------------------------

def test_registered_bool_takes_its_default_from_the_registry():
    r, _ = derive({"code/a.cpp": 'void f(){ if (getenv("MC2_REG_OFF")) go(); }\n'})
    g = r["MC2_REG_OFF"]
    assert (g["registered"], g["registryDefault"]) == (True, False)
    assert (g["default"], g["seedDefaultSource"]) == (False, "registry")
    assert g["registryKind"] == "Trace"
    assert g["defaultDisagreement"] == ""


def test_registry_doc_is_the_first_sentence_and_beats_a_comment():
    r, _ = derive({"code/a.cpp": '// A comment that should lose.\n'
                                  'static const bool s = (getenv("MC2_REG_ON") != nullptr);\n'})
    g = r["MC2_REG_ON"]
    assert g["doc"] == "Default-ON; =0 disables."
    assert g["featureId"] == "MC2_FEATURE_REG_ON"


def test_registry_and_reader_defaults_that_differ_are_noted_not_hidden():
    r, _ = derive({"code/a.cpp": 'static const bool s = (getenv("MC2_REG_DISAGREE") != nullptr);\n'})
    g = r["MC2_REG_DISAGREE"]
    assert g["registryDefault"] is True and g["readerDefault"] is False
    # The readers agree with each other, so what the engine does today outranks the row.
    assert (g["default"], g["seedDefaultSource"]) == (False, "reader-idiom")
    assert "registry defaultOn=true" in g["defaultDisagreement"]
    assert gg.default_disagreements(list(r.values())) == []


def test_the_registry_default_stands_when_the_readers_do_not_agree_with_each_other():
    r, _ = derive({
        "code/a.cpp": 'static const bool a = (getenv("MC2_REG_DISAGREE") != nullptr);\n',
        "mclib/b.cpp": 'static const bool b = []{ const char* v = getenv("MC2_REG_DISAGREE"); '
                       'return !(v && v[0]==\'0\'); }();\n'})
    g = r["MC2_REG_DISAGREE"]
    assert (g["default"], g["seedDefaultSource"]) == (True, "registry")
    assert g["defaultDisagreement"].startswith("readers disagree on unset")


def test_undocumented_disagreement_is_reported():
    rec = {"kind": "Bool", "registryDefault": True, "readerDefault": False, "conflict": False,
           "defaultDisagreement": "", "name": "MC2_X"}
    assert gg.default_disagreements([rec]) == ["MC2_X"]
    assert gg.default_disagreements([dict(rec, conflict=True)]) == []
    assert gg.default_disagreements([dict(rec, defaultDisagreement="noted")]) == []


def test_registry_only_gate_is_still_a_record_and_a_null_env_row_is_not():
    r, side = derive({"code/a.cpp": 'void f(){}\n'})
    assert r["MC2_REG_ONLY"]["registered"] and r["MC2_REG_ONLY"]["readers"] == []
    assert "MC2_FEATURE_NOENV" not in r
    assert side["nullEnvRegistryRows"] == ["MC2_FEATURE_NOENV"]
    assert "MC2_REG_ONLY" in side["registryOnly"]


def test_comment_above_the_reader_is_the_doc_of_an_unregistered_gate():
    r, _ = derive({"code/a.cpp": 'void g(){}\n'
                                  '// Dumps the thing. Second sentence.\n'
                                  '// Still the same block.\n'
                                  'static const bool s = (getenv("MC2_G") != nullptr);\n'})
    assert r["MC2_G"]["doc"] == "Dumps the thing."


def test_a_blank_line_cuts_the_comment_off_from_the_reader():
    r, _ = derive({"code/a.cpp": '// Unrelated.\n\nstatic const bool s = (getenv("MC2_G") != nullptr);\n'})
    assert r["MC2_G"]["doc"] == ""


# ---------------------------------------------------------------------------
# Conflict, owner, retire verdict, harness values, ranges
# ---------------------------------------------------------------------------

def test_disagreeing_readers_set_conflict():
    r, _ = derive({
        "code/a.cpp": 'static const bool a = (getenv("MC2_G") != nullptr);\n',
        "mclib/b.cpp": 'bool b(){ const char* v = getenv("MC2_G"); return v && v[0]==\'1\'; }\n'})
    assert r["MC2_G"]["conflict"] is True


def test_agreeing_readers_do_not():
    r, _ = derive({
        "code/a.cpp": 'static const bool a = (getenv("MC2_G") != nullptr);\n',
        "mclib/b.cpp": 'void f(){ if (getenv("MC2_G")) go(); }\n'})
    assert r["MC2_G"]["conflict"] is False


@pytest.mark.parametrize("files,owner", [
    (["GameOS/gameos/gos_terrain_lighting.cpp"], "terrain"),
    (["GameOS/gameos/gos_mech_batcher.cpp"], "mech"),
    (["GameOS/gameos/vulkan_terrain_island.cpp"], "vk"),
    (["GameOS/gameos/gos_postprocess.cpp"], "post"),
    (["GameOS/gameos/gos_static_prop_batcher.cpp"], "props"),
    (["GameOS/gameos/gameosmain.cpp"], "gameos"),
    (["code/mission.cpp"], "code"),
    (["editor/EditorPlaytest.cpp"], "editor"),
    (["tools/h/main.cpp"], "unclassified"),
])
def test_owner_comes_from_the_directory_and_file(files, owner):
    assert gg.gate_owner("MC2_X", files) == owner


def test_an_editor_read_does_not_outvote_engine_code():
    assert gg.gate_owner("MC2_X", ["editor/ModPicker.cpp", "mclib/file.cpp", "mclib/fx.cpp"]) == "code"


def test_a_specific_domain_outranks_a_generic_one_and_ties_fall_to_the_name():
    assert gg.gate_owner("MC2_X", ["GameOS/gameos/gos_terrain_a.cpp", "GameOS/gameos/gameosmain.cpp",
                                   "code/a.cpp", "code/b.cpp"]) == "terrain"
    tie = ["GameOS/gameos/gos_terrain_a.cpp", "GameOS/gameos/gos_mech_batcher.cpp"]
    assert gg.gate_owner("MC2_X", tie) == "unclassified"
    assert gg.gate_owner("MC2_MECH_X", tie) == "mech"
    assert gg.gate_owner("MC2_VK_POST_X", ["GameOS/gameos/gos_postprocess.cpp",
                                           "GameOS/gameos/vulkan_post_fog_native.cpp"]) == "vk"


def test_delete_and_merge_verdicts_are_carried_but_keep_is_not():
    plan_a = ("| Gate | Default | Verdict | Sites | Action |\n|---|---|---|---|---|\n"
              "| MC2_DEAD | gone | DELETE-SAFE | x | y |\n"
              "| MC2_NRR | off | NRR | x | y |\n"
              "| MC2_LIVE | off | KEEP | x | y |\n")
    plan_b = ("| `MC2_FOLD` | OFF | 1 | 2 | 2026-10-07 | **MERGE** | none |\n"
              "| `MC2_OLD` | OFF | 1 | 2 | 2026-01-01 | **DELETE** | none |\n"
              "| `MC2_STAY` | OFF | 1 | 2 | 2026-01-01 | **KEEP** | none |\n")
    v = gg.plan_verdicts(plan_a, plan_b)
    assert v == {"MC2_DEAD": "tierA:DELETE-SAFE", "MC2_NRR": "tierA:NEEDS-READER-REMOVAL",
                 "MC2_FOLD": "tierB:MERGE", "MC2_OLD": "tierB:DELETE"}
    r, _ = derive({"code/a.cpp": 'void f(){ if (getenv("MC2_FOLD")) go(); }\n'}, plan_b=plan_b)
    assert r["MC2_FOLD"]["retireVerdict"] == "tierB:MERGE"
    assert r["MC2_FOLD"]["retireBy"] == ""


def test_harness_values_are_the_distinct_assigned_literals():
    text = '''
set MC2_X=1
$env:MC2_X = "0"
env["MC2_X"] = "2"
{"MC2_X": "1", "MC2_Y": 5}
x = os.environ.get("MC2_X", "99")
cmd = ["MC2_A", "MC2_B"]
MC2_X=%OTHER%
'''
    v = gg.config_values(text, bare_ok=True)
    assert v["MC2_X"] == {"0", "1", "2"}
    assert v["MC2_Y"] == {"5"}
    assert "MC2_A" not in v


def test_a_bare_word_after_equals_is_a_variable_in_python_but_a_value_in_a_shell():
    assert gg.config_values("env['MC2_X'] = myvar\n", bare_ok=False) == {}
    assert gg.config_values("set MC2_X=on\n", bare_ok=True)["MC2_X"] == {"on"}
    assert gg.config_values("env['MC2_X'] = 1\n", bare_ok=False)["MC2_X"] == {"1"}


def test_harness_values_flow_into_the_record_and_the_seed_does_not_read_itself():
    cfg = {"scripts/run.py": 'env["MC2_G"] = "1"\n',
           "tools/gate_table.seed.json": '{"MC2_G": "7"}'}
    inp = gg.Inputs({"code/a.cpp": 'static const bool s = (getenv("MC2_G") != nullptr);\n'}, REGISTRY, cfg)
    assert "tools/gate_table.seed.json" in gg.CONFIG_EXCLUDE
    recs, _ = gg.derive(gg.Inputs(inp.cpp, REGISTRY, {"scripts/run.py": cfg["scripts/run.py"]}))
    assert {r["name"]: r for r in recs}["MC2_G"]["harnessValues"] == ["1"]


def test_putenv_values_in_cpp_harnesses_count():
    v = gg.cpp_putenv_values('_putenv_s("MC2_A", "3"); _putenv("MC2_B=on"); _putenv_s("MC2_C", name);')
    assert v["MC2_A"] == {"3"} and v["MC2_B"] == {"on"} and "MC2_C" not in v


def test_two_value_reader_records_the_accepted_values():
    code = ('int f(){ const char* v = std::getenv("MC2_FACTOR"); if (!v || !v[0]) return 1; '
            'int f = std::atoi(v); if (f == 2) return 2; if (f == 4) return 4; return 1; }\n')
    r, _ = derive({"GameOS/gameos/gos_terrain_height_tex.cpp": code})
    g = r["MC2_FACTOR"]
    assert g["kind"] == "Int" and g["default"] == 1
    assert g["numericRange"]["enum"] == ["2", "4"]


def test_bounded_reader_records_lo_and_hi():
    code = ('unsigned f(){ const char* v = std::getenv("MC2_DIM"); '
            'if (v && v[0]) { const long n = std::atol(v); if (n >= 2 && n <= 256) return (unsigned)n; } return 64u; }\n')
    r, _ = derive({"code/a.cpp": code})
    g = r["MC2_DIM"]
    assert g["default"] == 64
    assert (g["numericRange"]["lo"], g["numericRange"]["hi"]) == (2, 256)
    assert g["numericRange"]["style"] == "compare"


def test_exclusive_bounds_are_flagged():
    code = ('unsigned f(){ const char* v = std::getenv("MC2_POOL"); '
            'if (v && v[0]) { const long n = std::atol(v); if (n > 0 && n <= 4096) return (unsigned)n; } return 0u; }\n')
    r, _ = derive({"code/a.cpp": code})
    rng = r["MC2_POOL"]["numericRange"]
    assert (rng["lo"], rng["loExclusive"], rng["hi"]) == (0, True, 4096)


def test_a_range_written_as_a_rejection_is_oriented_to_what_is_accepted():
    code = ('float f(){ const char* v = getenv("MC2_SMOOTH"); float x = 100.f; '
            'if (v) { x = (float)atof(v); if (x < 0.f || x > 2000.f) x = 100.f; } return x; }\n')
    r, _ = derive({"code/a.cpp": code})
    rng = r["MC2_SMOOTH"]["numericRange"]
    assert (rng["lo"], rng["hi"]) == (0.0, 2000.0)
    assert "loExclusive" not in rng and "hiExclusive" not in rng
    assert rng["style"] == "compare-reject"


def test_clamp_style_is_recorded():
    code = ('float f(){ const char* v = getenv("MC2_K"); '
            'return v ? std::clamp((float)atof(v), 0.0f, 8.0f) : 1.0f; }\n')
    r, _ = derive({"code/a.cpp": code})
    rng = r["MC2_K"]["numericRange"]
    assert (rng["lo"], rng["hi"], rng["style"]) == (0.0, 8.0, "clamp")


# ---------------------------------------------------------------------------
# Sentences, history, serialisation, determinism
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("text,n", [
    ("One sentence.", 1),
    ("First. Second.", 2),
    ("Uses e.g. a thing, then more.", 1),
    ("See vs. Foo for more.", 1),
    ("Value 0.5 is fine. Next one.", 2),
    ("file gos_x.cpp:12 does it. Then another.", 2),
    ("lowercase follows. so no split", 1),
    ("", 0),
])
def test_sentence_count(text, n):
    assert len(gg.split_sentences(text)) == n


def test_history_holds_exactly_the_multi_sentence_registry_rows_verbatim():
    md = gg.render_history(gg.Inputs({}, REGISTRY))
    assert "## MC2_REG_ON" in md
    assert "Default-ON; =0 disables. Second sentence has the history. Third one too." in md
    assert "## MC2_REG_OFF" not in md          # one sentence: lives in the table's doc
    assert "## MC2_REG_ONLY" in md              # 'e.g. Foo' is no boundary but 'bar. Next.' is
    assert "MC2_FEATURE_NOENV" not in md
    assert md == gg.render_history(gg.Inputs({}, REGISTRY))
    assert md.index("## MC2_REG_ON") < md.index("## MC2_REG_ONLY")


def test_nothing_is_lost_between_the_row_doc_and_the_history():
    r, _ = derive({"code/a.cpp": 'void f(){}\n'})
    md = gg.render_history(gg.Inputs({}, REGISTRY))
    for name, rec in r.items():
        if rec["registered"] and "## " + name not in md:
            assert rec["doc"] in REGISTRY    # a one-sentence doc IS the whole registry text


def test_seed_is_deterministic_and_independent_of_input_order():
    files = {
        "code/b.cpp": 'void f(){ if (getenv("MC2_B")) go(); }\n',
        "code/a.cpp": 'void f(){ if (getenv("MC2_A")) go(); }\n',
        "mclib/c.cpp": 'int f(){ const char* v = getenv("MC2_C"); return v ? atoi(v) : 2; }\n',
    }
    rev = dict(reversed(list(files.items())))
    a = gg.dumps_seed(gg.derive(gg.Inputs(files, REGISTRY, {}))[0])
    b = gg.dumps_seed(gg.derive(gg.Inputs(rev, REGISTRY, {}))[0])
    assert a == b
    assert "\r" not in a and a.endswith("\n")
    names = [g["name"] for g in json.loads(a)["gates"]]
    assert names == sorted(names)


def test_seed_record_has_every_design_field():
    r, _ = derive({"code/a.cpp": 'void f(){ if (getenv("MC2_A")) go(); }\n'})
    keys = list(r["MC2_A"])
    for k in ("name", "kind", "default", "rule", "owner", "retireBy", "doc", "readers", "conflict",
              "registered", "registryDefault", "seedDefaultSource", "harnessValues", "numericRange"):
        assert k in keys
    assert keys[:7] == ["name", "kind", "default", "rule", "owner", "retireBy", "doc"]
    assert r["MC2_A"]["kind"] in ("Bool", "Tri", "Int", "Float", "String", "Enum")
    assert r["MC2_A"]["seedDefaultSource"] in ("registry", "reader-idiom", "unknown")


def test_diff_seed_reports_added_removed_and_changed_gates():
    old = {"version": 1, "gates": [{"name": "MC2_A", "kind": "Bool"}, {"name": "MC2_B", "kind": "Bool"}]}
    new = {"version": 1, "gates": [{"name": "MC2_A", "kind": "Int"}, {"name": "MC2_C", "kind": "Bool"}]}
    d = gg.diff_seed(old, new)
    assert "new gate not in the seed: MC2_C" in d
    assert "seed gate no longer found in the tree: MC2_B" in d
    assert "MC2_A drifted in: kind" in d
    assert gg.diff_seed(old, old) == []


def test_stats_lists_every_unknown_default():
    r, side = derive({"tools/h.cpp": 'void f(){ _putenv_s("MC2_W", "1"); }\n'})
    out = gg.stats_text(list(r.values()), side)
    assert "seedDefaultSource=unknown" in out and "MC2_W" in out
    assert "kind histogram" in out and "owner histogram" in out and "rule histogram" in out


def test_scan_sites_classifies_like_the_interpreter_the_conflict_census_uses():
    code = ('bool a(){ const char* v = getenv("MC2_P"); return v && v[0]==\'1\'; }\n'
            'static const bool b = (getenv("MC2_Q") != nullptr);\n')
    mine, _ = gg.scan_sites([("code/a.cpp", gi.strip_comments(code))])
    theirs, _ = gi.scan_sources([("code/a.cpp", gi.strip_comments(code))])
    assert [(s["gate"], s["line"], s["cls"], s["vector"]) for s in mine] == \
           [(s["gate"], s["line"], s["cls"], s["vector"]) for s in theirs]


def test_committed_seed_has_the_documented_shape():
    path = os.path.join(ROOT, gg.SEED_REL)
    if not os.path.isfile(path):
        pytest.skip("seed not generated yet")
    doc = json.load(open(path, encoding="utf-8"))
    names = [g["name"] for g in doc["gates"]]
    assert doc["count"] == len(names) and names == sorted(names) and len(set(names)) == len(names)
    assert all(n.startswith("MC2_") for n in names)
    assert gg.default_disagreements(doc["gates"]) == []
    raw = open(path, "rb").read()
    assert b"\r" not in raw and raw.endswith(b"\n")
