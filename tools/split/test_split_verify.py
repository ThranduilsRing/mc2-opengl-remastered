"""What: unit tests for tools/split/split_verify.py (the fail-closed split checks).
Why: the checks guard every file split; each one needs a synthetic violation that must fail and an allowlist that must silence it."""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import split_verify as sv  # noqa: E402

OLD = b"""#include "a.h"
#define FEAT 1
static const bool s_gate = (getenv("MC2_X") != nullptr);
static int counter = 0;
char table[2] = {1, 2};
#ifdef FEAT
void f() { counter++; }
#endif
void g() { f(); }
"""


def run(old, files, allow=None, stem="m"):
    return sv.verify(old, {k: v.encode() if isinstance(v, str) else v for k, v in files.items()}, stem, "code/m.cpp", allow or {}, False, False)


def kinds(findings):
    return sorted(set(c for c, _ in findings))


GOOD = {
    "code/m.cpp": '#include "m_internal.h"\nstatic int counter = 0;\nvoid f() { counter++; }\n',
    "code/m_g.cpp": '#include "m_internal.h"\nvoid g() { f(); }\n',
    "code/m_internal.h": '#ifndef M_INTERNAL_H\n#define M_INTERNAL_H\n#include "a.h"\n#define FEAT 1\nextern char table[2];\nvoid f();\n#endif // M_INTERNAL_H\n',
}


def old_with(extra=b""):
    return OLD + extra


def test_move_only_split_is_clean_apart_from_the_dropped_gate():
    old = OLD.replace(b'static const bool s_gate = (getenv("MC2_X") != nullptr);\n', b"").replace(b"char table[2] = {1, 2};\n", b"")
    good = dict(GOOD)
    good["code/m.cpp"] = good["code/m.cpp"]
    f = run(old, good)
    assert [m for c, m in f if c != "lines"] == [], f


def test_eager_file_scope_gate_turned_lazy_is_flagged():
    new = dict(GOOD)
    new["code/m.cpp"] = '#include "m_internal.h"\nstatic int counter = 0;\nbool gate() { static const bool v = (getenv("MC2_X") != nullptr); return v; }\nvoid f() { counter++; }\n'
    f = run(OLD, new)
    assert any("getenv" in m for c, m in f if c == "lines"), f


def test_static_to_inline_change_needs_an_explicit_allowance():
    old = b"static int counter = 0;\n"
    new = {"code/m.cpp": '#include "m_internal.h"\n', "code/m_internal.h": '#ifndef M_INTERNAL_H\n#define M_INTERNAL_H\ninline int counter = 0;\n#endif // M_INTERNAL_H\n'}
    assert "lines" in kinds(run(old, new))
    ok = run(old, new, {"edits": [["static int counter = 0;", "inline int counter = 0;"]], "header_vars": ["counter"]})
    assert ok == [], ok


def test_non_ascii_character_lost_is_flagged():
    old = "printf(\"a — b\n\");\n".encode("utf-8")
    new = {"code/m.cpp": 'printf("a - b\n");\n'}
    assert "lines" in kinds(run(old, new))


def test_macro_defined_after_its_use_in_the_piece_is_flagged():
    new = dict(GOOD)
    new["code/m_internal.h"] = '#ifndef M_INTERNAL_H\n#define M_INTERNAL_H\n#include "a.h"\nextern char table[2];\nvoid f();\n#endif // M_INTERNAL_H\n'
    new["code/m.cpp"] = '#include "m_internal.h"\nstatic int counter = 0;\n#ifdef FEAT\nvoid f() { counter++; }\n#endif\n'
    f = run(OLD, new)
    assert "macros" in kinds(f), f


def test_header_static_and_mutable_inline_variables_are_flagged():
    h = "#ifndef M_INTERNAL_H\n#define M_INTERNAL_H\nstatic int a = 0;\ninline int b = 0;\ninline constexpr int c = 1;\nstatic inline int fn() { return 1; }\n#endif // M_INTERNAL_H\n"
    f = sv.check_header(sv.lines_of(h.encode()), {})
    msgs = " | ".join(m for _, m in f)
    assert "static int a" in msgs and "inline int b" in msgs and "inline constexpr" not in msgs.replace("(use inline constexpr", "") and "fn" not in msgs, msgs
    assert sv.check_header(sv.lines_of(h.encode()), {"header_vars": ["a", "b"]}) == []


def test_inline_function_called_from_another_piece_is_flagged():
    files = {"x.cpp": "inline int helper(int v) { return v; }\nint a() { return helper(1); }\n", "y.cpp": "int b() { return helper(2); }\n", "x_internal.h": "int b();\n"}
    f = sv.check_inline_calls({k: sv.lines_of(v.encode()) for k, v in files.items()}, "x_internal.h", {})
    assert f and f[0][0] == "inline", f
    assert sv.check_inline_calls({k: sv.lines_of(v.encode()) for k, v in files.items()}, "x_internal.h", {"inline_calls": ["helper"]}) == []


def test_registrar_order_must_survive_in_one_piece():
    old = sv.lines_of(b"static int a = REGISTER_ONE();\nstatic int b = REGISTER_TWO();\n")
    same = {"x.cpp": sv.lines_of(b"static int a = REGISTER_ONE();\nstatic int b = REGISTER_TWO();\n")}
    split = {"x.cpp": sv.lines_of(b"static int a = REGISTER_ONE();\n"), "y.cpp": sv.lines_of(b"static int b = REGISTER_TWO();\n")}
    swapped = {"x.cpp": sv.lines_of(b"static int b = REGISTER_TWO();\nstatic int a = REGISTER_ONE();\n")}
    assert sv.check_registrars(old, same) == []
    assert sv.check_registrars(old, split) and sv.check_registrars(old, swapped)


