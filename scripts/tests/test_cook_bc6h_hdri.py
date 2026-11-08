"""Cooked sun metadata uses the same hemisphere and azimuth convention as the reader."""
import importlib.util
from pathlib import Path
import struct

import numpy as np
import pytest

spec = importlib.util.spec_from_file_location("cook_bc6h_hdri", Path(__file__).resolve().parents[2] / "tools/cook_bc6h_hdri.py")
cook = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cook)


def test_sun_azimuth_wrap_and_upper_hemisphere():
    sky = np.zeros((4, 8, 3), dtype=np.float32)
    sky[0, 0] = 10
    sky[0, 7] = 10
    sky[3, 4] = 1000  # Below horizon must not pull the centroid.
    assert abs(cook.scan_sun_azimuth(sky)) == pytest.approx(np.pi, abs=1e-6)
    sky[:] = 0
    assert cook.scan_sun_azimuth(sky) is None
    sky[0, 2] = 10
    assert cook.scan_sun_azimuth(sky) == pytest.approx((2.5 / 8 - .5) * 2 * np.pi)


@pytest.mark.parametrize("azimuth", [None, 1.25, -3.0])
def test_sidecar_embeds_sun_without_changing_block_payload(tmp_path, azimuth):
    file = tmp_path / "sky.ktx2"
    data = bytearray(128)
    data[:12] = bytes.fromhex('ab4b5458203230bb0d0a1a0a')
    struct.pack_into('<I', data, 40, 1)
    struct.pack_into('<QQQ', data, 80, 112, 16, 16)
    data[112:] = bytes(range(16))
    file.write_bytes(data)
    cook.write_sun_metadata(file, azimuth)
    result = file.read_bytes()
    offset, length = struct.unpack_from('<II', result, 56)
    entry_length = struct.unpack_from('<I', result, offset)[0]
    entry = result[offset + 4:offset + 4 + entry_length]
    key = b'MC2.sunAzimuth\0'
    assert entry[:len(key)] == key
    valid, az = struct.unpack('<Bf', entry[len(key):])
    assert bool(valid) == (azimuth is not None)
    assert az == pytest.approx(azimuth or 0)
    payload = struct.unpack_from('<Q', result, 80)[0]
    assert payload % 16 == 0
    assert result[payload:payload + 16] == bytes(range(16))
    assert offset + length <= payload


def test_metadata_preserves_other_keys_and_replaces_old_sun(tmp_path):
    file = tmp_path / "sky.ktx2"
    values = [b'KTXwriter\0test\0', b'zzCustom\0value']
    kv = b''.join(struct.pack('<I', len(value)) + value + bytes(-len(value) % 4) for value in values)
    payload = (104 + len(kv) + 15) & ~15
    data = bytearray(payload + 16)
    struct.pack_into('<I', data, 40, 1)
    struct.pack_into('<II', data, 56, 104, len(kv))
    struct.pack_into('<QQQ', data, 80, payload, 16, 16)
    data[104:104 + len(kv)] = kv
    data[payload:] = bytes(range(16))
    file.write_bytes(data)
    cook.write_sun_metadata(file, 1.0)
    first_size = file.stat().st_size
    cook.write_sun_metadata(file, 2.0)
    assert file.stat().st_size == first_size
    result = file.read_bytes()
    at, size = struct.unpack_from('<II', result, 56)
    end = at + size
    entries = []
    while at < end:
        length = struct.unpack_from('<I', result, at)[0]
        entries.append(result[at + 4:at + 4 + length].split(b'\0', 1))
        at += 4 + ((length + 3) & ~3)
    assert [key for key, _ in entries] == [b'KTXwriter', b'MC2.sunAzimuth', b'zzCustom']
    assert entries[0][1] == b'test\0'
    assert entries[1][1] == struct.pack('<Bf', 1, 2.0)
    assert entries[2][1] == b'value'
    offset = struct.unpack_from('<Q', result, 80)[0]
    assert offset % 16 == 0
    assert result[offset:offset + 16] == bytes(range(16))
