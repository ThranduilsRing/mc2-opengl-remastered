"""container_callsites: --check follows the content seal and the kind on each row's line; --remap moves rows across an edit and drops the rows on edited lines;
--add restores a row for a call that moved into a helper; the per-batch header honours a file's per-class labels."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tests", "analyze"))
sys.path.insert(0, os.path.join(HERE, "..", "scripts"))
import console_child  # noqa: E402
import container_callsites as mod  # noqa: E402
import estring_callsites as ec  # noqa: E402

SRC = b"void f() {\n  open.find(1);\n  open.change(2, 3);\n}\n"


def sandbox(tmp_path, monkeypatch, src=SRC):
    monkeypatch.setattr(mod, "ROOT", str(tmp_path))
    monkeypatch.setattr(ec, "ROOT", str(tmp_path))
    monkeypatch.setattr(mod, "FILES_JSON", str(tmp_path / "files.json"))
    (tmp_path / "a.cpp").write_bytes(src)
    listing = tmp_path / "callsites.txt"
    listing.write_text("# total 2 sites in 1 files\na.cpp:2:PriorityQueue::find\na.cpp:3:PriorityQueue::change\n", encoding="utf-8")
    assert mod.seal(str(listing)) == 0
    return listing


def test_check_passes_on_a_sealed_list_and_fails_when_the_file_is_edited(tmp_path, monkeypatch):
    listing = sandbox(tmp_path, monkeypatch)
    assert mod.check(str(listing)) == 0
    (tmp_path / "a.cpp").write_bytes(b"// new first line\n" + SRC)           # rows now sit one line off, and the seal no longer matches
    assert mod.check(str(listing)) == 1


def test_check_fails_when_a_row_names_a_member_its_line_no_longer_has(tmp_path, monkeypatch):
    listing = sandbox(tmp_path, monkeypatch)
    listing.write_text(listing.read_text(encoding="utf-8").replace("a.cpp:3:PriorityQueue::change", "a.cpp:3:PriorityQueue::remove"), encoding="utf-8")
    assert mod.check(str(listing)) == 1


def test_check_fails_when_the_header_count_is_stale(tmp_path, monkeypatch):
    listing = sandbox(tmp_path, monkeypatch)
    listing.write_text(listing.read_text(encoding="utf-8").replace("# total 2 sites", "# total 3 sites"), encoding="utf-8")
    assert mod.check(str(listing)) == 1


def git(tmp_path, *args):
    return subprocess.run(["git", *args], cwd=tmp_path, capture_output=True, text=True, check=True, **console_child.child_kwargs(redirected=True)).stdout


def test_remap_moves_rows_below_an_edit_and_drops_the_rows_on_edited_lines(tmp_path, monkeypatch):
    listing = sandbox(tmp_path, monkeypatch)
    git(tmp_path, "init", "-q")
    git(tmp_path, "-c", "user.name=t", "-c", "user.email=t@t", "add", "a.cpp")
    git(tmp_path, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "base")
    # the find() call is rewritten (its row goes), two lines are added above the change() call (its row moves down by two)
    (tmp_path / "a.cpp").write_bytes(b"void f() {\n  reKey(1);\n  // why\n  // why\n  open.change(2, 3);\n}\n")
    assert mod.remap(str(listing), "HEAD", ["a.cpp"]) == 0
    rows = mod.read_rows(str(listing))
    assert [(r[1], r[3]) for r in rows] == [(5, "PriorityQueue::change")]
    assert mod.check(str(listing)) == 0


def test_add_restores_a_row_for_a_call_that_moved_into_a_helper(tmp_path, monkeypatch):
    listing = sandbox(tmp_path, monkeypatch)
    (tmp_path / "a.cpp").write_bytes(b"void reKey() {\n  open.find(1);\n  open.change(2, 3);\n}\n")
    assert mod.drop(str(listing), ["a.cpp:2", "a.cpp:3"]) == 0
    assert mod.add(str(listing), ["a.cpp:2:PriorityQueue::find", "a.cpp:3:PriorityQueue::change"]) == 0
    assert [(r[1], r[3]) for r in mod.read_rows(str(listing))] == [(2, "PriorityQueue::find"), (3, "PriorityQueue::change")]
    assert mod.check(str(listing)) == 0
    assert mod.add(str(listing), ["a.cpp:2:PriorityQueue::find"]) == 0        # idempotent: no duplicate row
    assert len(mod.read_rows(str(listing))) == 2


def test_header_counts_a_file_by_the_class_of_each_row_when_the_table_says_so(tmp_path, monkeypatch):
    sandbox(tmp_path, monkeypatch)
    (tmp_path / "files.json").write_text(
        '{"files": {"a.cpp": "B2.1"}, "row_batch": {"a.cpp": {"PriorityQueue": "B2.5"}}}', encoding="utf-8")
    text = mod.render([("a.cpp", 2, "0", "PriorityQueue::find"), ("a.cpp", 3, "0", "SortList::sort")])
    assert "#       1  B2.5" in text and "#       1  B2.1" in text
