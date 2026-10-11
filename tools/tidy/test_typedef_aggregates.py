"""typedef_aggregates: the text transform only (no build)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import typedef_aggregates as ta  # noqa: E402


def test_anonymous_struct_takes_the_typedef_name():
    assert ta.rewrite("typedef struct {\n int a;\n} Foo;\n") == "struct Foo {\n int a;\n};\n"


def test_tag_equal_to_name_is_just_the_struct():
    assert ta.rewrite("typedef struct Foo {\n int a;\n} Foo;\n") == "struct Foo {\n int a;\n};\n"


def test_distinct_tag_keeps_the_alias():
    assert ta.rewrite("  typedef struct _Foo { int a; } Foo;\n") == "  struct _Foo { int a; };\n  using Foo = _Foo;\n"


def test_enum_and_union_forms():
    assert ta.rewrite("typedef enum { A, B } Kind;") == "enum Kind { A, B };"
    assert ta.rewrite("typedef union U { int i; float f; } U;") == "union U { int i; float f; };"


def test_nested_aggregate_typedefs_both_rewrite():
    src = "typedef struct Outer {\n  typedef enum { X } Inner;\n  Inner i;\n} Outer;\n"
    assert ta.rewrite(src) == "struct Outer {\n  enum Inner { X };\n  Inner i;\n};\n"


def test_unsupported_shapes_are_left_alone():
    for src in ("typedef struct { int a; } Foo, *PFoo;\n",          # several declarators
                "typedef struct Foo Foo;\n",                          # forward typedef, no body
                "typedef int (*Fn)(int);\n",
                "typedef enum E : int { A } E;\n",                    # underlying type
                "// typedef struct { int a; } Commented;\n",
                '/* typedef struct { int a; } Block; */\n',
                'const char *s = "typedef struct { int a; } InString;";\n'):
        assert ta.rewrite(src) == src, src


def test_braces_in_strings_and_comments_do_not_confuse_the_match():
    src = 'typedef struct {\n  const char *s = "}"; // }\n  int a; /* } */\n} Foo;\n'
    assert ta.rewrite(src).endswith("};\n") and ta.rewrite(src).startswith("struct Foo {")


def test_idempotent():
    once = ta.rewrite("typedef struct _Foo { int a; } Foo;\n")
    assert ta.rewrite(once) == once
