"""check_deployed_spv: tampered byte, changed binding, orphan, clean copy (synthetic SPIR-V, tmp dirs)."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import check_deployed_spv as cds  # noqa: E402


def ins(op, *a):
    return [((len(a) + 1) << 16) | op, *a]


def spv(binding=1, push=16, pad=0):
    """Module: one uniform buffer (set 0, `binding`), a push-constant block of `push` bytes."""
    w = [cds.MAGIC, 0x10000, 0, 40, 0]
    w += ins(21, 1, 32, 0)                       # uint
    w += ins(30, 2, 1)                           # UBO struct { uint }
    w += ins(32, 3, 2, 2)                        # ptr Uniform
    w += ins(59, 3, 4, 2)                        # var Uniform
    w += ins(71, 2, cds.DEC_BLOCK)
    w += ins(72, 2, 0, cds.DEC_OFFSET, 0)
    w += ins(71, 4, cds.DEC_SET, 0)
    w += ins(71, 4, cds.DEC_BINDING, binding)
    w += ins(30, 5, *([1] * (push // 4)))        # push struct of N uints
    for k in range(push // 4):
        w += ins(72, 5, k, cds.DEC_OFFSET, 4 * k)
    w += ins(32, 6, 9, 5)                        # ptr PushConstant
    w += ins(59, 6, 7, 9)
    w += [0] * pad
    return struct.pack("<%dI" % len(w), *w)


def tree(root, files):
    d = Path(root) / "shaders" / "vulkan"
    d.mkdir(parents=True)
    for n, b in files.items():
        (d / n).write_bytes(b)
    return root


def run(tmp_path, build_files, inst_files, strict=False):
    b = tree(tmp_path / "b", build_files)
    i = tree(tmp_path / "i", inst_files)
    return cds.audit(i, b, strict)


def test_reflect_basic():
    r = cds.reflect(spv(binding=3, push=16))
    assert r["descriptors"] == [(0, 3, "uniform_buffer", 1)] and r["push"] == 16


def test_clean_copy_passes(tmp_path):
    f, w, n = run(tmp_path, {"a.spv": spv()}, {"a.spv": spv()})
    assert (f, w, n) == ([], [], 1)


def test_tampered_byte_fails(tmp_path):
    bad = bytearray(spv())
    bad[-1] ^= 0xFF
    f, _, _ = run(tmp_path, {"a.spv": spv()}, {"a.spv": bytes(bad)})
    assert any("a.spv: sha256 differs" in m for m in f)


def test_changed_binding_fails_naming_binding(tmp_path):
    f, _, _ = run(tmp_path, {"a.spv": spv(binding=1)}, {"a.spv": spv(binding=2)})
    assert any("a.spv: ABI set=0 binding=1" in m for m in f)
    assert any("a.spv: ABI set=0 binding=2" in m for m in f)


def test_push_size_change_fails(tmp_path):
    f, _, _ = run(tmp_path, {"a.spv": spv(push=16)}, {"a.spv": spv(push=32)})
    assert any("push-constant size build=16 install=32" in m for m in f)


def test_missing_fails(tmp_path):
    f, _, _ = run(tmp_path, {"a.spv": spv()}, {})
    assert any("a.spv: missing from install" in m for m in f)


def test_orphan_warns_and_strict_fails(tmp_path):
    f, w, _ = run(tmp_path, {"a.spv": spv()}, {"a.spv": spv(), "dead.spv": spv()})
    assert not f and any("dead.spv: orphan" in m for m in w)
    f2, _, _ = run(tmp_path / "s", {"a.spv": spv()}, {"a.spv": spv(), "dead.spv": spv()}, strict=True)
    assert any("dead.spv: orphan" in m for m in f2)
