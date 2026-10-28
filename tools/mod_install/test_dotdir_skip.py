"""
Unit tests for S5 -- unified dot-dir skip rule (ruling C4).

Spec: docs/superpowers/strategy/superpowers-execution-roadmap.md §4 C4, §5 S5.

Tests:
  1. Synthetic mod with .scratch/data/foo.txt + data/foo.txt: only data/foo.txt
     appears in the index (dot-dir skip).
  2. Dot-file inside data/ (data/.gitignore): skipped, not indexed.
  3. Normal file adjacent to dot entries: still indexed correctly.
  4. Dot-dir nested deeper (data/sub/.hidden/bar.txt): skipped.
  5. Resolver resolve() returns mod layer for real file, MISS for probe path
     that lives only under a dot-dir.

All tests use stdlib only (tempfile, os, pathlib).  No pytest required --
run with: python test_dotdir_skip.py
"""

from __future__ import annotations

import os
import sys
import tempfile
import traceback
from pathlib import Path

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from resolver import ResolverConfig, resolve, _index_mod_layer, normalize_key


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_file(path: str, content: str = "x") -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(content)


# ---------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------

def test_dotdir_not_indexed() -> None:
    """
    Synthetic mod: .scratch/data/foo.txt (dot-dir sibling of data/) is never
    walked because InitModSearchPaths only passes data/ to IndexModData.
    This test exercises the _index_mod_layer dot-dir skip within data/.
    Plant .scratch/ INSIDE data/ to exercise the guards directly.
    """
    with tempfile.TemporaryDirectory() as tmp:
        mod_id = "test-mod"
        data_dir = os.path.join(tmp, "mods", mod_id, "data")

        # Real file: data/foo.txt -- must appear in index
        _make_file(os.path.join(data_dir, "foo.txt"), "real")

        # Dot-dir inside data/: data/.scratch/foo.txt -- must NOT appear
        _make_file(os.path.join(data_dir, ".scratch", "foo.txt"), "probe")

        index: dict = {}
        shadowed: dict = {}
        _index_mod_layer(index, shadowed, data_dir + "/", mod_id)

        assert normalize_key("data/foo.txt") in index, \
            "data/foo.txt must be in index"
        assert normalize_key("data/.scratch/foo.txt") not in index, \
            "data/.scratch/foo.txt must NOT be in index (dot-dir skip)"

        print("  PASS test_dotdir_not_indexed")


def test_dotfile_not_indexed() -> None:
    """Dot-file directly in data/ (e.g. data/.gitignore) must be skipped."""
    with tempfile.TemporaryDirectory() as tmp:
        mod_id = "test-mod"
        data_dir = os.path.join(tmp, "mods", mod_id, "data")

        _make_file(os.path.join(data_dir, "real.txt"), "real")
        _make_file(os.path.join(data_dir, ".gitignore"), "*.pyc")

        index: dict = {}
        shadowed: dict = {}
        _index_mod_layer(index, shadowed, data_dir + "/", mod_id)

        assert normalize_key("data/real.txt") in index, \
            "data/real.txt must be in index"
        assert normalize_key("data/.gitignore") not in index, \
            "data/.gitignore must NOT be in index (dot-file skip)"

        print("  PASS test_dotfile_not_indexed")


def test_normal_file_indexed() -> None:
    """Normal files adjacent to dot entries are still indexed correctly."""
    with tempfile.TemporaryDirectory() as tmp:
        mod_id = "test-mod"
        data_dir = os.path.join(tmp, "mods", mod_id, "data")

        _make_file(os.path.join(data_dir, "missions", "m01.fit"), "fit")
        _make_file(os.path.join(data_dir, ".modproject", "meta.json"), "{}")
        _make_file(os.path.join(data_dir, ".playtest", "run.log"), "log")

        index: dict = {}
        shadowed: dict = {}
        _index_mod_layer(index, shadowed, data_dir + "/", mod_id)

        assert normalize_key("data/missions/m01.fit") in index, \
            "data/missions/m01.fit must be in index"
        assert normalize_key("data/.modproject/meta.json") not in index, \
            ".modproject entries must NOT be in index"
        assert normalize_key("data/.playtest/run.log") not in index, \
            ".playtest entries must NOT be in index"

        print("  PASS test_normal_file_indexed")


def test_dotdir_nested_skip() -> None:
    """Dot-dir appearing deeper in the tree is also skipped."""
    with tempfile.TemporaryDirectory() as tmp:
        mod_id = "test-mod"
        data_dir = os.path.join(tmp, "mods", mod_id, "data")

        _make_file(os.path.join(data_dir, "sub", "real.tga"), "img")
        _make_file(os.path.join(data_dir, "sub", ".hidden", "secret.tga"), "s")

        index: dict = {}
        shadowed: dict = {}
        _index_mod_layer(index, shadowed, data_dir + "/", mod_id)

        assert normalize_key("data/sub/real.tga") in index
        assert normalize_key("data/sub/.hidden/secret.tga") not in index

        print("  PASS test_dotdir_nested_skip")


def test_resolve_probe_absent_from_index() -> None:
    """Layer 0 is explicitly mounted; hidden entries under data remain excluded."""
    with tempfile.TemporaryDirectory() as tmp:
        game_dir = tmp
        mods_root = os.path.join(tmp, "mods")
        mod_id = "my-mod"

        # Real data file
        _make_file(os.path.join(mods_root, mod_id, "data", "probe.txt"), "real")
        # Explicit scratch layer at the active mod root.
        _make_file(os.path.join(mods_root, mod_id, ".scratch", "data", "probe.txt"),
                   "scratch-override")
        # Dot-dir inside data/ (ruled out by the dot-skip guard)
        _make_file(os.path.join(mods_root, mod_id, "data", ".hidden", "probe.txt"),
                   "hidden-override")

        cfg = ResolverConfig(
            game_dir=game_dir,
            mods_root=mods_root,
            active_mod=mod_id,
            fst_files=[],
        )

        # Scratch wins over the project file.
        result = resolve("data/probe.txt", cfg)
        assert result["layer"] == f"mod:{mod_id}#scratch"
        assert open(result["path"]).read() == "scratch-override"
        # Dot-dir entry inside data/ is not reachable
        result2 = resolve("data/.hidden/probe.txt", cfg)
        assert result2["layer"] == "MISS", \
            f"Expected MISS for dot-dir path, got {result2['layer']}"

        print("  PASS test_resolve_probe_absent_from_index")
        print(f"    real probe -> layer={result['layer']} path={result['path']}")
        print(f"    .hidden probe -> layer={result2['layer']}")


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

def main() -> int:
    tests = [
        test_dotdir_not_indexed,
        test_dotfile_not_indexed,
        test_normal_file_indexed,
        test_dotdir_nested_skip,
        test_resolve_probe_absent_from_index,
    ]
    passed = 0
    failed = 0
    print(f"Running {len(tests)} S5 dot-dir skip unit tests...\n")
    for t in tests:
        try:
            t()
            passed += 1
        except Exception:
            print(f"  FAIL {t.__name__}")
            traceback.print_exc()
            failed += 1
    print(f"\n{'OK' if failed == 0 else 'FAILED'}: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
