#!/usr/bin/env python3
"""tools/mc2texcook/tests/test_mc2texcook.py -- Unit tests for mc2texcook.

Run from repo root or the mc2texcook directory:
    py -3 tools/mc2texcook/tests/test_mc2texcook.py
    py -3 test_mc2texcook.py          (if cwd is the tests/ directory)

No external files required.  All test images are created in-memory.
"""
from __future__ import annotations

import io
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

# ---------------------------------------------------------------------------
# Bootstrap: make sure mc2texcook module is importable regardless of cwd.
# ---------------------------------------------------------------------------

_TOOLS_DIR = Path(__file__).resolve().parent.parent
if str(_TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(_TOOLS_DIR))

import mc2texcook  # noqa: E402


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

KTX2_MAGIC = b"\xabKTX 20\xbb\r\n\x1a\n"

VK_FORMAT_R8G8B8A8_UNORM = 37
VK_FORMAT_R8G8B8A8_SRGB  = 43


def _make_rgba_image(width: int = 4, height: int = 4):
    """Return a Pillow RGBA Image with a deterministic pattern."""
    from PIL import Image  # type: ignore
    img = Image.new("RGBA", (width, height))
    pixels = []
    for y in range(height):
        for x in range(width):
            pixels.append((x * 64, y * 64, 128, 255))
    img.putdata(pixels)
    return img


def _make_rgb_image(width: int = 4, height: int = 4):
    """Return a Pillow RGB Image."""
    from PIL import Image  # type: ignore
    img = Image.new("RGB", (width, height))
    pixels = [(x * 64, y * 64, 64) for y in range(height) for x in range(width)]
    img.putdata(pixels)
    return img


def _make_gray_image(width: int = 4, height: int = 4):
    """Return a Pillow L (grayscale) Image."""
    from PIL import Image  # type: ignore
    img = Image.new("L", (width, height))
    pixels = [((x + y) * 30) % 256 for y in range(height) for x in range(width)]
    img.putdata(pixels)
    return img


def _parse_ktx2_header(data: bytes) -> dict:
    """Parse magic + fixed header fields from raw KTX2 bytes.

    Returns a dict with keys:
        magic, vk_format, type_size, width, height, depth, layer_count,
        face_count, level_count, sc_scheme,
        dfd_offset, dfd_length, kvd_offset, kvd_length,
        sgd_offset, sgd_length
    """
    assert len(data) >= 80, f"data too short: {len(data)} bytes"
    magic = data[:12]
    (vk_format, type_size, width, height, depth, layer_count, face_count,
     level_count, sc_scheme) = struct.unpack_from("<9I", data, 12)
    dfd_offset, dfd_length, kvd_offset, kvd_length = struct.unpack_from("<4I", data, 48)
    sgd_offset, sgd_length = struct.unpack_from("<2Q", data, 64)
    return dict(
        magic=magic,
        vk_format=vk_format,
        type_size=type_size,
        width=width,
        height=height,
        depth=depth,
        layer_count=layer_count,
        face_count=face_count,
        level_count=level_count,
        sc_scheme=sc_scheme,
        dfd_offset=dfd_offset,
        dfd_length=dfd_length,
        kvd_offset=kvd_offset,
        kvd_length=kvd_length,
        sgd_offset=sgd_offset,
        sgd_length=sgd_length,
    )


def _parse_level_index(data: bytes, level_count: int) -> list[dict]:
    """Parse the level index from KTX2 bytes.

    Returns a list of {byte_offset, byte_length, uncompressed_byte_length}.
    """
    HEADER_SIZE = 80
    entries = []
    for i in range(level_count):
        off = HEADER_SIZE + i * 24
        byte_offset, byte_length, uncompressed = struct.unpack_from("<3Q", data, off)
        entries.append(dict(
            byte_offset=byte_offset,
            byte_length=byte_length,
            uncompressed_byte_length=uncompressed,
        ))
    return entries


