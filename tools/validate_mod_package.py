#!/usr/bin/env python3
"""Read-only package shape/integrity validation; legacy deploy inventory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re

ID = re.compile(r"[a-z0-9][a-z0-9._-]*\Z")
VERSION = re.compile(r"\d+(\.\d+){0,3}([-+][0-9A-Za-z.-]+)?\Z")


def read_json(path):
    value = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected object")
    return value


def safe_path(value):
    if not isinstance(value, str) or not value or "\\" in value:
        return False
    return all(p and not p.startswith(".") and not any(c in p for c in ':<>"|?*\x00')
               and not p.endswith((" ", ".")) for p in value.split("/"))


def payload_files(root):
    """Same project exclusions as the shipped packer; never follow links."""
    for directory, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if not d.startswith(".")
                   and not (Path(directory) == root and d in {"src", "out"})]
        for name in files:
            if not name.startswith(".") and not (Path(directory) == root and name == "package.json"):
                yield Path(directory) / name


def validate(root, check_files=False):
    root = Path(root)
    errors = []
    try:
        pkg = read_json(root / "package.json")
        mod = read_json(root / "mod.json")
        if pkg.get("schema") != "mc2-package/1":
            errors.append("schema must be mc2-package/1")
        if not isinstance(pkg.get("id"), str) or not ID.fullmatch(pkg["id"]):
            errors.append("invalid id")
        if pkg.get("id") != mod.get("id"):
            errors.append("id differs from mod.json")
        if not isinstance(pkg.get("name"), str):
            errors.append("name must be a string")
        if not isinstance(pkg.get("version"), str) or not VERSION.fullmatch(pkg["version"]):
            errors.append("invalid version")
        deps = pkg.get("dependencies", [])
        if not isinstance(deps, list) or any(
                not isinstance(d, dict) or not isinstance(d.get("id"), str)
                or not ID.fullmatch(d["id"]) or not isinstance(d.get("version", "*"), str)
                for d in deps):
            errors.append("dependencies must be objects with id and optional version string")
        elif [d["id"] for d in deps] != mod.get("dependencies", []):
            errors.append("dependencies differ from mod.json")
        entries = pkg.get("files")
        if not isinstance(entries, list) or not entries:
            errors.append("files must be a nonempty array")
            entries = []
        seen = set()
        for entry in entries:
            if not isinstance(entry, dict):
                errors.append("file entry must be an object")
                continue
            rel = entry.get("path")
            if not safe_path(rel) or rel == "package.json" or rel.split("/")[0] in {"src", "out"}:
                errors.append(f"unsafe/excluded path: {rel!r}")
                continue
            key = rel.lower()
            if key in seen:
                errors.append(f"duplicate path: {rel}")
            seen.add(key)
            size, sha = entry.get("size"), entry.get("sha256")
            if type(size) is not int or size < 0:
                errors.append(f"invalid size: {rel}")
            if not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{64}", sha):
                errors.append(f"invalid sha256: {rel}")
            path = root / rel
            if check_files:
                if not path.resolve().is_relative_to(root.resolve()) or not path.is_file():
                    errors.append(f"missing/escaped file: {rel}")
                elif path.stat().st_size != size or file_hash(path) != sha:
                    errors.append(f"integrity mismatch: {rel}")
        if "mod.json" not in seen:
            errors.append("files must include mod.json")
        if check_files:
            actual = {p.relative_to(root).as_posix().lower() for p in payload_files(root)}
            for rel in sorted(actual - seen):
                errors.append(f"undeclared file: {rel}")
    except (OSError, ValueError, TypeError) as exc:
        errors.append(str(exc))
    return errors


def file_hash(path):
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("roots", nargs="+", type=Path)
    parser.add_argument("--check-files", action="store_true")
    parser.add_argument("--legacy-install", action="store_true",
                        help="Inventory existing deploys; missing package metadata is LEGACY, not package PASS")
    args = parser.parse_args(argv)
    failed = False
    for root in args.roots:
        if args.legacy_install and not (root / "package.json").exists():
            if not (root / "data").is_dir():
                print(f"FAIL {root}: missing data directory")
                failed = True
                continue
            count = sum(1 for _ in payload_files(root / "data"))
            print(f"LEGACY {root}: {count} loose data files; no package.json (not integrity-certified)")
            mods = root / "mods"
            candidates = sorted(p for p in mods.iterdir() if p.is_dir() and not p.name.startswith(".")) if mods.is_dir() else []
            for mod in candidates:
                if (mod / "package.json").exists():
                    errors = validate(mod, args.check_files)
                    failed |= bool(errors)
                    print(f"{'FAIL' if errors else 'PASS'} {mod}: {'; '.join(errors)}")
                else:
                    print(f"LEGACY {mod}: no package.json")
            continue
        errors = validate(root, args.check_files)
        failed |= bool(errors)
        print(f"{'FAIL' if errors else 'PASS'} {root}: {'; '.join(errors)}")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
