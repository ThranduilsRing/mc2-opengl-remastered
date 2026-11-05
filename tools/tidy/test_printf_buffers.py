"""printf_buffers: the printf -> std::format mapping and the call-level decisions (text only, no build)."""
import os
import pytest
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import printf_buffers as pb  # noqa: E402


def conv(src):
    return pb.convert(src)


def test_snprintf_into_its_own_array_becomes_to_buf():
    out, recs, left = conv('#include <cstdio>\nvoid f(){ char b[16]; snprintf(b, sizeof(b), "%d:%02d", x, y); }\n')
    assert 'mc2::fmt::to_buf(b, "{:d}:{:02d}", mc2::fmt::as_i32(x), mc2::fmt::as_i32(y));' in out
    assert '#include "mc2fmt.h"' in out and recs[0]["old"] == "%d:%02d" and not left


def test_explicit_size_keeps_the_size_and_uses_to_buf_n():
    out, _, _ = conv('#include <a.h>\nvoid f(){ snprintf(buf, 256, "<%s>", p); }\n')
    assert 'mc2::fmt::to_buf_n(buf, 256, "<{}>", mc2::fmt::safe(p));' in out


def test_sprintf_gets_the_array_bound_only_when_the_array_is_visible():
    out, _, _ = conv('#include <a.h>\nvoid f(){ char date[16]; sprintf(date, "%02d/%02d/%04d", d, m, y); }\n')
    assert 'mc2::fmt::to_buf(date, "{:02d}/{:02d}/{:04d}"' in out
    src = '#include <a.h>\nvoid f(){ char* date = new char[16]; sprintf(date, "%d", d); }\n'
    assert conv(src)[0] == src and "visible char array" in conv(src)[2][0][1]
    src = '#include <a.h>\nvoid f(char date[16]){ sprintf(date, "%d", d); }\n'          # a parameter array is a pointer
    assert conv(src)[0] == src
    src = '#include <a.h>\nchar date[16];\nvoid f(){ char* date = p; sprintf(date, "%d", d); }\n'     # shadowed by a pointer
    assert conv(src)[0] == src


def test_sizeof_of_a_non_array_keeps_the_size_in_to_buf_n():
    out, _, _ = conv('#include <a.h>\nvoid f(char* b){ snprintf(b, sizeof(b), "%d", v); }\n')
    assert 'mc2::fmt::to_buf_n(b, sizeof(b), "{:d}", mc2::fmt::as_i32(v));' in out


def test_snprintf_s_truncate_statement_form_only():
    out, _, _ = conv('#include <a.h>\nvoid f(){ char n[8]; _snprintf_s(n, sizeof(n), _TRUNCATE, "%ld", v); int k = _snprintf_s(n, 8, _TRUNCATE, "%d", v); }\n')
    assert 'mc2::fmt::to_buf(n, "{:d}", mc2::fmt::as_long(v));' in out
    assert "int k = _snprintf_s" in out                      # value used: left alone


def test_string_argument_classes_decide_safe():
    out, recs, _ = conv('#include <a.h>\nvoid f(){ snprintf(b, 8, "%s|%s|%s", "lit", name.c_str(), maybe); }\n')
    assert '"{}|{}|{}", "lit", name.c_str(), mc2::fmt::safe(maybe))' in out
    assert [s["cls"] for s in recs[0]["specs"]] == ["literal", "nonnull-accessor", "maybe-null"]


def test_widths_flags_and_alignment():
    out, _, _ = conv('#include <a.h>\nvoid f(){ snprintf(b, 64, "%-8s|%8s|%+.2f|%08.3f|%-5d|%x|%X|%05u|%c|%p", s, t, f, g, i, a, bb, u, ch, ptr); }\n')
    assert '"{:<8}|{:>8}|{:+.2f}|{:08.3f}|{:<5d}|{:x}|{:X}|{:05d}|{:c}|{:016X}"' in out
    assert "mc2::fmt::as_ptr(ptr)" in out and "mc2::fmt::as_u32(u)" in out and "mc2::fmt::as_char(ch)" in out


def test_length_modifiers_follow_llp64():
    out, _, _ = conv('#include <a.h>\nvoid f(){ snprintf(b, 64, "%ld %lu %lld %llu %zu", a, b2, c, d, e); }\n')
    assert "as_long(a)" in out and "as_ulong(b2)" in out and "as_i64(c)" in out and "as_u64(d)" in out and "as_size(e)" in out


def test_braces_and_percent_literals():
    out, _, _ = conv('#include <a.h>\nvoid f(){ snprintf(b, 64, "{\\"k\\":%d}%%", v); }\n')
    assert '"{{\\"k\\":{:d}}}%"' in out


def test_unprovable_calls_are_left_alone_with_a_reason():
    cases = {'snprintf(b, 8, "%.3d", v);': "precision", 'snprintf(b, 8, "%#x", v);': "# flag", 'snprintf(b, 8, "%*d", w, v);': "*",
             'snprintf(b, 8, "%hd", v);': "length", 'snprintf(b, 8, "%.5s", s);': "modifiers on %s", 'snprintf(b, 8, fmt, v);': "string literal",
             'snprintf(b, 8, "%d %d", v);': "fewer", 'snprintf(b, 8, "%d", v, w);': "more arguments", 'snprintf(b, 8, "%n", &v);': "conversion"}
    for call, reason in cases.items():
        src = "#include <a.h>\nvoid f(){ " + call + " }\n"
        out, recs, left = conv(src)
        assert out == src and not recs and left and reason in left[0][1], (call, left)


