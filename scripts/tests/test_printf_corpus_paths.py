"""printf_corpus.jsonl may only name files that still exist in the tree.

The corpus is append-only (tools/tidy/printf_corpus.py --add records converted
format pairs and never forgets their files), so a deleted source file leaves
stale rows behind. The doctest tier cannot catch that: the generated
printf_corpus_gen.inc keeps the cases of dropped rows and still compiles.
"""
import json
import os


def _repo_root():
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def test_every_corpus_row_names_only_existing_files():
    root = _repo_root()
    corpus = os.path.join(root, "tests", "unit", "printf_corpus.jsonl")
    with open(corpus, encoding="utf-8") as fh:
        rows = [json.loads(line) for line in fh if line.strip()]
    assert rows, "printf_corpus.jsonl is empty"
    missing = {}
    for row in rows:
        # Retired sources have no surviving paths; their format regression cases remain useful.
        for path in row["files"]:
            if not os.path.exists(os.path.join(root, path.replace("/", os.sep))):
                missing.setdefault(path, []).append(row["old"])
    assert not missing, (
        "printf_corpus.jsonl rows name files that no longer exist "
        f"({len(missing)} path(s), first: {sorted(missing)[0]})"
    )
