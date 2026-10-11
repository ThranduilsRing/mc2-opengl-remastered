"""apply_edit: exact search/replace edits from a JSON file (no build)."""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import apply_edit as ae  # noqa: E402


def _write(root, name, data: bytes):
    p = os.path.join(root, name)
    with open(p, "wb") as fh:
        fh.write(data)
    return p


def _edits(root, edits):
    p = os.path.join(root, "edits.json")
    with open(p, "w", encoding="utf-8") as fh:
        json.dump(edits, fh)
    return p


def _read(path):
    with open(path, "rb") as fh:
        return fh.read()


def test_applies_single_edit_and_prints_diff(tmp_path, capsys):
    root = str(tmp_path)
    f = _write(root, "a.cpp", b"int a = 1;\nint b = 2;\n")
    rc = ae.main([_edits(root, [{"file": "a.cpp", "search": "int b = 2;", "replace": "int b = 3;"}]), "--root", root])
    assert rc == 0
    assert _read(f) == b"int a = 1;\nint b = 3;\n"
    out = capsys.readouterr().out
    assert "-int b = 2;" in out and "+int b = 3;" in out and "a/a.cpp" in out


def test_check_writes_nothing(tmp_path, capsys):
    root = str(tmp_path)
    f = _write(root, "a.txt", b"x\n")
    rc = ae.main([_edits(root, [{"file": "a.txt", "search": "x", "replace": "y"}]), "--check", "--root", root])
    assert rc == 0 and _read(f) == b"x\n"
    assert "would apply" in capsys.readouterr().out


def test_count_mismatch_refuses_and_leaves_every_file_untouched(tmp_path, capsys):
    root = str(tmp_path)
    a = _write(root, "a.txt", b"one\n")
    b = _write(root, "b.txt", b"two two\n")
    rc = ae.main([_edits(root, [{"file": "a.txt", "search": "one", "replace": "1"},
                                {"file": "b.txt", "search": "two", "replace": "2"}]), "--root", root])
    assert rc == 1
    assert _read(a) == b"one\n" and _read(b) == b"two two\n"
    assert "found 2 time(s), expected 1" in capsys.readouterr().err


def test_explicit_count_replaces_all_occurrences(tmp_path):
    root = str(tmp_path)
    f = _write(root, "b.txt", b"two two\n")
    assert ae.main([_edits(root, [{"file": "b.txt", "search": "two", "replace": "2", "count": 2}]), "--root", root]) == 0
    assert _read(f) == b"2 2\n"


def test_missing_search_refuses(tmp_path):
    root = str(tmp_path)
    f = _write(root, "a.txt", b"abc\n")
    assert ae.main([_edits(root, [{"file": "a.txt", "search": "zzz", "replace": "q"}]), "--root", root]) == 1
    assert _read(f) == b"abc\n"


def test_missing_file_and_empty_search_refuse(tmp_path):
    root = str(tmp_path)
    assert ae.main([_edits(root, [{"file": "nope.txt", "search": "a", "replace": "b"}]), "--root", root]) == 1
    _write(root, "a.txt", b"abc\n")
    assert ae.main([_edits(root, [{"file": "a.txt", "search": "", "replace": "b"}]), "--root", root]) == 1


def test_edits_to_one_file_apply_in_order(tmp_path):
    root = str(tmp_path)
    f = _write(root, "a.txt", b"alpha\n")
    edits = [{"file": "a.txt", "search": "alpha", "replace": "beta"}, {"file": "a.txt", "search": "beta", "replace": "gamma"}]
    assert ae.main([_edits(root, edits), "--root", root]) == 0
    assert _read(f) == b"gamma\n"


def test_backslashes_and_quotes_survive(tmp_path):
    root = str(tmp_path)
    f = _write(root, "a.cpp", b'printf("old");\n')
    new = 'printf("a\\\\b\\n");'     # the C++ text: printf("a\\b\n");
    assert ae.main([_edits(root, [{"file": "a.cpp", "search": 'printf("old");', "replace": new}]), "--root", root]) == 0
    assert _read(f) == b'printf("a\\\\b\\n");\n'


def test_crlf_file_is_matched_and_written_with_crlf(tmp_path):
    root = str(tmp_path)
    f = _write(root, "w.cpp", b"a\r\nb\r\nc\r\n")
    assert ae.main([_edits(root, [{"file": "w.cpp", "search": "a\nb", "replace": "a\nB\nB2"}]), "--root", root]) == 0
    assert _read(f) == b"a\r\nB\r\nB2\r\nc\r\n"


def test_non_utf8_bytes_round_trip(tmp_path):
    root = str(tmp_path)
    f = _write(root, "s.cpp", b'x = "caf\xe9";\ny = 1;\n')
    assert ae.main([_edits(root, [{"file": "s.cpp", "search": "y = 1;", "replace": "y = 2;"}]), "--root", root]) == 0
    assert _read(f) == b'x = "caf\xe9";\ny = 2;\n'


def test_bad_json_and_bad_fields_exit_2(tmp_path):
    root = str(tmp_path)
    bad = os.path.join(root, "bad.json")
    with open(bad, "w") as fh:
        fh.write("{not json")
    assert ae.main([bad, "--root", root]) == 2
    assert ae.main([_edits(root, [{"file": "a", "search": "b"}]), "--root", root]) == 2
    assert ae.main([_edits(root, [{"file": "a", "search": "b", "replace": "c", "count": 0}]), "--root", root]) == 2
    assert ae.main([_edits(root, [{"file": "a", "search": "b", "replace": "c", "cnt": 1}]), "--root", root]) == 2
    assert ae.main([_edits(root, []), "--root", root]) == 2


def test_wrapped_object_form_and_absolute_path(tmp_path):
    root = str(tmp_path)
    f = _write(root, "a.txt", b"q\n")
    assert ae.main([_edits(root, {"edits": [{"file": f, "search": "q", "replace": "r"}]}), "--root", root]) == 0
    assert _read(f) == b"r\n"
