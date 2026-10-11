"""log_tag_census: reading tagged log lines out of calls, including continuation lines, and the equality a conversion must keep."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "tidy"))
import log_tag_census as lc  # noqa: E402
import printf_buffers as pb  # noqa: E402


def census(src):
    return dict(lc.census_of_text(src))


def test_fprintf_with_the_format_on_a_continuation_line():
    src = 'void f(){ fprintf(stderr,\n    "[PERF v1] avg_fps=%.1f p50_ms=%.2f "\n    "samples=%zu\\n", a, b, c); }\n'
    assert census(src) == {("PERF", 1, "stderr", ("avg_fps", "p50_ms", "samples")): 1}


def test_unversioned_tag_and_stdout():
    assert census('void f(){ printf("[HEARTBEAT] frames=%d elapsed_ms=%llu\\n", a, b); }') == {("HEARTBEAT", -1, "stdout", ("frames", "elapsed_ms")): 1}


def test_event_macros_and_the_facade_template():
    src = ('void f(){ MC2_EVENT(Terrain, "WATER_PARITY", 1, "event=a frame={} n={}", x, y);\n'
           ' MC2_EVENT_OUT(Core, "SMOKE", 1, "event=summary result={}", r);\n'
           ' mc2::log::event<"PERF", 1>(mc2::log::Stream::Out, "avg_fps={:.1f}", v); }')
    assert census(src) == {("WATER_PARITY", 1, "stderr", ("event", "frame", "n")): 1, ("SMOKE", 1, "stdout", ("event", "result")): 1,
                           ("PERF", 1, "stdout", ("avg_fps",)): 1}


def test_a_converted_buffer_call_yields_the_same_record_as_the_snprintf_it_replaced():
    src = '#include <cstdio>\nvoid f(){ char b[64]; snprintf(b, sizeof(b), "[HEARTBEAT] frames=%d elapsed_ms=%d", a, c); }\n'
    new, recs, _ = pb.convert(src)
    assert recs and "mc2::fmt::to_buf" in new
    assert census(src) == census(new) == {("HEARTBEAT", -1, "buffer", ("frames", "elapsed_ms")): 1}


def test_untagged_lines_and_non_literal_formats_are_not_records():
    assert census('void f(){ printf("hello %d\\n", a); fprintf(stderr, fmt, a); }') == {}


def test_diff_reports_a_changed_key_order_and_a_missing_emitter():
    base = {"emitters": [{"tag": "A", "ver": 1, "stream": "stderr", "keys": ["x", "y"], "count": 1}]}
    now = lc.collections.Counter({("A", 1, "stderr", ("y", "x")): 1})
    d = lc.diff(now, base)
    assert len(d) == 2 and any("baseline 1, now 0" in x for x in d) and any("baseline 0, now 1" in x for x in d)


def test_the_frozen_baseline_matches_the_tree():
    assert lc.main(["--check"]) == 0
