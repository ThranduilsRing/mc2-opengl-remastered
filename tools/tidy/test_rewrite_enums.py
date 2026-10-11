"""What: tests for rewrite_enums.py - which enums are refused (arithmetic / index use, shared names, typedef, large values) and the rewrite text.
Why: the compile decides the rest, but each wrongly offered enum costs a bisect over its includers."""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rewrite_enums as re_


def run(files, **kw):
    enums, uses = re_.analyse(files, **kw)
    return {e.name: e for e in enums}


def test_plain_enum_is_a_candidate_and_rewrites_with_using_enum():
    src = "enum Mode { MODE_A, MODE_B = 2 };\nint f(Mode m) { return m == MODE_A; }\n"
    e = run({"a.h": src})["Mode"]
    assert e.reasons == [] and [n for n, _ in e.enumerators] == ["MODE_A", "MODE_B"]
    assert re_.rewrite_text(src, [e]) == "enum class Mode { MODE_A, MODE_B = 2 };\nusing enum Mode;\nint f(Mode m) { return m == MODE_A; }\n"


def test_multiline_body_and_class_scope_indent():
    src = "struct S {\n    enum Kind\n    {\n        K1,\n        K2\n    };\n    Kind k;\n};\n"
    e = run({"a.h": src})["Kind"]
    out = re_.rewrite_text(src, [e])
    assert "    enum class Kind\n" in out and "    };\n    using enum Kind;\n    Kind k;" in out


def test_arithmetic_flag_and_index_uses_are_refused():
    files = {"a.h": "enum F { F_A = 1, F_B = 2 };\nenum I { I_X, I_Y };\nenum C { C_A, C_B };\n",
             "a.cpp": "int m = F_A | F_B;\nint t[3]; int v = t[I_Y];\nvoid g(C c) { if (c == C_A) {} switch (c) { case C_B: break; } }\n"}
    e = run(files)
    assert "arithmetic-or-index-use" in e["F"].reasons and "arithmetic-or-index-use" in e["I"].reasons
    assert e["C"].reasons == []


def test_refusals_typedef_twice_shared_enumerators_and_large_values():
    files = {"a.h": "typedef enum T1 { T_A } T1;\nenum Dup { D_A };\nenum Sh1 { S_A };\nenum Sh2 { S_A };\nenum Big { B_A = 0xFFFFFFFF };\nenum BigFixed : unsigned { BF_A = 0xFFFFFFFF };\n",
             "b.h": "enum Dup { D_B };\n"}
    e = run(files)
    assert "T1" not in e and "name-defined-twice" in e["Dup"].reasons and "enumerator-name-shared" in e["Sh1"].reasons
    assert "needs-unscoped-underlying-type" in e["Big"].reasons and e["BigFixed"].reasons == []


def test_a_qualified_enumerator_in_arithmetic_or_an_int_compare_is_refused():
    files = {"a.h": "struct D { enum P { P_A, P_COUNT }; };\nenum Q { Q_A, Q_B };\n",
             "a.cpp": "void f(int i, int n) { for (int k = 0; k < D::P_COUNT; ++k) {} if (n >= D::P_COUNT) {} }\n"
                      "void g(Q q) { switch (q) { case Q_A: break; } if (q == ::Q_B) {} }\n"}
    e = run(files)
    assert "arithmetic-or-index-use" in e["P"].reasons and e["Q"].reasons == []


def test_the_skip_ledger_lists_names_and_ignores_comments(tmp_path):
    f = tmp_path / "skip.txt"
    f.write_text("# header\nFoo  reason one\n\nBar\tx y\n", encoding="utf-8")
    assert re_.load_skip(str(f)) == {"Foo", "Bar"} and re_.load_skip(str(tmp_path / "none.txt")) == set()
    shipped = [l for l in open(re_.SKIP_FILE, encoding="utf-8") if l.strip() and not l.lstrip().startswith("#")]
    assert shipped and all(len(l.split()) >= 2 for l in shipped)                  # every ledger line carries its reason
