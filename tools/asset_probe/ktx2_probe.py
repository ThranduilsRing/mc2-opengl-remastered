#!/usr/bin/env python3
"""tools/asset_probe/ktx2_probe.py -- offline KTX2 bake+inspect probe.

Part of TRACKG-ASSET-PIPELINE-PROBE-OPUS-1 (slice KTX2-BAKE-PROBE-1).

This is an OFFLINE, DETERMINISTIC tooling probe. It:
  1. Generates a tiny fixed 8x8 source image (no randomness) via Pillow.
  2. Cooks it with the EXISTING tools/mc2texcook/mc2texcook.py for the
     albedo + normal presets (subprocess + sys.executable).
  3. Parses the cooked .ktx2 header with a small self-contained struct reader.
  4. ASSERTS the cooked output is correct (vkFormat / levelCount / dims),
     exiting nonzero on any mismatch.
  5. Emits a JSON summary to stdout and writes a manifest-ready textureRef
     provenance block to the summary file.

It performs NO runtime renderer/loader changes and adds NO dependencies
(stdlib + Pillow only). ALL generated binary artifacts (.png, .ktx2) and the
JSON summary live under out/asset-pipeline-probe/, which is gitignored.
"""
from __future__ import annotations

import json
import struct
import subprocess
import sys
from pathlib import Path

# --- Repo / output layout -------------------------------------------------
THIS_DIR = Path(__file__).resolve().parent
REPO_ROOT = THIS_DIR.parent.parent  # tools/asset_probe -> tools -> repo root
MC2TEXCOOK = REPO_ROOT / "tools" / "mc2texcook" / "mc2texcook.py"
OUT_DIR = REPO_ROOT / "out" / "asset-pipeline-probe"

KTX2_MAGIC = b"\xabKTX 20\xbb\r\n\x1a\n"  # 12 bytes

# Preset -> expected (vkFormat, colorSpace) per mc2texcook + validator convention.
PRESET_EXPECT = {
    "albedo": {"vkFormat": 43, "colorSpace": "srgb", "slot": "albedo"},
    "normal": {"vkFormat": 37, "colorSpace": "linear", "slot": "normal"},
}

SRC_SIZE = 8  # 8x8 -> floor(log2(8))+1 = 4 mips


def _make_source_image(path: Path) -> None:
    """Write a deterministic 8x8 RGBA checker PNG (fixed colors, no randomness)."""
    from PIL import Image  # type: ignore

    img = Image.new("RGBA", (SRC_SIZE, SRC_SIZE))
    px = img.load()
    a = (200, 60, 40, 255)
    b = (40, 80, 200, 255)
    for y in range(SRC_SIZE):
        for x in range(SRC_SIZE):
            px[x, y] = a if ((x + y) & 1) == 0 else b
    img.save(path, format="PNG")


def _cook(src: Path, preset: str, out_ktx2: Path) -> None:
    """Drive mc2texcook.py via subprocess (sys.executable). Raises on failure."""
    cmd = [
        sys.executable,
        str(MC2TEXCOOK),
        str(src),
        "--preset", preset,
        "--output", str(out_ktx2),
    ]
    res = subprocess.run(cmd, capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if res.returncode != 0:
        raise RuntimeError(
            f"mc2texcook failed (preset={preset}, rc={res.returncode}):\n"
            f"stdout: {res.stdout}\nstderr: {res.stderr}"
        )


def _read_ktx2_header(path: Path) -> dict:
    """Self-contained KTX2 header reader matching mc2texcook's writer exactly.

    Layout: magic(12) then 9x uint32 LE starting at offset 12:
      vkFormat, typeSize, pixelWidth, pixelHeight, pixelDepth,
      layerCount, faceCount, levelCount, supercompressionScheme.
    """
    data = path.read_bytes()
    if data[:12] != KTX2_MAGIC:
        raise ValueError(f"{path.name}: bad KTX2 magic")
    (vk_format, type_size, width, height, depth,
     layers, faces, level_count, supercompression) = struct.unpack_from(
        "<9I", data, 12)
    return {
        "vkFormat": vk_format,
        "typeSize": type_size,
        "pixelWidth": width,
        "pixelHeight": height,
        "pixelDepth": depth,
        "layerCount": layers,
        "faceCount": faces,
        "levelCount": level_count,
        "supercompressionScheme": supercompression,
    }


def _expected_mips(w: int, h: int) -> int:
    """floor(log2(max(w,h)))+1 -- full mip chain down to 1x1."""
    return max(w, h).bit_length()  # bit_length of a power of two == log2+1


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    src = OUT_DIR / "ktx2_probe_src.png"
    _make_source_image(src)

    expected_mips = _expected_mips(SRC_SIZE, SRC_SIZE)  # == 4 for 8x8
    texture_refs = []
    presets_report = []
    errors = []

    for preset, expect in PRESET_EXPECT.items():
        out_ktx2 = OUT_DIR / f"ktx2_probe_{preset}.ktx2"
        _cook(src, preset, out_ktx2)
        hdr = _read_ktx2_header(out_ktx2)

        # --- ASSERTIONS ---
        if hdr["vkFormat"] != expect["vkFormat"]:
            errors.append(
                f"{preset}: vkFormat {hdr['vkFormat']} != expected {expect['vkFormat']}")
        if hdr["levelCount"] != expected_mips:
            errors.append(
                f"{preset}: levelCount {hdr['levelCount']} != expected {expected_mips}")
        if (hdr["pixelWidth"], hdr["pixelHeight"]) != (SRC_SIZE, SRC_SIZE):
            errors.append(
                f"{preset}: dims {hdr['pixelWidth']}x{hdr['pixelHeight']} "
                f"!= expected {SRC_SIZE}x{SRC_SIZE}")

        presets_report.append({
            "preset": preset,
            "ktx2": str(out_ktx2),
            "vkFormat": hdr["vkFormat"],
            "mips": hdr["levelCount"],
            "dims": [hdr["pixelWidth"], hdr["pixelHeight"]],
        })
        texture_refs.append({
            "slot": expect["slot"],
            "path": out_ktx2.name,
            "format": "ktx2",
            "vkFormat": hdr["vkFormat"],
            "mips": hdr["levelCount"],
            "dims": [hdr["pixelWidth"], hdr["pixelHeight"]],
            "colorSpace": expect["colorSpace"],
            "cooked": True,
        })

    summary = {
        "slice": "KTX2-BAKE-PROBE-1",
        "opus": "TRACKG-ASSET-PIPELINE-PROBE-OPUS-1",
        "source": str(src),
        "sourceDims": [SRC_SIZE, SRC_SIZE],
        "expectedMips": expected_mips,
        "presets": presets_report,
        "textureRefs": texture_refs,
        "ok": not errors,
        "errors": errors,
    }

    summary_path = OUT_DIR / "ktx2_probe_summary.json"
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")

    print(json.dumps(summary, indent=2))

    if errors:
        print(f"\nKTX2-BAKE-PROBE-1 FAILED: {len(errors)} assertion(s)",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