def _cook_to_bytes(image, preset: str) -> bytes:
    """Run the cook pipeline and return the raw KTX2 bytes (via a temp file)."""
    rgba, vk_format, dfd = mc2texcook._apply_preset(image, preset)
    mips = mc2texcook._generate_mips(rgba)
    with tempfile.NamedTemporaryFile(suffix=".ktx2", delete=False) as f:
        tmp = Path(f.name)
    mc2texcook.write_ktx2(tmp, mips, vk_format, dfd)
    data = tmp.read_bytes()
    tmp.unlink()
    return data


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestMagic(unittest.TestCase):
    def test_magic_correct(self):
        data = _cook_to_bytes(_make_rgba_image(), "albedo")
        self.assertEqual(data[:12], KTX2_MAGIC)


class TestHeaderSize(unittest.TestCase):
    def test_header_80_bytes_before_level_index(self):
        """Fixed header occupies exactly 80 bytes (magic+9*uint32+4*uint32+2*uint64)."""
        data = _cook_to_bytes(_make_rgba_image(4, 4), "normal")
        hdr = _parse_ktx2_header(data)
        # Level index starts at offset 80.  DFD starts right after the level index.
        level_index_size = hdr["level_count"] * 24
        self.assertEqual(hdr["dfd_offset"], 80 + level_index_size)


class TestAlbedoPreset(unittest.TestCase):
    def setUp(self):
        self.img = _make_rgba_image(4, 4)
        self.data = _cook_to_bytes(self.img, "albedo")
        self.hdr  = _parse_ktx2_header(self.data)

    def test_vk_format_srgb(self):
        self.assertEqual(self.hdr["vk_format"], VK_FORMAT_R8G8B8A8_SRGB)

    def test_dimensions(self):
        self.assertEqual(self.hdr["width"],  4)
        self.assertEqual(self.hdr["height"], 4)

    def test_level_count_correct(self):
        # 4x4 -> 4, 2x2, 1x1 = 3 mips
        self.assertEqual(self.hdr["level_count"], 3)

    def test_no_supercompression(self):
        self.assertEqual(self.hdr["sc_scheme"], 0)

    def test_type_size(self):
        self.assertEqual(self.hdr["type_size"], 1)

    def test_face_count(self):
        self.assertEqual(self.hdr["face_count"], 1)

    def test_dfd_length(self):
        # RGBA8 DFD = 4 (dfdTotalSize) + 8 (vendorId/type + version/blockSize) +
        #             16 (colorModel..bytesPlane) + 4*16 (samples) = 92 bytes
        self.assertEqual(self.hdr["dfd_length"], 92)

    def test_dfd_transfer_function_srgb(self):
        # Byte 14 of DFD (relative to dfd_offset) must be 2 (SRGB)
        dfd_start = self.hdr["dfd_offset"]
        self.assertEqual(self.data[dfd_start + 14], 2)


class TestEmissivePreset(unittest.TestCase):
    def test_vk_format_srgb(self):
        data = _cook_to_bytes(_make_rgba_image(), "emissive")
        hdr  = _parse_ktx2_header(data)
        self.assertEqual(hdr["vk_format"], VK_FORMAT_R8G8B8A8_SRGB)

    def test_dfd_transfer_function_srgb(self):
        data = _cook_to_bytes(_make_rgba_image(), "emissive")
        hdr  = _parse_ktx2_header(data)
        dfd_start = hdr["dfd_offset"]
        self.assertEqual(data[dfd_start + 14], 2)


class TestNormalPreset(unittest.TestCase):
    def setUp(self):
        self.img = _make_rgba_image(4, 4)
        self.data = _cook_to_bytes(self.img, "normal")
        self.hdr  = _parse_ktx2_header(self.data)

    def test_vk_format_unorm(self):
        self.assertEqual(self.hdr["vk_format"], VK_FORMAT_R8G8B8A8_UNORM)

    def test_dfd_transfer_function_linear(self):
        # Byte 14 of DFD must be 1 (LINEAR)
        dfd_start = self.hdr["dfd_offset"]
        self.assertEqual(self.data[dfd_start + 14], 1)


