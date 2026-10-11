"""null_tokens: the NULL -> nullptr text transform (no build)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import null_tokens as nt  # noqa: E402


def test_code_nulls_become_nullptr():
    assert nt.rewrite("Foo *p = NULL;\nif (p == NULL) return NULL;\n") == "Foo *p = nullptr;\nif (p == nullptr) return nullptr;\n"


def test_comments_strings_and_chars_are_left_alone():
    src = '// NULL here\n/* NULL */ const char *s = "NULL"; char c = \'N\'; int x = 0;\n'
    assert nt.rewrite(src) == src


def test_preprocessor_lines_about_null_are_left_alone():
    for src in ("#define NULL 0\n", "#ifndef NULL\n#define NULL ((void*)0)\n#endif\n", "#if defined(NULL)\n#endif\n", "#undef NULL\n"):
        assert nt.rewrite(src) == src


def test_other_defines_that_use_null_are_rewritten():
    assert nt.rewrite("#define CHECK(p) ((p) != NULL)\n") == "#define CHECK(p) ((p) != nullptr)\n"


def test_identifiers_containing_null_are_not_touched():
    src = "int NULLABLE = 1; int IS_NULL = 2; int NULL_FLAG = 3;\n"
    assert nt.rewrite(src) == src


def test_idempotent():
    once = nt.rewrite("f(NULL, NULL);\n")
    assert once == "f(nullptr, nullptr);\n" and nt.rewrite(once) == once
