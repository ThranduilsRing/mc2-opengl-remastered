#!/usr/bin/env python3
"""Reject BattleTech/MW5 asset signatures in a staged directory or zip (no extraction)."""
from __future__ import annotations

import argparse
import codecs
from fnmatch import fnmatchcase
from pathlib import Path, PurePosixPath
import re
import struct
import zipfile

CHUNK_SIZE = 1024 * 1024
TOKENS = tuple(b"chr" + name for name in (b"TxrMech_", b"PrfMech_", b"MatMech_"))
NODES = (b"j_" + b"COCKPIT", b"atlas_" + b"moveCore")
MW5_GENERATOR = b"tools/" + b"mw5_mechs"
MW5_NODES = (b"Torso_" + b"Twist", b"Foot0" + b"_", b"Weld" + b"Target_", b"_DM" + b"0")
PAK_MAGIC = struct.pack("<I", 0x5A6F12E1)
MC2_PACKET_MAGIC = struct.pack("<I", 0xFEEDFACE)
IOSTORE_MAGIC = b"-==-" * 4
UNREAL_MAGICS = (struct.pack("<I", 0x9E2A83C1), b"ACTR" + b"HEAD", b"ANIM" + b"HEAD")
MAGICS = (b"UnityFS", b"UnityWeb", b"UnityRaw", b"Kaydara FBX Binary")
ARCHIVE_MAGICS = (b"PK\x03\x04", b"PK\x05\x06", b"PK\x07\x08", b"7z\xbc\xaf\x27\x1c",
                  b"Rar!\x1a\x07\x00", b"Rar!\x1a\x07\x01\x00", b"\x1f\x8b")
TEXT_SUFFIXES = {".py", ".md", ".json", ".bat", ".txt", ".rst", ".csv", ".yaml", ".yml", ".toml"}
TOOL_DIRS = ("tools/bt2018_kit/", "tools/bt2018_mechs/", "tools/bt2018_import/",
             "tools/mw5_mechs/", "tools/mw5_kit/")
NAME_PATTERNS = (
    *(token.decode().lower() + "*" for token in TOKENS),
    "uixtxr_blip_*", "uixtxricon_*", "bt2018_*_hud_*.tga", "bt2018_*_bay.tga",
    "*_fbx2gltf.glb", "*.fbx", "*.assets", "*.ress", "versionmanifest.csv",
    "chassisdef_*.json", "mechdef_*.json", "hardpointdatadef_*.json",
    "movedef_*.json", "weapon_*.json", "assetstudio*", "fbx2gltf*", "*fbxsdk*.dll",
    "*.utoc", "*.ucas", "*.uasset", "*.uexp", "*.ubulk", "*.umap", "*.assetbundle", "*.uptnl", "*.usmap",
    "*.psk", "*.pskx", "*.psa", "*.uemodel", "*.ueanim",
    "umodel*", "fmodel*", "cue4parse*", "oo2core*", "oodle*", "weapon_mech_*",
    "*_skm.*", "*_msk.*", "*_mti.*", "timberwolf_*", "tbr_*",
)


def _text_exception(name: str) -> bool:
    if name in {"tools/bt2018_extract/recon.py", "tools/content_pipeline/frontends/bt2018.py",
                "tools/mw5_kit/extract/.gitignore", "tools/mw5_kit/extract/mw5extract.csproj",
                "tools/mw5_kit/extract/native.cs", "tools/mw5_kit/extract/program.cs"}:
        return True
    return (PurePosixPath(name).suffix in TEXT_SUFFIXES and
            (name.startswith(TOOL_DIRS + ("docs/", "scripts/tests/")) or
             name in {"scripts/bt2018_retarget_clips.py",
                      "tests/unit/fixtures/stock_names.txt", "tools/mech_import/build_marauder.py"}))


def _path_reason(name: str) -> str | None:
    parts = PurePosixPath(name).parts
    if any(part in {"mc2conversions", "battletech_2018_dump", "_bin",
                    "umodelexport", "mw5_extract", "mw5-ref"} for part in parts):
        return "extractor output/tool directory"
    if any(part in {"work", "workdir", "work_dir", ".venv"} for part in parts) and name.startswith(TOOL_DIRS):
        return "kit work directory"
    if any(part.lower().startswith(("bt2018", "mw5")) for part in parts):
        return "BattleTech/MW5 asset/tool name"
    if any(fnmatchcase(part, pattern) for part in parts for pattern in NAME_PATTERNS):
        return "forbidden asset/tool name"
    # The reviewed external manifest declares legacy deploy-only chassis files.
    if name.startswith("data/tgl/") and PurePosixPath(name).name in LEGACY_NAMES:
        return "legacy BattleTech chassis asset"
    return None


def _legacy_names() -> set[str]:
    import json
    manifest = Path(__file__).resolve().parents[1] / "docs/testing/mech_glb_external_pack.json"
    entries = json.loads(manifest.read_text(encoding="utf-8"))["entries"]
    names = set()
    for entry in entries:
        if entry.get("source_type") == "external":
            stem = PurePosixPath(entry["glb"]).stem.lower()
            names.update((stem + ".glb", stem + ".package.json"))
    return names


LEGACY_NAMES = _legacy_names()