def test_hoisted_include_after_a_define_is_checked_against_that_define(tmp_path, monkeypatch):
    hdr = tmp_path / "dep.h"
    hdr.write_bytes(b"#if FEAT\nint x;\n#endif\n")
    monkeypatch.setattr(sv, "_find_header_text", lambda inc, d: hdr.read_bytes())
    old = sv.lines_of(b"#define FEAT 1\n#include \"dep.h\"\nint y;\n")
    header = sv.lines_of(b"#include \"dep.h\"\n")
    assert sv.check_includes(old, header, {}, "code")
    assert sv.check_includes(old, header, {"includes": ["dep.h"]}, "code") == []


def _pieces(**files):
    return {k: sv.lines_of(v.encode()) for k, v in files.items()}


def test_file_static_function_called_from_another_piece_is_flagged():
    p = _pieces(**{"x.cpp": "static long bump(long s)\n{\n\treturn s + 1;\n}\nint a() { return 1; }\n",
                   "x_stats.cpp": "int b(long s) { return bump(s); }\n"})
    f = sv.check_static_calls(p, "x_internal.h", {})
    assert f and f[0][0] == "static" and "bump" in f[0][1], f
    assert sv.check_static_calls(p, "x_internal.h", {"static_calls": ["bump"]}) == []


def test_anonymous_namespace_function_called_from_another_piece_is_flagged():
    p = _pieces(**{"x.cpp": "namespace {\nvoid helper(int v)\n{\n\tv++;\n}\n} // namespace\nint a() { return 1; }\n",
                   "x_b.cpp": "void b() { helper(1); }\n"})
    f = sv.check_static_calls(p, "x_internal.h", {})
    assert f and "helper" in f[0][1], f


def test_static_function_used_only_in_its_own_piece_or_redefined_elsewhere_is_clean():
    own = _pieces(**{"x.cpp": "static int h(int v)\n{\n\treturn v;\n}\nint a() { return h(1); }\n", "x_b.cpp": "int b() { return 2; }\n"})
    assert sv.check_static_calls(own, "x_internal.h", {}) == []
    twin = _pieces(**{"x.cpp": "static int h(int v)\n{\n\treturn v;\n}\nint a() { return h(1); }\n",
                      "x_b.cpp": "static int h(int v)\n{\n\treturn v + 1;\n}\nint b() { return h(2); }\n"})
    assert sv.check_static_calls(twin, "x_internal.h", {}) == []
    member = _pieces(**{"x.cpp": "static int h(int v)\n{\n\treturn v;\n}\n", "x_b.cpp": "int b(Obj o) { return o.h(2) + Obj::h(3); }\n"})
    assert sv.check_static_calls(member, "x_internal.h", {}) == []


def test_static_function_check_runs_inside_verify():
    old = b"static int h(int v)\n{\n\treturn v;\n}\nint a() { return h(1); }\nint b() { return h(2); }\n"
    new = {"code/m.cpp": '#include "m_internal.h"\nstatic int h(int v)\n{\n\treturn v;\n}\nint a() { return h(1); }\n',
           "code/m_b.cpp": '#include "m_internal.h"\nint b() { return h(2); }\n',
           "code/m_internal.h": "#ifndef M_INTERNAL_H\n#define M_INTERNAL_H\n#endif // M_INTERNAL_H\n"}
    assert "static" in kinds(run(old, new))


def test_static_function_inside_a_named_namespace_called_from_another_piece_is_flagged():
    p = _pieces(**{"x.cpp": "namespace ns {\nstatic bool gate()\n{\n\treturn true;\n}\nbool open()\n{\n\treturn true;\n}\n}\n",
                   "x_b.cpp": "namespace ns {\nbool use() { return gate() && open(); }\n}\n"})
    f = sv.check_static_calls(p, "x_internal.h", {})
    assert [m.split()[1] for _, m in f] == ["gate"], f


def test_global_defined_in_the_header_even_inside_an_else_branch_is_flagged():
    h = ("#ifndef M_INTERNAL_H\n#define M_INTERNAL_H\n#ifdef FEAT\nextern bool seen;\n#else\nbool seen = false;\n#endif\n"
         "extern int e;\nconst int k = 3;\nstruct Foo;\nvoid f(int x);\ntypedef int T;\nusing U = int;\nint counter;\n#endif // M_INTERNAL_H\n")
    f = sv.check_header(sv.lines_of(h.encode()), {})
    flagged = sorted(m.split(": ", 1)[-1] for _, m in f)
    assert flagged == ["bool seen = false;", "int counter;"], flagged
    assert sv.check_header(sv.lines_of(h.encode()), {"header_vars": ["seen", "counter"]}) == []


def test_commented_out_definitions_in_the_header_are_not_flagged():
    h = "#ifndef M_INTERNAL_H\n#define M_INTERNAL_H\n/*\nint hidden = 0;\nint hidden2;\n*/\nint real; /* tail */\n#endif // M_INTERNAL_H\n"
    f = sv.check_header(sv.lines_of(h.encode()), {})
    assert [m.split(": ", 1)[-1] for _, m in f] == ["int real;"], f
