#!/usr/bin/env python3
"""Reject BattleTech asset signatures in a staged directory or zip (no extraction)."""
from __future__ import annotations

import argparse
import codecs
from fnmatch import fnmatchcase
from pathlib import Path, PurePosixPath
import struct
import zipfile

CHUNK_SIZE = 1024 * 1024
TOKENS = tuple(b"chr" + name for name in (b"TxrMech_", b"PrfMech_", b"MatMech_"))
NODES = (b"j_" + b"COCKPIT", b"atlas_" + b"moveCore")
MAGICS = (b"UnityFS", b"UnityWeb", b"UnityRaw", b"Kaydara FBX Binary")
TEXT_SUFFIXES = {".py", ".md", ".json", ".bat", ".txt", ".rst", ".csv", ".yaml", ".yml", ".toml"}
TOOL_DIRS = ("tools/bt2018_kit/", "tools/bt2018_mechs/", "tools/bt2018_import/")
NAME_PATTERNS = (
    *(token.decode().lower() + "*" for token in TOKENS),
    "uixtxr_blip_*", "uixtxricon_*", "bt2018_*_hud_*.tga", "bt2018_*_bay.tga",
    "*_fbx2gltf.glb", "*.fbx", "*.assets", "*.ress", "versionmanifest.csv",
    "chassisdef_*.json", "mechdef_*.json", "hardpointdatadef_*.json",
    "movedef_*.json", "weapon_*.json", "assetstudio*", "fbx2gltf*", "*fbxsdk*.dll",
)


def _text_exception(name: str) -> bool:
    return (PurePosixPath(name).suffix in TEXT_SUFFIXES and
            (name.startswith(TOOL_DIRS + ("docs/", "scripts/tests/")) or
             name == "scripts/bt2018_retarget_clips.py"))


def _path_reason(name: str) -> str | None:
    parts = PurePosixPath(name).parts
    if any(part in {"mc2conversions", "battletech_2018_dump", "_bin"} for part in parts):
        return "extractor output/tool directory"
    if any(part in {"work", "workdir", "work_dir", ".venv"} for part in parts) and name.startswith(TOOL_DIRS):
        return "kit work directory"
    for prefix in ("mods/bt2018_mechs/", "mods/bt2018mechs/", "data/tgl/bt2018/"):
        if name.startswith(prefix):
            return "BattleTech asset directory"
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


def _inspect(name: str, stream) -> list[str]:
    """Bounded reads; overlap catches signatures split across chunk boundaries."""
    name = PurePosixPath(name.replace("\\", "/")).as_posix().lower()
    candidate = _text_exception(name)
    decoder = codecs.getincrementaldecoder("utf-8")("strict")
    text = candidate
    reasons = set()
    content = False
    nodes = set()
    tail = b""
    first = True
    overlap = max(map(len, TOKENS + NODES + MAGICS)) - 1
    while chunk := stream.read(CHUNK_SIZE):
        if first:
            if chunk.startswith(MAGICS):
                reasons.add("Unity/FBX binary magic")
            # A renamed GLB remains binary even with a JSON-only synthetic payload.
            if len(chunk) >= 20 and chunk[:4] == b"glTF":
                if struct.unpack_from("<I", chunk, 4)[0] == 2 and chunk[16:20] == b"JSON":
                    text = False
            first = False
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
        tail = window[-overlap:]
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
                            violations.extend(_inspect(name, stream))
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
                            violations.extend(_inspect(name, stream))
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
