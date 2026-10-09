"""Tests for tools/axis_swizzle_ratchet.py (synthetic fixtures; stdlib + git only).

Run:
  py -3 -B -m pytest scripts/tests/test_axis_swizzle_ratchet.py -q -p no:cacheprovider
"""
import importlib.util
import io
import json
import os
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_spec = importlib.util.spec_from_file_location("axis_swizzle_ratchet", os.path.join(ROOT, "tools", "axis_swizzle_ratchet.py"))
asr = importlib.util.module_from_spec(_spec)
sys.modules["axis_swizzle_ratchet"] = asr
_spec.loader.exec_module(asr)


def shapes(src, path="x.cpp"):
    return [s.shape for s in asr.scan_text(path, src)["sites"]]


# ---------------------------------------------------------------- one case per shape
@pytest.mark.parametrize("src,shape", [
    ("vec3 g = vec3(-a.x, a.z, a.y);", "triplet-ctor"),
    ("vec3 g = vec3( - worldStuff.x ,  worldStuff.z ,  worldStuff.y );", "triplet-ctor"),
    ("vec4 g = vec4(-ld.light_dir[i].x, ld.light_dir[i].z, ld.light_dir[i].y, 1.0);", "triplet-ctor"),
    ("auto p = Stuff::Point3D(-p.x, p.z, p.y);", "triplet-ctor"),
    ("return Stuff::Vector3D(-physicalPos.x, physicalPos.z, physicalPos.y);", "triplet-ctor"),
    ("V3 r = v3(-v.x, v.z, v.y);", "triplet-ctor"),
    ("float c[3] = { -r.x, r.z, r.y };", "triplet-brace"),
    ('printf("%f %f %f", -o.x, o.z, o.y);', "triplet-arglist"),
    ("float lx = -d.x, ly = d.z, lz = d.y;", "decl-chain"),
    ("vec3 g = vec3(-a.x, a.zy);", "swizzle-zy"),
    ("vec3 g = a.xzy * vec3(-1.0, 1.0, 1.0);", "swizzle-xzy"),
    ("vec3 g = vec3(-1, 1, 1) * a.xzy;", "swizzle-xzy"),
    ("o.x = -i.x;\no.y = i.z;\no.z = i.y;\n", "assign-triple"),
    ("o.x = -i.x; o.y = i.z; o.z = i.y;", "assign-triple"),
    ("p->x = -q->x;\np->y = q->z;\np->z = q->y;\n", "assign-triple"),
    ("o[0] = -i[0];\no[1] = i[2];\no[2] = i[1];\n", "assign-triple"),
    ("X=-v.x; Y= v.z; Z= v.y;", "assign-scalars"),
    ("float px = -tv.position.x;\nfloat py = tv.position.z;\nfloat pz = tv.position.y;\n", "assign-scalars"),
])
def test_shape_detected(src, shape):
    assert shapes(src) == [shape]


def test_member_chain_and_index_bases():
    assert shapes("v3(-a.b[3].c.x, a.b[3].c.z, a.b[3].c.y)") == ["triplet-ctor"]
    assert shapes("v3(-a->b.x, a->b.z, a->b.y)") == ["triplet-ctor"]


def test_assign_triple_any_statement_order():
    assert shapes("o.y = i.z;\no.x = -i.x;\no.z = i.y;\n") == ["assign-triple"]


def test_line_number_is_first_line_of_the_site():
    r = asr.scan_text("x.cpp", "int a;\nint b;\no.x = -i.x;\no.y = i.z;\no.z = i.y;\n")
    assert [(s.line, s.shape) for s in r["sites"]] == [(3, "assign-triple")]
    r = asr.scan_text("x.vert", "void main(){\n  vec3 g = vec3(-a.x,\n                a.z,\n                a.y);\n}\n")
    assert [(s.line, s.shape) for s in r["sites"]] == [(2, "triplet-ctor")]


# ---------------------------------------------------------------- false-positive guard
@pytest.mark.parametrize("src", [
    "vec3 g = vec3(a.x, a.y, a.z);",
    "vec3 g = vec3(-a.x, a.y, a.z);",
    "vec3 g = vec3(-a.x, -a.z, -a.y);",
    "vec3 g = vec3(a.x, a.z, a.y);",                       # no negation: different convention
    "vec3 g = vec3(-a.x, a.z, a.w);",
    "vec3 g = vec3(-a.x, b.z, a.y);",                      # mixed bases are not a swap of one vector
    "vec3 g = vec3(-a.x, h, a.y);",                        # elevation substituted
    "vec3 g = vec3(-a.x, a.y, a.z) + vec3(-b.x, b.z, c.y);",
    "o.x = -i.x;\no.y = i.y;\no.z = i.z;\n",
    "o.x = -i.x;\no.y = terrainElevation(i);\no.z = i.y;\n",  # y looked up, not copied
    "o.x = -i.x;\ncall();\no.y = i.z;\no.z = i.y;\n",         # not adjacent statements
    "o.x = -i.x;\no.y = i.z;\no.z = i.z;\n",
    "v.x = -v.x;\nv.y = v.z;\nv.z = v.y;\n",                 # in place: not the conversion
    "a = -v.x; b = v.y; c = v.z;",
    "x = -v.x; x = v.z; y = v.y;",                         # duplicate lhs
    "vec3 g = a.xzy;",
])
def test_not_a_swap(src):
    assert shapes(src) == []


