"""What: tests for prep_namespace.py - anonymous namespaces are renamed, boundaries close and re-open the namespace, the using-directive lands after the first close,
line endings survive and a boundary outside a namespace is refused.
Why: the edit is repeated before every namespace-heavy split; a slip would change linkage silently."""
import os, sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prep_namespace as pn

SRC = """#include "a.h"

namespace {

int s_state = 0;

void helper()
{
    if (s_state) { s_state = 1; }
}

int other()
{
    return 2;
}

} // namespace

int api() { return other(); }
"""


def test_rename_and_using_directive():
    out, n = pn.prep(SRC, "detail", [])
    assert n == 1
    assert "namespace detail {" in out and "namespace {" not in out
    assert "} // namespace detail\nusing namespace detail;\n" in out


def test_boundary_closes_and_reopens_the_namespace():
    line = SRC.split("\n").index("int other()") + 1
    out, _ = pn.prep(SRC, "detail", [line])
    assert "}  // namespace detail\n\nnamespace detail {\n\nint other()" in out
    assert out.count("namespace detail {") == 2


def test_no_using_and_untouched_text_without_an_anonymous_namespace():
    out, n = pn.prep(SRC, "detail", [], using=False)
    assert "using namespace" not in out
    plain = "int f() { return 1; }\n"
    assert pn.prep(plain, "detail", []) == (plain, 0)


def test_crlf_is_preserved():
    out, _ = pn.prep(SRC.replace("\n", "\r\n"), "detail", [])
    assert "\r\n" in out and "\n" not in out.replace("\r\n", "")


def test_a_boundary_outside_the_namespace_is_refused():
    with pytest.raises(ValueError):
        pn.prep(SRC, "detail", [1])
    with pytest.raises(ValueError):
        pn.prep(SRC, "detail", [len(SRC.split("\n")) - 1])


def test_nested_braces_do_not_end_the_namespace_early():
    out, n = pn.prep(SRC, "detail", [])
    assert out.index("if (s_state)") < out.index("} // namespace detail")
