"""Tests for tools/gl_vk_feature_coverage.py (GL<->VK twin table, feature coverage, allowlist
discipline, and the gate-amnesty flip hook).

Everything runs on synthetic repos (an in-memory Source or a throwaway git repo); the real tree
is only touched by the last test, which pins that the three known gaps stay detected.
"""
import json
import os
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import gl_vk_cpp_gates as cg  # noqa: E402
import gl_vk_feature_coverage as cov  # noqa: E402
import gl_vk_twins as tw  # noqa: E402
from gl_vk_source import Source  # noqa: E402
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
# OWNER RULE: no window for any child process (hidden_kwargs: CREATE_NO_WINDOW + SW_HIDE).
from smoke_lib import nofocus  # noqa: E402

GL_FOO = """#version 430
uniform int   u_fooEnabled;
uniform float u_barScale;
uniform float u_unrelated;
#ifdef MC2_FANCY
// fancy
#endif
void main() { gl_FragColor = vec4(u_barScale * float(u_fooEnabled) + u_unrelated); }
"""
VK_FOO_PORTS_BAR = """#version 450
layout(set = 0, binding = 0) uniform P { float u_barScale; } p;
void main() { outColor = vec4(p.u_barScale); }
"""
CPP_FOO = """
static bool fooOn() { const char* v = getenv("MC2_FOO"); return v && v[0] == '1'; }
static float barScale() { const char* v = getenv("MC2_BAR"); return v ? (float)atof(v) : 1.0f; }
static bool fancyOn() { const char* v = getenv("MC2_FANCY_GATE"); return v && v[0] == '1'; }
void bindFoo(Prog* p) {
    p->setInt("u_fooEnabled", fooOn() ? 1 : 0);
    p->setFloat("u_barScale", barScale());
    p->setFloat("u_unrelated", 3.0f);
}
"""
SEED = {"gates": [
    {"name": "MC2_FOO", "default": False, "registryKind": "Feature"},
    {"name": "MC2_BAR", "default": 1.0, "registryKind": "Feature"},
    {"name": "MC2_FANCY_GATE", "default": False, "registryKind": "Feature"},
    {"name": "MC2_TRACE_ONLY", "default": False, "registryKind": "Trace"},
]}
VARIANTS = {"variants": [
    {"id": "foo", "key": "foo", "assemble": "engine", "defines": [], "optional_groups_on": [],
     "prefix": "", "source": "GameOS/gameos/foo.cpp:9", "stages": {"frag": "shaders/foo.frag"}},
    {"id": "foo", "key": "foo{MC2_FANCY}", "assemble": "engine", "defines": ["MC2_FANCY"],
     "optional_groups_on": ["if (fancyOn())"], "prefix": "", "source": "GameOS/gameos/foo.cpp:9",
     "stages": {"frag": "shaders/foo.frag"}},
]}


def repo(vk_foo=VK_FOO_PORTS_BAR, extra=None):
    files = {"shaders/foo.frag": GL_FOO, "shaders/vulkan/foo.frag": vk_foo,
             "GameOS/gameos/foo.cpp": CPP_FOO,
             cov.GATE_SEED: json.dumps(SEED), cov.VARIANTS: json.dumps(VARIANTS)}
    files.update(extra or {})
    return Source(ROOT, files=files)


def rows_for(src):
    twins = tw.derive(src, ext_pairs=[], twin_map={"families": {}, "no_gl_twin": {}})
    files, ndefs = cov.load_cpp(src)
    return twins, cov.build_rows(src, twins, files, cov.gate_info(src), ndefs)


def by_key(rows):
    return {(r["gate"], r["feature"]): r for r in rows}


# --- twin table ---------------------------------------------------------------

