"""cxx_calls: call finding, argument splitting and string-literal joining (text only)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cxx_calls as cc  # noqa: E402


def test_dead_ranges_cover_comments_strings_chars_and_raw_strings():
    t = 'a // x "y"\nb = "q\\"r"; /* c */ c = \'"\'; R"X(a "b" )X" end'
    dead = [t[a:b] for a, b in cc.dead_ranges(t)]
    assert dead == ['// x "y"', '"q\\"r"', '/* c */', "'\"'", 'R"X(a "b" )X"']


def test_calls_span_lines_and_split_arguments_at_top_level():
    t = 'void f(){ fprintf(stderr,\n  "a(%d,%d)"\n  "b", g(1, 2), h[3]);\n}'
    c = next(cc.find_calls(t, {"fprintf"}))
    assert [t[a:b] for a, b in c.args] == ["stderr", '"a(%d,%d)"\n  "b"', "g(1, 2)", "h[3]"]
    assert cc.string_literal_parts(c.arg_text(t, 1)) == "a(%d,%d)b"


def test_not_calls_inside_comments_strings_macros_or_members():
    t = '// printf("x");\nconst char *s = "printf(1)";\n#define P printf("x")\nvoid f(){ o.printf("x"); p->printf("y"); X::printf("z"); }'
    assert list(cc.find_calls(t, {"printf"})) == []


def test_std_and_extra_qualifiers():
    t = "void f(){ std::snprintf(b, 4, \"x\"); mc2::fmt::to_buf(b, \"y\"); }"
    assert [c.name for c in cc.find_calls(t, {"snprintf"})] == ["snprintf"]
    assert [c.name for c in cc.find_calls(t, {"to_buf"}, ("std::", "mc2::fmt::"))] == ["to_buf"]
    assert list(cc.find_calls(t, {"to_buf"})) == []


def test_string_literal_parts_refuses_anything_but_literals():
    assert cc.string_literal_parts('"a" "b"') == "ab"
    assert cc.string_literal_parts('"a" PRId64 "b"') is None
    assert cc.string_literal_parts("fmt") is None
    assert cc.string_literal_parts('L"wide"') is None
