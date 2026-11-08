"""Explicit artifact-backed FX proof; source boundary runs without artifacts."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
EXE = Path(os.environ.get("MC2FX_EXE", ROOT / "build64-ninja/out/tools/mc2fx/mc2fx.exe"))
STOCK_SHA = "0fc3f12eec5245a7544429c93fc891f0f770a4131cd4a8ca95616bf4b5361b3d"


def u32(value):
    return struct.pack("<I", value)


def floats(*values):
    return struct.pack("<" + "f" * len(values), *values)


def string(value):
    raw = value.encode("ascii")
    return u32(len(raw)) + (raw + b"\0" if raw else b"")


def curve(kind, version, seed=None):
    if kind == "X":
        value = (floats(1) if version < 15 else b"") + u32(3)
        value += floats(0, 1, 1, 0.5, -0.5, 1.5, 1, 0, 1.25)
    else:
        value = {"C": floats(1), "L": floats(1, 0.25), "S": floats(1, 0.25, 0.125, -0.0625)}[kind]
    return value if seed is None else value + curve(seed, version) + b"\1"


def state(version=9, texture="fixture_base", alpha=0):
    value = u32(1 | (alpha << 14)) + u32(0xFFFFFFFF) + u32(0xFFFFFFFF)
    value += u32(0) + u32(0xFFFFFFFF) + u32(0xFFFFFFFF)
    if version < 6:
        value += floats(0, 0)
        if version > 3:
            value += floats(0) + u32(0)
    return value + string(texture) + u32(0)


MESH_IDS = (87, 88, 89, 90, 91, 92, 95, 96, 97, 98, 99, 100, 101, 102, 107, 108, 109, 110, 111)


def mesh(class_id, version):
    triangle = class_id in (99, 100, 101, 102, 107, 108, 109, 110, 111)
    dual = class_id in (90, 91, 92, 107, 108, 109)
    detail = class_id in (96, 97, 98, 100, 110, 111)
    colored = class_id in (88, 89, 91, 92, 97, 98, 101, 102, 108, 109, 110, 111)
    lit = class_id in (89, 92, 98, 102, 109, 111)
    count = 3 if triangle else 4
    points = ((-1, -2, 0), (2, -2, 0), (2, 3, 0), (-1, 3, 0))[:count]
    uvs = ((0, 0), (1, 0), (1, 1), (0, 1))[:count]
    uv_bytes = b"".join(floats(*v) for v in uvs)
    value = u32(class_id) + u32(count) + b"".join(floats(*v) for v in points)
    value += u32(count * (2 if dual else 1)) + uv_bytes * (2 if dual else 1)
    value += u32(1) + bytes([count]) + u32(0) + state(version)
    value += u32(count) + struct.pack("<" + "H" * count, *range(count))
    if dual or detail:
        value += state(version, "fixture_detail", 2)
    if detail:
        value += floats(0.25, 0.5, 2, 3)
        if triangle:
            value += floats(10, 20)
    if class_id == 95:
        value += bytes([3])
        for pass_index in (1, 2):
            value += state(version, f"fixture_pass{pass_index}") + u32(count) + uv_bytes
    if colored:
        value += u32(count) + u32(0xFFA17F03) * count
    if lit:
        value += u32(count) + floats(0, 0, 1) * count
    return value


def shape(version):
    return u32(len(MESH_IDS)) + b"".join(mesh(ordinal, version) for ordinal in MESH_IDS)


def effect(ordinal, version, mlr):
    c = lambda age, seed=None: curve(age, version, seed)
    value = u32(1312 + ordinal) + string(f"fixture_{ordinal}") + u32(1)
    value += floats(0.25) + u32(3) + u32(0) + floats(*range(12))
    value += c("C") + c("S") + c("S") + u32(0x4D4C5223) + u32(mlr) + state(mlr, alpha=1)
    if 1 <= ordinal <= 8:
        value += c("C") + c("X") + c("L") * 3 + c("C") + c("S") + c("X", "X") + c("X", "S")
        value += c("C", "L") * 3 + c("S", "L") * 3 + c("X", "X") + c("X", "L") * 4 + u32(32)
        if ordinal >= 3:
            value += c("C", "L") + c("X", "X") + b"\1\0\1" + (b"\0" if version >= 10 else b"")
    if 9 <= ordinal <= 11:
        value += c("X", "L") * 4 + c("X", "X") + b"\1\0"
    if ordinal in (4, 5):
        value += c("X", "S") + c("C", "S")
        if ordinal == 5:
            value += c("X", "L") * 4 + u32(5)
    if ordinal in (6, 10):
        value += c("X" if ordinal == 6 else "C", "X") * 2
        value += c("X", "L") * 2 if version < 10 else c("X", "S") + c("C") * 2
        value += c("C") * 2 + (b"\1" if version >= 10 else b"")
    if ordinal == 8:
        value += u32(0)
    if ordinal == 12:
        value += c("C") * 2 + c("L") * 3 + c("C") + c("X") + c("X", "S")
        if version >= 12:
            value += c("X", "S")
        value += c("X", "L") * 4 + c("X", "S") * 2 + c("S", "S") * 2 + u32(16) + u32(0) + b"\0"
    if ordinal == 13:
        value += c("C") + c("S") + c("X", "X") + c("X", "S") + c("C", "L")
        value += c("S", "L") + c("X", "X") + c("X", "L") + c("C", "L") + floats(1, 2, 3)
    if ordinal == 14:
        value += c("X") * 4
        if version > 15:
            value += c("S") * 2
        if version > 16:
            value += string("fixture_light")
        value += b"\1"
    if ordinal in (7, 11, 13):
        value += u32(0x4D4C5223) + u32(mlr)
        if ordinal == 13:
            value += u32(3)
            for radius in (0.5, 2, 1.25):
                value += floats(1, -1, 0.5, 0.5, 0.5, 0, radius) + shape(mlr)
        else:
            value += shape(mlr) + floats(4)
    return value


def wire_fixture(version=17, mlr=9):
    concrete = (0, 2, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14)
    return u32(0x47465823) + u32(version) + u32(len(concrete)) + b"".join(effect(n, version, mlr) for n in concrete)


def run(*arguments):
    assert EXE.is_file(), f"build mc2fx first: missing {EXE}"
    result = subprocess.run([str(EXE), *map(str, arguments)], capture_output=True, text=True, timeout=90)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


def prove(payload, tmp_path, label):
    source, rebuilt = tmp_path / "input.fx", tmp_path / "rebuilt.fx"
    source.write_bytes(payload)
    assert "IDENTICAL" in run("rebuild", source, rebuilt)
    assert rebuilt.read_bytes() == payload
    result = run("compare", source).strip()
    assert "PASS effects=" in result
    print(f"{label}: bytes={len(payload)} sha256={hashlib.sha256(payload).hexdigest()} {result}")


def test_boundary():
    """The independent reader and public records must survive deleting both legacy trees."""
    sources = sorted((ROOT / "mclib/fx").glob("fx_*.*"))
    assert len(sources) == 16
    for path in sources:
        text = path.read_text(encoding="utf-8")
        assert not re.search(r'#include\s*[<"][^">]*(?:gosfx|mlr|stuff|particles)[/\\]', text), path
        assert "gosFX::" not in text and "MidLevelRenderer::" not in text, path
    core = (ROOT / "tools/mc2fx/main.cpp").read_text(encoding="utf-8")
    for command in ("cmdDump", "cmdRebuild"):
        body = core.split(f"int {command}(", 1)[1].split("\nint ", 1)[0]
        assert "read_catalog" in body and "initEngineHeadless" not in body


@pytest.mark.parametrize("version,mlr", [(9, 3), (10, 5), (11, 6), (12, 9), (14, 9), (15, 9), (16, 9), (17, 9)])
def test_versioned_nonstock_catalogs(tmp_path, version, mlr):
    payload = wire_fixture(version, mlr)
    prove(payload, tmp_path, f"nonstock v{version}/MLR{mlr}")
    output = tmp_path / "full.json"
    run("dump", "--full", tmp_path / "input.fx", output)
    rows = json.loads(output.read_text())["effects"]
    assert len(rows) == 12 and {row["classID"] for row in rows} == {1312 + n for n in (0, 2, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14)}
    assert len(next(row for row in rows if row["classID"] == 1323)["geometry"][0]["primitives"]) == 19


def test_cli_corruption(tmp_path):
    payload = wire_fixture()
    bad = tmp_path / "bad.fx"
    for data in (payload[:7], payload[:-1], payload + b"\0", payload[:12] + u32(0xFFFFFFFF) + payload[16:]):
        bad.write_bytes(data)
        result = subprocess.run([str(EXE), "dump", str(bad)], capture_output=True, text=True, timeout=30)
        assert result.returncode == 4 and "FX byte" in result.stderr


def test_installed_catalogs(tmp_path):
    install_root = Path(os.environ.get("MC2_FX_INSTALL_ROOT", "A:/Games/mc2-opengl"))
    assert install_root.is_dir(), "installed provider evidence requires MC2_FX_INSTALL_ROOT"
    sys.path.insert(0, str(ROOT / "tools/mc2x_import"))
    from fst import fst_entries, decompress_entry
    catalogs = []
    stock = install_root / "mc2-win64-v0.4/data/effects/mc2.fx"
    stock_bytes = stock.read_bytes()
    assert len(stock_bytes) == 920265 and hashlib.sha256(stock_bytes).hexdigest() == STOCK_SHA
    catalogs.append(("stock loose mc2.fx", stock_bytes))
    for mod, relative, name in (
        ("MC2X-CVE-G base", "MC2X-CVE-G", "mc2.fx"),
        ("MC2X-CVE-G", "MC2X-CVE-G", "mc2x.fx"),
        ("MC2X-REAP3", "MC2X-REAP3", "mc2x.fx"),
        ("Exodus", "MC2-Exodus", "mc2.fx"),
        ("MagicExpansion", "MC2-MagicExpansion", "mc2.fx"),
        ("Omnitech", "MechCommanderOmnitech", "mc2.fx"),
        ("Carver5 (inherits Omnitech)", "MechCommanderOmnitech", "mc2.fx"),
    ):
        root = install_root / relative
        loose = root / "data/effects" / name
        found = False
        if loose.is_file():
            catalogs.append((f"{mod}: {loose}", loose.read_bytes()))
            found = True
        archive = root / "effect.fst"
        if archive.is_file():
            for off, comp, real, entry, is_lz, data in fst_entries(archive):
                if entry.lower().replace("\\", "/") == f"data/effects/{name}":
                    catalogs.append((f"{mod}: {archive}::{entry}", decompress_entry(off, comp, real, is_lz, data)))
                    found = True
        assert found, f"no resolved effect catalog provider found for {mod}"
    for label, payload in catalogs:
        prove(payload, tmp_path, label)