def test_same_name_suffix_and_map_twins():
    src = Source(ROOT, files={
        "shaders/a.frag": "", "shaders/vulkan/a.frag": "",
        "shaders/b.vert": "", "shaders/vulkan/b_tier1.vert": "",
        "shaders/c.frag": "", "shaders/vulkan/cc.frag": "",
        "shaders/lonely.frag": ""})
    t = tw.derive(src, ext_pairs=[], twin_map={"families": {"cc": {"gl": ["c"]}}, "no_gl_twin": {}})
    pairs = {(g, v) for g, v, _ in t.file_pairs}
    assert ("shaders/a.frag", "shaders/vulkan/a.frag") in pairs
    assert ("shaders/b.vert", "shaders/vulkan/b_tier1.vert") in pairs
    assert ("shaders/c.frag", "shaders/vulkan/cc.frag") in pairs
    assert t.gl_unpaired == ["shaders/lonely.frag"]
    assert t.unclassified_vk == []


def test_unclassified_vk_shader_is_reported_and_no_gl_twin_accepts_it():
    files = {"shaders/vulkan/new_island.frag": ""}
    t = tw.derive(Source(ROOT, files=files), ext_pairs=[], twin_map={"families": {}, "no_gl_twin": {}})
    assert t.unclassified_vk == ["shaders/vulkan/new_island.frag"]
    t = tw.derive(Source(ROOT, files=files), ext_pairs=[],
                  twin_map={"families": {}, "no_gl_twin": {"new_island": "fixture"}})
    assert t.unclassified_vk == [] and "shaders/vulkan/new_island.frag" in t.no_twin_vk


def test_map_entry_naming_a_missing_shader_is_an_error():
    with pytest.raises(ValueError):
        tw.derive(Source(ROOT, files={"shaders/vulkan/x.frag": ""}), ext_pairs=[],
                  twin_map={"families": {"x": {"gl": ["ghost"]}}, "no_gl_twin": {}})


def test_real_twin_table_classifies_every_vk_shader():
    t = tw.derive(Source(ROOT))
    assert t.unclassified_vk == []
    assert t.file_pairs and t.gl_unpaired


# --- shader scan --------------------------------------------------------------

def test_uniforms_and_toggles_ignore_samplers_comments_and_self_defined_macros():
    text = tw.expand_includes(Source(ROOT, files={"shaders/s.frag": """
uniform sampler2D u_tex;
uniform float u_a, u_b = 1.0;
// uniform int u_commented;
#define LOCAL_ONLY 1
#ifdef LOCAL_ONLY
#endif
#if defined(MC2_REAL_TOGGLE) && GL_ES
#endif
"""}), "shaders/s.frag")[0]
    assert tw.uniforms_of(text) == {"u_a", "u_b"}
    assert tw.toggles_of(text) == {"MC2_REAL_TOGGLE"}


# --- gate attribution ---------------------------------------------------------

def test_setter_value_reaches_gate_through_accessor_and_local_alias():
    src = Source(ROOT, files={"GameOS/gameos/x.cpp": """
static bool onA() { const char* v = getenv("MC2_A"); return v && v[0]; }
void f(Prog* p) {
    const bool lit = onA() && ready;
    p->setInt("u_x", lit ? 1 : 0);
    static const bool s = [](){ const char* e = getenv("MC2_B"); return e && e[0]; }();
    p->setInt("u_y", s ? 1 : 0);
}
"""})
    files, ndefs = cov.load_cpp(src)
    idx = cg.GateIndex(files, ndefs)
    (px, ox), = cg.feature_sites(files, "u_x")
    (py, oy), = cg.feature_sites(files, "u_y")
    assert idx.attribute(px, ox) == {"MC2_A": 2}
    assert idx.attribute(py, oy) == {"MC2_B": 1}


def test_phase_frame_member_set_by_an_earlier_method_reaches_the_gate():
    # A function split into phase helpers hands its locals over in a *Frame struct: the flag is set in one method
    # and read by the setter in a later one. An ordinary class gets no such resolution.
    body = """
static bool onA() { const char* v = getenv("MC2_A"); return v && v[0]; }
void %s::bind() { active = onA() && ready; }
void %s::draw(Prog* p) { p->setInt("u_x", active ? 1 : 0); }
"""
    for owner, want in (("DrawFrame", {"MC2_A": 2}), ("Drawer", {})):
        src = Source(ROOT, files={"GameOS/gameos/x.cpp": body % (owner, owner)})
        files, ndefs = cov.load_cpp(src)
        idx = cg.GateIndex(files, ndefs)
        (px, ox), = cg.feature_sites(files, "u_x")
        assert idx.attribute(px, ox) == want, owner


