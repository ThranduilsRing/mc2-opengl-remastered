"""printf_corpus: the generated doctest text must stay valid C++ whatever the format contains (text only)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import printf_corpus as pc  # noqa: E402

BS = chr(92)


def record(old, new, specs=()):
    return {"old": old, "new": new, "specs": list(specs), "files": ["x.cpp"]}


def test_a_title_cut_inside_an_escape_does_not_leave_half_of_it():
    old = "x" * 68 + BS + '"' + "tail %d"          # the 70-character cut falls between the backslash and the quote
    text = pc.emit([record(old, old.replace("%d", "{:d}"), [{"conv": "d", "length": "", "wrap": "as_i32", "cls": ""}])])
    title_line = next(ln for ln in text.splitlines() if ln.startswith("TEST_CASE("))
    assert title_line.endswith('")') and title_line.count('"') == 2, title_line


def test_the_old_and_new_formats_are_emitted_verbatim_with_their_wrapped_arguments():
    spec = {"conv": "d", "length": "", "wrap": "as_i32", "cls": ""}
    text = pc.emit([record("a %d", "a {:d}", [spec])])
    assert 'std::snprintf(a, sizeof a, "a %d", v0)' in text and 'mc2::fmt::to_buf(b, "a {:d}", mc2::fmt::as_i32(v0))' in text


def test_string_classes_decide_the_table_and_the_safe_wrapper():
    maybe = {"conv": "s", "length": "", "wrap": "safe", "cls": "maybe-null"}
    nonnull = {"conv": "s", "length": "", "wrap": "", "cls": "nonnull-accessor"}
    text = pc.emit([record("%s|%s", "{}|{}", [maybe, nonnull])])
    assert "kStrNull[" in text and "kStrNonNull[" in text and "mc2::fmt::safe(v0)" in text and "mc2::fmt::safe(v1)" not in text
