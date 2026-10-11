"""What: tests for check_batch_allowed.py - the pre-batch report of the headers the taint ledger (and the other ledgers) would skip.
Why: ten headers sat behind one flaky object unnoticed; the report must name the object, the headers it hides and the reason of every other skip."""
import os, sys, types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_batch_allowed as cba
import rewrite_defines as rd


def test_report_names_the_object_hiding_the_most_headers():
    skips = [("a.h", "tainted", "obj1,obj2", 3), ("b.h", "tainted", "obj1", 2), ("c.h", "dirty", "", 1), ("d.h", "too-many-includers", "900 objects (limit 200)", 4)]
    plan = [(5, "e.h", ["s1", "s2"], {"o"}), (6, "f.h", ["s3"], {"o"})]
    text = "\n".join(cba.report(plan, skips, done={"f.h"}, taint={"obj1", "obj2", "obj3"}))
    assert "would run: 1 file(s), 2 site(s)" in text and "run   e.h" in text
    assert "skip  tainted" in text and "a.h" in text and "<- obj1,obj2" in text
    assert "skip  done" in text and "f.h" in text and "skip  dirty" in text and "900 objects (limit 200)" in text
    assert "obj1 hides 2 header(s): a.h, b.h" in text and "obj2 hides 1 header(s): a.h" in text
    assert text.index("obj1 hides") < text.index("obj2 hides")


def test_a_clean_plan_reports_no_hidden_headers():
    text = "\n".join(cba.report([(1, "a.h", ["s"], {"o"})], [], taint=set()))
    assert "would skip: 0 file(s)" in text and "hides" not in text


def sites_for(path, name):
    s = rd.Site(path, 1, name, "int", "1", ("#define %s 1" % name, 0))
    return s


def test_the_real_planner_reports_a_tainted_header_and_a_header_without_objects():
    args = types.SimpleNamespace(scope="all", s2t={}, taint={"t/bad.obj"}, max_header_includers=10)
    sites = [sites_for("code/one.h", "WIDGET_ONE"), sites_for("code/two.h", "WIDGET_TWO"), sites_for("code/three.h", "WIDGET_THREE")]
    uses = {s.name: [] for s in sites}
    includers = {"code/one.h": {"t/bad.obj", "t/ok.obj"}, "code/two.h": {"t/ok.obj"}}
    skips = []
    plan = rd.plan_batches({}, sites, uses, includers, args, set(), skips)
    assert [p[1] for p in plan] == ["code/two.h"]
    kinds = {s[0]: s[1:3] for s in skips}
    assert kinds["code/one.h"] == ("tainted", "t/bad.obj") and kinds["code/three.h"][0] == "no-objects"
    assert cba.hidden_by_object(skips) == [("t/bad.obj", ["code/one.h"])]


def test_planning_without_a_skip_list_is_unchanged():
    args = types.SimpleNamespace(scope="header", s2t={}, taint=set(), max_header_includers=10)
    sites = [sites_for("code/one.h", "WIDGET_ONE")]
    plan = rd.plan_batches({}, sites, {"WIDGET_ONE": []}, {"code/one.h": {"t/ok.obj"}}, args, set())
    assert [(p[0], p[1]) for p in plan] == [(1, "code/one.h")]


def test_only_restricts_the_plan_to_the_named_files():
    plan = [(1, "a.h", [1], {"o"}), (2, "b.h", [2], {"o"}), (3, "c.h", [3], {"o"})]
    assert [p[1] for p in rd.restrict_plan(plan, set())] == ["a.h", "b.h", "c.h"]
    assert [p[1] for p in rd.restrict_plan(plan, {"b.h"}, ["b.h", "c.h"])] == ["c.h"]
    assert rd.restrict_plan(plan, set(), ["zzz.h"]) == []