def test_ambiguous_helper_name_does_not_leak_a_gate():
    src = Source(ROOT, files={
        "GameOS/gameos/a.cpp": 'bool apply() { return getenv("MC2_NOISE") != nullptr; }\n',
        "GameOS/gameos/b.cpp": "bool apply() { return true; }\n",
        "GameOS/gameos/c.cpp": 'void f(P* p) { p->setInt("u_x", apply()); }\n'})
    files, ndefs = cov.load_cpp(src)
    (px, ox), = cg.feature_sites(files, "u_x")
    assert cg.GateIndex(files, ndefs).attribute(px, ox) == {}


def test_location_variable_setter_is_followed():
    src = Source(ROOT, files={"GameOS/gameos/loc.cpp": """
static GLint s_loc_u_k = -1;
static bool kOn() { const char* v = getenv("MC2_K"); return v && v[0]; }
void init(Prog* p) { s_loc_u_k = glGetUniformLocation(p, "u_k"); }
void draw() { glUniform1i(s_loc_u_k, kOn() ? 1 : 0); }
"""})
    files, ndefs = cov.load_cpp(src)
    idx = cg.GateIndex(files, ndefs)
    got = {}
    for path, off in cg.feature_sites(files, "u_k"):
        got.update(idx.attribute(path, off))
    assert got == {"MC2_K": 1}


def test_enclosing_condition_is_attributed_separately_from_the_value():
    src = Source(ROOT, files={"GameOS/gameos/c.cpp": """
static bool gOn() { const char* v = getenv("MC2_G"); return v && v[0]; }
void f(Prog* p) {
    if (gOn()) {
        p->setFloat("u_w", 2.0f);
    }
}
"""})
    files, ndefs = cov.load_cpp(src)
    idx = cg.GateIndex(files, ndefs)
    (pw, ow), = cg.feature_sites(files, "u_w")
    assert idx.attribute(pw, ow) == {}
    assert idx.attribute_cond(pw, ow) == {"MC2_G": 1}


# --- PORTED / NOT_PORTED detection ---------------------------------------------

def test_not_ported_and_ported_rows():
    twins, rows = rows_for(repo())
    r = by_key(rows)
    foo = r[("MC2_FOO", "u_fooEnabled")]
    assert (foo["status"], foo["confidence"], foo["gl_shader"]) == ("NOT_PORTED", "HIGH", "shaders/foo.frag")
    assert foo["vk_twin"] == "shaders/vulkan/foo.*"
    bar = r[("MC2_BAR", "u_barScale")]
    assert (bar["status"], bar["confidence"]) == ("PORTED", "HIGH")
    assert bar["gate_default_on"] is True and foo["gate_default_on"] is False
    assert not [x for x in rows if x["feature"] == "u_unrelated"]      # no gate drives it


def test_define_toggle_from_variant_group_is_a_feature():
    _, rows = rows_for(repo())
    fancy = by_key(rows)[("MC2_FANCY_GATE", "MC2_FANCY")]
    assert (fancy["kind"], fancy["status"], fancy["confidence"]) == ("define", "NOT_PORTED", "HIGH")
    vk = VK_FOO_PORTS_BAR + "\n#define MC2_FANCY 1\n"
    _, rows = rows_for(repo(vk_foo=vk))
    assert by_key(rows)[("MC2_FANCY_GATE", "MC2_FANCY")]["status"] == "PORTED"