def test_comments_strings_macros_and_members_are_not_calls():
    src = '#include <a.h>\n// snprintf(b, 8, "%d", v);\n#define M(b) snprintf(b, 8, "%d", 1)\nvoid f(){ o.snprintf(b, 8, "%d", v); const char *s = "snprintf(b, 8, \\"%d\\", v)"; }\n'
    assert conv(src) == (src, [], [])


def test_include_goes_after_the_last_top_level_include_not_inside_a_conditional():
    out, _, _ = conv('#include <a.h>\n#ifdef X\n#include <b.h>\n#endif\nvoid f(){ snprintf(b, 8, "%d", v); }\n')
    assert out.index('#include "mc2fmt.h"') > out.index("#include <a.h>") and out.index('#include "mc2fmt.h"') < out.index("#ifdef X")


def test_idempotent():
    once = conv('#include <a.h>\nvoid f(){ snprintf(b, sizeof(b), "%d", v); }\n')[0]
    assert conv(once) == (once, [], [])


def test_float_default_precision_and_wrong_type_args_are_compiler_findings_not_casts():
    out, _, _ = conv('#include <a.h>\nvoid f(){ snprintf(b, 64, "%f|%e|%g", x, y, z); }\n')
    assert '"{:f}|{:e}|{:g}", x, y, z)' in out         # no cast: an int passed to %f must fail to compile


def test_include_precedes_the_first_converted_call_even_when_more_includes_follow_the_code():
    src = '#include <a.h>\nvoid f(){ snprintf(b, 8, "%d", v); }\n#include "late.cpp"\n'
    out, _, _ = conv(src)
    assert out.index('#include "mc2fmt.h"') < out.index("void f()") and out.count("mc2fmt.h") == 1
    assert conv('void f(){ snprintf(b, 8, "%d", v); }\n#include "late.cpp"\n')[0] == 'void f(){ snprintf(b, 8, "%d", v); }\n#include "late.cpp"\n'


def test_assume_array_and_only_lines_for_the_per_site_trial():
    src = '#include <a.h>\nvoid f(){\n sprintf(a->name, "%d", x);\n sprintf(p, "%d", y);\n}\n'
    assert conv(src)[0] == src                                                    # no visible array: both refused by default
    out, recs, _ = pb.convert(src, assume_array=True, only_lines={3})
    assert 'mc2::fmt::to_buf(a->name, "{:d}", mc2::fmt::as_i32(x));' in out and "sprintf(p," in out and len(recs) == 1


def test_path_separator_macro_is_carried_through_and_put_back():
    out, recs, left = conv('#include <a.h>\nvoid f(){ char p[64]; sprintf(p, "data" PATH_SEPARATOR "%s" PATH_SEPARATOR "%d.var", d, n); }\n')
    assert 'mc2::fmt::to_buf(p, "data" PATH_SEPARATOR "{}" PATH_SEPARATOR "{:d}.var", mc2::fmt::safe(d), mc2::fmt::as_i32(n));' in out
    assert recs[0]["old"] == "data/%s/%d.var" and recs[0]["new"] == "data/{}/{:d}.var" and not left


def test_other_macros_in_a_format_still_bail():
    src = '#include <a.h>\nvoid f(){ sprintf(p, "a" OTHER "%d", n); }\n'
    assert conv(src)[0] == src


def test_a_multi_line_call_keeps_its_argument_lines_so_two_max_calls_do_not_land_on_one_line():
    nl = chr(10)
    src = ("#include <a.h>" + nl + "void f(){ char b[64]; snprintf(b, sizeof(b)," + nl + '    "%d/%d",' + nl + "    std::max(1, x)," + nl
           + "    std::max(1, y)); }" + nl)
    out = conv(src)[0]
    assert "std::max(1, x)" in out and out.count("std::max") == 2
    assert not any(ln.count("std::max") >= 2 for ln in out.splitlines()), out


def test_estring_data_is_maybe_null_and_gets_safe():
    """EString::Data() returns null for the shared empty buffer; printf printed (null), std::format would dereference it."""
    out, recs, _ = conv('#include <a.h>\nvoid f(){ snprintf(b, 8, "%s", s.Data()); snprintf(b, 8, "%s", p->Description().Data()); }\n')
    assert out.count("mc2::fmt::safe(s.Data())") == 1 and out.count("mc2::fmt::safe(p->Description().Data())") == 1
    assert [s["cls"] for r in recs for s in r["specs"]] == ["maybe-null", "maybe-null"]


def test_char_with_width_aligns_like_printf():
    """std::format left-aligns a char by default; printf right-aligns unless the minus flag is given."""
    new, _wrapped, _specs = pb.translate_format("%5c|%-5c|%c", ["a", "b", "c"])
    assert new == "{:>5c}|{:<5c}|{:c}"


def test_precision_on_unsigned_is_a_zero_padded_width_and_never_on_signed():
    new, _w, _s = pb.translate_format("0x%.8x|%8.8X|%.3u", ["a", "b", "c"])
    assert new == "0x{:08x}|{:08X}|{:03d}"
    for bad in ("%.4d", "%10.4x", "%.0x", "%-.4x", "%#.4x"):
        with pytest.raises(pb.Bail):
            pb.translate_format(bad, ["a"])