def test_comments_and_strings_are_ignored():
    src = ('// vec3(-a.x, a.z, a.y) Stuff->GL\n'
           '/* o.x = -i.x; o.y = i.z; o.z = i.y; */\n'
           'const char* s = "vec3(-a.x, a.z, a.y)";\n')
    assert shapes(src) == []


# ---------------------------------------------------------------- helpers / waivers / ports
def test_helper_definition_is_not_a_hand_copy_and_calls_are_counted():
    src = ("static V3 swapStuffToMC2(const V3& v) { return v3(-v.x, v.z, v.y); }\n"
           "V3 a = swapStuffToMC2(b);\n"
           "V3 c = swapStuffToMC2(d);\n")
    r = asr.scan_text("tools/h.cpp", src)
    assert r["sites"] == [] and len(r["helper_def_sites"]) == 1
    assert r["helper_defs"] == 1 and r["helper_calls"] == 2


def test_multiline_helper_definition_is_exempt():
    src = "vec3 mc2ToRender(vec3 v)\n{\n    return vec3(-v.x, v.z, v.y);\n}\nvec3 g = mc2ToRender(p);\n"
    r = asr.scan_text("shaders/x.vert", src)
    assert r["sites"] == [] and r["helper_calls"] == 1


def test_helper_home_file_is_exempt():
    r = asr.scan_text("shaders/include/axis.hglsl", "vec3 f(vec3 v){ return vec3(-v.x, v.z, v.y); }\n")
    assert r["sites"] == []


def test_member_call_is_not_a_helper_call():
    assert asr.scan_text("x.cpp", "obj.mc2ToRender(x);\n")["helper_calls"] == 0


def test_waiver_marker_exempts_a_site():
    r = asr.scan_text("x.cpp", "// axis-swizzle: allow (documented exception)\nV3 a = v3(-v.x, v.z, v.y);\n")
    assert r["sites"] == [] and len(r["waived"]) == 1


def test_matrix_helper_references_counted_separately():
    r = asr.scan_text("x.cpp", "auto m = kAxisSwapMC2toGL; auto n = makeAxisSwapMC2toGL();\n")
    assert r["matrix_refs"] == 2 and r["sites"] == [] and r["helper_calls"] == 0


def test_port_block_spans_recorded():
    src = "//PORT-BEGIN:calc\nvec3 g = vec3(-a.x, a.z, a.y);\n//PORT-END:calc\n"
    r = asr.scan_text("shaders/x.hglsl", src)
    assert r["port_spans"] == [(1, 3, "calc")]


# ---------------------------------------------------------------- aggregation / dirs
def test_scan_aggregates_per_dir_and_file():
    sw = "vec3 g = vec3(-a.x, a.z, a.y);\n"
    res = asr.scan({"shaders/a.vert": sw + sw, "shaders/vulkan/a.vert": sw,
                    "mclib/b.cpp": "o.x = -i.x; o.y = i.z; o.z = i.y;"})
    assert res.total == 4
    assert res.per_dir == {"shaders": 2, "shaders/vulkan": 1, "mclib": 1}
    assert res.per_file["shaders/a.vert"] == 2
    assert res.per_shape["triplet-ctor"] == 3 and res.per_shape["assign-triple"] == 1


def test_wanted_filters():
    assert asr.wanted("shaders/a.vert") and asr.wanted("mclib/a.cpp")
    assert not asr.wanted("3rdparty/x/a.cpp") and not asr.wanted("docs/a.md")
    assert not asr.wanted("graphify-out/a.cpp") and not asr.wanted("shaders/include/x.gen.hglsl")


# ---------------------------------------------------------------- twins
def test_twins_by_name_tier1_and_registry():
    paths = {"shaders/a.vert", "shaders/vulkan/a.vert", "shaders/mech.vert", "shaders/vulkan/mech_tier1.vert",
             "shaders/include/lighting.hglsl", "shaders/lone.frag"}
    pairs = [("shaders/include/lighting.hglsl", "shaders/vulkan/mech_tier1.vert")]
    assert asr.twins_for("shaders/a.vert", paths, pairs) == [("shaders/vulkan/a.vert", "same name")]
    assert asr.twins_for("shaders/vulkan/a.vert", paths, pairs) == [("shaders/a.vert", "same name")]
    assert asr.twins_for("shaders/mech.vert", paths, pairs) == [("shaders/vulkan/mech_tier1.vert", "_tier1 name")]
    assert dict(asr.twins_for("shaders/vulkan/mech_tier1.vert", paths, pairs)) == {
        "shaders/mech.vert": "_tier1 name", "shaders/include/lighting.hglsl": "glsl_port_diff registry"}
    assert asr.twins_for("shaders/lone.frag", paths, pairs) == []
    assert asr.twins_for("mclib/x.cpp", paths, pairs) == []


