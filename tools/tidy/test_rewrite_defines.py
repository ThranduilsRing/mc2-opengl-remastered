"""What: tests for rewrite_defines.py - literal typing (MSVC LLP64, suffixes), the refusal reasons, the rewrite text and its line endings.
Why: the tool rewrites thousands of defines unattended; the proof catches a wrong object, but a wrong type or a missed refusal wastes a batch."""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rewrite_defines as rd


def t(v):
    return rd.parse_literal(v)


def test_integer_types_follow_the_literal_rules():
    assert t("5") == ("int", "5") and t("(5)") == ("int", "5") and t("-1") == ("int", "-1")
    assert t("0x7FFFFFFF") == ("int", "0x7FFFFFFF") and t("0x80000000") == ("unsigned int", "0x80000000")
    assert t("0xFFFFFFFFu") == ("unsigned int", "0xFFFFFFFFu") and t("10L") == ("long", "10L") and t("7UL") == ("unsigned long", "7UL")
    assert t("0b101")[0] == "int" and t("017") == ("int", "017")


def test_ambiguous_or_out_of_range_literals_are_refused():
    assert t("2147483648") is None            # a decimal that does not fit int is long long: left alone
    assert t("-0x80000000") is None and t("-1u") is None and t("019") is None and t("1 + 2") is None and t("FOO") is None


def test_float_and_string_literals():
    assert t("1.5") == ("double", "1.5") and t("2.0f") == ("float", "2.0f") and t("1e-3") == ("double", "1e-3") and t(".5") == ("double", ".5")
    assert t('"abc"') == ("const char*", '"abc"') and t('L"w"') is None and t('"a" "b"') is None


def run(files, **kw):
    sites, uses = rd.analyse(files, **kw)
    return {s.name: s for s in sites}, uses


def test_value_only_use_is_eligible_and_rewrites_in_place():
    src = "#define A 5   // five\nint x[A];\nint f() { return A; }\n"
    s, _ = run({"a.cpp": src})
    assert s["A"].reasons == []
    out = rd.rewrite_text(src, [s["A"]], "constexpr")
    assert out.split("\n")[0] == "constexpr int A = 5;   // five" and out.endswith("int f() { return A; }\n")


def test_refusals():
    files = {"a.h": "#define IFD 1\n#define PASTE 2\n#define BODY 3\n#define STR \"x\"\n#define UNI 4\n#define TWICE 5\n#define SZ \"yy\"\n#define OKAY 9\n"
                    "#ifdef IFD\n#endif\n#define Y(a) a##PASTE\n#define Z BODY+1\n#undef UNI\nconst char* p = STR \"tail\";\nint n = sizeof(SZ);\n",
             "b.cpp": "#define TWICE 6\nint v = OKAY;\n"}
    s, _ = run(files)
    why = {k: v.reasons for k, v in s.items()}
    assert "directive-ifdef" in why["IFD"] and "macro-body" in why["PASTE"] and "macro-body" in why["BODY"]
    assert "string-used-as-literal" in why["STR"] and "undef" in why["UNI"] and "defined-more-than-once" in why["TWICE"] and "string-used-as-literal" in why["SZ"]
    assert why["OKAY"] == []


def test_scope_guards_c_side_and_outside_names():
    files = {"a.h": "#define IN_FN 1\nstruct S {\n#define INSIDE 2\n};\n#ifndef G_H\n#define G_H 1\n#endif\n#define CSIDE 3\n#define SCRIPTED 4\nnamespace n {\n#define NSOK 5\n}\n"}
    s, _ = run(files, c_side_headers=frozenset({"a.h"}), other_idents=frozenset({"SCRIPTED"}))
    assert "not-namespace-scope" in s["INSIDE"].reasons and "G_H" not in s
    assert "included-from-c-side" in s["CSIDE"].reasons and "named-outside-c++" in s["SCRIPTED"].reasons
    s2, _ = run(files)
    assert s2["NSOK"].reasons == []


def test_asm_files_and_crlf():
    s, _ = run({"a.cpp": "#define K 3\nvoid f() { __asm mov eax, K }\n"})
    assert "asm-file" in s["K"].reasons
    src = "#define K 3\r\nint a = K;\r\n"
    s, _ = run({"a.cpp": src})
    assert rd.rewrite_text(src, [s["K"]], "inline constexpr") == "inline constexpr int K = 3;\r\nint a = K;\r\n"