class TestOrmPreset(unittest.TestCase):
    def setUp(self):
        self.img = _make_rgb_image(4, 4)
        self.data = _cook_to_bytes(self.img, "orm")
        self.hdr  = _parse_ktx2_header(self.data)

    def test_vk_format_unorm(self):
        self.assertEqual(self.hdr["vk_format"], VK_FORMAT_R8G8B8A8_UNORM)

    def test_alpha_channel_is_255(self):
        """All alpha bytes in mip 0 must be 255."""
        entries = _parse_level_index(self.data, self.hdr["level_count"])
        mip0 = entries[0]
        mip0_data = self.data[mip0["byte_offset"]: mip0["byte_offset"] + mip0["byte_length"]]
        # Every 4th byte (alpha) must be 255
        alpha_bytes = mip0_data[3::4]
        self.assertTrue(all(b == 255 for b in alpha_bytes),
                        f"Not all alpha bytes are 255: {list(alpha_bytes)}")

    def test_mip0_byte_length(self):
        entries = _parse_level_index(self.data, self.hdr["level_count"])
        expected = 4 * 4 * 4  # width*height*4 bytes
        self.assertEqual(entries[0]["byte_length"], expected)


class TestBc7SlotAware(unittest.TestCase):
    """SLOT-AWARE-BC7: ORM/normal/mask must be cooked LINEAR BC7_UNORM, not SRGB."""

    @unittest.skipUnless(shutil.which("ktx"), "KTX-Software CLI not on PATH")
    def test_bc7_orm_is_linear_unorm(self):
        import batch_cook, tempfile, subprocess
        from pathlib import Path
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "thing.orm.png"
            _make_rgb_image((64, 64)).save(src)
            out = Path(td) / "thing.orm.ktx2"
            batch_cook._cook_one_bc7(src, out, ktx_tool="ktx", txm_size=0, preset="orm")
            info = subprocess.run(["ktx", "info", str(out)], capture_output=True, text=True).stdout
            self.assertIn("BC7_UNORM", info)
            self.assertNotIn("BC7_SRGB", info)


class TestMaskPreset(unittest.TestCase):
    def test_grayscale_input(self):
        img  = _make_gray_image(4, 4)
        data = _cook_to_bytes(img, "mask")
        hdr  = _parse_ktx2_header(data)
        self.assertEqual(hdr["vk_format"], VK_FORMAT_R8G8B8A8_UNORM)

        entries = _parse_level_index(data, hdr["level_count"])
        mip0 = entries[0]
        mip0_data = data[mip0["byte_offset"]: mip0["byte_offset"] + mip0["byte_length"]]

        # R=G=B for every pixel; alpha=255
        for i in range(0, len(mip0_data), 4):
            r, g, b, a = mip0_data[i], mip0_data[i+1], mip0_data[i+2], mip0_data[i+3]
            self.assertEqual(r, g, f"pixel {i//4}: R({r}) != G({g})")
            self.assertEqual(g, b, f"pixel {i//4}: G({g}) != B({b})")
            self.assertEqual(a, 255, f"pixel {i//4}: alpha={a} != 255")

    def test_rgba_passthrough(self):
        img  = _make_rgba_image(4, 4)
        data = _cook_to_bytes(img, "mask")
        hdr  = _parse_ktx2_header(data)
        self.assertEqual(hdr["vk_format"], VK_FORMAT_R8G8B8A8_UNORM)