def _inspect(name: str, stream, *, reject_archives: bool = False) -> list[str]:
    """Bounded reads; overlap catches signatures split across chunk boundaries."""
    name = PurePosixPath(name.replace("\\", "/")).as_posix().lower()
    candidate = _text_exception(name)
    decoder = codecs.getincrementaldecoder("utf-8")("strict")
    text = candidate
    reasons = set()
    content = False
    nodes = set()
    mw5_nodes = set()
    mw5_generator = False
    json_remaining = 0
    json_tail = b""
    footer = b""
    tail = b""
    first = True
    mc2_packet = False
    overlap = max(map(len, TOKENS + NODES + MAGICS + MW5_NODES +
                      (MW5_GENERATOR, IOSTORE_MAGIC))) - 1
    while chunk := stream.read(CHUNK_SIZE):
        json_chunk = chunk
        if first:
            mc2_packet = chunk.startswith(MC2_PACKET_MAGIC)
            if reject_archives and chunk.startswith(ARCHIVE_MAGICS):
                return [f"{name}: nested archive not allowed in release payload"]
            if chunk.startswith(MAGICS):
                reasons.add("Unity/FBX binary magic")
            if chunk.startswith(UNREAL_MAGICS):
                reasons.add("Unreal/ActorX binary magic")
            # A renamed GLB remains binary even with a JSON-only synthetic payload.
            if len(chunk) >= 20 and chunk[:4] == b"glTF":
                if struct.unpack_from("<I", chunk, 4)[0] == 2 and chunk[16:20] == b"JSON":
                    text = False
                    json_remaining = struct.unpack_from("<I", chunk, 12)[0]
                    json_chunk = chunk[20:]
            first = False
        if json_remaining:
            json_window = json_tail + json_chunk[:json_remaining]
            json_tail = json_window[-overlap * 6:]
            # Normalize ASCII JSON escapes, retaining raw overlap for split escapes.
            json_window = re.sub(rb'\\u00([0-9a-fA-F]{2})|\\([\\/"])',
                                 lambda m: bytes([int(m[1], 16)]) if m[1] else m[2], json_window)
            mw5_generator |= MW5_GENERATOR in json_window
            mw5_nodes.update(node for node in MW5_NODES if node in json_window)
            content |= any(token.lower() in json_window.lower() for token in TOKENS)
            nodes.update(node for node in NODES if node in json_window)
            json_remaining = max(0, json_remaining - len(json_chunk))
        if text:
            try:
                decoder.decode(chunk)
                text = b"\0" not in chunk
            except UnicodeDecodeError:
                text = False
        window = tail + chunk
        lower = window.lower()
        content |= any(token.lower() in lower for token in TOKENS)
        nodes.update(node for node in NODES if node in window)
        if IOSTORE_MAGIC in window:
            reasons.add("IoStore container signature")
        footer = (footer + chunk)[-256:]
        tail = window[-overlap:]
    if PAK_MAGIC in footer:
        reasons.add("Unreal pak footer signature")
    if PurePosixPath(name).suffix == ".pak" and not mc2_packet:
        reasons.add("foreign archive: missing MC2 PacketFile magic")
    if mw5_generator or (MW5_NODES[0] in mw5_nodes and mw5_nodes.intersection(MW5_NODES[1:])):
        reasons.add("MW5 GLB signature")
    if text:
        try:
            decoder.decode(b"", final=True)
        except UnicodeDecodeError:
            text = False
    reason = _path_reason(name)
    # Authored text recipes may have asset-like names; output/work directories
    # remain forbidden even when they contain text with an approved extension.
    if reason and (reason.endswith("directory") or not (candidate and text)):
        reasons.add(reason)
    if not (candidate and text):
        if content:
            reasons.add("BattleTech content signature")
        if len(nodes) == len(NODES):
            reasons.add("BattleTech skeleton signatures")
    return [f"{name}: {reason}" for reason in sorted(reasons)]


def scan(path: str | Path) -> list[str]:
    """Return violations; unreadable inputs fail closed. Zip members are streamed."""
    path = Path(path)
    violations = []
    try:
        if path.is_dir():
            for item in sorted(path.rglob("*")):
                name = item.relative_to(path).as_posix()
                if item.is_symlink():
                    violations.append(f"{name}: symlink cannot be reviewed")
                elif item.is_file():
                    try:
                        with item.open("rb") as stream:
                            violations.extend(_inspect(name, stream, reject_archives=True))
                    except OSError as exc:
                        violations.append(f"{name}: unreadable: {exc}")
        else:
            with zipfile.ZipFile(path) as archive:
                for member in archive.infolist():
                    name = member.filename.replace("\\", "/")
                    if ".." in PurePosixPath(name).parts or name.startswith("/") or ":" in name:
                        violations.append(f"{name}: unsafe archive path")
                    elif not member.is_dir():
                        with archive.open(member) as stream:
                            violations.extend(_inspect(name, stream, reject_archives=True))
    except (OSError, ValueError, RuntimeError, zipfile.BadZipFile) as exc:
        violations.append(f"{path}: scan failed: {exc}")
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, help="staged package directory or zip")
    args = parser.parse_args()
    violations = scan(args.path)
    for violation in violations:
        print(violation)
    return int(bool(violations))


if __name__ == "__main__":
    raise SystemExit(main())