def test_taint_names_come_from_the_revert_reason():
    why = "6 of 144 includers differ or fail to compile (CMakeFiles/mc2.dir/code/infowindow.cpp.obj, out/editor/CMakeFiles/EditRel.dir/Editor.cpp.obj)"
    assert rd.taint_from_reasons([("code/mover.h", why)]) == {"CMakeFiles/mc2.dir/code/infowindow.cpp.obj", "out/editor/CMakeFiles/EditRel.dir/Editor.cpp.obj"}
    assert rd.taint_from_reasons([("x.h", "kept set failed its final build")]) == set()


def test_string_constants_are_refused_unless_asked_for():
    s, _ = run({"a.cpp": '#define NAME "x"\nconst char* p = NAME;\n'})
    assert "string-literal-codegen-differs" in s["NAME"].reasons
    sites, _ = rd.analyse({"a.cpp": '#define NAME "x"\nconst char* p = NAME;\n'}, allow_strings=True)
    assert sites[0].reasons == []


def test_never_touch_list_covers_the_step4_files_and_the_overload_sensitive_tus():
    tus = {"code/artlry.cpp"}
    for rel in ("mclib/estring.cpp", "mclib/stuff/vector3d.hpp", "mclib/mlr/mlr.hpp", "mclib/gosfx/fcurve.cpp", "mclib/file.h", "mclib/packet.cpp", "mclib/abldbug.h",
                "code/multplyr.cpp", "code/dmultplyr.h", "code/team_internal.h", "mclib/heap.h"):
        assert rd.never_touch(rel, tus), rel
    assert rd.never_touch("code/artlry.cpp", tus)
    assert not rd.never_touch("code/chatwindow.cpp", tus) and not rd.never_touch("mclib/timing.h", tus)


def test_the_audit_doc_yields_fifty_overload_sensitive_tus():
    assert len(rd.overload_sensitive_tus()) == 50


def test_a_constant_in_the_arguments_of_a_wrapper_of_a_stringizing_macro_is_refused():
    """MC2_VERIFY_BOUNDS-style: the inner `#cond` sees the EXPANDED text, so the string literal would change from the value to the name."""
    files = {"v.h": "#define VERIFY(cond, ...) ((!!(cond)) || Fail(#cond, __VA_ARGS__))\n"
                    "#define VERIFY_BOUNDS(idx, count) \\n    VERIFY((idx) < (count), \"x\")\n"
                    "#define DIRECT_ONLY(x) Log(#x)\n",
             "a.cpp": "#include \"v.h\"\n#define MAXC 4\n#define MAXD 5\n#define MAXE 6\n"
                      "void f(int i) {\n  VERIFY_BOUNDS(i,\n      MAXC);\n  DIRECT_ONLY(MAXD);\n  int z = MAXE;\n}\n"}
    s, _ = run(files)
    assert s["MAXC"].reasons == ["stringized-by-wrapper-macro"]      # through the wrapper, over two lines
    assert s["MAXD"].reasons == [] and s["MAXE"].reasons == []        # a direct stringizer keeps the raw name; a plain use is a value
    assert rd.stringizing_wrappers({"v.h": rd.cr.strip(files["v.h"]) + (None,)}) == {"VERIFY_BOUNDS"}


def test_a_constant_named_in_a_doctest_assertion_is_refused_but_other_uses_stay_eligible():
    hdr = "#define GADGET_OK 0\n#define GADGET_MISSING 7\n#define GADGET_PLAIN 3\n"
    tests = {"tests/unit/t.cpp": 'TEST_CASE("t") {\n  REQUIRE(f() == GADGET_OK);\n  CHECK_EQ(g(), GADGET_MISSING);\n  int n = GADGET_PLAIN;\n}\n'}
    sites, _ = rd.analyse({"a.h": hdr}, test_files=tests)
    by = {s.name: s.reasons for s in sites}
    assert by["GADGET_OK"] == ["odr-used-by-doctest"] and by["GADGET_MISSING"] == ["odr-used-by-doctest"] and by["GADGET_PLAIN"] == []
    sites, _ = rd.analyse({"a.h": hdr})
    assert all(not s.reasons for s in sites)                    # without test files nothing is known about assertions


def test_a_name_in_a_comment_or_string_of_a_test_does_not_count():
    sites, _ = rd.analyse({"a.h": "#define GADGET_OK 0\n"}, test_files={"t.cpp": 'CHECK(f() == 1); // GADGET_OK\nCHECK(s == "GADGET_OK");\n'})
    assert not sites[0].reasons
