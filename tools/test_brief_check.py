"""Tests for tools/brief_check.py (throwaway git repos and tmp files only).

Run:
  py -3 -B -m pytest tools/test_brief_check.py -q -p no:cacheprovider
"""
import importlib.util
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.append(os.path.join(HERE, "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

_spec = importlib.util.spec_from_file_location("brief_check", os.path.join(HERE, "brief_check.py"))
bc = importlib.util.module_from_spec(_spec)
sys.modules["brief_check"] = bc
_spec.loader.exec_module(bc)

TOOL = os.path.join(HERE, "brief_check.py")


def _run(*args):
    r = subprocess.run([sys.executable, "-B", TOOL] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       **console_child.child_kwargs(redirected=True))
    return r.returncode, r.stdout.decode("utf-8", "replace")


def _git(repo, *args):
    r = subprocess.run(["git", "-C", repo, *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                       **console_child.child_kwargs(redirected=True))
    assert r.returncode == 0, r.stderr
    return r.stdout.strip()


def _make_repo(root):
    repo = os.path.join(str(root), "repo")
    os.makedirs(repo)
    _git(repo, "init", "-q")
    for rel, content in (("tools/foo.py", "print('foo')\n"), ("docs/bar.md", "# bar\n"),
                         ("docs/render-backend-seams/gated-layout-macros.md", "# macros\n")):
        p = os.path.join(repo, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            f.write(content)
    _git(repo, "add", "-A")
    _git(repo, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "x")
    return repo


def _drive(path):
    return path.replace("\\", "/")


def test_extract_paths_shas_and_placeholders():
    text = ("tip c291d3d26 and abcdef12345 in A:/Games/mc2-qwen/tools/lane_merge.py, "
            "E:/mc2-installs/lane3, G:/mc2-tmp/qwen/pt, tools/foo.py and "
            "docs/render-backend-seams/gated-layout-macros.md; skip G:/mc2-tmp/<slot>/x.md, "
            "E:/mc2-installs/laneNN and tools/*.py; a full sha "
            "c291d3d26c291d3d26c291d3d26c291d3d26c291d3d is not one; "
            "https://github.com/foo/bar.py neither is a cite; see A:/Games/mc2-qwen/README.md.")
    got = bc.extract(text)
    assert ("path", "A:/Games/mc2-qwen/tools/lane_merge.py") in got
    assert ("path", "E:/mc2-installs/lane3") in got
    assert ("path", "G:/mc2-tmp/qwen/pt") in got
    assert ("path", "tools/foo.py") in got
    assert ("path", "docs/render-backend-seams/gated-layout-macros.md") in got
    assert ("path", "A:/Games/mc2-qwen/README.md") in got   # the sentence period is not part of the path
    assert ("sha", "c291d3d26") in got
    assert ("sha", "abcdef12345") in got
    assert ("skip", "G:/mc2-tmp/<slot>/x.md") in got
    assert ("skip", "E:/mc2-installs/laneNN") in got
    assert ("skip", "tools/*.py") in got
    assert not any(c.startswith("c291d3d26c") for _k, c in got)   # the 40-hex run is not extracted
    assert not any("github" in c for _k, c in got)


def test_extract_dedupes_first_seen_order():
    got = bc.extract("tools/foo.py then tools/foo.py again and tools/bar.py")
    assert [c for _k, c in got] == ["tools/foo.py", "tools/bar.py"]


def test_check_brief_statuses(tmp_path):
    repo = _make_repo(tmp_path)
    sha = _git(repo, "rev-parse", "HEAD")
    brief = os.path.join(str(tmp_path), "brief.md")
    with open(brief, "w", encoding="utf-8") as f:
        f.write(f"work in {_drive(repo)} and tools/foo.py; the tip is {sha[:9]}; "
                f"also {_drive(os.path.join(repo, 'nope', 'x.md'))}, tools/nope.py, "
                f"G:/mc2-tmp/<slot>/x.md and deadbeef.\n")
    findings = bc.check_brief(open(brief, encoding="utf-8").read(), repo)
    by_cite = {cite: status for status, cite in findings}
    assert by_cite[_drive(repo)] == "OK"
    assert by_cite["tools/foo.py"] == "OK"
    assert by_cite[sha[:9]] == "OK"
    assert by_cite[_drive(os.path.join(repo, "nope", "x.md"))] == "MISSING"
    assert by_cite["tools/nope.py"] == "MISSING"
    assert by_cite["G:/mc2-tmp/<slot>/x.md"] == "SKIP"
    assert by_cite["deadbeef"] == "UNKNOWN"


def test_cli_exit_codes(tmp_path, capsys):
    repo = _make_repo(tmp_path)
    sha = _git(repo, "rev-parse", "HEAD")
    good = os.path.join(str(tmp_path), "good.md")
    with open(good, "w", encoding="utf-8") as f:
        f.write(f"only {_drive(repo)} and {sha[:9]} here.\n")
    assert bc.main([good, "--repo", repo]) == 0
    out = capsys.readouterr().out
    assert f"OK {_drive(repo)}" in out and f"OK {sha[:9]}" in out

    bad = os.path.join(str(tmp_path), "bad.md")
    with open(bad, "w", encoding="utf-8") as f:
        f.write("tools/nope.py and deadbeef and G:/mc2-tmp/<slot>/x.md\n")
    assert bc.main([bad, "--repo", repo]) == 1
    out = capsys.readouterr().out
    for line in ("MISSING tools/nope.py", "UNKNOWN deadbeef", "SKIP G:/mc2-tmp/<slot>/x.md"):
        assert line in out.splitlines()
    assert bc.main([os.path.join(str(tmp_path), "absent.md"), "--repo", repo]) == 2


def test_cli_subprocess(tmp_path):
    repo = _make_repo(tmp_path)
    brief = os.path.join(str(tmp_path), "brief.md")
    with open(brief, "w", encoding="utf-8") as f:
        f.write("tools/foo.py and tools/nope.py\n")
    rc, out = _run(brief, "--repo", repo)
    assert rc == 1
    assert "OK tools/foo.py" in out and "MISSING tools/nope.py" in out