def test_registry_pairs_parse():
    txt = 'Port("calc_light", "shaders/include/lighting.hglsl", "shaders/vulkan/mech_tier1.vert",\n rename=[])'
    assert asr.registry_pairs(txt) == [("shaders/include/lighting.hglsl", "shaders/vulkan/mech_tier1.vert")]


def test_doc_is_deterministic_and_lists_sites():
    sw = "vec3 g = vec3(-a.x, a.z, a.y);\n"
    files = {"shaders/a.vert": sw, "shaders/vulkan/a.vert": sw}
    d1 = asr.render_doc(asr.scan(files), set(files), [])
    d2 = asr.render_doc(asr.scan(dict(reversed(list(files.items())))), set(files), [])
    assert d1 == d2
    assert "`shaders/a.vert:1` triplet-ctor" in d1
    assert "| `shaders/a.vert` | 1 | `shaders/vulkan/a.vert` | same name | 1 |" in d1


# ---------------------------------------------------------------- check()
def _base(total, dirs=None):
    return {"total": total, "dirs": dirs or {}, "files": {}}


def test_check_fails_only_when_total_rises():
    res = asr.scan({"mclib/a.cpp": "v3(-v.x, v.z, v.y);\nv3(-w.x, w.z, w.y);\n"})
    assert asr.check(res, _base(1), out=io.StringIO()) == 1
    assert asr.check(res, _base(2), out=io.StringIO()) == 0
    assert asr.check(res, _base(5), out=io.StringIO()) == 0


def test_check_dir_move_only_warns():
    res = asr.scan({"mclib/a.cpp": "v3(-v.x, v.z, v.y);\n"})
    out = io.StringIO()
    assert asr.check(res, _base(1, {"code": 1}), out=out) == 0
    assert "warn: mclib rose" in out.getvalue()


# ---------------------------------------------------------------- end to end on a git repo
def _git(repo, *args):
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false"] + list(args),
                          cwd=repo, check=True, capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout


@pytest.fixture()
def repo(tmp_path):
    r = str(tmp_path)
    _git(r, "init", "-q")
    os.makedirs(os.path.join(r, "shaders"))
    with open(os.path.join(r, "shaders", "a.vert"), "w") as fh:
        fh.write("vec3 g = vec3(-a.x, a.z, a.y);\n")
    _git(r, "add", "-A")
    _git(r, "commit", "-q", "-m", "init")
    return r


def test_cli_baseline_check_round_trip_is_head_based(repo, capsys):
    bl = os.path.join(repo, "bl.json")
    assert asr.main(["--repo", repo, "--baseline", bl, "--write-baseline"]) == 0
    with open(bl) as fh:
        assert json.load(fh)["total"] == 1
    assert asr.main(["--repo", repo, "--baseline", bl, "--check"]) == 0

    # an uncommitted new site must not move the HEAD-based gate ...
    with open(os.path.join(repo, "shaders", "b.vert"), "w") as fh:
        fh.write("vec3 g = vec3(-a.x, a.z, a.y);\n")
    assert asr.main(["--repo", repo, "--baseline", bl, "--check"]) == 0
    # ... but --worktree sees it
    assert asr.main(["--repo", repo, "--baseline", bl, "--check", "--worktree"]) == 1

    # once committed, the ratchet trips
    _git(repo, "add", "shaders/b.vert")
    _git(repo, "commit", "-q", "-m", "add copy")
    assert asr.main(["--repo", repo, "--baseline", bl, "--check"]) == 1
    assert "shaders/b.vert" in capsys.readouterr().out


def test_cli_tighten_lowers_baseline(repo):
    bl = os.path.join(repo, "bl.json")
    base = {"total": 5, "dirs": {"shaders": 5}, "files": {"shaders/a.vert": 5}, "helper_calls": 0}
    with open(bl, "w") as fh:
        json.dump(base, fh)
    assert asr.main(["--repo", repo, "--baseline", bl, "--check", "--tighten"]) == 0
    with open(bl) as fh:
        assert json.load(fh)["total"] == 1


def test_cli_missing_baseline_is_usage_error(repo):
    assert asr.main(["--repo", repo, "--baseline", os.path.join(repo, "nope.json"), "--check"]) == 2


def test_cli_write_doc(repo):
    os.makedirs(os.path.join(repo, "docs"))
    assert asr.main(["--repo", repo, "--write-doc"]) == 0
    with open(os.path.join(repo, "docs", "axis-conversion-sites.md"), encoding="utf-8") as fh:
        assert "shaders/a.vert:1" in fh.read()