def test_stem_and_packed_slot_spellings_count_as_ported_with_medium_confidence():
    vk = """#version 450
layout(set = 0, binding = 0) uniform P {
    int   fooEnabled;
    vec4  f8;   // (u_barScale, pad, pad, pad)
} U;
void main() { outColor = vec4(float(U.fooEnabled) + U.f8.x); }
"""
    _, rows = rows_for(repo(vk_foo=vk))
    r = by_key(rows)
    assert (r[("MC2_FOO", "u_fooEnabled")]["status"], r[("MC2_FOO", "u_fooEnabled")]["confidence"]) \
        == ("PORTED", "MEDIUM")
    assert (r[("MC2_BAR", "u_barScale")]["status"], r[("MC2_BAR", "u_barScale")]["vk_evidence"]) \
        == ("PORTED", "packed:f8")


def test_name_only_in_a_vk_comment_is_not_ported():
    vk = "// u_fooEnabled is handled elsewhere\nvoid main() {}\n"
    _, rows = rows_for(repo(vk_foo=vk))
    assert by_key(rows)[("MC2_FOO", "u_fooEnabled")]["status"] == "NOT_PORTED"


def test_name_in_vk_island_cpp_only_lowers_confidence():
    extra = {"GameOS/gameos/vulkan_foo_island.cpp": "struct P { int u_fooEnabled; };\n"}
    _, rows = rows_for(repo(extra=extra))
    r = by_key(rows)[("MC2_FOO", "u_fooEnabled")]
    assert (r["status"], r["confidence"], r["vk_evidence"]) == ("NOT_PORTED", "MEDIUM", "cpp-only")


def test_shader_without_a_vk_twin_is_no_twin():
    src = repo(extra={"shaders/vulkan/foo.frag": None})
    del src._mem["shaders/vulkan/foo.frag"]
    _, rows = rows_for(src)
    assert {x["status"] for x in rows} == {"NO_TWIN"}


def test_trace_gates_are_not_features():
    cpp = CPP_FOO + 'void g(Prog* p) { p->setInt("u_fooEnabled", getenv("MC2_TRACE_ONLY") ? 1 : 0); }\n'
    _, rows = rows_for(repo(extra={"GameOS/gameos/foo.cpp": cpp}))
    assert not [x for x in rows if x["gate"] == "MC2_TRACE_ONLY"]


def test_stems_never_degrade_to_a_single_generic_word():
    assert cov.stems_of("u_terrainAmbientFloor") == ["terrainAmbientFloor", "ambientFloor"]
    assert cov.stems_of("u_mode") == []
    assert cov.stems_of("u_waterDebugMode") == ["waterDebugMode"]     # debugMode would collide


# --- allowlist discipline ------------------------------------------------------

def test_missing_and_stale_allowlist_entries(tmp_path):
    _, rows = rows_for(repo())
    need = cov.enforced(rows)
    assert need == {("MC2_FOO", "shaders/foo.frag"), ("MC2_FANCY_GATE", "shaders/foo.frag")}
    allow = {("MC2_FOO", "shaders/foo.frag"): "why", ("MC2_GONE", "shaders/foo.frag"): "why"}
    new, stale = cov.grade(rows, allow)
    assert new == [("MC2_FANCY_GATE", "shaders/foo.frag")]
    assert stale == [("MC2_GONE", "shaders/foo.frag")]
    assert cov.grade(rows, {k: "w" for k in need}) == ([], [])


def test_medium_confidence_rows_are_advisory_not_enforced():
    extra = {"GameOS/gameos/vulkan_foo_island.cpp": "struct P { int u_fooEnabled; };\n"}
    _, rows = rows_for(repo(extra=extra))
    assert ("MC2_FOO", "shaders/foo.frag") not in cov.enforced(rows)


def test_allowlist_file_parsing(tmp_path):
    p = tmp_path / "a.allowlist"
    p.write_text("# c\n\nMC2_X | shaders/a.frag | because | with a pipe\n", encoding="utf-8")
    assert cov.load_allowlist(str(p)) == {("MC2_X", "shaders/a.frag"): "because | with a pipe"}
    p.write_text("MC2_X | shaders/a.frag\n", encoding="utf-8")
    with pytest.raises(ValueError):
        cov.load_allowlist(str(p))