class TestMipChain(unittest.TestCase):
    def test_mip_count_4x4(self):
        # 4x4: mip 0 (4x4), mip 1 (2x2), mip 2 (1x1) = 3 levels
        from PIL import Image  # type: ignore
        img  = Image.new("RGBA", (4, 4), (0, 0, 0, 255))
        mips = mc2texcook._generate_mips(img)
        self.assertEqual(len(mips), 3)
        self.assertEqual(mips[0].size, (4, 4))
        self.assertEqual(mips[1].size, (2, 2))
        self.assertEqual(mips[2].size, (1, 1))

    def test_mip_count_8x4(self):
        # 8x4: (8,4)->(4,2)->(2,1)->(1,1) = 4 levels
        from PIL import Image  # type: ignore
        img  = Image.new("RGBA", (8, 4), (0, 0, 0, 255))
        mips = mc2texcook._generate_mips(img)
        self.assertEqual(len(mips), 4)
        self.assertEqual(mips[3].size, (1, 1))

    def test_mip_count_1x1(self):
        from PIL import Image  # type: ignore
        img  = Image.new("RGBA", (1, 1), (255, 0, 0, 255))
        mips = mc2texcook._generate_mips(img)
        self.assertEqual(len(mips), 1)

    def test_fully_transparent_rgb_preserved_in_mips(self):
        """Regression: fully-transparent (alpha=0) source with meaningful RGB
        must keep its RGB in coarse mips. Pillow's RGBA LANCZOS premultiplies
        alpha and would zero the RGB -> props (cliffs/fences/markers) turn black
        at distance. RGB and alpha are resized independently to prevent this."""
        from PIL import Image  # type: ignore
        # 16x16 white but fully transparent (alpha=0 everywhere).
        img  = Image.new("RGBA", (16, 16), (255, 255, 255, 0))
        mips = mc2texcook._generate_mips(img)
        self.assertEqual(len(mips), 5)  # 16,8,4,2,1
        for lvl, m in enumerate(mips):
            px = m.tobytes()
            r = sum(px[0::4]) / (len(px) // 4)
            a = sum(px[3::4]) / (len(px) // 4)
            self.assertGreater(r, 250, f"mip {lvl}: RGB zeroed (premultiply bug); mean R={r}")
            self.assertLess(a, 5, f"mip {lvl}: alpha should stay ~0, got {a}")

    def test_level_index_offsets_contiguous(self):
        """Level index offsets should describe contiguous non-overlapping regions."""
        img  = _make_rgba_image(4, 4)
        data = _cook_to_bytes(img, "normal")
        hdr  = _parse_ktx2_header(data)
        entries = _parse_level_index(data, hdr["level_count"])

        # Each entry: offset + length <= next entry's offset (or end of file)
        for i, e in enumerate(entries):
            end = e["byte_offset"] + e["byte_length"]
            if i + 1 < len(entries):
                self.assertLessEqual(end, entries[i + 1]["byte_offset"])
            else:
                self.assertLessEqual(end, len(data))


class TestPixelData(unittest.TestCase):
    def test_mip0_pixel_bytes_correct(self):
        """Pixel bytes at mip 0 must match raw PIL tobytes()."""
        from PIL import Image  # type: ignore

        # Solid red RGBA image so we know exactly what bytes to expect.
        img = Image.new("RGBA", (4, 4), (200, 50, 30, 255))
        data = _cook_to_bytes(img, "normal")
        hdr  = _parse_ktx2_header(data)
        entries = _parse_level_index(data, hdr["level_count"])
        mip0 = entries[0]
        actual = data[mip0["byte_offset"]: mip0["byte_offset"] + mip0["byte_length"]]
        expected = img.tobytes()
        self.assertEqual(actual, expected)

    def test_mip0_byte_length_matches_wh(self):
        data = _cook_to_bytes(_make_rgba_image(8, 4), "albedo")
        hdr  = _parse_ktx2_header(data)
        entries = _parse_level_index(data, hdr["level_count"])
        self.assertEqual(entries[0]["byte_length"], 8 * 4 * 4)

    def test_uncompressed_byte_length_equals_byte_length(self):
        """For sc_scheme=NONE, uncompressed == byte_length for every mip."""
        data = _cook_to_bytes(_make_rgba_image(4, 4), "albedo")
        hdr  = _parse_ktx2_header(data)
        entries = _parse_level_index(data, hdr["level_count"])
        for e in entries:
            self.assertEqual(e["byte_length"], e["uncompressed_byte_length"])


class TestValidateManifestCompat(unittest.TestCase):
    """Verify that _read_ktx2_header from validate_manifest.py accepts our output."""

    def _read_ktx2_header_inline(self, path: Path):
        """Inline copy of validate_manifest._read_ktx2_header for self-contained testing."""
        import struct
        KTX2_MAGIC = b"\xabKTX 20\xbb\r\n\x1a\n"
        try:
            with open(path, "rb") as f:
                magic = f.read(12)
                if magic != KTX2_MAGIC:
                    return f"bad magic: {magic!r}"
                data = f.read(4 * 9)
                if len(data) < 36:
                    return "truncated header"
                (vk_format, _type_size, width, height, _depth, layer_count,
                 _faces, _levels, _sc) = struct.unpack_from("<9I", data)
        except OSError as e:
            return str(e)
        ALPHA_FORMATS = set(range(37, 51)) | {135, 136, 145, 146}
        return {
            "width": width,
            "height": height,
            "vk_format": vk_format,
            "layers": max(layer_count, 1),
            "has_alpha": vk_format in ALPHA_FORMATS,
        }

    def _cook_and_check(self, img, preset: str, expected_vk: int,
                        expected_w: int, expected_h: int):
        with tempfile.NamedTemporaryFile(suffix=".ktx2", delete=False) as f:
            tmp = Path(f.name)
        try:
            rgba, vk_format, dfd = mc2texcook._apply_preset(img, preset)
            mips = mc2texcook._generate_mips(rgba)
            mc2texcook.write_ktx2(tmp, mips, vk_format, dfd)
            result = self._read_ktx2_header_inline(tmp)
        finally:
            tmp.unlink(missing_ok=True)

        self.assertIsInstance(result, dict, f"preset={preset}: {result}")
        self.assertEqual(result["vk_format"], expected_vk, f"preset={preset} vk_format mismatch")
        self.assertEqual(result["width"],     expected_w,  f"preset={preset} width mismatch")
        self.assertEqual(result["height"],    expected_h,  f"preset={preset} height mismatch")
        self.assertTrue(result["has_alpha"],  f"preset={preset}: expected has_alpha=True")

    def test_albedo(self):
        self._cook_and_check(_make_rgba_image(4, 4), "albedo",
                             VK_FORMAT_R8G8B8A8_SRGB, 4, 4)

    def test_emissive(self):
        self._cook_and_check(_make_rgba_image(4, 4), "emissive",
                             VK_FORMAT_R8G8B8A8_SRGB, 4, 4)

    def test_normal(self):
        self._cook_and_check(_make_rgba_image(4, 4), "normal",
                             VK_FORMAT_R8G8B8A8_UNORM, 4, 4)

    def test_orm(self):
        self._cook_and_check(_make_rgb_image(4, 4), "orm",
                             VK_FORMAT_R8G8B8A8_UNORM, 4, 4)

    def test_mask(self):
        self._cook_and_check(_make_gray_image(4, 4), "mask",
                             VK_FORMAT_R8G8B8A8_UNORM, 4, 4)


# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# TXM reader tests
# ---------------------------------------------------------------------------

def _make_txm_bytes(pixels_bgra: bytes) -> bytes:
    """Encode raw 64×64 BGRA pixel bytes with the MC2 LZW compressor (Python impl).

    The Python compressor here is a simple but correct LZW encoder that matches
    the bit layout consumed by mc2texcook._lzw_decomp().
    """
    assert len(pixels_bgra) == 64 * 64 * 4

    # Simple LZW encode: LSB-first, 9-12 bit codes, clear=256, eof=257.
    src = pixels_bgra
    out_bits: list[int] = []  # flat list of bits (lsb-first)

    def emit(code: int, nbits: int) -> None:
        for i in range(nbits):
            out_bits.append((code >> i) & 1)

    code_size = 9
    max_code = 1 << code_size
    table: dict[bytes, int] = {bytes([i]): i for i in range(256)}
    next_code = 258
    emit(256, code_size)  # CLEAR

    buf = bytes([src[0]])
    for byte in src[1:]:
        candidate = buf + bytes([byte])
        if candidate in table:
            buf = candidate
        else:
            emit(table[buf], code_size)
            if next_code < 4096:
                table[candidate] = next_code
                next_code += 1
                if next_code > max_code and code_size < 12:
                    code_size += 1
                    max_code = 1 << code_size
            elif next_code == 4096:
                emit(256, code_size)  # CLEAR
                code_size = 9; max_code = 512
                table = {bytes([i]): i for i in range(256)}
                next_code = 258
            buf = bytes([byte])
    emit(table[buf], code_size)
    emit(257, code_size)  # EOF

    # Pack bits into bytes (lsb-first)
    result = bytearray()
    acc = 0
    count = 0
    for bit in out_bits:
        acc |= bit << count
        count += 1
        if count == 8:
            result.append(acc)
            acc = 0
            count = 0
    if count:
        result.append(acc)
    return bytes(result)


class TestTxmReader(unittest.TestCase):

    def _solid_bgra(self, b: int, g: int, r: int, a: int = 255) -> bytes:
        return bytes([b, g, r, a] * (64 * 64))

    def test_lzw_decomp_roundtrip_solid(self) -> None:
        """_lzw_decomp can recover a uniform 64×64 BGRA block."""
        raw = self._solid_bgra(0x8a, 0x91, 0x94)
        compressed = _make_txm_bytes(raw)
        decompressed = mc2texcook._lzw_decomp(compressed)
        self.assertEqual(decompressed, raw)

    def test_lzw_decomp_clear_code(self) -> None:
        """_lzw_decomp handles CLEAR code resets correctly."""
        # Two distinct solid blocks concatenated will usually force a CLEAR
        # when the compressor's dictionary fills.  For a 16384-byte block of
        # uniform data this is unlikely, but we at least verify the result is
        # correct (the same uniform block) regardless of how many CLEARs occurred.
        raw = self._solid_bgra(0x20, 0x40, 0x60)
        compressed = _make_txm_bytes(raw)
        decompressed = mc2texcook._lzw_decomp(compressed)
        self.assertEqual(decompressed, raw)

    def test_read_txm_size_and_mode(self) -> None:
        """_read_txm returns a 64×64 RGBA image."""
        raw_bgra = self._solid_bgra(0x10, 0x20, 0x30)
        compressed = _make_txm_bytes(raw_bgra)
        with tempfile.NamedTemporaryFile(suffix=".txm", delete=False) as f:
            tmp = Path(f.name)
            f.write(compressed)
        try:
            img = mc2texcook._read_txm(tmp)
            self.assertEqual(img.mode, "RGBA")
            self.assertEqual(img.size, (64, 64))
        finally:
            tmp.unlink()

    def test_read_txm_bgra_to_rgba_swap(self) -> None:
        """_read_txm swaps B↔R so stored BGRA becomes RGBA."""
        # Source BGRA: B=0x10, G=0x20, R=0x30, A=0xFF
        raw_bgra = self._solid_bgra(0x10, 0x20, 0x30, 0xFF)
        compressed = _make_txm_bytes(raw_bgra)
        with tempfile.NamedTemporaryFile(suffix=".txm", delete=False) as f:
            tmp = Path(f.name)
            f.write(compressed)
        try:
            img = mc2texcook._read_txm(tmp)
            px = img.getpixel((0, 0))  # (R, G, B, A) in PIL RGBA
            # After swap: R=0x30, G=0x20, B=0x10, A=0xFF
            self.assertEqual(px, (0x30, 0x20, 0x10, 0xFF))
        finally:
            tmp.unlink()

    def test_read_txm_wrong_size_raises(self) -> None:
        """_read_txm raises ValueError when decompressed size ≠ 16384."""
        # Encode only 8 bytes → wrong size
        tiny_raw = bytes([0x01] * 8)
        code_size = 9
        out_bits: list[int] = []
        def emit(code: int, n: int) -> None:
            for i in range(n): out_bits.append((code >> i) & 1)
        emit(256, code_size)
        for b in tiny_raw: emit(b, code_size)
        emit(257, code_size)
        acc = 0; count = 0; compressed = bytearray()
        for bit in out_bits:
            acc |= bit << count; count += 1
            if count == 8: compressed.append(acc); acc = 0; count = 0
        if count: compressed.append(acc)

        with tempfile.NamedTemporaryFile(suffix=".txm", delete=False) as f:
            tmp = Path(f.name)
            f.write(bytes(compressed))
        try:
            with self.assertRaises(ValueError, msg="Expected ValueError for wrong size"):
                mc2texcook._read_txm(tmp)
        finally:
            tmp.unlink()

    def test_load_image_routes_txm(self) -> None:
        """_load_image dispatches .txm files to _read_txm."""
        raw_bgra = self._solid_bgra(0xAA, 0xBB, 0xCC)
        compressed = _make_txm_bytes(raw_bgra)
        with tempfile.NamedTemporaryFile(suffix=".txm", delete=False) as f:
            tmp = Path(f.name)
            f.write(compressed)
        try:
            img = mc2texcook._load_image(tmp)
            self.assertEqual(img.mode, "RGBA")
            self.assertEqual(img.size, (64, 64))
        finally:
            tmp.unlink()


# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    unittest.main(verbosity=2)
