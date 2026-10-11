"""What: tests for batch_run.py - the batch-of-one fallback that re-proves every header a multi-header batch rejected.
Why: three headers failed a 5-header batch and passed alone; the rewriters must find that out themselves and report it, without relaxing the proof of the alone run."""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import batch_run as br


def picked(*names):
    return [(n, ["item-" + n], {"obj-" + n}) for n in names]


def test_a_batch_of_one_has_nothing_to_retry():
    assert br.alone_candidates(picked("a.h"), [("a.h", "differs")]) == []


def test_only_the_rejected_headers_are_retried_in_batch_order():
    p = picked("a.h", "b.h", "c.h", "d.h")
    assert [x[0] for x in br.alone_candidates(p, [("d.h", "x"), ("b.h", "y")])] == ["b.h", "d.h"]
    assert br.alone_candidates(p, []) == []


def fake_attempt(passes, calls):
    """An attempt() that keeps a header when its name is in `passes`, and records every call (it must always be a batch of one)."""
    def attempt(items):
        calls.append([i[0] for i in items])
        (rel, _, _), = items
        if rel in passes:
            return br.Outcome(items, kept=[rel], inc={rel: {"obj-" + rel}})
        return br.Outcome(items, rejected=[(rel, "obj-%s differs" % rel)], inc={rel: {"obj-" + rel}})
    return attempt


def test_fallback_keeps_the_headers_that_pass_alone_and_reports_the_rest():
    p = picked("a.h", "b.h", "c.h")
    out = br.Outcome(p, kept=["a.h"], rejected=[("b.h", "interaction"), ("c.h", "real")], inc={"a.h": {"obj-a.h"}})
    calls, lines = [], []
    alone, still = br.run_alone_fallback(out, fake_attempt({"b.h"}, calls), report=lines.append)
    assert calls == [["b.h"], ["c.h"]]
    assert [o.kept for o in alone] == [["b.h"]] and alone[0].kept_items() == 1 and alone[0].proven_objects() == 1
    assert still == [("c.h", "obj-c.h differs")]
    text = "\n".join(lines)
    assert "1 header(s) passed alone" in text and "b.h" in text and "c.h still fails alone: obj-c.h differs" in text


def test_fallback_stops_when_an_alone_baseline_cannot_build():
    p = picked("a.h", "b.h", "c.h")
    out = br.Outcome(p, rejected=[("a.h", "x"), ("b.h", "y"), ("c.h", "z")])
    calls = []

    def attempt(items):
        calls.append(items[0][0])
        return br.Outcome(items, rc=2)
    alone, still = br.run_alone_fallback(out, attempt, report=lambda s: None)
    assert calls == ["a.h"] and alone == [] and still == [("a.h", "the base build of the proof set failed")]


def test_fallback_on_a_clean_batch_does_nothing():
    out = br.Outcome(picked("a.h", "b.h"), kept=["a.h", "b.h"])
    calls = []
    alone, still = br.run_alone_fallback(out, fake_attempt(set(), calls), report=lambda s: None)
    assert calls == [] and alone == [] and still == []


def test_outcome_counts_items_and_objects_of_the_kept_files_only():
    p = [("a.h", [1, 2, 3], {"o1", "o2"}), ("b.h", [4], {"o2", "o3"})]
    o = br.Outcome(p, kept=["a.h"], rejected=[("b.h", "w")], inc={"a.h": {"o1", "o2"}, "b.h": {"o2", "o3"}})
    assert o.kept_items() == 3 and o.proven_objects() == 2