def test_suggest_line_names_the_feature_and_twin():
    _, rows = rows_for(repo())
    line = cov.suggest_line(rows, "MC2_FOO", "shaders/foo.frag")
    assert line.startswith("MC2_FOO | shaders/foo.frag | uniform u_fooEnabled")
    assert "shaders/vulkan/foo" in line


# --- gate-amnesty flip hook ----------------------------------------------------

def seed_text(**defaults):
    return json.dumps({"gates": [{"name": k, "default": v} for k, v in defaults.items()]})


def test_flip_off_to_on_of_an_allowlisted_gate_is_reported_with_its_why():
    allow = {("MC2_SUN_AXIS_FIX", "shaders/mech.vert"): "VK calc_light lacks the sun-axis parameter",
             ("MC2_OTHER", "shaders/x.frag"): "other"}
    base = cov.gate_defaults(None, seed_text(MC2_SUN_AXIS_FIX=False, MC2_OTHER=False))
    head = cov.gate_defaults(None, seed_text(MC2_SUN_AXIS_FIX=True, MC2_OTHER=False))
    msgs = cov.flip_check(allow, base, head)
    assert len(msgs) == 1 and "MC2_SUN_AXIS_FIX" in msgs[0] and "calc_light lacks" in msgs[0]


def test_numeric_default_going_nonzero_counts_as_a_flip_and_staying_on_does_not():
    allow = {("MC2_WATER_SHORE_MEANDER", "shaders/w.frag"): "meander missing in VK"}
    off, on = (cov.gate_defaults(None, seed_text(MC2_WATER_SHORE_MEANDER=v)) for v in (0.0, 75.0))
    assert cov.flip_check(allow, off, on)
    assert not cov.flip_check(allow, on, on)
    assert not cov.flip_check(allow, on, off)          # turning a gate OFF is fine


def test_flip_check_end_to_end_on_a_throwaway_git_repo(tmp_path):
    def git(*a):
        subprocess.run(["git", "-c", "user.email=t@t", "-c", "user.name=t", *a], cwd=tmp_path, check=True,
                       capture_output=True,
                       **nofocus.hidden_kwargs())
    (tmp_path / "tools").mkdir()
    (tmp_path / "tools" / "vk_not_ported.allowlist").write_text(
        "MC2_FLIPME | shaders/foo.frag | VK twin lacks it\n", encoding="utf-8")
    seed = tmp_path / "tools" / "gate_table.seed.json"
    seed.write_text(seed_text(MC2_FLIPME=False), encoding="utf-8")
    git("init", "-q")
    git("add", "-A")
    git("commit", "-qm", "base")
    assert cov.main(["--flip-check", "HEAD"], root=str(tmp_path)) == 0
    seed.write_text(seed_text(MC2_FLIPME=True), encoding="utf-8")
    assert cov.main(["--flip-check", "HEAD"], root=str(tmp_path)) == 1
    git("commit", "-qam", "flip")
    assert cov.main(["--flip-check", "HEAD~1"], root=str(tmp_path)) == 1
    assert cov.main(["--flip-check", "HEAD"], root=str(tmp_path)) == 0


# --- the real tree --------------------------------------------------------------

def test_real_tree_known_gaps_are_not_ported_and_allowlisted():
    # Graded at HEAD, not the working tree: other lanes edit shaders continuously and the
    # committed allowlist is only promised to match the committed shaders.
    twins, rows = cov.compute(Source(ROOT, ref="HEAD"))
    np_pairs = {(r["gate"], r["gl_shader"]) for r in rows if r["status"] == "NOT_PORTED"}
    for pair in (("MC2_SKYBOX_FOG_EXCLUDE", "shaders/fog_oob.frag"),
                 ("MC2_SKYBOX_FOG_EXCLUDE", "shaders/edge_fog.frag"),
                 ("MC2_WATER_SHORE_MEANDER", "shaders/gos_terrain_water_mdi.frag"),
                 ("MC2_SUN_AXIS_FIX", "shaders/mech.vert")):
        assert pair in np_pairs
    allow = cov.load_allowlist(os.path.join(ROOT, cov.ALLOWLIST))
    assert cov.grade(rows, allow) == ([], [])
