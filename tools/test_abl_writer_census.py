"""abl_writer_census: finding script-buffer writers by function, and the checks that keep the frozen census honest."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import abl_writer_census as awc  # noqa: E402


def calls(src):
    return [(func, callee) for _, func, callee in awc.scan(src)]


def test_calls_are_attributed_to_their_function():
    src = (
        "void execA (void) {\n"
        "\tfloat* p = ABLi_popRealPtr();\n"
        "\tmemcpy(a, b, 4);\n"
        "}\n"
        "int execB (void) {\n"
        "\tstrcpy(x, y);\n"
        "}\n"
    )
    assert calls(src) == [("execA", "ABLi_popRealPtr"), ("execA", "memcpy"), ("execB", "strcpy")]


def test_comments_and_string_literals_are_not_calls():
    src = (
        "void execA (void) {\n"
        "\t// strcpy(a, b);\n"
        "\t/* memcpy(a, b, 1); */\n"
        "\tputs(\"strcat(a, b)\");\n"
        "}\n"
    )
    assert calls(src) == []


def test_namespace_blocks_are_transparent_and_declarations_are_not_functions():
    src = (
        "namespace {\n"
        "void helper (char* d) {\n"
        "\tmemset(d, 0, 4);\n"
        "}\n"
        "}\n"
        "void declared (void);\n"
        "void execC (void) {\n"
        "\tif (x) {\n"
        "\t\tsprintf(buf, \"%d\", 1);\n"
        "\t}\n"
        "}\n"
    )
    assert calls(src) == [("helper", "memset"), ("execC", "sprintf")]


def test_an_ifdef_that_unbalances_braces_does_not_swallow_the_next_function():
    src = (
        "void execGate (void) {\n"
        "#ifdef USE_GATES\n"
        "\tif (obj) {\n"
        "\t\tABLi_pokeBoolean(1);\n"
        "#endif\n"
        "}\n"
        "void execNext (void) {\n"
        "\tchar* s = ABLi_popCharPtr();\n"
        "}\n"
    )
    assert calls(src) == [("execNext", "ABLi_popCharPtr")]


def test_a_longer_identifier_is_not_a_callee():
    assert calls("void f (void) {\n\tmy_memcpy(a, b, 1);\n\tx.strcpy_s(a);\n}\n") == []


def test_the_committed_baseline_has_a_verdict_and_a_reason_for_every_row():
    doc = awc.load_baseline()
    for key, row in doc["sites"].items():
        assert row["verdict"] in awc.VERDICTS, key
        assert row["why"], key
