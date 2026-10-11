"""printf_log: untagged printf/fprintf logging -> mc2::fmt::print_* (text only)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import printf_log as pl  # noqa: E402

NL = "\\n"          # the two characters backslash and n, as they appear inside a C string literal


def conv(src, path=""):
    return pl.convert(src, path)


def test_stderr_stdout_and_a_file_stream():
    src = ('#include <cstdio>\nvoid f(){\n fprintf(stderr, "a %d' + NL + '", x);\n printf("b %s' + NL + '", s);\n'
           ' fprintf(fp, "c' + NL + '");\n fprintf(stdout, "d' + NL + '");\n}\n')
    out, recs, left = conv(src)
    assert 'mc2::fmt::print_err("a {:d}' + NL + '", mc2::fmt::as_i32(x));' in out
    assert 'mc2::fmt::print_out("b {}' + NL + '", mc2::fmt::safe(s));' in out
    assert 'mc2::fmt::print(fp, "c' + NL + '");' in out and 'mc2::fmt::print_out("d' + NL + '");' in out
    assert len(recs) == 4 and not left and '#include "mc2fmt.h"' in out


def test_tagged_lines_are_left_for_b5_and_b6():
    for lit in ('[B2] right=%d' + NL, '[GL] id=%d' + NL, '[PERF v1] avg_fps=%f' + NL, '[HEARTBEAT] frames=%d' + NL, 'WARN [GPUPROPS] failed to compile' + NL):
        src = '#include <a.h>\nvoid f(){ fprintf(stderr, "' + lit + '", x); }\n'
        out, recs, left = conv(src)
        assert out == src and not recs and "tagged" in left[0][1], lit


def test_value_is_kept_and_non_literal_and_other_functions_are_left_alone():
    src = ('#include <a.h>\nvoid f(){ int n = printf("x' + NL + '"); fprintf(stderr, fmt, a); vfprintf(stderr, "x", ap); puts("x");'
           ' fprintf(*p, "x"); }\n')
    out, recs, left = conv(src)
    assert "int n = mc2::fmt::print_out(" in out and len(recs) == 1 and len(left) == 2


def test_crash_and_allocator_files_are_excluded_by_path():
    src = '#include <a.h>\nvoid f(){ fprintf(stderr, "x' + NL + '"); }\n'
    for path in ("GameOS/gameos/gos_crashbundle.cpp", "mclib/heap.cpp", "mclib/err.cpp", "GameOS/gameos/gameos_debugging.cpp"):
        assert conv(src, path)[0] == src, path
    assert conv(src, "mclib/other.cpp")[0] != src


def test_idempotent():
    once = conv('#include <a.h>\nvoid f(){ printf("x %d' + NL + '", v); }\n')[0]
    assert conv(once) == (once, [], [])


def test_tag_classes_follow_the_consumed_list_and_one_tag_can_be_selected(monkeypatch):
    src = ('#include <a.h>\nvoid f(){ fprintf(stderr, "[PERF v1] a=%d' + NL + '", x); fprintf(stderr, "[ZZZ_NOBODY v1] b=%d' + NL
           + '", y); fprintf(stderr, "plain' + NL + '"); }\n')
    assert pl.tag_class("[PERF v1] a=%d") == "consumed" and pl.tag_class("[ZZZ_NOBODY v1]") == "unconsumed" and pl.tag_class("plain") == "untagged"
    _, recs, _ = pl.convert(src, "", classes=("unconsumed",))
    assert [r["old"] for r in recs] == ["[ZZZ_NOBODY v1] b=%d" + NL]
    _, recs, _ = pl.convert(src, "", classes=("consumed",))
    assert [r["old"] for r in recs] == ["[PERF v1] a=%d" + NL]
    monkeypatch.setenv("MC2_PRINTF_LOG_TAG", "OTHER")
    assert pl.convert(src, "", classes=("consumed",))[1] == []


def test_a_line_read_out_of_the_exe_image_is_never_converted():
    src = '#include <a.h>\nvoid f(){ printf("[BUILD_FINGERPRINT v1] sha=%s' + NL + '", s); }\n'
    out, recs, left = conv(src)
    assert out == src and not recs and "exe image" in left[0][1]
