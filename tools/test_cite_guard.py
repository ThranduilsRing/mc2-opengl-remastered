"""Tests for tools/cite_guard.py (throwaway git repos built per module; stdlib + git only).

Run:
  py -3 -B -m pytest tools/test_cite_guard.py -q -p no:cacheprovider
"""
import importlib.util
import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.append(os.path.join(HERE, "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

_spec = importlib.util.spec_from_file_location("cite_guard", os.path.join(HERE, "cite_guard.py"))
cg = importlib.util.module_from_spec(_spec)
sys.modules["cite_guard"] = cg
_spec.loader.exec_module(cg)

TOOL = os.path.join(HERE, "cite_guard.py")


def _run(*args):
    r = subprocess.run([sys.executable, "-B", TOOL] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       **console_child.child_kwargs(redirected=True))
    return r.returncode, r.stdout.decode("utf-8", "replace")


@pytest.fixture(scope="module")
def world(tmp_path_factory):
    root = str(tmp_path_factory.mktemp("cite_world"))
    cg.make_world(root)
    repo = cg.Repo(root)
    yield repo
    repo.git.close()


def ev(world, text, rel="docs/d.md"):
    return cg.evaluate(world, cg.parse_doc(rel, text))


def statuses(world, text, rel="docs/d.md"):
    return [r.status for _c, r in ev(world, text, rel)]


def guard(world, base, new, rel="docs/d.md"):
    return cg.guard_texts(world, rel, base.encode("latin-1"), new.encode("latin-1"))


def test_selftest_green():
    rc, out = _run("--selftest")
    assert rc == 0, out
    assert "ok" in out.splitlines()[-1]


@pytest.mark.parametrize("form", ["src/a.cpp:5", "src/a.cpp:5-9", "src/a.cpp:5,9", "src/a.cpp#L5", "src/a.cpp#L5-L9",
                                  "src/a.cpp (line 5)", "src/a.cpp:5 (alpha)", "src/a.cpp:5:alpha", "src/a.cpp::alpha",
                                  "src/a.cpp"])
def test_grammar_forms(form):
    cs = cg.parse_line("see `%s` here" % form, 1)
    assert len(cs) == 1 and cs[0].text == form


def test_grammar_paths_and_non_cites():
    cs = cg.parse_line(r"at src\a.cpp:5, A:/Games/mc2-main/src/a.cpp:12 and [x](src/a.cpp#L3)", 1)
    assert [c.kind for c in cs] == ["line"] * 3
    assert cg.norm_path(r"A:\Games\mc2-main\src\a.cpp") == "src/a.cpp"
    assert cg.norm_path("E:/elsewhere/a.cpp") is None and cg.norm_path("build64-ninja/a.cpp") is None
    assert not cg.parse_line("localhost:8080 12:30 e.g.:3 v1.2:4 foo_*.cpp $ROOT/x.cpp", 1)


def test_valid_cites_are_ok(world):
    assert statuses(world, "`src/a.cpp:5` (alpha) and src/a.cpp:12 (beta), `Gamma` at src/a.cpp:21.\n") == ["OK"] * 3


def test_stale_line_reports_current_location(world):
    r = ev(world, "The beta body, src/a.cpp:25 (beta).\n")[0][1]
    assert r.status == "STALE-LINE" and (r.sugg["path"], r.sugg["line"]) == ("src/a.cpp", 12)
    r = ev(world, "src/a.cpp:99 (alpha)\n")[0][1]
    assert r.status == "STALE-LINE" and "beyond" in r.detail


def test_split_classification_names_pieces(world):
    r = ev(world, "Called from src/gone.cpp:10 (moved_fn).\n")[0][1]
    assert r.status == "SPLIT" and "src/gone_a.cpp" in r.detail and "src/gone_b.cpp" in r.detail
    assert (r.sugg["path"], r.sugg["line"]) == ("src/gone_a.cpp", 3)


def test_split_source_line_is_compared_by_content(world):
    assert statuses(world, "src/big.cpp:3\n") == ["OK"]
    r = ev(world, "src/big.cpp:25\n")[0][1]
    assert r.status == "STALE-LINE" and r.sugg["path"] == "src/big_part.cpp" and r.sugg["how"] == "content at split base"


def test_deleted_and_renamed_report_history(world):
    d = ev(world, "src/old_name.cpp:3\n")[0][1]
    assert d.status == "DELETED" and cg.re.search(r"deleted in [0-9a-f]{7,} \(\d{4}-\d{2}-\d{2}\)", d.detail)
    r = ev(world, "src/ren_old.cpp:4 (ren_fn)\n")[0][1]
    assert r.status == "RENAMED" and "src/ren_new.cpp" in r.detail
    assert statuses(world, "src/never/there.cpp:3\n") == ["UNKNOWN"]


def test_quoted_history_is_info_only(world):
    txt = ("```text\nsrc/old_name.cpp:3\n```\n```cpp\nsrc/old_name.cpp:4 FAIL\n```\n"
           "src/old_name.cpp:5 (quoted history)\n<!-- history -->\n```\nsrc/old_name.cpp:6\n```\n"
           "src/old_name.cpp:7\n```sh\nsrc/old_name.cpp:8\n```\n")
    assert statuses(world, txt) == ["INFO"] * 4 + ["DELETED"] * 2


def test_diff_guard_accepts_cite_replacement(world):
    base = "# T\n\nSee `src/a.cpp:25` (beta) here.\nAlso src/gone.cpp:10 (moved_fn).\n"
    new = "# T\n\nSee `src/a.cpp:12` (beta) here.\nAlso src/gone_a.cpp:3 (moved_fn).\n"
    assert guard(world, base, new).ok


def test_diff_guard_rejects_prose_edit(world):
    base = "# T\n\nSee `src/a.cpp:25` (beta) here.\n"
    g = guard(world, base, "# T\n\nSee `src/a.cpp:12` (beta) there.\n")
    assert not g.ok and g.findings[0][0] == "prose" and "base L3" in g.findings[0][1]
    assert not guard(world, base, base + "more\n").ok
    assert not guard(world, base, base.rstrip("\n")).ok
    assert not guard(world, base, "# T\n\nSee `src/a.cpp:12` (beta) here. src/a.cpp:5\n").ok


def test_diff_guard_rejects_other_symbol_and_bad_target(world):
    base = "See src/a.cpp:25 (beta) now.\n"
    assert "symbol changed" in guard(world, base, "See src/a.cpp:5 (alpha) now.\n").findings[0][1]
    assert not guard(world, base, "See src/a.cpp:5 now.\n").ok
    assert not guard(world, base, "See src/a.cpp:99 (beta) now.\n").ok
    assert not guard(world, "See src/a.cpp:5 now.\n", "See src/big.cpp:5 now.\n").ok
    assert not guard(world, "`src/a.cpp:25` (beta)\n", "`src/a.cpp:12`(beta)\n").ok


def test_diff_guard_symbol_less_cite_must_follow_the_relocation(world):
    r = ev(world, "src/big.cpp:25\n")[0][1]
    assert guard(world, "src/big.cpp:25\n", "src/big_part.cpp:%d\n" % r.sugg["line"]).ok
    assert not guard(world, "src/big.cpp:25\n", "src/big_part.cpp:1\n").ok
    assert not guard(world, "src/big.cpp:25\n", "src/a.cpp:25\n").ok


def test_diff_guard_history_is_immutable(world):
    h = "```text\nsrc/a.cpp:25 (beta)\n```\n"
    assert not guard(world, h, h.replace("25", "12")).ok


def test_status_header(world):
    body = "# T\n\ntext src/a.cpp:5 (alpha)\n"
    hdr = "<!-- doc-status: stale | reviewed:2026-10-10 -->\n"
    assert guard(world, body, hdr + body).ok
    assert guard(world, hdr + body, hdr.replace("stale", "current") + body).ok
    assert guard(world, body, "<!-- doc-status: superseded-by:docs/target.md | reviewed:2026-10-10 -->\n" + body).ok
    for bad in ("<!-- doc-status: superseded-by:docs/missing.md | reviewed:2026-10-10 -->\n",
                "<!-- doc-status: fresh | reviewed:2026-10-10 -->\n",
                "<!-- doc-status: stale | reviewed:2026-13-40 -->\n"):
        assert not guard(world, body, bad + body).ok
    assert not guard(world, hdr + body, body).ok
    assert not guard(world, body, hdr + "\n" + body).ok


def test_crlf_and_bom_tolerance(world):
    base = "# T\r\n\r\nSee src/a.cpp:25 (beta).\r\n"
    assert guard(world, base, "# T\r\n\r\nSee src/a.cpp:12 (beta).\r\n").ok
    g = guard(world, base, "# T\n\nSee src/a.cpp:12 (beta).\n")
    assert g.ok and g.notes
    assert not cg.guard_texts(world, "docs/d.md", base.encode(), b"\xef\xbb\xbf" + base.encode()).ok


def test_fix_suggest_rows_apply_verbatim(world):
    text = "Body: src/a.cpp:25 (beta).\nGone: src/old_name.cpp:3\nIn `moved_fn` src/gone.cpp:10.\nok src/a.cpp:5 (alpha)\n"
    rows = cg.suggest_for(world, cg.parse_doc("docs/d.md", text))
    assert [r.status for _c, r, _n in rows] == ["STALE-LINE", "DELETED", "SPLIT"]
    assert [n for _c, _r, n in rows] == ["src/a.cpp:12 (beta)", None, "src/gone_a.cpp:3"]
    for c, _r, new in rows:
        if new:
            assert guard(world, text, text.replace(c.text, new, 1)).ok


def test_ambiguous_symbol_is_not_suggested(world, tmp_path):
    root = str(tmp_path / "amb")
    os.makedirs(root)
    subprocess.run(["git", "init", "-q", root], check=True, **console_child.child_kwargs(redirected=True),
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    cg.commit_files(root, {"a.cpp": "void twin() {\n}\nvoid twin(int) {\n}\n", "d.md": "x\n"}, "c1")
    repo = cg.Repo(root)
    try:
        doc = cg.parse_doc("docs/d.md", "see a.cpp:9 (twin_)\n`twin` at a.cpp:9\n")
        rows = cg.suggest_for(repo, doc)
        assert all(new is None for _c, _r, new in rows)
    finally:
        repo.git.close()


def test_cli_check_and_fix_suggest_and_exit_codes(world):
    root = world.root
    cg.commit_files(root, {"docs/bad.md": "Body: src/a.cpp:25 (beta).\n", "docs/good.md": "ok src/a.cpp:5 (alpha)\n"}, "docs")
    rc, out = _run("--repo", root, "--check", "docs/bad.md")
    assert rc == 1 and "docs/bad.md:1:7: STALE-LINE src/a.cpp:25 (beta)" in out and "stale-line=1" in out
    rc, out = _run("--repo", root, "--check", "docs/good.md")
    assert rc == 0 and "ok=1" in out
    rc, out = _run("--repo", root, "--fix-suggest", "docs/bad.md")
    assert rc == 0 and "REPLACE `src/a.cpp:25 (beta)` WITH `src/a.cpp:12 (beta)`" in out
    assert _run("--repo", root, "--fix-suggest", "docs/bad.md", "docs/good.md")[0] == 2
    assert _run("--repo", root, "--diff-guard")[0] == 2
    assert _run("--repo", os.path.join(root, "nope"), "--check")[0] == 2


def test_cli_diff_guard_flow(world):
    root = world.root
    cg.commit_files(root, {"docs/flow.md": "# F\n\nBody: src/a.cpp:25 (beta).\n"}, "flow")
    path = os.path.join(root, "docs", "flow.md")
    with open(path, "w", newline="") as fh:
        fh.write("<!-- doc-status: current | reviewed:2026-10-10 -->\n# F\n\nBody: src/a.cpp:12 (beta).\n")
    rc, out = _run("--repo", root, "--diff-guard", "--base", "HEAD", "docs/flow.md")
    assert rc == 0 and out.startswith("OK"), out
    with open(path, "w", newline="") as fh:
        fh.write("# F\n\nBody: src/a.cpp:12 (beta) and more.\n")
    rc, out = _run("--repo", root, "--diff-guard", "--base", "HEAD", "docs/flow.md")
    assert rc == 1 and "[prose]" in out and "base L3" in out
    assert _run("--repo", root, "--diff-guard", "--base", "HEAD", "docs/never-committed.md")[0] == 1
    _run("--repo", root, "--check", "docs/flow.md")
    subprocess.run(["git", "-C", root, "checkout", "-q", "--", "docs/flow.md"], check=True,
                   **console_child.child_kwargs(redirected=True), stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def test_triage_report_columns_and_superseded_hint(world):
    root = world.root
    body = "a src/a.cpp:5 (alpha) b src/a.cpp:12 (beta) c src/big.cpp:3 d src/gone_a.cpp:3 (moved_fn)\n"
    cg.commit_files(root, {"docs/old-plan.md": body}, "older")
    cg.commit_files(root, {"docs/new-plan.md": "<!-- doc-status: current | reviewed:2026-10-10 -->\n" + body}, "newer twin")
    rc, out = _run("--repo", root, "--triage-report", "docs/old-plan.md", "docs/new-plan.md")
    rows = {ln.split(" | ")[0]: ln.split(" | ") for ln in out.splitlines()[1:]}
    assert rc == 0 and out.splitlines()[0].startswith("# path | cites | broken | stale-line")
    old, new = rows["docs/old-plan.md"], rows["docs/new-plan.md"]
    assert old[1] == "4" and old[2] == "0" and old[5] == "none" and old[6].startswith("docs/new-plan.md")
    assert new[5].startswith("current@reviewed:2026-10-10") and new[6] == "-"
    assert len(old[4]) == 10


def test_scope_rules():
    assert cg.in_scope("docs/a.md") and cg.in_scope("README.md") and cg.in_scope(".claude/cleanup/x.md")
    assert not cg.in_scope("docs/archive/a.md") and not cg.in_scope("code/a.md") and not cg.in_scope("tools/CMakeLists.txt")
    assert not cg.in_scope("scripts/x-allowlist.txt") and not cg.in_scope("a/build64-x/b.md")
