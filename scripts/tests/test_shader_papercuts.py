"""ab.py side-A shader default and ninja_build orphan .spv pruning (tmp dirs only)."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tools" / "visual_verify"))
import ab  # noqa: E402
import ninja_build  # noqa: E402


def _spv(root: Path, name: str, data: bytes):
    p = root / "shaders" / "vulkan" / name
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)


def test_side_a_defaults_to_own_shaders_when_sets_differ(tmp_path, capsys):
    data, a_dir = tmp_path / "data", tmp_path / "v04"
    _spv(data, "x.spv", b"new")
    _spv(data, "water.spv", b"w")
    _spv(a_dir, "x.spv", b"old")
    assert ab.default_shaders_a(data, a_dir / "mc2.exe") == a_dir
    assert "defaults to its own" in capsys.readouterr().out


def test_side_a_keeps_install_shaders_when_identical_or_absent(tmp_path):
    data, a_dir, bare = tmp_path / "data", tmp_path / "v04", tmp_path / "bare"
    _spv(data, "x.spv", b"s")
    _spv(a_dir, "x.spv", b"s")
    bare.mkdir()
    assert ab.default_shaders_a(data, a_dir / "mc2.exe") is None
    assert ab.default_shaders_a(data, bare / "mc2.exe") is None


def test_prune_removes_only_orphan_spv_under_shaders(tmp_path):
    tree = tmp_path / "build"
    _spv(tree, "keep.frag.spv", b"1")
    _spv(tree, "gone.comp.spv", b"2")
    other = tree / "other" / "stray.spv"
    other.parent.mkdir()
    other.write_bytes(b"3")
    note = tree / "shaders" / "vulkan" / "notes.txt"
    note.write_text("x")
    removed = ninja_build.prune_orphan_spv(str(tmp_path), "build", {"shaders/vulkan/keep.frag.spv"})
    assert [Path(r).name for r in removed] == ["gone.comp.spv"]
    assert (tree / "shaders/vulkan/keep.frag.spv").exists() and other.exists() and note.exists()


def test_prune_does_nothing_without_a_known_output_set(tmp_path):
    _spv(tmp_path / "build", "a.spv", b"1")
    assert ninja_build.prune_orphan_spv(str(tmp_path), "build", set()) == []
    assert (tmp_path / "build/shaders/vulkan/a.spv").exists()
