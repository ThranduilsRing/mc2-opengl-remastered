"""Offline normal-array sweep; MC2_TEST_INSTALL selects a deployed asset set."""
import os
from pathlib import Path
import struct

import numpy as np
import pytest


@pytest.mark.skipif(not os.environ.get("MC2_TEST_INSTALL"), reason="requires deployed terrain assets")
@pytest.mark.parametrize("slot", range(7))
def test_terrain_slot_has_nonflat_normal(slot):
    data = Path(os.environ["MC2_TEST_INSTALL"]) / "data"
    replacement = {1: "grass_turf", 2: "dirt_sand", 6: "forest_floor"}
    path = (data / "terrain_layers" / f"{replacement[slot]}_normal.ktx2" if slot in replacement
            else data / "textures" / f"mat{slot}_normal.ktx2")
    blob = path.read_bytes()
    assert blob[:12] == b"\xabKTX 20\xbb\r\n\x1a\n"
    assert struct.unpack_from("<I", blob, 12)[0] == 37  # linear RGBA8
    # Check both authored normals and the mip used at gameplay distances.
    for mip in (0, 4):
        offset, size, _ = struct.unpack_from("<3Q", blob, 80 + 24 * mip)
        xy = np.frombuffer(blob[offset:offset + size], dtype=np.uint8).reshape(-1, 4)[:, :2]
        assert np.min(np.std(xy, axis=0)) > 1.0, (slot, path, mip)
